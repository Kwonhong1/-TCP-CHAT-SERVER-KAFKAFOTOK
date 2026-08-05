#include <boost/asio.hpp>
#include <boost/asio/co_spawn.hpp>
#include <boost/asio/detached.hpp>
#include <boost/asio/use_awaitable.hpp>
#include <memory>
#include <iostream>
#include <string>
#include <vector>
#include <unordered_map>
#include <queue>
#include <cstring>
#include <thread>
#include <atomic>
#include <algorithm>
#include <chrono>

using boost::asio::ip::tcp;
using boost::asio::awaitable;
using boost::asio::use_awaitable;
using boost::asio::co_spawn;
using boost::asio::detached;

// 메시지 타입 정의
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
    SERVER_NOTIFICATION = 1013,
    REGISTER_REQUEST = 1014,
    REGISTER_RESPONSE = 1015,
    JOIN_ROOM_RESPONSE = 1016,
    LEAVE_ROOM_RESPONSE = 1017,
    WHISPER_REQUEST = 1018,
    WHISPER_RESPONSE = 1019,
    WHISPER_NOTIFICATION = 1020,
    RECONNECT_REQUEST = 1021,
    RECONNECT_RESPONSE = 1022
};

#pragma pack(push, 1)
struct PacketHeader
{
    uint16_t packet_size;
    MessageType message_type;
    uint32_t user_id;
    uint32_t sequence_number;
};

struct LoginRequest
{
    PacketHeader header;
    char username[32];
    char password[64];
};

struct LoginResponse
{
    PacketHeader header;
    bool success;
    uint32_t assigned_user_id;
    char reconnect_token[64];
    char error_message[128];
};

struct RegisterRequest
{
    PacketHeader header;
    char username[32];
    char password[64];
};

struct RegisterResponse
{
    PacketHeader header;
    bool success;
    uint32_t assigned_user_id;
    char error_message[128];
};

struct ChatMessage
{
    PacketHeader header;
    uint32_t room_id;
    char message[512];
};

struct ServerNotification
{
    PacketHeader header;
    char message[256];
};

struct RoomInfo
{
    uint32_t room_id;
    char room_name[32];
    uint32_t current_users;
    uint32_t max_users;
};

struct CreateRoomRequest
{
    PacketHeader header;
    char room_name[32];
    uint32_t max_users;
};

struct CreateRoomResponse
{
    PacketHeader header;
    bool success;
    uint32_t created_room_id;
    char error_message[128];
};

struct RoomListRequest
{
    PacketHeader header;
};

struct RoomListResponse
{
    PacketHeader header;
    uint32_t room_count;
    RoomInfo rooms[16];
};

struct JoinRoomRequest
{
    PacketHeader header;
    uint32_t room_id;
};

struct JoinRoomResponse
{
    PacketHeader header;
    bool success;
    uint32_t room_id;
    char error_message[128];
};

struct LeaveRoomRequest
{
    PacketHeader header;
    uint32_t room_id;
};

struct LeaveRoomResponse
{
    PacketHeader header;
    bool success;
    char error_message[128];
};

struct WhisperRequest
{
    PacketHeader header;
    uint32_t room_id;
    char target_username[32];
    char message[512];
};

struct WhisperResponse
{
    PacketHeader header;
    bool success;
    char error_message[128];
};

struct WhisperNotification
{
    PacketHeader header;
    char sender_username[32];
    char message[512];
};

struct ReconnectRequest
{
    PacketHeader header;
    uint32_t user_id;
    char reconnect_token[64];
    uint32_t last_room_id;
};

struct ReconnectResponse
{
    PacketHeader header;
    bool success;
    uint32_t restored_room_id;
    char error_message[128];
};
#pragma pack(pop)

// 전방 선언
class ChatServer;
class ChatSession;

//=====================
// 유저
//=====================
class User : public std::enable_shared_from_this<User>
{
public:
    User(boost::asio::io_context& io_context, uint32_t id, const std::string& username)
        : strand_(boost::asio::make_strand(io_context)),
          id_(id), password_(0), username_(username), is_online_(false),
          disconnect_timer_(strand_) {}

    void SetPassword(uint64_t password) { password_ = password; }
    uint32_t GetId() const { return id_; }
    uint64_t GetPassword() const { return password_; }
    const std::string& GetUsername() const { return username_; }
    bool IsOnline() const { return is_online_; }
    void SetOnline(bool online) { is_online_ = online; }
    void SetSession(std::shared_ptr<ChatSession> session) { session_ = session; }
    std::weak_ptr<ChatSession> GetSession() const { return session_; }

    void SetReconnectToken(const std::string& token) { reconnect_token_ = token; }
    const std::string& GetReconnectToken() const { return reconnect_token_; }

