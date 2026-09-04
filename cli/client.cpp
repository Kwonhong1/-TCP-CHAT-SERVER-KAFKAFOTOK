#include <boost/asio.hpp>
#include <boost/asio/ssl.hpp>

#include <iostream>
#include <string>
#include <vector>
#include <queue>
#include <memory>
#include <thread>
#include <atomic>
#include <cstring>
#include <limits>
#include <chrono>

#include "chat_protocol.pb.h"

using boost::asio::ip::tcp;
namespace ssl = boost::asio::ssl;

constexpr size_t MAX_PACKET_SIZE = 4 * 1024; // 서버와 동일한 4KB 제한

//=====================
// 서버 코드와 1:1 완벽 일치하는 MessageType Enum
//=====================
enum class MessageType : uint16_t
{
    LOGIN_PROMPT = 1000,
    LOGIN_REQUEST = 1001,
    LOGIN_RESPONSE = 1002,
    LOGOUT_REQUEST = 1003,
    LOGOUT_RESPONSE = 1004,
    CHAT_MESSAGE = 1005,
    JOIN_ROOM = 1006,
    LEAVE_ROOM = 1007,
    CREATE_ROOM_REQUEST = 1008,
    CREATE_ROOM_RESPONSE = 1009,
    ROOM_LIST_REQUEST = 1010,
    ROOM_LIST_RESPONSE = 1011,
    CHAT_HISTORY_REQUEST = 1012,
    CHAT_HISTORY_RESPONSE = 1013,
    SERVER_NOTIFICATION = 1014,
    REGISTER_REQUEST = 1015,
    REGISTER_RESPONSE = 1016,
    JOIN_ROOM_RESPONSE = 1017,
    LEAVE_ROOM_RESPONSE = 1018,
    WHISPER_REQUEST = 1019,
    WHISPER_RESPONSE = 1020,
    WHISPER_NOTIFICATION = 1021,
    KICK_USER_REQUEST = 1023,
    KICK_USER_RESPONSE = 1024,
    KICKED_NOTIFICATION = 1025,
    TRANSFER_MASTER_REQUEST = 1026,
    TRANSFER_MASTER_RESPONSE = 1027,
    MASTER_CHANGED_NOTIFICATION = 1028
};

#pragma pack(push, 1)
struct PacketHeader
{
    uint16_t packet_size;
    MessageType message_type;
    uint32_t user_id;
    uint32_t sequence_number;
};
#pragma pack(pop)

class ChatClient : public std::enable_shared_from_this<ChatClient> {
public:
    ChatClient(boost::asio::io_context& io_context, ssl::context& ssl_ctx)
        : io_context_(io_context), ssl_socket_(io_context, ssl_ctx) {}

    void Start(const std::string& host, const std::string& port) {
        tcp::resolver resolver(io_context_);
        auto endpoints = resolver.resolve(host, port);

        boost::asio::async_connect(ssl_socket_.lowest_layer(), endpoints,
            [this, self = shared_from_this()](boost::system::error_code ec, tcp::endpoint) {
                if (!ec) {
                    DoHandshake();
                } else {
                    is_connect_failed_ = true;
                    std::cerr << "[네트워크] 서버 연결 실패: " << ec.message() << std::endl;
                }
            });
    }

    template <typename T>
    void SendProtoMessage(MessageType msg_type, const T& proto_msg) {
        std::string payload;
        proto_msg.SerializeToString(&payload);

        PacketHeader header{};
        header.packet_size = static_cast<uint16_t>(sizeof(PacketHeader) + payload.size());
        header.message_type = msg_type;
        header.user_id = user_id_;
        header.sequence_number = 0;

        auto buf = std::make_shared<std::vector<char>>(header.packet_size);
        std::memcpy(buf->data(), &header, sizeof(PacketHeader));
        if (!payload.empty()) {
            std::memcpy(buf->data() + sizeof(PacketHeader), payload.data(), payload.size());
        }

        boost::asio::post(io_context_, [this, self = shared_from_this(), buf]() {
            bool write_in_progress = !write_queue_.empty();
            write_queue_.push(*buf);
            if (is_connected_ && !write_in_progress) {
                DoWrite();
            }
        });
    }

