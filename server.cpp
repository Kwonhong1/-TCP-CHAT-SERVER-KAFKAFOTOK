#include <boost/asio.hpp>
#include <boost/bind/bind.hpp>
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

using boost::asio::ip::tcp;

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
    JOIN_ROOM_RESPONSE = 1016,  // 추가
    LEAVE_ROOM_RESPONSE = 1017  // 추가
    WHISPER_REQUEST=1018,
    WHISPER_RESPONSE=1019,
    WHISPER_NOTIFICATION=1020
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

// --- 신규 추가: 방 입장 / 퇴장 패킷 구조체 ---
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
#pragma pack(pop)

class ChatServer;
class ChatRoom;
class ChatSession;

//=====================
// 유저 및 관리자
//=====================
class User
{
public:
    User(uint32_t id, const std::string& username)
        : id_(id), password_(0), username_(username), is_online_(false) {}

    void SetPassword(uint64_t password) { password_ = password; }
    uint32_t GetId() const { return id_; }
    uint64_t GetPassword() const { return password_; }
    const std::string& GetUsername() const { return username_; }
    bool IsOnline() const { return is_online_; }
    void SetOnline(bool online) { is_online_ = online; }
    void SetSession(std::shared_ptr<ChatSession> session) { session_ = session; }
    std::weak_ptr<ChatSession> GetSession() const { return session_; }

private:
    uint32_t id_;
    uint64_t password_;
    std::string username_;
    bool is_online_;
    std::weak_ptr<ChatSession> session_;
};

class UserManager : public std::enable_shared_from_this<UserManager>
{
public:
    explicit UserManager(boost::asio::io_context& io_context)
        : strand_(boost::asio::make_strand(io_context)), next_user_id_(1) {}

    template <typename Callback>
    void CreateUser(const std::string& username, uint64_t password, Callback&& callback)
    {
        boost::asio::post(strand_, [this, self = shared_from_this(), username, password, cb = std::forward<Callback>(callback)]() mutable {
            for (const auto& [id, user] : users_) {
                if (user->GetUsername() == username) {
                    cb(nullptr);
                    return;
                }
            }
            uint32_t user_id = next_user_id_++;
            auto user = std::make_shared<User>(user_id, username);
            user->SetPassword(password);
            users_[user_id] = user;
            std::cout << "[UserManager] New user created: " << username << " (ID: " << user_id << ")" << std::endl;
            cb(user);
        });
    }

    template <typename Callback>
    void GetUserByUsername(const std::string& username, Callback&& callback)
    {
        boost::asio::post(strand_, [this, self = shared_from_this(), username, cb = std::forward<Callback>(callback)]() mutable {
            for (const auto& [id, user] : users_) {
                if (user->GetUsername() == username) {
                    cb(user);
                    return;
                }
            }
            cb(nullptr);
        });
    }

    template <typename Callback>
    void GetUser(uint32_t user_id, Callback&& callback)
    {
        boost::asio::post(strand_, [this, self = shared_from_this(), user_id, cb = std::forward<Callback>(callback)]() mutable {
            auto it = users_.find(user_id);
            cb((it != users_.end()) ? it->second : nullptr);
        });
    }

private:
    boost::asio::strand<boost::asio::io_context::executor_type> strand_;
    std::unordered_map<uint32_t, std::shared_ptr<User>> users_;
    uint32_t next_user_id_;
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
// 디스패처 인터페이스
//=====================
class IMessageHandler
{
public:
    virtual ~IMessageHandler() = default;
    virtual void HandleMessage(std::shared_ptr<ChatSession> session, const char* data, size_t size) = 0;
};

class MessageDispatcher
{
public:
    void RegisterHandler(MessageType type, std::unique_ptr<IMessageHandler> handler)
    {
        handlers_[type] = std::move(handler);
    }

