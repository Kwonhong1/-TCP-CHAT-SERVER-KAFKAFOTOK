#include <boost/asio.hpp>
#include <boost/asio/ssl.hpp>
#include <boost/endian/conversion.hpp>

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
#include <future>

#include "chat_protocol.pb.h"

using boost::asio::ip::tcp;
namespace ssl = boost::asio::ssl;

constexpr size_t MAX_PACKET_SIZE = 20 * 1024;
constexpr std::size_t PACKET_HEADER_SIZE = 12;

//==================================================
// MessageType
//==================================================
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
    MASTER_CHANGED_NOTIFICATION = 1028,
    PING = 1029,
    PONG = 1030
};

//==================================================
// Packet Header
//==================================================
struct PacketHeader
{
    uint16_t packet_size = 0;
    MessageType message_type{};
    uint32_t user_id = 0;
    uint32_t sequence_number = 0;
};

//==================================================
// Packet Header Encoding
//==================================================
void EncodePacketHeader(const PacketHeader& header, char* dst)
{
    uint16_t packet_size = boost::endian::native_to_little(header.packet_size);
    uint16_t message_type = boost::endian::native_to_little(static_cast<uint16_t>(header.message_type));
    uint32_t user_id = boost::endian::native_to_little(header.user_id);
    uint32_t sequence_number = boost::endian::native_to_little(header.sequence_number);

    std::memcpy(dst + 0, &packet_size, sizeof(packet_size));
    std::memcpy(dst + 2, &message_type, sizeof(message_type));
    std::memcpy(dst + 4, &user_id, sizeof(user_id));
    std::memcpy(dst + 8, &sequence_number, sizeof(sequence_number));
}

PacketHeader DecodePacketHeader(const char* src)
{
    uint16_t packet_size = 0;
    uint16_t message_type = 0;
    uint32_t user_id = 0;
    uint32_t sequence_number = 0;

    std::memcpy(&packet_size, src + 0, sizeof(packet_size));
    std::memcpy(&message_type, src + 2, sizeof(message_type));
    std::memcpy(&user_id, src + 4, sizeof(user_id));
    std::memcpy(&sequence_number, src + 8, sizeof(sequence_number));

    PacketHeader header{};
    header.packet_size = boost::endian::little_to_native(packet_size);
    header.message_type = static_cast<MessageType>(boost::endian::little_to_native(message_type));
    header.user_id = boost::endian::little_to_native(user_id);
    header.sequence_number = boost::endian::little_to_native(sequence_number);

    return header;
}

//==================================================
// ChatClient
//==================================================
class ChatClient : public std::enable_shared_from_this<ChatClient>
{
public:
    ChatClient(boost::asio::io_context& io_context, ssl::context& ssl_ctx)
        : io_context_(io_context), ssl_socket_(io_context, ssl_ctx) {}

    std::future<bool> ConnectAsync(const std::string& host, const std::string& port)
    {
        auto promise = std::make_shared<std::promise<bool>>();
        auto future = promise->get_future();

        tcp::resolver resolver(io_context_);
        auto endpoints = resolver.resolve(host, port);

        boost::asio::async_connect(
            ssl_socket_.lowest_layer(),
            endpoints,
            [this, self = shared_from_this(), promise](boost::system::error_code ec, tcp::endpoint)
            {
                if (!ec) {
                    DoHandshake(promise);
                } else {
                    is_connected_ = false;
                    std::cerr << "[네트워크] 서버 연결 실패: " << ec.message() << '\n';
                    promise->set_value(false);
                }
            });

        return future;
    }

    template <typename T>
    void SendProtoMessage(MessageType msg_type, const T& proto_msg)
    {
        std::string payload;

        if (!proto_msg.SerializeToString(&payload)) {
            std::cerr << "[직렬화] Protobuf 직렬화 실패\n";
            return;
        }

        const size_t total_size = PACKET_HEADER_SIZE + payload.size();

        if (total_size > MAX_PACKET_SIZE || total_size > std::numeric_limits<uint16_t>::max()) {
            std::cerr << "[보안] 전송 패킷 크기 초과: " << total_size << '\n';
            return;
        }

        PacketHeader header{};
        header.packet_size = static_cast<uint16_t>(total_size);
        header.message_type = msg_type;
        header.user_id = user_id_.load();
        header.sequence_number = 0;

        std::vector<char> packet(total_size);
        EncodePacketHeader(header, packet.data());

        if (!payload.empty()) {
            std::memcpy(packet.data() + PACKET_HEADER_SIZE, payload.data(), payload.size());
        }

        boost::asio::post(
            io_context_,
            [this, self = shared_from_this(), packet = std::move(packet)]() mutable
            {
                bool write_in_progress = !write_queue_.empty();
                write_queue_.push(std::move(packet));

                if (is_connected_ && !write_in_progress) {
                    DoWrite();
                }
            });
    }

