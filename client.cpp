#include <boost/asio.hpp>
#include <iostream>
#include <thread>
#include <queue>
#include <vector>
#include <string>
#include <mutex>
#include <memory>
#include <cstring>
#include <unordered_map>
#include <atomic>
#include <chrono>
#include <limits>
#include <algorithm>
#include <sstream>

using boost::asio::ip::tcp;

// =========================================================
// 프로토콜 정의 (서버 1:1 동기화 완료)
// =========================================================
enum class MessageType : uint16_t
{
    LOGIN_PROMPT                = 1000,
    LOGIN_REQUEST               = 1001,
    LOGIN_RESPONSE              = 1002,
    LOGOUT_REQUEST              = 1003,
    LOGOUT_RESPONSE             = 1004,
    CHAT_MESSAGE                = 1005,
    JOIN_ROOM                   = 1006,
    LEAVE_ROOM                  = 1007,
    CREATE_ROOM_REQUEST         = 1008,
    CREATE_ROOM_RESPONSE        = 1009,
    ROOM_LIST_REQUEST           = 1010,
    ROOM_LIST_RESPONSE          = 1011,
    SERVER_NOTIFICATION         = 1013,
    REGISTER_REQUEST            = 1014,
    REGISTER_RESPONSE           = 1015,
    JOIN_ROOM_RESPONSE          = 1016,
    LEAVE_ROOM_RESPONSE         = 1017,
    WHISPER_REQUEST             = 1018,
    WHISPER_RESPONSE            = 1019,
    WHISPER_NOTIFICATION        = 1020,
    RECONNECT_REQUEST           = 1021,
    RECONNECT_RESPONSE          = 1022,
    KICK_USER_REQUEST           = 1023,
    KICK_USER_RESPONSE          = 1024,
    KICKED_NOTIFICATION         = 1025,
    TRANSFER_MASTER_REQUEST     = 1026,
    TRANSFER_MASTER_RESPONSE    = 1027,
    MASTER_CHANGED_NOTIFICATION = 1028
};

enum class AuthStatus { NONE, WAITING, SUCCESS, FAILED };

#pragma pack(push, 1)
struct PacketHeader { uint16_t packet_size; MessageType message_type; uint32_t user_id; uint32_t sequence_number; };
struct LoginRequest { PacketHeader header; char username[32]; char password[64]; };
struct LoginResponse { PacketHeader header; bool success; uint32_t assigned_user_id; char reconnect_token[64]; char error_message[128]; };
struct RegisterRequest { PacketHeader header; char username[32]; char password[64]; };
struct RegisterResponse { PacketHeader header; bool success; uint32_t assigned_user_id; char error_message[128]; };
struct ChatMessage { PacketHeader header; uint32_t room_id; char message[512]; };
struct ServerNotification { PacketHeader header; char message[256]; };
struct RoomInfo { uint32_t room_id; char room_name[32]; uint32_t current_users; uint32_t max_users; uint32_t owner_id; };
struct CreateRoomRequest { PacketHeader header; char room_name[32]; uint32_t max_users; };
struct CreateRoomResponse { PacketHeader header; bool success; uint32_t created_room_id; char error_message[128]; };
struct RoomListRequest { PacketHeader header; };
struct RoomListResponse { PacketHeader header; uint32_t room_count; RoomInfo rooms[16]; };
struct JoinRoomRequest { PacketHeader header; uint32_t room_id; };
struct JoinRoomResponse { PacketHeader header; bool success; uint32_t room_id; uint32_t owner_id; char error_message[128]; };
struct LeaveRoomRequest { PacketHeader header; uint32_t room_id; };
struct LeaveRoomResponse { PacketHeader header; bool success; char error_message[128]; };
struct WhisperRequest { PacketHeader header; uint32_t room_id; char target_username[32]; char message[512]; };
struct WhisperResponse { PacketHeader header; bool success; char error_message[128]; };
struct WhisperNotification { PacketHeader header; char sender_username[32]; char message[512]; };
struct ReconnectRequest { PacketHeader header; uint32_t user_id; char reconnect_token[64]; uint32_t last_room_id; };
struct ReconnectResponse { PacketHeader header; bool success; uint32_t restored_room_id; uint32_t owner_id; char error_message[128]; };
struct KickUserRequest { PacketHeader header; uint32_t room_id; uint32_t target_user_id; };
struct KickUserResponse { PacketHeader header; bool success; uint32_t target_user_id; char error_message[128]; };
struct KickedNotification { PacketHeader header; uint32_t room_id; char reason[128]; };
struct TransferMasterRequest { PacketHeader header; uint32_t room_id; uint32_t target_user_id; };
struct TransferMasterResponse { PacketHeader header; bool success; uint32_t target_user_id; char error_message[128]; };
struct MasterChangedNotification { PacketHeader header; uint32_t room_id; uint32_t new_master_id; char new_master_username[32]; };
#pragma pack(pop)