    // 코루틴 기반 연결 해제 타이머
    template <typename OnExpiredCallback>
    void StartDisconnectTimer(OnExpiredCallback&& on_expired)
    {
        co_spawn(strand_, [this, self = shared_from_this(), cb = std::forward<OnExpiredCallback>(on_expired)]() mutable -> awaitable<void> {
            boost::system::error_code ec;
            disconnect_timer_.expires_after(std::chrono::seconds(60));
            co_await disconnect_timer_.async_wait(boost::asio::redirect_error(use_awaitable, ec));
            if (!ec)
            {
                cb();
            }
        }, detached);
    }

    void CancelDisconnectTimer()
    {
        boost::asio::post(strand_, [this, self = shared_from_this()]() {
            boost::system::error_code ec;
            disconnect_timer_.cancel(ec);
        });
    }

private:
    boost::asio::strand<boost::asio::io_context::executor_type> strand_;
    uint32_t id_;
    uint64_t password_;
    std::string username_;
    bool is_online_;
    std::weak_ptr<ChatSession> session_;

    std::string reconnect_token_;
    boost::asio::steady_timer disconnect_timer_;
};

//=====================
// 패킷 버퍼
//=====================
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

//=====================
// 디스패처 인터페이스 (코루틴화)
//=====================
class IMessageHandler
{
public:
    virtual ~IMessageHandler() = default;
    virtual awaitable<void> HandleMessage(std::shared_ptr<ChatSession> session, const char* data, size_t size) = 0;
};

class MessageDispatcher
{
public:
    void RegisterHandler(MessageType type, std::unique_ptr<IMessageHandler> handler)
    {
        handlers_[type] = std::move(handler);
    }

    awaitable<void> DispatchMessage(std::shared_ptr<ChatSession> session, const PacketHeader& header, const char* data, size_t size)
    {
        auto it = handlers_.find(header.message_type);
        if (it != handlers_.end())
        {
            co_await it->second->HandleMessage(session, data, size);
        }
        else
        {
            std::cout << "[Server] Unknown message type received: " << static_cast<uint16_t>(header.message_type) << std::endl;
        }
    }

private:
    std::unordered_map<MessageType, std::unique_ptr<IMessageHandler>> handlers_;
};

//=====================
// 유저 매니저
//=====================
class UserManager : public std::enable_shared_from_this<UserManager>
{
public:
    explicit UserManager(boost::asio::io_context& io_context)
        : io_context_(io_context), strand_(boost::asio::make_strand(io_context)), next_user_id_(1) {}

    awaitable<std::shared_ptr<User>> CreateUser(std::string username, uint64_t password)
    {
        co_await boost::asio::post(strand_, use_awaitable);

        for (const auto& [id, user] : users_) {
            if (user->GetUsername() == username) {
                co_return nullptr;
            }
        }
        uint32_t user_id = next_user_id_++;
        auto user = std::make_shared<User>(io_context_, user_id, username);
        user->SetPassword(password);
        users_[user_id] = user;
        std::cout << "[UserManager] New user created: " << username << " (ID: " << user_id << ")" << std::endl;
        co_return user;
    }

    awaitable<std::shared_ptr<User>> GetUserByUsername(std::string username)
    {
        co_await boost::asio::post(strand_, use_awaitable);

        for (const auto& [id, user] : users_) {
            if (user->GetUsername() == username) {
                co_return user;
            }
        }
        co_return nullptr;
    }

    awaitable<std::shared_ptr<User>> GetUser(uint32_t user_id)
    {
        co_await boost::asio::post(strand_, use_awaitable);

        auto it = users_.find(user_id);
        co_return (it != users_.end()) ? it->second : nullptr;
    }

    awaitable<std::pair<bool, std::string>> TryReconnect(uint32_t user_id, std::string token, std::shared_ptr<ChatSession> new_session)
    {
        co_await boost::asio::post(strand_, use_awaitable);

        auto it = users_.find(user_id);
        if (it == users_.end()) {
            co_return std::make_pair(false, "USER_NOT_FOUND");
        }

        auto user = it->second;
        if (user->GetReconnectToken() != token) {
            co_return std::make_pair(false, "INVALID_TOKEN");
        }

        user->CancelDisconnectTimer();
        user->SetSession(new_session);
        co_return std::make_pair(true, "");
    }

private:
    boost::asio::io_context& io_context_;
    boost::asio::strand<boost::asio::io_context::executor_type> strand_;
    std::unordered_map<uint32_t, std::shared_ptr<User>> users_;
    uint32_t next_user_id_;
};

//=====================
// 채팅방
//=====================
class ChatRoom : public std::enable_shared_from_this<ChatRoom>
{
public:
    ChatRoom(boost::asio::io_context& io_context, uint32_t room_id, std::string name, uint32_t max_users)
        : strand_(boost::asio::make_strand(io_context)), room_id_(room_id), name_(name), max_users_(max_users), current_users_(0) {}