    bool IsConnected() const { return is_connected_; }
    bool IsConnectFailed() const { return is_connect_failed_; }
    
    void SetUserId(uint32_t id) { user_id_ = id; }
    uint32_t GetUserId() const { return user_id_; }

    void SetReconnectToken(const std::string& token) { reconnect_token_ = token; }
    std::string GetReconnectToken() const { return reconnect_token_; }

    void SetLastRoomId(uint32_t room_id) { last_room_id_ = room_id; }
    uint32_t GetLastRoomId() const { return last_room_id_; }

    void SetCurrentRoomOwnerId(uint32_t owner_id) { current_room_owner_id_ = owner_id; }
    bool IsRoomOwner() const { return user_id_ != 0 && user_id_ == current_room_owner_id_; }

    void SetRoomCreatedFlag(bool flag) { room_created_flag_ = flag; }
    bool GetRoomCreatedFlag() const { return room_created_flag_; }

    void SetAuthResponseReceived(bool flag) { auth_response_received_ = flag; }
    bool GetAuthResponseReceived() const { return auth_response_received_; }

    void Close() {
        boost::asio::post(io_context_, [this, self = shared_from_this()]() {
            if (ssl_socket_.lowest_layer().is_open()) {
                boost::system::error_code ec;
                ssl_socket_.lowest_layer().close(ec);
            }
            is_connected_ = false;
        });
    }

private:
    void DoHandshake() {
        ssl_socket_.async_handshake(ssl::stream_base::client,
            [this, self = shared_from_this()](boost::system::error_code ec) {
                if (!ec) {
                    is_connected_ = true;
                    std::cout << "[네트워크] SSL/TLS 암호화 연결 성공!\n";
                    DoReadHeader();
                } else {
                    is_connect_failed_ = true;
                    std::cerr << "[네트워크] SSL 핸드셰이크 실패: " << ec.message() << std::endl;
                }
            });
    }

    void DoReadHeader() {
        header_buffer_.resize(sizeof(PacketHeader));
        boost::asio::async_read(ssl_socket_, boost::asio::buffer(header_buffer_),
            [this, self = shared_from_this()](boost::system::error_code ec, std::size_t) {
                if (!ec) {
                    PacketHeader header;
                    std::memcpy(&header, header_buffer_.data(), sizeof(PacketHeader));

                    if (header.packet_size < sizeof(PacketHeader) || header.packet_size > MAX_PACKET_SIZE) {
                        std::cerr << "[보안] 비정상적인 패킷 크기 수신: " << header.packet_size << std::endl;
                        Close();
                        return;
                    }

                    uint16_t payload_size = header.packet_size - sizeof(PacketHeader);
                    if (payload_size > 0) {
                        DoReadPayload(header, payload_size);
                    } else {
                        ProcessPacket(header, nullptr, 0);
                        DoReadHeader();
                    }
                } else {
                    std::cerr << "\n[네트워크] 서버와의 연결이 종료되었습니다." << std::endl;
                    Close();
                }
            });
    }

    void DoReadPayload(PacketHeader header, uint16_t payload_size) {
        payload_buffer_.resize(payload_size);
        boost::asio::async_read(ssl_socket_, boost::asio::buffer(payload_buffer_),
            [this, self = shared_from_this(), header](boost::system::error_code ec, std::size_t) {
                if (!ec) {
                    ProcessPacket(header, payload_buffer_.data(), payload_buffer_.size());
                    DoReadHeader();
                } else {
                    Close();
                }
            });
    }