class ChatClient;

// =========================================================
// 디스패처 인터페이스 & 클래스
// =========================================================
class IMessageHandler
{
public:
    virtual ~IMessageHandler() = default;
    virtual void HandleMessage(std::shared_ptr<ChatClient> client, const char* data, size_t size) = 0;
};

class MessageDispatcher
{
public:
    void RegisterHandler(MessageType type, std::unique_ptr<IMessageHandler> handler)
    {
        handlers_[type] = std::move(handler);
    }

    void DispatchMessage(std::shared_ptr<ChatClient> client, const PacketHeader& header, const char* data, size_t size)
    {
        auto it = handlers_.find(header.message_type);
        if (it != handlers_.end())
        {
            it->second->HandleMessage(client, data, size);
        }
        else
        {
            std::cout << "\n[System] Unhandled message type: " << static_cast<uint16_t>(header.message_type) << std::endl;
        }
    }

private:
    std::unordered_map<MessageType, std::unique_ptr<IMessageHandler>> handlers_;
};

// =========================================================
// 패킷 버퍼
// =========================================================
class PacketBuffer
{
public:
    PacketBuffer() : read_pos_(0), write_pos_(0) {}

    bool HasCompletePacket() const
    {
        if (GetReadableSize() < sizeof(PacketHeader)) return false;
        const PacketHeader* header = reinterpret_cast<const PacketHeader*>(buffer_.data() + read_pos_);
        return GetReadableSize() >= header->packet_size;
    }

    bool ReadPacket(std::vector<char>& packet_data)
    {
        if (!HasCompletePacket()) return false;
        const PacketHeader* header = reinterpret_cast<const PacketHeader*>(buffer_.data() + read_pos_);
        packet_data.resize(header->packet_size);
        std::memcpy(packet_data.data(), buffer_.data() + read_pos_, header->packet_size);
        read_pos_ += header->packet_size;

        if (read_pos_ > buffer_.size() / 2)
        {
            std::memmove(buffer_.data(), buffer_.data() + read_pos_, GetReadableSize());
            write_pos_ -= read_pos_;
            read_pos_ = 0;
        }
        return true;
    }

    void WriteData(const char* data, size_t size)
    {
        if (write_pos_ + size > buffer_.size())
        {
            buffer_.resize(write_pos_ + size);
        }
        std::memcpy(buffer_.data() + write_pos_, data, size);
        write_pos_ += size;
    }

    size_t GetReadableSize() const { return write_pos_ - read_pos_; }

private:
    std::vector<char> buffer_ = std::vector<char>(8192);
    size_t read_pos_;
    size_t write_pos_;
};

// =========================================================
// ChatClient 클래스
// =========================================================
class ChatClient : public std::enable_shared_from_this<ChatClient>
{
public:
    ChatClient(boost::asio::io_context& io_context);

    void Start(const std::string& host, const std::string& port)
    {
        host_ = host;
        port_ = port;
        tcp::resolver resolver(io_context_);
        endpoints_ = resolver.resolve(host, port);
        DoConnect();
    }

    void Send(const void* data, size_t size)
    {
        auto buf = std::make_shared<std::vector<char>>(
            static_cast<const char*>(data),
            static_cast<const char*>(data) + size
        );

        boost::asio::post(io_context_, [this, self = shared_from_this(), buf]() {
            bool write_in_progress = !write_queue_.empty();
            write_queue_.push(*buf);

            if (is_connected_ && !write_in_progress)
            {
                DoWrite();
            }
        });
    }

    void SetUserId(uint32_t id) { user_id_ = id; }
    uint32_t GetUserId() const { return user_id_; }
    void SetAuthStatus(AuthStatus status) { auth_status_ = status; }
    AuthStatus GetAuthStatus() const { return auth_status_; }

    void SetReconnectToken(const std::string& token) { reconnect_token_ = token; }
    std::string GetReconnectToken() const { return reconnect_token_; }

    void SetLastRoomId(uint32_t room_id) { last_room_id_ = room_id; }
    uint32_t GetLastRoomId() const { return last_room_id_; }

    void SetCurrentRoomOwnerId(uint32_t owner_id) { current_room_owner_id_ = owner_id; }
    uint32_t GetCurrentRoomOwnerId() const { return current_room_owner_id_; }
    bool IsRoomOwner() const { return user_id_ != 0 && user_id_ == current_room_owner_id_; }