    uint32_t GetId() const { return room_id_; }
    std::string GetName() const { return name_; }
    uint32_t GetMaxUsers() const { return max_users_; }
    uint32_t GetCurrentUsers() const { return current_users_.load(); }

    awaitable<std::pair<bool, std::string>> AddUser(std::shared_ptr<User> user)
    {
        co_await boost::asio::post(strand_, use_awaitable);

        auto it = users_.find(user->GetId());
        if (it != users_.end()) {
            co_return std::make_pair(true, "");
        }

        if (users_.size() >= max_users_) {
            co_return std::make_pair(false, "ROOM_FULL");
        }

        users_[user->GetId()] = user;
        current_users_++;
        BroadcastNotification(user->GetUsername() + " joined the room.", user->GetId());
        co_return std::make_pair(true, "");
    }

    awaitable<std::pair<bool, std::string>> RemoveUser(uint32_t user_id)
    {
        co_await boost::asio::post(strand_, use_awaitable);

        auto it = users_.find(user_id);
        if (it == users_.end()) {
            co_return std::make_pair(false, "USER_NOT_IN_ROOM");
        }
        std::string username = it->second->GetUsername();
        users_.erase(it);
        current_users_--;
        BroadcastNotification(username + " left the room.", user_id);
        co_return std::make_pair(true, "");
    }

    void BroadcastMessage(const ChatMessage& msg, uint32_t sender_id);
    void BroadcastNotification(const std::string& notification_text, uint32_t except_user_id = 0);

private:
    boost::asio::strand<boost::asio::io_context::executor_type> strand_;
    uint32_t room_id_;
    std::string name_;
    uint32_t max_users_;
    std::atomic<uint32_t> current_users_;
    std::unordered_map<uint32_t, std::shared_ptr<User>> users_;
};

//=====================
// 세션 클래스 (코루틴 기반 메시지 루프)
//=====================
class ChatSession : public std::enable_shared_from_this<ChatSession>
{
public:
    ChatSession(tcp::socket socket, ChatServer& server)
        : strand_(boost::asio::make_strand(socket.get_executor())),
          socket_(std::move(socket)), server_(server), user_id_(0), is_authenticated_(false), is_disconnected_(false), idle_timer_(strand_) {}

    ~ChatSession()
    {
        boost::system::error_code ec;
        idle_timer_.cancel(ec);
        socket_.close(ec);
    }

    void Start()
    {
        co_spawn(strand_, [this, self = shared_from_this()]() -> awaitable<void> {
            StartIdleTimer();
            
            PacketHeader prompt_header{};
            prompt_header.packet_size = sizeof(PacketHeader);
            prompt_header.message_type = MessageType::LOGIN_PROMPT;
            prompt_header.user_id = 0;
            prompt_header.sequence_number = 0;

            SendMessage(&prompt_header, sizeof(PacketHeader));

            co_await ReadLoop();
        }, detached);
    }

    void SendMessage(const void* data, size_t size)
    {
        if (is_disconnected_) return;

        std::vector<char> message(static_cast<const char*>(data), static_cast<const char*>(data) + size);

        co_spawn(strand_, [this, self = shared_from_this(), msg = std::move(message)]() mutable -> awaitable<void> {
            bool write_in_progress = !write_queue_.empty();
            write_queue_.push(std::move(msg));

            if (!write_in_progress)
            {
                co_await WriteLoop();
            }
        }, detached);
    }

    void SetUserId(uint32_t id) { user_id_ = id; }
    uint32_t GetUserId() const { return user_id_; }
    void SetAuthenticated(bool auth) { is_authenticated_ = auth; }
    bool IsAuthenticated() const { return is_authenticated_; }

    void Disconnect();

    void Kick()
    {
        boost::asio::post(strand_, [this, self = shared_from_this()]() {
            if (is_disconnected_) return;
            is_disconnected_ = true;

            boost::system::error_code ec;
            idle_timer_.cancel(ec);
            socket_.close(ec);
        });
    }

    void ResetTimer()
    {
        idle_timer_.expires_after(std::chrono::seconds(300));
    }

private:
    void StartIdleTimer()
    {
        co_spawn(strand_, [this, self = shared_from_this()]() -> awaitable<void> {
            while (!is_disconnected_)
            {
                boost::system::error_code ec;
                idle_timer_.expires_after(std::chrono::seconds(300));
                co_await idle_timer_.async_wait(boost::asio::redirect_error(use_awaitable, ec));

                if (!ec)
                {
                    std::cout << "[System] Session timed out due to inactivity. User ID: " << user_id_ << '\n';
                    Disconnect();
                    break;
                }
                else if (ec == boost::asio::error::operation_aborted)
                {
                    // 타이머 리셋 요청 시 루프 계속 진행
                    continue;
                }
                else
                {
                    break;
                }
            }
        }, detached);
    }