    void DispatchMessage(std::shared_ptr<ChatSession> session, const PacketHeader& header, const char* data, size_t size)
    {
        auto it = handlers_.find(header.message_type);
        if (it != handlers_.end())
        {
            it->second->HandleMessage(session, data, size);
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
// 세션 클래스
//=====================
class ChatSession : public std::enable_shared_from_this<ChatSession>
{
public:
    ChatSession(tcp::socket socket, ChatServer& server)
        : strand_(boost::asio::make_strand(socket.get_executor())),
          socket_(std::move(socket)), server_(server), user_id_(0), is_authenticated_(false), is_disconnected_(false) {}

    ~ChatSession()
    {
        boost::system::error_code ec;
        socket_.close(ec);
    }

    void Start();

    void SendMessage(const void* data, size_t size)
    {
        if (is_disconnected_) return;

        std::vector<char> message(static_cast<const char*>(data), static_cast<const char*>(data) + size);

        boost::asio::post(strand_, [this, self = shared_from_this(), msg = std::move(message)]() mutable {
            bool write_in_progress = !write_queue_.empty();
            write_queue_.push(std::move(msg));

            if (!write_in_progress)
            {
                Do_write();
            }
        });
    }

    void SetUserId(uint32_t id) { user_id_ = id; }
    uint32_t GetUserId() const { return user_id_; }
    void SetAuthenticated(bool auth) { is_authenticated_ = auth; }
    bool IsAuthenticated() const { return is_authenticated_; }

    void Disconnect();

private:
    void Do_read()
    {
        auto self = shared_from_this();
        socket_.async_read_some(boost::asio::buffer(read_buffer_),
            boost::asio::bind_executor(strand_, [this, self](boost::system::error_code ec, std::size_t length) {
                if (!ec)
                {
                    packet_buffer_.WriteData(read_buffer_.data(), length);
                    std::vector<char> packet_data;
                    while (packet_buffer_.ReadPacket(packet_data))
                    {
                        ProcessPacket(packet_data.data(), packet_data.size());
                    }
                    Do_read();
                }
                else
                {
                    Disconnect();
                }
            }));
    }

    void Do_write()
    {
        auto self = shared_from_this();
        if (write_queue_.empty()) return;

        const auto& message = write_queue_.front();

        boost::asio::async_write(socket_, boost::asio::buffer(message.data(), message.size()),
            boost::asio::bind_executor(strand_, [this, self](boost::system::error_code ec, std::size_t) {
                if (!ec)
                {
                    write_queue_.pop();
                    if (!write_queue_.empty()) Do_write();
                }
                else
                {
                    Disconnect();
                }
            }));
    }

    void ProcessPacket(const char* data, size_t size);

    boost::asio::strand<boost::asio::any_io_executor> strand_;
    tcp::socket socket_;
    ChatServer& server_;
    uint32_t user_id_;
    bool is_authenticated_;
    bool is_disconnected_;

    std::queue<std::vector<char>> write_queue_;
    std::vector<char> read_buffer_ = std::vector<char>(4096);
    PacketBuffer packet_buffer_;
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

    template <typename Callback>
    void AddUser(std::shared_ptr<User> user, Callback&& callback)
    {
        boost::asio::post(strand_, [this, self = shared_from_this(), user, cb = std::forward<Callback>(callback)]() mutable {
            if (users_.size() >= max_users_) {
                cb(false, "ROOM_FULL");
                return;
            }
            if (users_.find(user->GetId()) != users_.end()) {
                cb(false, "ALREADY_IN_ROOM");
                return;
            }
            users_[user->GetId()] = user;
            current_users_++;
            BroadcastNotification(user->GetUsername() + " joined the room.", user->GetId());
            cb(true, "");
        });
    }

    void AddUser(std::shared_ptr<User> user)
    {
        AddUser(user, [](bool, const std::string&) {});
    }

    template <typename Callback>
    void RemoveUser(uint32_t user_id, Callback&& callback)
    {
        boost::asio::post(strand_, [this, self = shared_from_this(), user_id, cb = std::forward<Callback>(callback)]() mutable {
            auto it = users_.find(user_id);
            if (it == users_.end()) {
                cb(false, "USER_NOT_IN_ROOM");
                return;
            }
            std::string username = it->second->GetUsername();
            users_.erase(it);
            current_users_--;
            BroadcastNotification(username + " left the room.", user_id);
            cb(true, "");
        });
    }

    void RemoveUser(uint32_t user_id)
    {
        RemoveUser(user_id, [](bool, const std::string&) {});
    }

    void BroadcastMessage(const ChatMessage& msg, uint32_t sender_id)
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

    void BroadcastNotification(const std::string& notification_text, uint32_t except_user_id = 0)
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

private:
    boost::asio::strand<boost::asio::io_context::executor_type> strand_;
    uint32_t room_id_;
    std::string name_;
    uint32_t max_users_;
    std::atomic<uint32_t> current_users_;
    std::unordered_map<uint32_t, std::shared_ptr<User>> users_;
};

//=====================
// 서버 클래스
//=====================
class ChatServer : public std::enable_shared_from_this<ChatServer>
{
public:
    ChatServer(boost::asio::io_context& io_context, short port);

    void OnSessionDisconnected(std::shared_ptr<ChatSession> session)
    {
        uint32_t user_id = session->GetUserId();
        user_manager_->GetUser(user_id, [this, self = shared_from_this(), user_id](std::shared_ptr<User> user) {
            if (user)
            {
                user->SetOnline(false);
                boost::asio::post(strand_, [this, self, user_id]() {
                    for (auto& [id, room] : rooms_) room->RemoveUser(user_id);
                    std::cout << "[System] User disconnected ID: " << user_id << std::endl;
                });
            }
        });
    }

    template <typename Callback>
    void CreateRoom(const std::string& name, uint32_t max_users, Callback&& callback)
    {
        boost::asio::post(strand_, [this, self = shared_from_this(), name, max_users, cb = std::forward<Callback>(callback)]() mutable {
            uint32_t new_room_id = next_room_id_++;
            auto new_room = std::make_shared<ChatRoom>(io_context_, new_room_id, name, max_users);
            rooms_[new_room_id] = new_room;
            std::cout << "[Server] Room Created: " << name << " (ID: " << new_room_id << ")" << std::endl;
            cb(new_room_id);
        });
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

    template <typename Callback>
    void GetRoomList(Callback&& callback)
    {
        boost::asio::post(strand_, [this, self = shared_from_this(), cb = std::forward<Callback>(callback)]() mutable {
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
            cb(room_list);
        });
    }


    template <typename Callback>
    void SendWhisper(uint32_t sender_id, const std::string& sender_username, const std::string& target_username, const std::string& message, Callback&& callback)
    {
        boost::asio::post(strand_, [this, self = shared_from_this(), sender_id, sender_username, target_username, message, cb = std::forward<Callback>(callback)]() mutable {
            if (sender_username == target_username) {
                cb(false, "CANNOT_WHISPER_SELF");
                return;
            }

            std::shared_ptr<User> target_user = nullptr;
            for (const auto& [id, user] : users_) {
                if (user->GetUsername() == target_username) {
                    target_user = user;
                    break;
                }
            }

            if (!target_user) {
                cb(false, "USER_NOT_IN_ROOM");
                return;
            }

            auto target_session = target_user->GetSession().lock();
            if (!target_session) {
                cb(false, "TARGET_DISCONNECTED");
                return;
            }

            // 받는 유저에게 귓속말 전송
            WhisperNotification notif{};
            notif.header.packet_size = sizeof(WhisperNotification);
            notif.header.message_type = MessageType::WHISPER_NOTIFICATION;
            std::strncpy(notif.sender_username, sender_username.c_str(), sizeof(notif.sender_username) - 1);
            std::strncpy(notif.message, message.c_str(), sizeof(notif.message) - 1);

            target_session->SendMessage(&notif, sizeof(WhisperNotification));
            cb(true, "");
        });
    }

    MessageDispatcher& GetDispatcher() { return dispatcher_; }
    UserManager& GetUserManager() { return *user_manager_; }

    template <typename Callback>
    void GetRoom(uint32_t room_id, Callback&& callback)
    {
        boost::asio::post(strand_, [this, self = shared_from_this(), room_id, cb = std::forward<Callback>(callback)]() mutable {
            auto it = rooms_.find(room_id);
            cb((it != rooms_.end()) ? it->second : nullptr);
        });
    }

private:
    void do_accept()
    {
        acceptor_.async_accept([this](boost::system::error_code ec, tcp::socket socket) {
            if (!ec) std::make_shared<ChatSession>(std::move(socket), *this)->Start();
            do_accept();
        });
    }

    boost::asio::io_context& io_context_;
    boost::asio::strand<boost::asio::io_context::executor_type> strand_;
    tcp::acceptor acceptor_;
    MessageDispatcher dispatcher_;
    std::shared_ptr<UserManager> user_manager_;
    std::unordered_map<uint32_t, std::shared_ptr<ChatRoom>> rooms_;
    uint32_t next_room_id_ = 1;
};

void ChatSession::Start()
{
    Do_read();

    PacketHeader prompt_header{};
    prompt_header.packet_size = sizeof(PacketHeader);
    prompt_header.message_type = MessageType::LOGIN_PROMPT;
    prompt_header.user_id = 0;
    prompt_header.sequence_number = 0;

    SendMessage(&prompt_header, sizeof(PacketHeader));
}

void ChatSession::Disconnect()
{
    if (is_disconnected_) return;
    is_disconnected_ = true;

    boost::system::error_code ec;
    socket_.close(ec);
    server_.OnSessionDisconnected(shared_from_this());
}

void ChatSession::ProcessPacket(const char* data, size_t size)
{
    if (size < sizeof(PacketHeader)) return;
    const auto& header = *reinterpret_cast<const PacketHeader*>(data);
    server_.GetDispatcher().DispatchMessage(shared_from_this(), header, data, size);
}

//=====================
// 메시지 핸들러 구현부
//=====================
class CreateRoomHandler : public IMessageHandler
{
public:
    explicit CreateRoomHandler(ChatServer& server) : server_(server) {}

    void HandleMessage(std::shared_ptr<ChatSession> session, const char* data, size_t size) override
    {
        if (!session->IsAuthenticated() || size < sizeof(CreateRoomRequest)) return;
        const auto& req = *reinterpret_cast<const CreateRoomRequest*>(data);

        std::string room_name = req.room_name;
        uint32_t max_users = req.max_users;

        server_.CreateRoom(room_name, max_users, [session](uint32_t created_room_id) {
            CreateRoomResponse res{};
            res.header.message_type = MessageType::CREATE_ROOM_RESPONSE;
            res.header.packet_size = sizeof(CreateRoomResponse);
            res.success = true;
            res.created_room_id = created_room_id;

            session->SendMessage(&res, sizeof(CreateRoomResponse));
        });
    }

private:
    ChatServer& server_;
};

class RoomListHandler : public IMessageHandler
{
public:
    explicit RoomListHandler(ChatServer& server) : server_(server) {}

    void HandleMessage(std::shared_ptr<ChatSession> session, const char* data, size_t size) override
    {
        if (!session->IsAuthenticated() || size < sizeof(RoomListRequest)) return;

        server_.GetRoomList([session](const std::vector<RoomInfo>& list) {
            RoomListResponse res{};
            res.header.message_type = MessageType::ROOM_LIST_RESPONSE;
            res.header.packet_size = sizeof(RoomListResponse);
            res.room_count = static_cast<uint32_t>(std::min(list.size(), size_t(16)));

            for (size_t i = 0; i < res.room_count; ++i)
            {
                res.rooms[i] = list[i];
            }

            session->SendMessage(&res, sizeof(RoomListResponse));
        });
    }

private:
    ChatServer& server_;
};

class LoginHandler : public IMessageHandler
{
public:
    LoginHandler(UserManager& user_manager, ChatServer& server)
        : user_manager_(user_manager), server_(server) {}

    void HandleMessage(std::shared_ptr<ChatSession> session, const char* data, size_t size) override
    {
        if (size < sizeof(LoginRequest)) return;
        const auto& request = *reinterpret_cast<const LoginRequest*>(data);

        std::string username = request.username;
        uint64_t pass = 0;
        try { pass = std::stoull(request.password); } catch (...) { pass = 0; }

        user_manager_.GetUserByUsername(username, [session, pass, username, &server = server_](std::shared_ptr<User> existing_user) {
            LoginResponse response{};
            response.header.message_type = MessageType::LOGIN_RESPONSE;
            response.header.packet_size = sizeof(LoginResponse);

            if (!existing_user)
            {
                response.success = false;
                std::strncpy(response.error_message, "USER_NOT_FOUND", sizeof(response.error_message) - 1);
                std::cout << "[Login Fail] User not found: " << username << std::endl;
            }
            else if (existing_user->GetPassword() == pass)
            {
                existing_user->SetSession(session);
                existing_user->SetOnline(true);
                session->SetUserId(existing_user->GetId());
                session->SetAuthenticated(true);

                response.success = true;
                response.assigned_user_id = existing_user->GetId();
                std::cout << "[Login Success] User: " << username << " (ID: " << existing_user->GetId() << ")" << std::endl;

                server.GetRoom(1, [existing_user](std::shared_ptr<ChatRoom> lobby) {
                    if (lobby) lobby->AddUser(existing_user);
                });
            }
            else
            {
                response.success = false;
                std::strncpy(response.error_message, "WRONG_PASSWORD", sizeof(response.error_message) - 1);
                std::cout << "[Login Fail] Incorrect password for: " << username << std::endl;
            }

            session->SendMessage(&response, sizeof(LoginResponse));
        });
    }

private:
    UserManager& user_manager_;
    ChatServer& server_;
};

class RegisterHandler : public IMessageHandler
{
public:
    RegisterHandler(UserManager& user_manager) : user_manager_(user_manager) {}

    void HandleMessage(std::shared_ptr<ChatSession> session, const char* data, size_t size) override
    {
        if (size < sizeof(RegisterRequest)) return;
        const auto& request = *reinterpret_cast<const RegisterRequest*>(data);

        std::string username = request.username;
        uint64_t pass = 0;
        try { pass = std::stoull(request.password); } catch (...) { pass = 0; }

        user_manager_.CreateUser(username, pass, [session, username](std::shared_ptr<User> new_user) {
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
        });
    }

private:
    UserManager& user_manager_;
};

class ChatMessageHandler : public IMessageHandler
{
public:
    ChatMessageHandler(ChatServer& server) : server_(server) {}

    void HandleMessage(std::shared_ptr<ChatSession> session, const char* data, size_t size) override
    {
        if (!session->IsAuthenticated() || size < sizeof(ChatMessage)) return;
        const auto& message = *reinterpret_cast<const ChatMessage*>(data);

        server_.GetRoom(message.room_id, [message, sender_id = session->GetUserId()](std::shared_ptr<ChatRoom> room) {
            if (room)
            {
                room->BroadcastMessage(message, sender_id);
            }
        });
    }

private:
    ChatServer& server_;
};

// --- 신규 추가: JOIN_ROOM 처리 핸들러 ---
class JoinRoomHandler : public IMessageHandler
{
public:
    JoinRoomHandler(UserManager& user_manager, ChatServer& server)
        : user_manager_(user_manager), server_(server) {}

    void HandleMessage(std::shared_ptr<ChatSession> session, const char* data, size_t size) override
    {
        if (!session->IsAuthenticated() || size < sizeof(JoinRoomRequest)) return;
        const auto& req = *reinterpret_cast<const JoinRoomRequest*>(data);

        uint32_t user_id = session->GetUserId();
        uint32_t target_room_id = req.room_id;

        user_manager_.GetUser(user_id, [this, session, target_room_id, user_id](std::shared_ptr<User> user) {
            if (!user) return;

            server_.GetRoom(target_room_id, [session, user, target_room_id, user_id](std::shared_ptr<ChatRoom> room) {
                JoinRoomResponse res{};
                res.header.message_type = MessageType::JOIN_ROOM_RESPONSE;
                res.header.packet_size = sizeof(JoinRoomResponse);
                res.room_id = target_room_id;

                if (!room)
                {
                    res.success = false;
                    std::strncpy(res.error_message, "ROOM_NOT_FOUND", sizeof(res.error_message) - 1);
                    session->SendMessage(&res, sizeof(JoinRoomResponse));
                    return;
                }

                room->AddUser(user, [session, res, user_id, target_room_id](bool success, const std::string& err) mutable {
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
                });
            });
        });
    }

private:
    UserManager& user_manager_;
    ChatServer& server_;
};

// --- 신규 추가: LEAVE_ROOM 처리 핸들러 ---
class LeaveRoomHandler : public IMessageHandler
{
public:
    explicit LeaveRoomHandler(ChatServer& server) : server_(server) {}

    void HandleMessage(std::shared_ptr<ChatSession> session, const char* data, size_t size) override
    {
        if (!session->IsAuthenticated() || size < sizeof(LeaveRoomRequest)) return;
        const auto& req = *reinterpret_cast<const LeaveRoomRequest*>(data);

        uint32_t user_id = session->GetUserId();
        uint32_t target_room_id = req.room_id;

        server_.GetRoom(target_room_id, [session, user_id](std::shared_ptr<ChatRoom> room) {
            LeaveRoomResponse res{};
            res.header.message_type = MessageType::LEAVE_ROOM_RESPONSE;
            res.header.packet_size = sizeof(LeaveRoomResponse);

            if (!room)
            {
                res.success = false;
                std::strncpy(res.error_message, "ROOM_NOT_FOUND", sizeof(res.error_message) - 1);
                session->SendMessage(&res, sizeof(LeaveRoomResponse));
                return;
            }

            room->RemoveUser(user_id, [session, res, user_id, room](bool success, const std::string& err) mutable {
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
            });
        });
    }

private:
    ChatServer& server_;
};

class WhisperHandler : public IMessageHandler
{
public:
    WhisperHandler(UserManager& user_manager, ChatServer& server)
        : user_manager_(user_manager), server_(server) {}

    void HandleMessage(std::shared_ptr<ChatSession> session, const char* data, size_t size) override
    {
        if (!session->IsAuthenticated() || size < sizeof(WhisperRequest)) return;
        const auto& req = *reinterpret_cast<const WhisperRequest*>(data);

        uint32_t sender_id = session->GetUserId();
        std::string target_username = req.target_username;
        std::string message = req.message;
        uint32_t room_id = req.room_id;

        user_manager_.GetUser(sender_id, [this, session, sender_id, target_username, message, room_id](std::shared_ptr<User> sender_user) {
            if (!sender_user) return;

            server_.GetRoom(room_id, [session, sender_user, sender_id, target_username, message](std::shared_ptr<ChatRoom> room) {
                WhisperResponse res{};
                res.header.message_type = MessageType::WHISPER_RESPONSE;
                res.header.packet_size = sizeof(WhisperResponse);

                if (!room)
                {
                    res.success = false;
                    std::strncpy(res.error_message, "ROOM_NOT_FOUND", sizeof(res.error_message) - 1);
                    session->SendMessage(&res, sizeof(WhisperResponse));
                    return;
                }

                room->SendWhisper(sender_id, sender_user->GetUsername(), target_username, message, [session, res](bool success, const std::string& err) mutable {
                    res.success = success;
                    if (!success)
                    {
                        std::strncpy(res.error_message, err.c_str(), sizeof(res.error_message) - 1);
                    }
                    session->SendMessage(&res, sizeof(WhisperResponse));
                });
            });
        });
    }

private:
    UserManager& user_manager_;
    ChatServer& server_;
};



//=====================
// 서버 생성자
//=====================
ChatServer::ChatServer(boost::asio::io_context& io_context, short port)
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

    // --- 핸들러 등록 ---
    dispatcher_.RegisterHandler(MessageType::JOIN_ROOM, std::make_unique<JoinRoomHandler>(*user_manager_, *this));
    dispatcher_.RegisterHandler(MessageType::LEAVE_ROOM, std::make_unique<LeaveRoomHandler>(*this));
    dispatcher_.RegisterHandler(MessageType::WHISPER_REQUEST, std::make_unique<WhisperHandler>(*user_manager_, *this));
    
    do_accept();
}

int main()
{
    try
    {
        boost::asio::io_context io_context;
        auto server = std::make_shared<ChatServer>(io_context, 8080);
        server->CreateRoom(1, "Lobby", 100);

        std::cout << "[Server] Running on port 8080 (Strand-based)..." << std::endl;

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