    bool IsConnected() const { return is_connected_; }
    bool IsConnectFailed() const { return connect_failed_; }

    void BeginRoomOperation() { room_operation_pending_ = true; room_operation_success_ = false; }
    void CompleteRoomOperation(bool success) { room_operation_success_ = success; room_operation_pending_ = false; }
    bool IsRoomOperationPending() const { return room_operation_pending_; }
    bool IsRoomOperationSuccessful() const { return room_operation_success_; }

private:
    void DoConnect()
    {
        boost::asio::async_connect(socket_, endpoints_,
            [this, self = shared_from_this()](boost::system::error_code ec, tcp::endpoint) {
                if (!ec)
                {
                    std::cout << "[System] Connected to server!" << std::endl;
                    is_connected_ = true;
                    connect_failed_ = false;

                    if (!reconnect_token_.empty() && user_id_ != 0)
                    {
                        SendReconnectRequest();
                    }

                    DoRead();

                    if (!write_queue_.empty())
                    {
                        DoWrite();
                    }
                }
                else
                {
                    std::cout << "[System] Connect failed: " << ec.message() << std::endl;
                    connect_failed_ = true;
                }
            });
    }

    void SendReconnectRequest()
    {
        ReconnectRequest req{};
        req.header.packet_size = sizeof(ReconnectRequest);
        req.header.message_type = MessageType::RECONNECT_REQUEST;
        req.header.user_id = user_id_;
        req.user_id = user_id_;
        req.last_room_id = last_room_id_;
        std::strncpy(req.reconnect_token, reconnect_token_.c_str(), sizeof(req.reconnect_token) - 1);

        Send(&req, sizeof(ReconnectRequest));
        std::cout << "[System] Attempting automatic reconnection to server..." << std::endl;
    }

    void DoRead()
    {
        socket_.async_read_some(boost::asio::buffer(read_buffer_),
            [this, self = shared_from_this()](boost::system::error_code ec, std::size_t length) {
                if (!ec)
                {
                    packet_buffer_.WriteData(read_buffer_.data(), length);
                    std::vector<char> packet_data;
                    while (packet_buffer_.ReadPacket(packet_data))
                    {
                        ProcessPacket(packet_data.data(), packet_data.size());
                    }
                    DoRead();
                }
                else
                {
                    std::cout << "\n[System] Disconnected from server." << std::endl;
                    is_connected_ = false;
                    boost::system::error_code close_ec;
                    socket_.close(close_ec);

                    if (auth_status_ == AuthStatus::SUCCESS && !reconnect_token_.empty())
                    {
                        ScheduleReconnect();
                    }
                    else
                    {
                        auth_status_ = AuthStatus::FAILED;
                    }
                }
            });
    }

    void ScheduleReconnect()
    {
        reconnect_timer_.expires_after(std::chrono::seconds(3));
        reconnect_timer_.async_wait([this, self = shared_from_this()](boost::system::error_code ec) {
            if (!ec)
            {
                std::cout << "[System] Trying to reconnect..." << std::endl;
                tcp::resolver resolver(io_context_);
                endpoints_ = resolver.resolve(host_, port_);
                DoConnect();
            }
        });
    }

    void ProcessPacket(const char* data, size_t size)
    {
        if (size < sizeof(PacketHeader)) return;
        const auto& header = *reinterpret_cast<const PacketHeader*>(data);
        if (header.packet_size != size || header.packet_size < sizeof(PacketHeader)) return;
        dispatcher_.DispatchMessage(shared_from_this(), header, data, size);
    }

    void DoWrite()
    {
        boost::asio::async_write(socket_, boost::asio::buffer(write_queue_.front()),
            [this, self = shared_from_this()](boost::system::error_code ec, std::size_t) {
                if (!ec)
                {
                    write_queue_.pop();
                    if (!write_queue_.empty())
                    {
                        DoWrite();
                    }
                }
                else
                {
                    is_connected_ = false;
                    boost::system::error_code close_ec;
                    socket_.close(close_ec);
                }
            });
    }

    boost::asio::io_context& io_context_;
    tcp::socket socket_;
    tcp::resolver::results_type endpoints_;
    std::string host_;
    std::string port_;

    std::atomic<bool> is_connected_{false};
    std::atomic<bool> connect_failed_{false};
    std::atomic<AuthStatus> auth_status_{AuthStatus::NONE};
    std::atomic<uint32_t> user_id_{0};

    std::string reconnect_token_;
    std::atomic<uint32_t> last_room_id_{0};
    std::atomic<uint32_t> current_room_owner_id_{0};
    std::atomic<bool> room_operation_pending_{false};
    std::atomic<bool> room_operation_success_{false};