    void ProcessPacket(const PacketHeader& header, const char* payload, size_t payload_size) {
        switch (header.message_type) {
        case MessageType::LOGIN_PROMPT:
            std::cout << "[시스템] 서버 연결 확인. 인증 진행이 가능합니다.\n";
            break;

        case MessageType::LOGIN_RESPONSE: {
            chat::LoginResponse res;
            if (res.ParseFromArray(payload, static_cast<int>(payload_size))) {
                if (res.success()) {
                    SetUserId(res.assigned_user_id());
                    SetReconnectToken(res.reconnect_token());
                    std::cout << "\n[시스템] 로그인 성공! (유저 ID: " << res.assigned_user_id() << ")\n";
                } else {
                    std::cout << "\n[시스템] 로그인 실패: " << res.error_message() << "\n";
                }
            }
            SetAuthResponseReceived(true);
            break;
        }

        case MessageType::REGISTER_RESPONSE: {
            chat::RegisterResponse res;
            if (res.ParseFromArray(payload, static_cast<int>(payload_size))) {
                if (res.success()) {
                    std::cout << "\n[시스템] 회원가입 완료! (할당 유저 ID: " << res.assigned_user_id() << ")\n";
                } else {
                    std::cout << "\n[시스템] 회원가입 실패: " << res.error_message() << "\n";
                }
            }
            SetAuthResponseReceived(true);
            break;
        }

        case MessageType::CREATE_ROOM_RESPONSE: {
            chat::CreateRoomResponse res;
            if (res.ParseFromArray(payload, static_cast<int>(payload_size))) {
                if (res.success()) {
                    SetLastRoomId(res.created_room_id());
                    SetCurrentRoomOwnerId(res.owner_id());
                    SetRoomCreatedFlag(true);
                    std::cout << "\n[시스템] 방 생성 성공! (방 번호: " << res.created_room_id() << ")\n";
                } else {
                    SetRoomCreatedFlag(false);
                    std::cout << "\n[시스템] 방 생성 실패: " << res.error_message() << "\n";
                }
            }
            break;
        }

        case MessageType::ROOM_LIST_RESPONSE: {
            chat::RoomListResponse res;
            if (res.ParseFromArray(payload, static_cast<int>(payload_size))) {
                std::cout << "\n================ [현재 개설된 방 목록] ================\n";
                for (const auto& room : res.rooms()) {
                    std::cout << "방 ID: " << room.room_id()
                        << " | 제목: " << room.room_name()
                        << " | 인원: (" << room.current_users() << "/" << room.max_users() << ")"
                        << " | 방장 ID: " << room.owner_id() << "\n";
                }
                std::cout << "=======================================================\n";
            }
            break;
        }

        case MessageType::JOIN_ROOM_RESPONSE: {
            chat::JoinRoomResponse res;
            if (res.ParseFromArray(payload, static_cast<int>(payload_size))) {
                if (res.success()) {
                    SetLastRoomId(res.room_id());
                    SetCurrentRoomOwnerId(res.owner_id());
                    std::cout << "\n[시스템] #" << res.room_id() << "번 방 입장에 성공했습니다.\n";
                } else {
                    std::cout << "\n[시스템] 방 입장 실패: " << res.error_message() << "\n";
                }
            }
            break;
        }

        case MessageType::LEAVE_ROOM_RESPONSE: {
            chat::LeaveRoomResponse res;
            if (res.ParseFromArray(payload, static_cast<int>(payload_size))) {
                if (res.success()) {
                    SetLastRoomId(0);
                    SetCurrentRoomOwnerId(0);
                    std::cout << "\n[시스템] 정상적으로 퇴장했습니다.\n";
                }
            }
            break;
        }

        case MessageType::CHAT_MESSAGE: {
            chat::ChatMessage msg;
            if (msg.ParseFromArray(payload, static_cast<int>(payload_size))) {
                std::string sender = msg.sender_username().empty() ? std::to_string(msg.sender_id()) : msg.sender_username();
                std::cout << "\n[" << sender << "]: " << msg.message() << std::endl;
            }
            break;
        }

        case MessageType::CHAT_HISTORY_RESPONSE: {
            chat::ChatHistoryResponse res;
            if (res.ParseFromArray(payload, static_cast<int>(payload_size))) {
                if (res.success()) {
                    std::cout << "\n================ [이전 대화 기록] ================\n";
                    for (const auto& msg : res.messages()) {
                        std::cout << "[" << msg.sender_username() << "]: " << msg.message() << "\n";
                    }
                    std::cout << "==================================================\n";
                } else {
                    std::cout << "\n[시스템] 기록 불러오기 실패: " << res.error_message() << "\n";
                }
            }
            break;
        }

        case MessageType::WHISPER_NOTIFICATION: {
            chat::WhisperNotification noti;
            if (noti.ParseFromArray(payload, static_cast<int>(payload_size))) {
                std::cout << "\n[귓속말 - " << noti.sender_username() << "]: " << noti.message() << std::endl;
            }
            break;
        }

        case MessageType::KICKED_NOTIFICATION: {
            chat::KickedNotification noti;
            if (noti.ParseFromArray(payload, static_cast<int>(payload_size))) {
                SetLastRoomId(0);
                SetCurrentRoomOwnerId(0);
                std::cout << "\n[알림] 방에서 강퇴당했습니다. 사유: " << noti.reason() << std::endl;
            }
            break;
        }

        case MessageType::MASTER_CHANGED_NOTIFICATION: {
            chat::MasterChangedNotification noti;
            if (noti.ParseFromArray(payload, static_cast<int>(payload_size))) {
                SetCurrentRoomOwnerId(noti.new_master_id());
                std::cout << "\n[알림] 방장이 변경되었습니다! (새 방장 유저 ID: " << noti.new_master_id() << ")" << std::endl;
            }
            break;
        }

        default:
            break;
        }
    }