    awaitable<void> ReadLoop()
    {
        try
        {
            while (!is_disconnected_)
            {
                size_t length = co_await socket_.async_read_some(boost::asio::buffer(read_buffer_), use_awaitable);
                ResetTimer();
                
                packet_buffer_.WriteData(read_buffer_.data(), length);
                std::vector<char> packet_data;
                while (packet_buffer_.ReadPacket(packet_data))
                {
                    co_await ProcessPacket(packet_data.data(), packet_data.size());
                }
            }
        }
        catch (const boost::system::system_error&)
        {
            Disconnect();
        }
    }

    awaitable<void> WriteLoop()
    {
        try
        {
            while (!write_queue_.empty() && !is_disconnected_)
            {
                const auto& message = write_queue_.front();
                co_await boost::asio::async_write(socket_, boost::asio::buffer(message.data(), message.size()), use_awaitable);
                write_queue_.pop();
            }
        }
        catch (const boost::system::system_error&)
        {
            Disconnect();
        }
    }

    awaitable<void> ProcessPacket(const char* data, size_t size);

    boost::asio::strand<boost::asio::any_io_executor> strand_;
    tcp::socket socket_;
    ChatServer& server_;
    uint32_t user_id_;
    bool is_authenticated_;
    bool is_disconnected_;
    boost::asio::steady_timer idle_timer_;
    std::queue<std::vector<char>> write_queue_;
    std::vector<char> read_buffer_ = std::vector<char>(4096);
    PacketBuffer packet_buffer_;
};

// ChatRoom 내부 함수 정의
inline void ChatRoom::BroadcastMessage(const ChatMessage& msg, uint32_t sender_id)
{
    boost::asio::post(strand_, [this, self = shared_from_this(), msg, sender_id]() {
        for (auto& [id, user] : users_)
        {
            if (auto session = user->GetSession().lock())
            {
                session->SendMessage(&msg, sizeof(ChatMessage));
            }
        }
    });
}

inline void ChatRoom::BroadcastNotification(const std::string& notification_text, uint32_t except_user_id)
{
    ServerNotification notif{};
    notif.header.packet_size = sizeof(ServerNotification);
    notif.header.message_type = MessageType::SERVER_NOTIFICATION;
    notif.header.user_id = 0;
    notif.header.sequence_number = 0;
    std::strncpy(notif.message, notification_text.c_str(), sizeof(notif.message) - 1);

    for (auto& [id, user] : users_)
    {
        if (id == except_user_id) continue;
        if (auto session = user->GetSession().lock())
        {
            session->SendMessage(&notif, sizeof(ServerNotification));
        }
    }
}

//=====================
// 서버 클래스
//=====================
class ChatServer : public std::enable_shared_from_this<ChatServer>
{
public:
    ChatServer(boost::asio::io_context& io_context, short port);

    boost::asio::io_context& GetIOContext() { return io_context_; }
    MessageDispatcher& GetDispatcher() { return dispatcher_; }
    UserManager& GetUserManager() { return *user_manager_; }

    void OnSessionDisconnected(std::shared_ptr<ChatSession> session)
    {
        uint32_t user_id = session->GetUserId();
        if (user_id == 0) return;

        co_spawn(strand_, [this, self = shared_from_this(), user_id]() -> awaitable<void> {
            auto user = co_await user_manager_->GetUser(user_id);
            if (user)
            {
                user->SetOnline(false);
                std::cout << "[System] User connection lost (Grace period 60s started). User ID: " << user_id << std::endl;

                user->StartDisconnectTimer([this, self, user_id]() {
                    co_spawn(strand_, [this, self, user_id]() -> awaitable<void> {
                        for (auto& [id, room] : rooms_) {
                            co_await room->RemoveUser(user_id);
                        }
                        std::cout << "[System] User disconnect grace period expired. Removed from all rooms ID: " << user_id << std::endl;
                    }, detached);
                });
            }
        }, detached);
    }

    awaitable<uint32_t> CreateRoom(std::string name, uint32_t max_users)
    {
        co_await boost::asio::post(strand_, use_awaitable);

        uint32_t new_room_id = next_room_id_++;
        auto new_room = std::make_shared<ChatRoom>(io_context_, new_room_id, name, max_users);
        rooms_[new_room_id] = new_room;
        std::cout << "[Server] Room Created: " << name << " (ID: " << new_room_id << ")" << std::endl;
        co_return new_room_id;
    }