    boost::asio::steady_timer reconnect_timer_{io_context_};

    MessageDispatcher dispatcher_;
    PacketBuffer packet_buffer_;
    std::vector<char> read_buffer_;
    std::queue<std::vector<char>> write_queue_;
};

// =========================================================
// 클라이언트 핸들러 구현부
// =========================================================
class LoginPromptHandler : public IMessageHandler
{
public:
    void HandleMessage(std::shared_ptr<ChatClient> client, const char* data, size_t size) override
    {
        std::cout << "[Server] Welcome! Please register or log in." << std::endl;
    }
};

class LoginResponseHandler : public IMessageHandler
{
public:
    void HandleMessage(std::shared_ptr<ChatClient> client, const char* data, size_t size) override
    {
        if (size < sizeof(LoginResponse)) return;
        const auto& res = *reinterpret_cast<const LoginResponse*>(data);

        if (res.success)
        {
            client->SetUserId(res.assigned_user_id);
            client->SetReconnectToken(res.reconnect_token);
            client->SetAuthStatus(AuthStatus::SUCCESS);
            std::cout << "\n[System] Login Success! User ID: " << res.assigned_user_id << std::endl;
        }
        else
        {
            client->SetAuthStatus(AuthStatus::FAILED);
            std::cout << "\n[System] Login Failed: " << res.error_message << std::endl;
        }
    }
};

class RegisterResponseHandler : public IMessageHandler
{
public:
    void HandleMessage(std::shared_ptr<ChatClient> client, const char* data, size_t size) override
    {
        if (size < sizeof(RegisterResponse)) return;
        const auto& res = *reinterpret_cast<const RegisterResponse*>(data);

        if (res.success)
        {
            client->SetAuthStatus(AuthStatus::NONE);
            std::cout << "\n[System] Registration Success! User ID: " << res.assigned_user_id << ". Please Log In." << std::endl;
        }
        else
        {
            client->SetAuthStatus(AuthStatus::FAILED);
            std::cout << "\n[System] Registration Failed: " << res.error_message << std::endl;
        }
    }
};

class ChatMessageHandler : public IMessageHandler
{
public:
    void HandleMessage(std::shared_ptr<ChatClient> client, const char* data, size_t size) override
    {
        if (size < sizeof(ChatMessage)) return;
        const auto& msg = *reinterpret_cast<const ChatMessage*>(data);
        
        // 서버 프로토콜에는 sender 문자열이 없으므로 user_id를 표시한다.
        std::cout << "\n[User " << msg.header.user_id << "]: " << msg.message << std::endl;
        std::cout << "Message: " << std::flush;
    }
};

class ServerNotificationHandler : public IMessageHandler
{
public:
    void HandleMessage(std::shared_ptr<ChatClient> client, const char* data, size_t size) override
    {
        // [수정] ServerNotification 구조체로 안전하게 역직렬화
        if (size < sizeof(ServerNotification)) return;
        const auto& notif = *reinterpret_cast<const ServerNotification*>(data);
        std::cout << "\n[Notification] " << notif.message << std::endl;
        std::cout << "Message: " << std::flush;
    }
};

class CreateRoomResponseHandler : public IMessageHandler
{
public:
    void HandleMessage(std::shared_ptr<ChatClient> client, const char* data, size_t size) override
    {
        if (size < sizeof(CreateRoomResponse)) return;
        const auto& res = *reinterpret_cast<const CreateRoomResponse*>(data);

        if (res.success)
        {
            client->SetLastRoomId(res.created_room_id);
            client->SetCurrentRoomOwnerId(client->GetUserId());
            std::cout << "\n[System] Room Created Successfully! Room ID: " << res.created_room_id << std::endl;
            std::cout << "[System] You are now the Room Leader." << std::endl;
            client->CompleteRoomOperation(true);
        }
        else
        {
            std::cout << "\n[System] Failed to Create Room: " << res.error_message << std::endl;
            client->CompleteRoomOperation(false);
        }
    }
};

class RoomListResponseHandler : public IMessageHandler
{
public:
    void HandleMessage(std::shared_ptr<ChatClient> client, const char* data, size_t size) override
    {
        if (size < sizeof(RoomListResponse)) return;
        const auto& res = *reinterpret_cast<const RoomListResponse*>(data);

        std::cout << "\n========== Room List (" << res.room_count << ") ==========" << std::endl;
        for (uint32_t i = 0; i < res.room_count; ++i)
        {
            const auto& r = res.rooms[i];
            std::cout << "ID: [" << r.room_id << "] " << r.room_name 
                      << " (" << r.current_users << "/" << r.max_users << ")"
                      << " [Host ID: " << r.owner_id << "]" << std::endl;
        }
        std::cout << "====================================" << std::endl;
    }
};