    void SendRawMessage(MessageType msg_type)
    {
        PacketHeader header{};
        header.packet_size = static_cast<uint16_t>(PACKET_HEADER_SIZE);
        header.message_type = msg_type;
        header.user_id = user_id_.load();
        header.sequence_number = 0;

        std::vector<char> packet(PACKET_HEADER_SIZE);
        EncodePacketHeader(header, packet.data());

        boost::asio::post(
            io_context_,
            [this, self = shared_from_this(), packet = std::move(packet)]() mutable
            {
                bool write_in_progress = !write_queue_.empty();
                write_queue_.push(std::move(packet));

                if (is_connected_ && !write_in_progress) {
                    DoWrite();
                }
            });
    }

    void StartHeartbeatTimer()
    {
        if (!is_connected_) return;

        ping_timer_.expires_after(std::chrono::seconds(15));
        ping_timer_.async_wait(
            [this, self = shared_from_this()](boost::system::error_code ec)
            {
                if (!ec && is_connected_) {
                    SendRawMessage(MessageType::PING);
                    StartHeartbeatTimer();
                }
            });
    }

    //==================================================
    // Promise 등록
    //==================================================
    void RegisterAuthPromise(std::shared_ptr<std::promise<bool>> promise)
    {
        boost::asio::post(io_context_, [this, promise]()
        {
            if (auth_promise_) {
                try { auth_promise_->set_value(false); } catch (...) {}
            }
            auth_promise_ = promise;
        });
    }

    void RegisterRoomPromise(std::shared_ptr<std::promise<uint32_t>> promise)
    {
        boost::asio::post(io_context_, [this, promise]()
        {
            if (room_promise_) {
                try { room_promise_->set_value(0); } catch (...) {}
            }
            room_promise_ = promise;
        });
    }

    void RegisterLeavePromise(std::shared_ptr<std::promise<bool>> promise)
    {
        boost::asio::post(io_context_, [this, promise]()
        {
            if (leave_promise_) {
                try { leave_promise_->set_value(false); } catch (...) {}
            }
            leave_promise_ = promise;
        });
    }

    //==================================================
    // 상태
    //==================================================
    bool IsConnected() const { return is_connected_.load(); }

    void SetUserId(uint32_t id) { user_id_ = id; }
    uint32_t GetUserId() const { return user_id_.load(); }

    void SetLastRoomId(uint32_t room_id) { last_room_id_ = room_id; }
    uint32_t GetLastRoomId() const { return last_room_id_.load(); }

    void SetCurrentRoomOwnerId(uint32_t owner_id) { current_room_owner_id_ = owner_id; }

    bool IsRoomOwner() const
    {
        return user_id_.load() != 0 && user_id_.load() == current_room_owner_id_.load();
    }

    //==================================================
    // Close
    //==================================================
    void Close()
    {
        boost::asio::post(io_context_, [this, self = shared_from_this()]()
        {
            boost::system::error_code ec;

            ping_timer_.cancel(ec);

            if (ssl_socket_.lowest_layer().is_open()) {
                ssl_socket_.lowest_layer().close(ec);
            }

            is_connected_ = false;

            if (auth_promise_) {
                try { auth_promise_->set_value(false); } catch (...) {}
                auth_promise_.reset();
            }

            if (room_promise_) {
                try { room_promise_->set_value(0); } catch (...) {}
                room_promise_.reset();
            }

            if (leave_promise_) {
                try { leave_promise_->set_value(false); } catch (...) {}
                leave_promise_.reset();
            }
        });
    }

private:
    //==================================================
    // TLS
    //==================================================
    void DoHandshake(std::shared_ptr<std::promise<bool>> promise)
    {
        ssl_socket_.async_handshake(
            ssl::stream_base::client,
            [this, self = shared_from_this(), promise](boost::system::error_code ec)
            {
                if (!ec) {
                    is_connected_ = true;
                    std::cout << "[네트워크] SSL/TLS 암호화 연결 성공!\n";

                    StartHeartbeatTimer();
                    DoReadHeader();
                    promise->set_value(true);
                } else {
                    is_connected_ = false;
                    std::cerr << "[네트워크] SSL 핸드셰이크 실패: " << ec.message() << '\n';
                    promise->set_value(false);
                }
            });
    }