    void DoWrite() {
        boost::asio::async_write(ssl_socket_, boost::asio::buffer(write_queue_.front()),
            [this, self = shared_from_this()](boost::system::error_code ec, std::size_t) {
                if (!ec) {
                    write_queue_.pop();
                    if (!write_queue_.empty()) {
                        DoWrite();
                    }
                } else {
                    Close();
                }
            });
    }

    boost::asio::io_context& io_context_;
    ssl::stream<tcp::socket> ssl_socket_;
    std::atomic<bool> is_connected_{ false };
    std::atomic<bool> is_connect_failed_{ false };
    std::queue<std::vector<char>> write_queue_;

    std::vector<char> header_buffer_;
    std::vector<char> payload_buffer_;

    std::atomic<uint32_t> user_id_{ 0 };
    std::string reconnect_token_;
    std::atomic<uint32_t> last_room_id_{ 0 };
    std::atomic<uint32_t> current_room_owner_id_{ 0 };
    std::atomic<bool> room_created_flag_{ false };
    std::atomic<bool> auth_response_received_{ false };
};

void RunRoomLoop(std::shared_ptr<ChatClient> client, uint32_t room_id) {
    while (client->IsConnected()) {
        if (client->GetLastRoomId() == 0) break;

        std::string input;
        std::getline(std::cin, input);
        if (input.empty()) continue;

        if (input == "/leave") {
            chat::LeaveRoomRequest leave_req;
            leave_req.set_room_id(room_id);
            client->SendProtoMessage(MessageType::LEAVE_ROOM, leave_req);
            client->SetLastRoomId(0);
            client->SetCurrentRoomOwnerId(0);
            client->SetRoomCreatedFlag(false);
            break;
        }
        else if (input == "/history") {
            chat::ChatHistoryRequest req;
            req.set_room_id(room_id);
            req.set_last_message_id(0);
            req.set_count(20); // 서버 요구 규격 count 적용
            client->SendProtoMessage(MessageType::CHAT_HISTORY_REQUEST, req);
        }
        else if (input.rfind("/kick ", 0) == 0) {
            if (!client->IsRoomOwner()) {
                std::cout << "[시스템] 방장만 /kick 명령어를 사용할 수 있습니다.\n";
                continue;
            }
            try {
                uint32_t target_id = std::stoul(input.substr(6));
                chat::KickUserRequest kick_req;
                kick_req.set_room_id(room_id);
                kick_req.set_target_user_id(target_id);
                client->SendProtoMessage(MessageType::KICK_USER_REQUEST, kick_req);
            } catch (const std::exception&) {
                std::cout << "[시스템] 사용법: /kick [유저ID]\n";
            }
        }
        else if (input.rfind("/pass ", 0) == 0) {
            if (!client->IsRoomOwner()) {
                std::cout << "[시스템] 방장만 /pass 명령어를 사용할 수 있습니다.\n";
                continue;
            }
            try {
                uint32_t target_id = std::stoul(input.substr(6));
                chat::TransferMasterRequest pass_req;
                pass_req.set_room_id(room_id);
                pass_req.set_new_master_id(target_id);
                client->SendProtoMessage(MessageType::TRANSFER_MASTER_REQUEST, pass_req);
            } catch (const std::exception&) {
                std::cout << "[시스템] 사용법: /pass [유저ID]\n";
            }
        }
        else if (input.rfind("/w ", 0) == 0) {
            size_t space_pos = input.find(' ', 3);
            if (space_pos != std::string::npos) {
                std::string target_name = input.substr(3, space_pos - 3);
                std::string msg = input.substr(space_pos + 1);
                chat::WhisperRequest w_req;
                w_req.set_target_username(target_name);
                w_req.set_message(msg);
                client->SendProtoMessage(MessageType::WHISPER_REQUEST, w_req);
            }
        }
        else {
            chat::ChatMessage chat_msg;
            chat_msg.set_room_id(room_id);
            chat_msg.set_sender_id(client->GetUserId());
            chat_msg.set_message(input);
            chat_msg.set_timestamp(std::chrono::duration_cast<std::chrono::seconds>(
                std::chrono::system_clock::now().time_since_epoch()).count());
            client->SendProtoMessage(MessageType::CHAT_MESSAGE, chat_msg);
        }
    }
}