class JoinRoomResponseHandler : public IMessageHandler
{
public:
    void HandleMessage(std::shared_ptr<ChatClient> client, const char* data, size_t size) override
    {
        if (size < sizeof(JoinRoomResponse)) return;
        const auto& res = *reinterpret_cast<const JoinRoomResponse*>(data);

        if (res.success)
        {
            client->SetLastRoomId(res.room_id);
            client->SetCurrentRoomOwnerId(res.owner_id);
            std::cout << "\n[System] Successfully joined room ID: " << res.room_id << std::endl;
            if (client->IsRoomOwner())
            {
                std::cout << "[System] You are the Room Leader of this room." << std::endl;
            }
            client->CompleteRoomOperation(true);
        }
        else
        {
            std::cout << "\n[System] Failed to join room: " << res.error_message << std::endl;
            client->CompleteRoomOperation(false);
        }
    }
};

class LeaveRoomResponseHandler : public IMessageHandler
{
public:
    void HandleMessage(std::shared_ptr<ChatClient> client, const char* data, size_t size) override
    {
        if (size < sizeof(LeaveRoomResponse)) return;
        const auto& res = *reinterpret_cast<const LeaveRoomResponse*>(data);

        if (res.success)
        {
            client->SetLastRoomId(0);
            client->SetCurrentRoomOwnerId(0);
            std::cout << "\n[System] Successfully left room." << std::endl;
        }
        else
        {
            std::cout << "\n[System] Failed to leave room: " << res.error_message << std::endl;
        }
    }
};

class WhisperResponseHandler : public IMessageHandler
{
public:
    void HandleMessage(std::shared_ptr<ChatClient> client, const char* data, size_t size) override
    {
        if (size < sizeof(WhisperResponse)) return;
        const auto& res = *reinterpret_cast<const WhisperResponse*>(data);

        if (!res.success)
        {
            std::cout << "\n[System] Whisper failed: " << res.error_message << std::endl;
            std::cout << "Message: " << std::flush;
        }
    }
};

class WhisperNotificationHandler : public IMessageHandler
{
public:
    void HandleMessage(std::shared_ptr<ChatClient> client, const char* data, size_t size) override
    {
        if (size < sizeof(WhisperNotification)) return;
        const auto& notif = *reinterpret_cast<const WhisperNotification*>(data);

        std::cout << "\n[Whisper from " << notif.sender_username << "]: " << notif.message << std::endl;
        std::cout << "Message: " << std::flush;
    }
};

class ReconnectResponseHandler : public IMessageHandler
{
public:
    void HandleMessage(std::shared_ptr<ChatClient> client, const char* data, size_t size) override
    {
        if (size < sizeof(ReconnectResponse)) return;
        const auto& res = *reinterpret_cast<const ReconnectResponse*>(data);

        if (res.success)
        {
            client->SetLastRoomId(res.restored_room_id);
            client->SetCurrentRoomOwnerId(res.owner_id);
            std::cout << "\n[System] Reconnection & session restoration successful!" << std::endl;
            if (res.restored_room_id != 0)
            {
                std::cout << "[System] Restored to previous Room ID: " << res.restored_room_id << std::endl;
                if (client->IsRoomOwner())
                {
                    std::cout << "[System] You are the Room Leader." << std::endl;
                }
            }
            else
            {
                std::cout << "[System] Returned to Lobby." << std::endl;
            }
            std::cout << "Message: " << std::flush;
        }
        else
        {
            std::cout << "\n[System] Reconnection failed: " << res.error_message << std::endl;
            client->SetAuthStatus(AuthStatus::FAILED);
        }
    }
};

class KickUserResponseHandler : public IMessageHandler
{
public:
    void HandleMessage(std::shared_ptr<ChatClient> client, const char* data, size_t size) override
    {
        if (size < sizeof(KickUserResponse)) return;
        const auto& res = *reinterpret_cast<const KickUserResponse*>(data);

        if (res.success)
        {
            std::cout << "\n[System] User kicked successfully." << std::endl;
        }
        else
        {
            std::cout << "\n[System] Kick failed: " << res.error_message << std::endl;
        }
        std::cout << "Message: " << std::flush;
    }
};

class TransferMasterResponseHandler : public IMessageHandler
{
public:
    void HandleMessage(std::shared_ptr<ChatClient> client, const char* data, size_t size) override
    {
        if (size < sizeof(TransferMasterResponse)) return;
        const auto& res = *reinterpret_cast<const TransferMasterResponse*>(data);

        if (res.success)
        {
            std::cout << "\n[System] Master privilege transferred successfully." << std::endl;
        }
        else
        {
            std::cout << "\n[System] Transfer failed: " << res.error_message << std::endl;
        }
        std::cout << "Message: " << std::flush;
    }
};