    //==================================================
    // Read
    //==================================================
    void DoReadHeader()
    {
        header_buffer_.resize(PACKET_HEADER_SIZE);

        boost::asio::async_read(
            ssl_socket_,
            boost::asio::buffer(header_buffer_),
            [this, self = shared_from_this()](boost::system::error_code ec, std::size_t)
            {
                if (ec) {
                    std::cerr << "\n[네트워크] 서버와의 연결이 종료되었습니다.\n";
                    Close();
                    return;
                }

                PacketHeader header = DecodePacketHeader(header_buffer_.data());

                if (header.packet_size < PACKET_HEADER_SIZE || header.packet_size > MAX_PACKET_SIZE) {
                    std::cerr << "[보안] 비정상 패킷 수신: " << header.packet_size << '\n';
                    Close();
                    return;
                }

                size_t payload_size = header.packet_size - PACKET_HEADER_SIZE;

                if (payload_size > 0) {
                    DoReadPayload(header, payload_size);
                } else {
                    ProcessPacket(header, nullptr, 0);
                    DoReadHeader();
                }
            });
    }

    void DoReadPayload(PacketHeader header, size_t payload_size)
    {
        payload_buffer_.resize(payload_size);

        boost::asio::async_read(
            ssl_socket_,
            boost::asio::buffer(payload_buffer_),
            [this, self = shared_from_this(), header](boost::system::error_code ec, std::size_t)
            {
                if (ec) {
                    Close();
                    return;
                }

                ProcessPacket(header, payload_buffer_.data(), payload_buffer_.size());
                DoReadHeader();
            });
    }