int main() {
    try {
        boost::asio::io_context io_context;

        ssl::context ssl_ctx(ssl::context::tlsv12_client);
        ssl_ctx.set_verify_mode(ssl::verify_none);

        auto client = std::make_shared<ChatClient>(io_context, ssl_ctx);
        auto work_guard = boost::asio::make_work_guard(io_context);

        std::thread io_thread([&io_context]() { io_context.run(); });

        client->Start("127.0.0.1", "8080");

        while (!client->IsConnected() && !client->IsConnectFailed()) {
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
        }

        if (client->IsConnectFailed()) {
            work_guard.reset();
            if (io_thread.joinable()) io_thread.join();
            return 0;
        }

        bool is_running = true;

        // 1. 인증 메인 루프 (회원가입 / 로그인 / 토큰 재연결)
        while (client->IsConnected() && client->GetUserId() == 0) {
            std::cout << "\n=== [인증 메뉴] ===\n";
            std::cout << "1. 회원가입\n";
            std::cout << "2. 로그인\n";
            std::cout << "선택: ";
            int choice = 0;
            if (!(std::cin >> choice)) break;

            if (choice == 1) {
                std::string username, password;
                std::cout << "아이디: "; std::cin >> username;
                std::cout << "비밀번호: "; std::cin >> password;

                chat::RegisterRequest req;
                req.set_username(username);
                req.set_password(password);

                client->SetAuthResponseReceived(false);
                client->SendProtoMessage(MessageType::REGISTER_REQUEST, req);

                for (int i = 0; i < 500 && client->IsConnected(); ++i) {
                    if (client->GetAuthResponseReceived()) break;
                    std::this_thread::sleep_for(std::chrono::milliseconds(10));
                }
            }
            else if (choice == 2) {
                std::string username, password;
                std::cout << "아이디: "; std::cin >> username;
                std::cout << "비밀번호: "; std::cin >> password;

                chat::LoginRequest req;
                req.set_username(username);
                req.set_password(password);

                client->SetAuthResponseReceived(false);
                client->SendProtoMessage(MessageType::LOGIN_REQUEST, req);

                for (int i = 0; i < 500 && client->IsConnected(); ++i) {
                    if (client->GetAuthResponseReceived()) break;
                    std::this_thread::sleep_for(std::chrono::milliseconds(10));
                }
            }
        }

        // 2. 로그인 성공 후 메인 로비 루프
        while (client->IsConnected() && is_running) {
            std::cout << "\n=== [메인 메뉴] (내 유저 ID: " << client->GetUserId() << ") ===\n";
            std::cout << "1. 방 목록 조회\n";
            std::cout << "2. 방 만들기\n";
            std::cout << "3. 방 입장하기\n";
            std::cout << "4. 프로그램 종료\n";
            std::cout << "선택: ";

            int menu_choice = 0;
            if (!(std::cin >> menu_choice)) break;

            if (menu_choice == 1) {
                chat::RoomListRequest req;
                client->SendProtoMessage(MessageType::ROOM_LIST_REQUEST, req);
                std::this_thread::sleep_for(std::chrono::milliseconds(200));
            }
            else if (menu_choice == 2) {
                std::string room_name;
                uint32_t max_users = 10;
                std::cout << "방 제목: "; std::cin >> room_name;
                std::cout << "최대 인원: "; std::cin >> max_users;

                chat::CreateRoomRequest req;
                req.set_room_name(room_name);
                req.set_max_users(max_users);

                client->SetLastRoomId(0);
                client->SetCurrentRoomOwnerId(0);
                client->SetRoomCreatedFlag(false);
                client->SendProtoMessage(MessageType::CREATE_ROOM_REQUEST, req);

                for (int i = 0; i < 500 && client->IsConnected(); ++i) {
                    if (client->GetRoomCreatedFlag()) break;
                    std::this_thread::sleep_for(std::chrono::milliseconds(10));
                }

                const uint32_t created_room_id = client->GetLastRoomId();
                if (created_room_id > 0) {
                    std::cout << "\n>>> #" << created_room_id << "번 방 입장 ('/history': 기록, '/leave': 퇴장) <<<\n";
                    std::cin.ignore(std::numeric_limits<std::streamsize>::max(), '\n');
                    RunRoomLoop(client, created_room_id);
                }
            }
            else if (menu_choice == 3) {
                uint32_t target_room_id = 0;
                std::cout << "입장할 방 번호: "; std::cin >> target_room_id;

                chat::JoinRoomRequest join_req;
                join_req.set_room_id(target_room_id);

                client->SetLastRoomId(0);
                client->SetCurrentRoomOwnerId(0);
                client->SendProtoMessage(MessageType::JOIN_ROOM, join_req);

                for (int i = 0; i < 400 && client->IsConnected(); ++i) {
                    if (client->GetLastRoomId() == target_room_id) break;
                    std::this_thread::sleep_for(std::chrono::milliseconds(10));
                }

                if (client->GetLastRoomId() == target_room_id) {
                    std::cout << "\n>>> #" << target_room_id << "번 방 입장 ('/history': 기록, '/leave': 퇴장) <<<\n";
                    std::cin.ignore(std::numeric_limits<std::streamsize>::max(), '\n');
                    RunRoomLoop(client, target_room_id);
                }
            }
            else if (menu_choice == 4) {
                is_running = false;
            }
        }

        client->Close();
        work_guard.reset();
        if (io_thread.joinable()) io_thread.join();

    } catch (const std::exception& e) {
        std::cerr << "예외 발생: " << e.what() << std::endl;
    }
    return 0;
}