class MasterChangedNotificationHandler : public IMessageHandler
{
public:
    void HandleMessage(std::shared_ptr<ChatClient> client, const char* data, size_t size) override
    {
        if (size < sizeof(MasterChangedNotification)) return;
        const auto& notif = *reinterpret_cast<const MasterChangedNotification*>(data);

        client->SetCurrentRoomOwnerId(notif.new_master_id);
        std::cout << "\n[System] Room Leader changed to User ID: " << notif.new_master_id << std::endl;
        if (client->IsRoomOwner())
        {
            std::cout << "[System] You are now the Room Leader! (/kick, /pass commands available)" << std::endl;
        }
        std::cout << "Message: " << std::flush;
    }
};

class KickedNotificationHandler : public IMessageHandler
{
public:
    void HandleMessage(std::shared_ptr<ChatClient> client, const char* data, size_t size) override
    {
        client->SetLastRoomId(0);
        client->SetCurrentRoomOwnerId(0);
        std::cout << "\n[System] You have been kicked from the room by the host!" << std::endl;
        std::cout << "[System] Returning to Lobby menu..." << std::endl;
    }
};

// =========================================================
// ChatClient 생성자
// =========================================================
ChatClient::ChatClient(boost::asio::io_context& io_context)
    : io_context_(io_context), socket_(io_context), read_buffer_(4096)
{
    dispatcher_.RegisterHandler(MessageType::LOGIN_PROMPT, std::make_unique<LoginPromptHandler>());
    dispatcher_.RegisterHandler(MessageType::LOGIN_RESPONSE, std::make_unique<LoginResponseHandler>());
    dispatcher_.RegisterHandler(MessageType::REGISTER_RESPONSE, std::make_unique<RegisterResponseHandler>());
    dispatcher_.RegisterHandler(MessageType::CHAT_MESSAGE, std::make_unique<ChatMessageHandler>());
    dispatcher_.RegisterHandler(MessageType::SERVER_NOTIFICATION, std::make_unique<ServerNotificationHandler>());

    dispatcher_.RegisterHandler(MessageType::CREATE_ROOM_RESPONSE, std::make_unique<CreateRoomResponseHandler>());
    dispatcher_.RegisterHandler(MessageType::ROOM_LIST_RESPONSE, std::make_unique<RoomListResponseHandler>());
    dispatcher_.RegisterHandler(MessageType::JOIN_ROOM_RESPONSE, std::make_unique<JoinRoomResponseHandler>());
    dispatcher_.RegisterHandler(MessageType::LEAVE_ROOM_RESPONSE, std::make_unique<LeaveRoomResponseHandler>());

    dispatcher_.RegisterHandler(MessageType::WHISPER_RESPONSE, std::make_unique<WhisperResponseHandler>());
    dispatcher_.RegisterHandler(MessageType::WHISPER_NOTIFICATION, std::make_unique<WhisperNotificationHandler>());

    dispatcher_.RegisterHandler(MessageType::RECONNECT_RESPONSE, std::make_unique<ReconnectResponseHandler>());

    dispatcher_.RegisterHandler(MessageType::KICK_USER_RESPONSE, std::make_unique<KickUserResponseHandler>());
    dispatcher_.RegisterHandler(MessageType::TRANSFER_MASTER_RESPONSE, std::make_unique<TransferMasterResponseHandler>());
    dispatcher_.RegisterHandler(MessageType::MASTER_CHANGED_NOTIFICATION, std::make_unique<MasterChangedNotificationHandler>());
    dispatcher_.RegisterHandler(MessageType::KICKED_NOTIFICATION, std::make_unique<KickedNotificationHandler>());
}