    void CreateRoom(uint32_t room_id, const std::string& name, uint32_t max_users)
    {
        boost::asio::post(strand_, [this, self = shared_from_this(), room_id, name, max_users]() {
            rooms_[room_id] = std::make_shared<ChatRoom>(io_context_, room_id, name, max_users);
            if (room_id >= next_room_id_)
            {
                next_room_id_ = room_id + 1;
            }
        });
    }

    awaitable<std::vector<RoomInfo>> GetRoomList()
    {
        co_await boost::asio::post(strand_, use_awaitable);

        std::vector<RoomInfo> room_list;
        for (const auto& [id, room] : rooms_)
        {
            RoomInfo info{};
            info.room_id = room->GetId();
            std::strncpy(info.room_name, room->GetName().c_str(), sizeof(info.room_name) - 1);
            info.current_users = room->GetCurrentUsers();
            info.max_users = room->GetMaxUsers();
            room_list.push_back(info);
        }
        co_return room_list;
    }

    awaitable<std::shared_ptr<ChatRoom>> GetRoom(uint32_t room_id)
    {
        co_await boost::asio::post(strand_, use_awaitable);

        auto it = rooms_.find(room_id);
        co_return (it != rooms_.end()) ? it->second : nullptr;
    }

    void StartAccept()
    {
        co_spawn(acceptor_.get_executor(), [this, self = shared_from_this()]() -> awaitable<void> {
            while (true)
            {
                tcp::socket socket = co_await acceptor_.async_accept(use_awaitable);
                std::make_shared<ChatSession>(std::move(socket), *this)->Start();
            }
        }, detached);
    }

private:
    boost::asio::io_context& io_context_;
    boost::asio::strand<boost::asio::io_context::executor_type> strand_;
    tcp::acceptor acceptor_;
    MessageDispatcher dispatcher_;
    std::shared_ptr<UserManager> user_manager_;
    std::unordered_map<uint32_t, std::shared_ptr<ChatRoom>> rooms_;
    uint32_t next_room_id_ = 1;
};

inline void ChatSession::Disconnect()
{
    if (is_disconnected_) return;
    is_disconnected_ = true;

    boost::system::error_code ec;
    idle_timer_.cancel(ec);
    socket_.close(ec);
    server_.OnSessionDisconnected(shared_from_this());
}

inline awaitable<void> ChatSession::ProcessPacket(const char* data, size_t size)
{
    if (size < sizeof(PacketHeader)) co_return;
    const auto& header = *reinterpret_cast<const PacketHeader*>(data);
    co_await server_.GetDispatcher().DispatchMessage(shared_from_this(), header, data, size);
}

//=====================
// 코루틴 메시지 핸들러 구현
//=====================
class ReconnectHandler : public IMessageHandler
{
public:
    ReconnectHandler(UserManager& user_manager, ChatServer& server)
        : user_manager_(user_manager), server_(server) {}

    awaitable<void> HandleMessage(std::shared_ptr<ChatSession> session, const char* data, size_t size) override
    {
        if (size < sizeof(ReconnectRequest)) co_return;
        const auto& req = *reinterpret_cast<const ReconnectRequest*>(data);

        uint32_t user_id = req.user_id;
        std::string token = req.reconnect_token;
        uint32_t last_room_id = req.last_room_id;

        auto [success, err] = co_await user_manager_.TryReconnect(user_id, token, session);

        ReconnectResponse res{};
        res.header.message_type = MessageType::RECONNECT_RESPONSE;
        res.header.packet_size = sizeof(ReconnectResponse);

        if (!success)
        {
            res.success = false;
            std::strncpy(res.error_message, err.c_str(), sizeof(res.error_message) - 1);
            session->SendMessage(&res, sizeof(ReconnectResponse));
            co_return;
        }

        auto user = co_await user_manager_.GetUser(user_id);
        if (!user) co_return;

        user->SetOnline(true);
        session->SetUserId(user->GetId());
        session->SetAuthenticated(true);

        auto room = co_await server_.GetRoom(last_room_id);
        if (room)
        {
            auto [joined, join_err] = co_await room->AddUser(user);
            res.success = true;
            res.restored_room_id = joined ? last_room_id : 0;
        }
        else
        {
            res.success = true;
            res.restored_room_id = 0;
        }

        session->SendMessage(&res, sizeof(ReconnectResponse));
    }

private:
    UserManager& user_manager_;
    ChatServer& server_;
};

class CreateRoomHandler : public IMessageHandler
{
public:
    explicit CreateRoomHandler(ChatServer& server) : server_(server) {}

    awaitable<void> HandleMessage(std::shared_ptr<ChatSession> session, const char* data, size_t size) override
    {
        if (!session->IsAuthenticated() || size < sizeof(CreateRoomRequest)) co_return;
        const auto& req = *reinterpret_cast<const CreateRoomRequest*>(data);

        std::string room_name = req.room_name;
        uint32_t max_users = req.max_users;

        uint32_t created_room_id = co_await server_.CreateRoom(room_name, max_users);

        CreateRoomResponse res{};
        res.header.message_type = MessageType::CREATE_ROOM_RESPONSE;
        res.header.packet_size = sizeof(CreateRoomResponse);
        res.success = true;
        res.created_room_id = created_room_id;

        session->SendMessage(&res, sizeof(CreateRoomResponse));
    }

private:
    ChatServer& server_;
};