    //==================================================
    // Packet 처리
    //==================================================
    void ProcessPacket(const PacketHeader& header, const char* payload, size_t payload_size)
    {
        switch (header.message_type)
        {
        case MessageType::PONG:
            break;

        case MessageType::LOGIN_PROMPT:
            std::cout << "[시스템] 서버 연결 확인. 인증 진행이 가능합니다.\n";
            break;

        case MessageType::LOGIN_RESPONSE: {
            chat::LoginResponse res;
            bool success = false;

            if (res.ParseFromArray(payload, static_cast<int>(payload_size))) {
                if (res.success()) {
                    SetUserId(res.assigned_user_id());
                    std::cout << "\n[시스템] 로그인 성공! (유저 ID: " << res.assigned_user_id() << ")\n";
                    success = true;
                } else {
                    std::cout << "\n[시스템] 로그인 실패: " << res.error_message() << '\n';
                }
            }

            if (auth_promise_) {
                try { auth_promise_->set_value(success); } catch (...) {}
                auth_promise_.reset();
            }
            break;
        }

        case MessageType::REGISTER_RESPONSE: {
            chat::RegisterResponse res;
            bool success = false;

            if (res.ParseFromArray(payload, static_cast<int>(payload_size))) {
                if (res.success()) {
                    std::cout << "\n[시스템] 회원가입 완료! (유저 ID: " << res.assigned_user_id() << ")\n";
                    success = true;
                } else {
                    std::cout << "\n[시스템] 회원가입 실패: " << res.error_message() << '\n';
                }
            }

            if (auth_promise_) {
                try { auth_promise_->set_value(success); } catch (...) {}
                auth_promise_.reset();
            }
            break;
        }

        case MessageType::CREATE_ROOM_RESPONSE: {
            chat::CreateRoomResponse res;
            uint32_t room_id = 0;

            if (res.ParseFromArray(payload, static_cast<int>(payload_size))) {
                if (res.success()) {
                    room_id = res.created_room_id();
                    SetLastRoomId(room_id);
                    SetCurrentRoomOwnerId(res.owner_id());

                    std::cout << "\n[시스템] 방 생성 성공! (방 번호: " << room_id << ")\n";
                } else {
                    std::cout << "\n[시스템] 방 생성 실패: " << res.error_message() << '\n';
                }
            }

            if (room_promise_) {
                try { room_promise_->set_value(room_id); } catch (...) {}
                room_promise_.reset();
            }
            break;
        }

        case MessageType::JOIN_ROOM_RESPONSE: {
            chat::JoinRoomResponse res;
            uint32_t room_id = 0;

            if (res.ParseFromArray(payload, static_cast<int>(payload_size))) {
                if (res.success()) {
                    room_id = res.room_id();
                    SetLastRoomId(room_id);
                    SetCurrentRoomOwnerId(res.owner_id());

                    std::cout << "\n[시스템] #" << room_id << "번 방 입장에 성공했습니다.\n";

                    if (res.recent_messages_size() > 0) {
                        std::cout << "\n========== [최근 대화] ==========\n";

                        for (const auto& msg : res.recent_messages()) {
                            std::string sender = msg.sender_username().empty()
                                ? std::to_string(msg.sender_id())
                                : msg.sender_username();

                            std::cout << "[" << sender << "]: " << msg.message() << '\n';
                        }

                        std::cout << "=================================\n";
                    }
                } else {
                    std::cout << "\n[시스템] 방 입장 실패: " << res.error_message() << '\n';
                }
            }

            if (room_promise_) {
                try { room_promise_->set_value(room_id); } catch (...) {}
                room_promise_.reset();
            }
            break;
        }

        case MessageType::ROOM_LIST_RESPONSE: {
            chat::RoomListResponse res;

            if (res.ParseFromArray(payload, static_cast<int>(payload_size))) {
                std::cout << "\n================ [현재 개설된 방 목록] ================\n";

                if (res.rooms_size() == 0) {
                    std::cout << "현재 생성된 방이 없습니다.\n";
                } else {
                    for (const auto& room : res.rooms()) {
                        std::cout << "방 ID: " << room.room_id()
                                  << " | 제목: " << room.room_name()
                                  << " | 인원: (" << room.current_users() << "/" << room.max_users() << ")"
                                  << " | 방장 ID: " << room.owner_id() << '\n';
                    }
                }

                std::cout << "=======================================================\n";
            }
            break;
        }

        // 서버 응답이 성공한 경우에만 로컬 방 상태를 제거한다.
        case MessageType::LEAVE_ROOM_RESPONSE: {
            chat::LeaveRoomResponse res;
            bool success = false;

            if (res.ParseFromArray(payload, static_cast<int>(payload_size))) {
                if (res.success()) {
                    SetLastRoomId(0);
                    SetCurrentRoomOwnerId(0);
                    std::cout << "\n[시스템] 정상적으로 퇴장했습니다.\n";
                    success = true;
                } else {
                    std::cout << "\n[시스템] 방 퇴장 실패: " << res.error_message() << '\n';
                }
            } else {
                std::cerr << "\n[시스템] 방 퇴장 응답 파싱에 실패했습니다.\n";
            }

            if (leave_promise_) {
                try { leave_promise_->set_value(success); } catch (...) {}
                leave_promise_.reset();
            }
            break;
        }

        case MessageType::CHAT_MESSAGE: {
            chat::ChatMessage msg;

            if (msg.ParseFromArray(payload, static_cast<int>(payload_size))) {
                std::string sender = msg.sender_username().empty()
                    ? std::to_string(msg.sender_id())
                    : msg.sender_username();

                std::cout << "\n[" << sender << "]: " << msg.message() << '\n';
            }
            break;
        }

        case MessageType::CHAT_HISTORY_RESPONSE: {
            chat::ChatHistoryResponse res;

            if (res.ParseFromArray(payload, static_cast<int>(payload_size))) {
                if (res.success()) {
                    std::cout << "\n================ [이전 대화 기록] ================\n";

                    for (const auto& msg : res.messages()) {
                        std::string sender = msg.sender_username().empty()
                            ? std::to_string(msg.sender_id())
                            : msg.sender_username();

                        std::cout << "[" << sender << "]: " << msg.message() << '\n';
                    }

                    std::cout << "==================================================\n";
                } else {
                    std::cout << "\n[시스템] 기록 불러오기 실패: " << res.error_message() << '\n';
                }
            }
            break;
        }

        case MessageType::SERVER_NOTIFICATION: {
            chat::ServerNotification noti;

            if (noti.ParseFromArray(payload, static_cast<int>(payload_size))) {
                std::cout << "\n[서버] " << noti.message() << '\n';
            }
            break;
        }

        case MessageType::WHISPER_RESPONSE: {
            chat::WhisperResponse res;

            if (res.ParseFromArray(payload, static_cast<int>(payload_size))) {
                if (res.success()) {
                    std::cout << "\n[시스템] 귓속말을 전송했습니다.\n";
                } else {
                    std::cout << "\n[시스템] 귓속말 전송 실패: " << res.error_message() << '\n';
                }
            }
            break;
        }

        case MessageType::WHISPER_NOTIFICATION: {
            chat::WhisperNotification noti;

            if (noti.ParseFromArray(payload, static_cast<int>(payload_size))) {
                std::cout << "\n[귓속말 - " << noti.sender_username() << "]: " << noti.message() << '\n';
            }
            break;
        }

        case MessageType::KICK_USER_RESPONSE: {
            chat::KickUserResponse res;

            if (res.ParseFromArray(payload, static_cast<int>(payload_size))) {
                if (res.success()) {
                    std::cout << "\n[시스템] 해당 사용자를 강퇴했습니다.\n";
                } else {
                    std::cout << "\n[시스템] 강퇴 실패: " << res.error_message() << '\n';
                }
            }
            break;
        }

        case MessageType::KICKED_NOTIFICATION: {
            chat::KickedNotification noti;

            if (noti.ParseFromArray(payload, static_cast<int>(payload_size))) {
                SetLastRoomId(0);
                SetCurrentRoomOwnerId(0);
                std::cout << "\n[알림] 방에서 강퇴당했습니다. 사유: " << noti.reason() << '\n';
            }
            break;
        }

        case MessageType::TRANSFER_MASTER_RESPONSE: {
            chat::TransferMasterResponse res;

            if (res.ParseFromArray(payload, static_cast<int>(payload_size))) {
                if (res.success()) {
                    std::cout << "\n[시스템] 방장 권한을 위임했습니다.\n";
                } else {
                    std::cout << "\n[시스템] 방장 위임 실패: " << res.error_message() << '\n';
                }
            }
            break;
        }

        case MessageType::MASTER_CHANGED_NOTIFICATION: {
            chat::MasterChangedNotification noti;

            if (noti.ParseFromArray(payload, static_cast<int>(payload_size))) {
                SetCurrentRoomOwnerId(noti.new_master_id());
                std::cout << "\n[알림] 방장이 변경되었습니다! (새 방장 유저 ID: "
                          << noti.new_master_id() << ")\n";
            }
            break;
        }

        default:
            break;
        }
    }