// =========================================================
// 방 채팅 루프
// =========================================================
static void RunRoomChatLoop(const std::shared_ptr<ChatClient>& client)
{
    const uint32_t target_room_id = client->GetLastRoomId();
    if (target_room_id == 0) return;

    std::cout << "\n==========================================" << std::endl;
    std::cout << " Entered Room [" << target_room_id << "]" << std::endl;
    std::cout << " Whisper Usage: /w <Username> <Message>" << std::endl;
    std::cout << " Host Commands: /kick <User_ID>, /pass <User_ID>" << std::endl;
    std::cout << " Type '/quit' or '/exit' to leave room." << std::endl;
    std::cout << "==========================================" << std::endl;

    std::cin.ignore(std::numeric_limits<std::streamsize>::max(), '\n');
    std::string line;

    while (client->IsConnected())
    {
        if (client->GetLastRoomId() != target_room_id)
            break;

        std::cout << "Message: ";
        if (!std::getline(std::cin, line)) break;
        if (line.empty()) continue;

        if (line == "/quit" || line == "/exit")
        {
            LeaveRoomRequest req{};
            req.header.packet_size = sizeof(req);
            req.header.message_type = MessageType::LEAVE_ROOM;
            req.header.user_id = client->GetUserId();
            req.room_id = target_room_id;
            client->Send(&req, sizeof(req));
            break;
        }

        if (line.rfind("/kick ", 0) == 0)
        {
            if (!client->IsRoomOwner())
            {
                std::cout << "[System] Only the room leader can kick users." << std::endl;
                continue;
            }
            std::stringstream ss(line);
            std::string cmd;
            uint32_t target_user_id = 0;
            ss >> cmd >> target_user_id;
            if (target_user_id == 0)
            {
                std::cout << "[System] Usage: /kick <user_id>" << std::endl;
                continue;
            }
            KickUserRequest req{};
            req.header.packet_size = sizeof(req);
            req.header.message_type = MessageType::KICK_USER_REQUEST;
            req.header.user_id = client->GetUserId();
            req.room_id = target_room_id;
            req.target_user_id = target_user_id;
            client->Send(&req, sizeof(req));
            continue;
        }

        if (line.rfind("/pass ", 0) == 0)
        {
            if (!client->IsRoomOwner())
            {
                std::cout << "[System] Only the room leader can transfer leadership." << std::endl;
                continue;
            }
            std::stringstream ss(line);
            std::string cmd;
            uint32_t target_user_id = 0;
            ss >> cmd >> target_user_id;
            if (target_user_id == 0)
            {
                std::cout << "[System] Usage: /pass <user_id>" << std::endl;
                continue;
            }
            TransferMasterRequest req{};
            req.header.packet_size = sizeof(req);
            req.header.message_type = MessageType::TRANSFER_MASTER_REQUEST;
            req.header.user_id = client->GetUserId();
            req.room_id = target_room_id;
            req.target_user_id = target_user_id;
            client->Send(&req, sizeof(req));
            continue;
        }

        if (line.rfind("/w ", 0) == 0 || line.rfind("/whisper ", 0) == 0)
        {
            std::stringstream ss(line);
            std::string cmd, target_user, msg_body;
            ss >> cmd >> target_user;
            std::getline(ss >> std::ws, msg_body);
            if (target_user.empty() || msg_body.empty())
            {
                std::cout << "[System] Usage: /w <Username> <Message>" << std::endl;
                continue;
            }
            WhisperRequest req{};
            req.header.packet_size = sizeof(req);
            req.header.message_type = MessageType::WHISPER_REQUEST;
            req.header.user_id = client->GetUserId();
            req.room_id = target_room_id;
            std::strncpy(req.target_username, target_user.c_str(), sizeof(req.target_username) - 1);
            std::strncpy(req.message, msg_body.c_str(), sizeof(req.message) - 1);
            client->Send(&req, sizeof(req));
            continue;
        }

        ChatMessage msg{};
        msg.header.packet_size = sizeof(msg);
        msg.header.message_type = MessageType::CHAT_MESSAGE;
        msg.header.user_id = client->GetUserId();
        msg.room_id = target_room_id;
        std::strncpy(msg.message, line.c_str(), sizeof(msg.message) - 1);
        client->Send(&msg, sizeof(msg));
    }
}