class RoomListHandler : public IMessageHandler
{
public:
    explicit RoomListHandler(ChatServer& server) : server_(server) {}

    awaitable<void> HandleMessage(std::shared_ptr<ChatSession> session, const char* data, size_t size) override
    {
        if (!session->IsAuthenticated() || size < sizeof(RoomListRequest)) co_return;

        auto list = co_await server_.GetRoomList();

        RoomListResponse res{};
        res.header.message_type = MessageType::ROOM_LIST_RESPONSE;
        res.header.packet_size = sizeof(RoomListResponse);
        res.room_count = static_cast<uint32_t>(std::min(list.size(), size_t(16)));

        for (size_t i = 0; i < res.room_count; ++i)
        {
            res.rooms[i] = list[i];
        }

        session->SendMessage(&res, sizeof(RoomListResponse));
    }

private:
    ChatServer& server_;
};

class LoginHandler : public IMessageHandler
{
public:
    LoginHandler(UserManager& user_manager, ChatServer& server)
        : user_manager_(user_manager), server_(server) {}

    awaitable<void> HandleMessage(std::shared_ptr<ChatSession> session, const char* data, size_t size) override
    {
        if (size < sizeof(LoginRequest)) co_return;
        const auto& request = *reinterpret_cast<const LoginRequest*>(data);

        std::string username = request.username;
        uint64_t pass = 0;
        try { pass = std::stoull(request.password); } catch (...) { pass = 0; }

        auto existing_user = co_await user_manager_.GetUserByUsername(username);

        LoginResponse response{};
        response.header.message_type = MessageType::LOGIN_RESPONSE;
        response.header.packet_size = sizeof(LoginResponse);

        if (!existing_user)
        {
            response.success = false;
            std::strncpy(response.error_message, "USER_NOT_FOUND", sizeof(response.error_message) - 1);
            std::cout << "[Login Fail] User not found: " << username << std::endl;
            session->SendMessage(&response, sizeof(LoginResponse));
        }
        else if (existing_user->GetPassword() == pass)
        {
            if (existing_user->IsOnline())
            {
                if (auto old_session = existing_user->GetSession().lock())
                {
                    std::cout << "[Login] Dual login detected for user: " << username 
                              << ". Kicking old session." << std::endl;

                    ServerNotification kick_notif{};
                    kick_notif.header.packet_size = sizeof(ServerNotification);
                    kick_notif.header.message_type = MessageType::SERVER_NOTIFICATION;
                    std::strncpy(kick_notif.message, "Logged in from another location.", sizeof(kick_notif.message) - 1);
                    old_session->SendMessage(&kick_notif, sizeof(ServerNotification));

                    old_session->Kick();
                }

                existing_user->CancelDisconnectTimer();
            }

            existing_user->SetSession(session);
            existing_user->SetOnline(true);

            std::string token = "TOKEN_" + std::to_string(existing_user->GetId()) + "_SECRET";
            existing_user->SetReconnectToken(token);
            std::strncpy(response.reconnect_token, token.c_str(), sizeof(response.reconnect_token) - 1);

            session->SetUserId(existing_user->GetId());
            session->SetAuthenticated(true);

            response.success = true;
            response.assigned_user_id = existing_user->GetId();
            std::cout << "[Login Success] User: " << username << " (ID: " << existing_user->GetId() << ")" << std::endl;

            auto lobby = co_await server_.GetRoom(1);
            if (lobby) co_await lobby->AddUser(existing_user);

            session->SendMessage(&response, sizeof(LoginResponse));
        }
        else
        {
            response.success = false;
            std::strncpy(response.error_message, "WRONG_PASSWORD", sizeof(response.error_message) - 1);
            std::cout << "[Login Fail] Incorrect password for: " << username << std::endl;
            session->SendMessage(&response, sizeof(LoginResponse));
        }
    }

private:
    UserManager& user_manager_;
    ChatServer& server_;
};

class RegisterHandler : public IMessageHandler
{
public:
    explicit RegisterHandler(UserManager& user_manager) : user_manager_(user_manager) {}