    //==================================================
    // Write
    //==================================================
    void DoWrite()
    {
        boost::asio::async_write(
            ssl_socket_,
            boost::asio::buffer(write_queue_.front()),
            [this, self = shared_from_this()](boost::system::error_code ec, std::size_t)
            {
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

private:
    boost::asio::io_context& io_context_;
    ssl::stream<tcp::socket> ssl_socket_;

    std::atomic<bool> is_connected_{ false };
    std::atomic<uint32_t> user_id_{ 0 };
    std::atomic<uint32_t> last_room_id_{ 0 };
    std::atomic<uint32_t> current_room_owner_id_{ 0 };

    std::queue<std::vector<char>> write_queue_;
    std::vector<char> header_buffer_;
    std::vector<char> payload_buffer_;

    boost::asio::steady_timer ping_timer_{ io_context_ };

    std::shared_ptr<std::promise<bool>> auth_promise_;
    std::shared_ptr<std::promise<uint32_t>> room_promise_;
    std::shared_ptr<std::promise<bool>> leave_promise_;
};

//==================================================
// Room Loop
//==================================================
void RunRoomLoop(std::shared_ptr<ChatClient> client, uint32_t room_id)
{
    while (client->IsConnected()) {
        if (client->GetLastRoomId() == 0) break;

        std::string input;
        std::getline(std::cin, input);

        if (input.empty()) continue;

        //==================================================
        // Leave
        //==================================================
        if (input == "/leave") {
            auto leave_promise = std::make_shared<std::promise<bool>>();
            auto leave_future = leave_promise->get_future();

            client->RegisterLeavePromise(leave_promise);

            chat::LeaveRoomRequest req;
            req.set_room_id(room_id);

            client->SendProtoMessage(MessageType::LEAVE_ROOM, req);

            // 서버 응답이 성공해야만 RoomLoop를 종료한다.
            if (leave_future.get()) {
                break;
            }

            std::cout << "[시스템] 방에 계속 남아 있습니다.\n";
        }

        //==================================================
        // History
        //==================================================
        else if (input == "/history") {
            chat::ChatHistoryRequest req;
            req.set_room_id(room_id);
            req.set_last_message_id(0);
            req.set_count(20);

            client->SendProtoMessage(MessageType::CHAT_HISTORY_REQUEST, req);
        }

        //==================================================
        // Kick
        //==================================================
        else if (input.rfind("/kick ", 0) == 0) {
            if (!client->IsRoomOwner()) {
                std::cout << "[시스템] 방장만 /kick 명령어를 사용할 수 있습니다.\n";
                continue;
            }

            try {
                uint32_t target_id = std::stoul(input.substr(6));

                chat::KickUserRequest req;
                req.set_room_id(room_id);
                req.set_target_user_id(target_id);

                client->SendProtoMessage(MessageType::KICK_USER_REQUEST, req);
            } catch (const std::exception&) {
                std::cout << "[시스템] 사용법: /kick [유저ID]\n";
            }
        }

        //==================================================
        // Transfer Master
        //==================================================
        else if (input.rfind("/pass ", 0) == 0) {
            if (!client->IsRoomOwner()) {
                std::cout << "[시스템] 방장만 /pass 명령어를 사용할 수 있습니다.\n";
                continue;
            }

            try {
                uint32_t target_id = std::stoul(input.substr(6));

                chat::TransferMasterRequest req;
                req.set_room_id(room_id);
                req.set_new_master_id(target_id);

                client->SendProtoMessage(MessageType::TRANSFER_MASTER_REQUEST, req);
            } catch (const std::exception&) {
                std::cout << "[시스템] 사용법: /pass [유저ID]\n";
            }
        }

        //==================================================
        // Whisper
        //==================================================
        else if (input.rfind("/w ", 0) == 0) {
            size_t space_pos = input.find(' ', 3);

            if (space_pos == std::string::npos) {
                std::cout << "[시스템] 사용법: /w [상대방이름] [내용]\n";
                continue;
            }

            std::string target_name = input.substr(3, space_pos - 3);
            std::string message = input.substr(space_pos + 1);

            chat::WhisperRequest req;
            req.set_room_id(room_id);
            req.set_target_username(target_name);
            req.set_message(message);

            client->SendProtoMessage(MessageType::WHISPER_REQUEST, req);
        }

        //==================================================
        // Chat
        //==================================================
        else {
            chat::ChatMessage msg;
            msg.set_room_id(room_id);
            msg.set_sender_id(client->GetUserId());
            msg.set_message(input);
            msg.set_timestamp(std::chrono::duration_cast<std::chrono::seconds>(
                std::chrono::system_clock::now().time_since_epoch()).count());

            client->SendProtoMessage(MessageType::CHAT_MESSAGE, msg);
        }
    }
}

//==================================================
// Main
//==================================================
int main()
{
    try {
        boost::asio::io_context io_context;

        ssl::context ssl_ctx(ssl::context::tlsv12_client);
        ssl_ctx.set_verify_mode(ssl::verify_none);

        auto client = std::make_shared<ChatClient>(io_context, ssl_ctx);
        auto work_guard = boost::asio::make_work_guard(io_context);

        std::thread io_thread([&io_context]() {
            io_context.run();
        });

        //==================================================
        // Connect
        //==================================================
        auto connect_future = client->ConnectAsync("127.0.0.1", "8080");

        if (!connect_future.get()) {
            work_guard.reset();
            if (io_thread.joinable()) io_thread.join();
            return 0;
        }

        //==================================================
        // Authentication
        //==================================================
        while (client->IsConnected() && client->GetUserId() == 0) {
            std::cout << "\n=== [인증 메뉴] ===\n";
            std::cout << "1. 회원가입\n";
            std::cout << "2. 로그인\n";
            std::cout << "선택: ";

            int choice = 0;
            if (!(std::cin >> choice)) break;

            if (choice != 1 && choice != 2) {
                std::cout << "[시스템] 잘못된 선택입니다.\n";
                continue;
            }

            std::string username;
            std::string password;

            std::cout << "아이디: ";
            std::cin >> username;

            std::cout << "비밀번호: ";
            std::cin >> password;

            auto auth_promise = std::make_shared<std::promise<bool>>();
            auto auth_future = auth_promise->get_future();

            client->RegisterAuthPromise(auth_promise);

            if (choice == 1) {
                chat::RegisterRequest req;
                req.set_username(username);
                req.set_password(password);

                client->SendProtoMessage(MessageType::REGISTER_REQUEST, req);
            } else {
                chat::LoginRequest req;
                req.set_username(username);
                req.set_password(password);

                client->SendProtoMessage(MessageType::LOGIN_REQUEST, req);
            }

            auth_future.get();
        }

        //==================================================
        // Lobby
        //==================================================
        bool is_running = true;

        while (client->IsConnected() && is_running) {
            std::cout << "\n=== [메인 메뉴] (내 유저 ID: " << client->GetUserId() << ") ===\n";
            std::cout << "1. 방 목록 조회\n";
            std::cout << "2. 방 만들기\n";
            std::cout << "3. 방 입장하기\n";
            std::cout << "4. 프로그램 종료\n";
            std::cout << "선택: ";

            int menu_choice = 0;
            if (!(std::cin >> menu_choice)) break;

            //==================================================
            // Room List
            //==================================================
            if (menu_choice == 1) {
                chat::RoomListRequest req;
                client->SendProtoMessage(MessageType::ROOM_LIST_REQUEST, req);
            }

            //==================================================
            // Create Room
            //==================================================
            else if (menu_choice == 2) {
                std::string room_name;
                uint32_t max_users = 10;

                std::cout << "방 제목: ";
                std::cin >> room_name;

                std::cout << "최대 인원: ";
                std::cin >> max_users;

                auto room_promise = std::make_shared<std::promise<uint32_t>>();
                auto room_future = room_promise->get_future();

                client->RegisterRoomPromise(room_promise);

                chat::CreateRoomRequest req;
                req.set_room_name(room_name);
                req.set_max_users(max_users);

                client->SendProtoMessage(MessageType::CREATE_ROOM_REQUEST, req);

                uint32_t created_room_id = room_future.get();

                if (created_room_id > 0) {
                    std::cout << "\n>>> #" << created_room_id
                              << "번 방 입장 ('/history': 기록, '/leave': 퇴장) <<<\n";

                    std::cin.ignore(std::numeric_limits<std::streamsize>::max(), '\n');
                    RunRoomLoop(client, created_room_id);
                }
            }

            //==================================================
            // Join Room
            //==================================================
            else if (menu_choice == 3) {
                uint32_t target_room_id = 0;

                std::cout << "입장할 방 번호: ";
                std::cin >> target_room_id;

                auto room_promise = std::make_shared<std::promise<uint32_t>>();
                auto room_future = room_promise->get_future();

                client->RegisterRoomPromise(room_promise);

                chat::JoinRoomRequest req;
                req.set_room_id(target_room_id);

                client->SendProtoMessage(MessageType::JOIN_ROOM, req);

                uint32_t joined_room_id = room_future.get();

                if (joined_room_id > 0) {
                    std::cout << "\n>>> #" << joined_room_id
                              << "번 방 입장 ('/history': 기록, '/leave': 퇴장) <<<\n";

                    std::cin.ignore(std::numeric_limits<std::streamsize>::max(), '\n');
                    RunRoomLoop(client, joined_room_id);
                }
            }

            //==================================================
            // Exit
            //==================================================
            else if (menu_choice == 4) {
                is_running = false;
            }

            else {
                std::cout << "[시스템] 잘못된 선택입니다.\n";
            }
        }

        client->Close();
        work_guard.reset();

        if (io_thread.joinable()) {
            io_thread.join();
        }
    }
    catch (const std::exception& e) {
        std::cerr << "예외 발생: " << e.what() << '\n';
    }

    return 0;
}