// =========================================================
// main() - CLI 루프 및 통신 처리
// =========================================================
int main()
{
    boost::asio::io_context io_context;
    auto client = std::make_shared<ChatClient>(io_context);

    std::cout << "[System] Connecting to server..." << std::endl;
    client->Start("127.0.0.1", "8080");

    std::thread t([&io_context]() { io_context.run(); });

    while (!client->IsConnected() && !client->IsConnectFailed())
        std::this_thread::sleep_for(std::chrono::milliseconds(50));

    if (client->IsConnectFailed())
    {
        std::cout << "[System] Exiting due to connection failure." << std::endl;
        io_context.stop();
        if (t.joinable()) t.join();
        return 0;
    }

    while (client->GetAuthStatus() != AuthStatus::SUCCESS && client->IsConnected())
    {
        std::cout << "\n=========================\n"
                  << " 1. Register (회원가입)\n"
                  << " 2. Login (로그인)\n"
                  << " 3. Exit (종료)\n"
                  << "Select Menu: ";

        int choice = 0;
        if (!(std::cin >> choice)) break;
        if (choice == 3) break;
        if (choice != 1 && choice != 2) continue;

        std::string username, password;
        std::cout << "Username: "; std::cin >> username;
        std::cout << "Password: "; std::cin >> password;

        if (choice == 1)
        {
            RegisterRequest req{};
            req.header.packet_size = sizeof(req);
            req.header.message_type = MessageType::REGISTER_REQUEST;
            std::strncpy(req.username, username.c_str(), sizeof(req.username) - 1);
            std::strncpy(req.password, password.c_str(), sizeof(req.password) - 1);
            client->SetAuthStatus(AuthStatus::WAITING);
            client->Send(&req, sizeof(req));
        }
        else
        {
            LoginRequest req{};
            req.header.packet_size = sizeof(req);
            req.header.message_type = MessageType::LOGIN_REQUEST;
            std::strncpy(req.username, username.c_str(), sizeof(req.username) - 1);
            std::strncpy(req.password, password.c_str(), sizeof(req.password) - 1);
            client->SetAuthStatus(AuthStatus::WAITING);
            client->Send(&req, sizeof(req));
        }

        while (client->GetAuthStatus() == AuthStatus::WAITING && client->IsConnected())
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }

    if (client->GetAuthStatus() != AuthStatus::SUCCESS)
    {
        io_context.stop();
        if (t.joinable()) t.join();
        return 0;
    }

    // 서버 로그인 시 Lobby(1)에 자동 등록하지만, 클라이언트 상태도 응답으로 동기화한다.
    client->BeginRoomOperation();
    JoinRoomRequest lobby_req{};
    lobby_req.header.packet_size = sizeof(lobby_req);
    lobby_req.header.message_type = MessageType::JOIN_ROOM;
    lobby_req.header.user_id = client->GetUserId();
    lobby_req.room_id = 1;
    client->Send(&lobby_req, sizeof(lobby_req));
    while (client->IsRoomOperationPending() && client->IsConnected())
        std::this_thread::sleep_for(std::chrono::milliseconds(20));

    bool is_running = client->IsConnected();
    while (is_running)
    {
        std::cout << "\n=== Lobby Menu ===\n"
                  << "1. Room List (방 목록 조회)\n"
                  << "2. Create Room (방 생성 -> 자동 입장)\n"
                  << "3. Enter Room (방 입장)\n"
                  << "4. Exit (종료)\n"
                  << "Select: ";

        int choice = 0;
        if (!(std::cin >> choice)) break;

        if (choice == 1)
        {
            RoomListRequest req{};
            req.header.packet_size = sizeof(req);
            req.header.message_type = MessageType::ROOM_LIST_REQUEST;
            req.header.user_id = client->GetUserId();
            client->Send(&req, sizeof(req));
            std::this_thread::sleep_for(std::chrono::milliseconds(200));
        }
        else if (choice == 2)
        {
            std::string room_name;
            uint32_t max_users = 10;
            std::cout << "Enter Room Name: "; std::cin >> room_name;
            std::cout << "Max Users: "; std::cin >> max_users;

            CreateRoomRequest req{};
            req.header.packet_size = sizeof(req);
            req.header.message_type = MessageType::CREATE_ROOM_REQUEST;
            req.header.user_id = client->GetUserId();
            std::strncpy(req.room_name, room_name.c_str(), sizeof(req.room_name) - 1);
            req.max_users = max_users;

            client->BeginRoomOperation();
            client->Send(&req, sizeof(req));
            while (client->IsRoomOperationPending() && client->IsConnected())
                std::this_thread::sleep_for(std::chrono::milliseconds(20));

            if (client->IsRoomOperationSuccessful() && client->GetLastRoomId() != 0)
                RunRoomChatLoop(client);
        }
        else if (choice == 3)
        {
            uint32_t target_room_id = 1;
            std::cout << "Enter Room ID to join: ";
            if (!(std::cin >> target_room_id))
            {
                std::cin.clear();
                std::cin.ignore(std::numeric_limits<std::streamsize>::max(), '\n');
                continue;
            }

            JoinRoomRequest req{};
            req.header.packet_size = sizeof(req);
            req.header.message_type = MessageType::JOIN_ROOM;
            req.header.user_id = client->GetUserId();
            req.room_id = target_room_id;

            client->BeginRoomOperation();
            client->Send(&req, sizeof(req));
            while (client->IsRoomOperationPending() && client->IsConnected())
                std::this_thread::sleep_for(std::chrono::milliseconds(20));

            if (client->IsRoomOperationSuccessful())
                RunRoomChatLoop(client);
        }
        else if (choice == 4)
        {
            is_running = false;
        }
    }

    std::cout << "[System] Disconnecting and shutting down..." << std::endl;
    io_context.stop();
    if (t.joinable()) t.join();
    return 0;
}