    awaitable<void> HandleMessage(std::shared_ptr<ChatSession> session, const char* data, size_t size) override
    {
        if (size < sizeof(RegisterRequest)) co_return;
        const auto& request = *reinterpret_cast<const RegisterRequest*>(data);

        std::string username = request.username;
        uint64_t pass = 0;
        try { pass = std::stoull(request.password); } catch (...) { pass = 0; }

        auto new_user = co_await user_manager_.CreateUser(username, pass);

        RegisterResponse response{};
        response.header.message_type = MessageType::REGISTER_RESPONSE;
        response.header.packet_size = sizeof(RegisterResponse);

        if (new_user)
        {
            response.success = true;
            response.assigned_user_id = new_user->GetId();
            std::cout << "[Register Success] New User Created: " << username << " (ID: " << new_user->GetId() << ")" << std::endl;
        }
        else
        {
            response.success = false;
            std::strncpy(response.error_message, "Username already exists.", sizeof(response.error_message) - 1);
        }

        session->SendMessage(&response, sizeof(RegisterResponse));
    }

private:
    UserManager& user_manager_;
};

class ChatMessageHandler : public IMessageHandler
{
public:
    explicit ChatMessageHandler(ChatServer& server) : server_(server) {}

    awaitable<void> HandleMessage(std::shared_ptr<ChatSession> session, const char* data, size_t size) override
    {
        if (!session->IsAuthenticated() || size < sizeof(ChatMessage)) co_return;
        const auto& message = *reinterpret_cast<const ChatMessage*>(data);

        auto room = co_await server_.GetRoom(message.room_id);
        if (room)
        {
            room->BroadcastMessage(message, session->GetUserId());
        }
    }

private:
    ChatServer& server_;
};

class JoinRoomHandler : public IMessageHandler
{
public:
    JoinRoomHandler(UserManager& user_manager, ChatServer& server)
        : user_manager_(user_manager), server_(server) {}

    awaitable<void> HandleMessage(std::shared_ptr<ChatSession> session, const char* data, size_t size) override
    {
        if (!session->IsAuthenticated() || size < sizeof(JoinRoomRequest)) co_return;
        const auto& req = *reinterpret_cast<const JoinRoomRequest*>(data);

        uint32_t user_id = session->GetUserId();
        uint32_t target_room_id = req.room_id;

        auto user = co_await user_manager_.GetUser(user_id);
        if (!user) co_return;

        auto room = co_await server_.GetRoom(target_room_id);

        JoinRoomResponse res{};
        res.header.message_type = MessageType::JOIN_ROOM_RESPONSE;
        res.header.packet_size = sizeof(JoinRoomResponse);
        res.room_id = target_room_id;

        if (!room)
        {
            res.success = false;
            std::strncpy(res.error_message, "ROOM_NOT_FOUND", sizeof(res.error_message) - 1);
            session->SendMessage(&res, sizeof(JoinRoomResponse));
            co_return;
        }

        auto [success, err] = co_await room->AddUser(user);
        res.success = success;
        if (!success)
        {
            std::strncpy(res.error_message, err.c_str(), sizeof(res.error_message) - 1);
        }
        else
        {
            std::cout << "[Room] User " << user_id << " joined room " << target_room_id << std::endl;
        }
        session->SendMessage(&res, sizeof(JoinRoomResponse));
    }

private:
    UserManager& user_manager_;
    ChatServer& server_;
};

class LeaveRoomHandler : public IMessageHandler
{
public:
    explicit LeaveRoomHandler(ChatServer& server) : server_(server) {}

    awaitable<void> HandleMessage(std::shared_ptr<ChatSession> session, const char* data, size_t size) override
    {
        if (!session->IsAuthenticated() || size < sizeof(LeaveRoomRequest)) co_return;
        const auto& req = *reinterpret_cast<const LeaveRoomRequest*>(data);

        uint32_t user_id = session->GetUserId();
        uint32_t target_room_id = req.room_id;

        auto room = co_await server_.GetRoom(target_room_id);

        LeaveRoomResponse res{};
        res.header.message_type = MessageType::LEAVE_ROOM_RESPONSE;
        res.header.packet_size = sizeof(LeaveRoomResponse);

        if (!room)
        {
            res.success = false;
            std::strncpy(res.error_message, "ROOM_NOT_FOUND", sizeof(res.error_message) - 1);
            session->SendMessage(&res, sizeof(LeaveRoomResponse));
            co_return;
        }

        auto [success, err] = co_await room->RemoveUser(user_id);
        res.success = success;
        if (!success)
        {
            std::strncpy(res.error_message, err.c_str(), sizeof(res.error_message) - 1);
        }
        else
        {
            std::cout << "[Room] User " << user_id << " left room " << room->GetId() << std::endl;
        }
        session->SendMessage(&res, sizeof(LeaveRoomResponse));
    }

private:
    ChatServer& server_;
};

class WhisperHandler : public IMessageHandler
{
public:
    WhisperHandler(UserManager& user_manager, ChatServer& server)
        : user_manager_(user_manager), server_(server) {}

    awaitable<void> HandleMessage(std::shared_ptr<ChatSession> session, const char* data, size_t size) override
    {
        if (!session->IsAuthenticated() || size < sizeof(WhisperRequest)) co_return;
        const auto& req = *reinterpret_cast<const WhisperRequest*>(data);

        uint32_t sender_id = session->GetUserId();
        std::string target_username = req.target_username;
        std::string message = req.message;
        uint32_t room_id = req.room_id;

        auto sender_user = co_await user_manager_.GetUser(sender_id);
        if (!sender_user) co_return;

        auto room = co_await server_.GetRoom(room_id);

        WhisperResponse res{};
        res.header.message_type = MessageType::WHISPER_RESPONSE;
        res.header.packet_size = sizeof(WhisperResponse);

        if (!room)
        {
            res.success = false;
            std::strncpy(res.error_message, "ROOM_NOT_FOUND", sizeof(res.error_message) - 1);
            session->SendMessage(&res, sizeof(WhisperResponse));
            co_return;
        }

        if (sender_user->GetUsername() == target_username)
        {
            res.success = false;
            std::strncpy(res.error_message, "CANNOT_WHISPER_SELF", sizeof(res.error_message) - 1);
            session->SendMessage(&res, sizeof(WhisperResponse));
            co_return;
        }

        auto target_user = co_await user_manager_.GetUserByUsername(target_username);

        if (!target_user)
        {
            res.success = false;
            std::strncpy(res.error_message, "USER_NOT_FOUND", sizeof(res.error_message) - 1);
        }
        else if (auto target_session = target_user->GetSession().lock())
        {
            WhisperNotification notif{};
            notif.header.packet_size = sizeof(WhisperNotification);
            notif.header.message_type = MessageType::WHISPER_NOTIFICATION;
            std::strncpy(notif.sender_username, sender_user->GetUsername().c_str(), sizeof(notif.sender_username) - 1);
            std::strncpy(notif.message, message.c_str(), sizeof(notif.message) - 1);

            target_session->SendMessage(&notif, sizeof(WhisperNotification));
            res.success = true;
        }
        else
        {
            res.success = false;
            std::strncpy(res.error_message, "TARGET_DISCONNECTED", sizeof(res.error_message) - 1);
        }

        session->SendMessage(&res, sizeof(WhisperResponse));
    }

private:
    UserManager& user_manager_;
    ChatServer& server_;
};

// ChatServer 생성자 구현
inline ChatServer::ChatServer(boost::asio::io_context& io_context, short port)
    : io_context_(io_context),
      strand_(boost::asio::make_strand(io_context)),
      acceptor_(io_context, tcp::endpoint(tcp::v4(), port)),
      user_manager_(std::make_shared<UserManager>(io_context))
{
    dispatcher_.RegisterHandler(MessageType::LOGIN_REQUEST, std::make_unique<LoginHandler>(*user_manager_, *this));
    dispatcher_.RegisterHandler(MessageType::REGISTER_REQUEST, std::make_unique<RegisterHandler>(*user_manager_));
    dispatcher_.RegisterHandler(MessageType::CHAT_MESSAGE, std::make_unique<ChatMessageHandler>(*this));
    dispatcher_.RegisterHandler(MessageType::CREATE_ROOM_REQUEST, std::make_unique<CreateRoomHandler>(*this));
    dispatcher_.RegisterHandler(MessageType::ROOM_LIST_REQUEST, std::make_unique<RoomListHandler>(*this));
    dispatcher_.RegisterHandler(MessageType::JOIN_ROOM, std::make_unique<JoinRoomHandler>(*user_manager_, *this));
    dispatcher_.RegisterHandler(MessageType::LEAVE_ROOM, std::make_unique<LeaveRoomHandler>(*this));
    dispatcher_.RegisterHandler(MessageType::WHISPER_REQUEST, std::make_unique<WhisperHandler>(*user_manager_, *this));
    dispatcher_.RegisterHandler(MessageType::RECONNECT_REQUEST, std::make_unique<ReconnectHandler>(*user_manager_, *this));

    StartAccept();
}

//=====================
// 진입점
//=====================
int main()
{
    try
    {
        boost::asio::io_context io_context;
        auto server = std::make_shared<ChatServer>(io_context, 8080);
        server->CreateRoom(1, "Lobby", 100);

        std::cout << "[Server] Running on port 8080 (Coroutine-based with Reconnection support)..." << std::endl;

        std::vector<std::thread> threads;
        for (int i = 0; i < 4; ++i)
        {
            threads.emplace_back([&io_context]() { io_context.run(); });
        }

        for (auto& t : threads)
        {
            if (t.joinable()) t.join();
        }
    }
    catch (std::exception& e)
    {
        std::cerr << "Exception: " << e.what() << std::endl;
    }
    return 0;
}
