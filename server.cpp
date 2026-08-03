#include <boost/asio.hpp>
#include <boost/bind/bind.hpp>
#include <memory>
#include <iostream>
#include <string>
#include <vector>
#include <unordered_map>
#include <mutex>
#include <atomic>
#include <queue>
#include <cstring>

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
    REGISTER_REQUEST = 1014,
    REGISTER_RESPONSE = 1015,
    SERVER_NOTIFICATION = 1013
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

class UserManager
{
public:
    UserManager() : next_user_id_(1) {}

    std::shared_ptr<User> CreateUser(const std::string& username, uint64_t password)
    {
        std::lock_guard<std::mutex> lock(users_mutex_);
        for (const auto& [id, user] : users_) {
            if (user->GetUsername() == username) return nullptr;
        }
        uint32_t user_id = next_user_id_++;
        auto user = std::make_shared<User>(user_id, username);
        user->SetPassword(password);
        users_[user_id] = user;
        std::cout << "[UserManager] New user created: " << username << " (ID: " << user_id << ")" << std::endl;
        return user;
    }

    std::shared_ptr<User> GetUserByUsername(const std::string& username)
    {
        std::lock_guard<std::mutex> lock(users_mutex_);
        for (const auto& [id, user] : users_) {
            if (user->GetUsername() == username) return user;
        }
        return nullptr;
    }

    std::shared_ptr<User> GetUser(uint32_t user_id)
    {
        std::lock_guard<std::mutex> lock(users_mutex_);
        auto it = users_.find(user_id);
        return (it != users_.end()) ? it->second : nullptr;
    }

private:
    std::unordered_map<uint32_t, std::shared_ptr<User>> users_;
    mutable std::mutex users_mutex_;
    std::atomic<uint32_t> next_user_id_;
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
        : socket_(std::move(socket)), server_(server), user_id_(0), is_authenticated_(false), is_disconnected_(false) {}

    ~ChatSession()
    {
        boost::system::error_code ec;
        socket_.close(ec);
    }

    void Start();

    void SendMessage(const void* data, size_t size)
    {
        if (is_disconnected_.load()) return;

        std::vector<char> message(static_cast<const char*>(data), static_cast<const char*>(data) + size);
        bool write_in_progress = false;

        {
            std::lock_guard<std::mutex> lock(write_mutex_);
            write_in_progress = !write_queue_.empty();
            write_queue_.push(std::move(message));
        }

        if (!write_in_progress)
        {
            Do_write();
        }
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
            [this, self](boost::system::error_code ec, std::size_t length) {
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
            });
    }

    void Do_write()
    {
        auto self = shared_from_this();
        std::vector<char> message;

        {
            std::lock_guard<std::mutex> lock(write_mutex_);
            if (write_queue_.empty()) return;
            message = write_queue_.front();
        }

        boost::asio::async_write(socket_, boost::asio::buffer(message.data(), message.size()),
            [this, self](boost::system::error_code ec, std::size_t) {
                if (!ec)
                {
                    bool has_more = false;
                    {
                        std::lock_guard<std::mutex> lock(write_mutex_);
                        write_queue_.pop();
                        has_more = !write_queue_.empty();
                    }
                    if (has_more) Do_write();
                }
                else
                {
                    Disconnect();
                }
            });
    }

    void ProcessPacket(const char* data, size_t size);

    tcp::socket socket_;
    ChatServer& server_;
    uint32_t user_id_;
    std::atomic<bool> is_authenticated_;
    std::atomic<bool> is_disconnected_;
    std::mutex write_mutex_;

    std::queue<std::vector<char>> write_queue_;
    std::vector<char> read_buffer_ = std::vector<char>(4096);
    PacketBuffer packet_buffer_;
};

//=====================
// 채팅방
//=====================
class ChatRoom
{
public:
    ChatRoom(std::string name, uint32_t max_users) : name_(name), max_users_(max_users) {}

    bool AddUser(std::shared_ptr<User> user)
    {
        {
            std::lock_guard<std::mutex> lock(users_mutex_);
            if (users_.size() >= max_users_ || users_.find(user->GetId()) != users_.end()) return false;
            users_[user->GetId()] = user;
        }
        BroadcastNotification(user->GetUsername() + " joined the room.", user->GetId());
        return true;
    }

    bool RemoveUser(uint32_t user_id)
    {
        std::lock_guard<std::mutex> lock(users_mutex_);
        auto it = users_.find(user_id);
        if (it == users_.end()) return false;
        std::string username = it->second->GetUsername();
        users_.erase(it);
        BroadcastNotification(username + " left the room.", user_id);
        return true;
    }

    void BroadcastMessage(const ChatMessage& message, uint32_t sender_id)
    {
        std::lock_guard<std::mutex> lock(users_mutex_);
        for (const auto& [id, user] : users_)
        {
            if (id == sender_id) continue;
            auto session = user->GetSession().lock();
            if (session) session->SendMessage(&message, sizeof(ChatMessage));
        }
    }

    void BroadcastNotification(const std::string& notification, uint32_t exclude_user_id)
    {
        PacketHeader header{};
        header.packet_size = sizeof(PacketHeader) + static_cast<uint16_t>(notification.size());
        header.message_type = MessageType::SERVER_NOTIFICATION;

        std::vector<char> packet(header.packet_size);
        std::memcpy(packet.data(), &header, sizeof(PacketHeader));
        std::memcpy(packet.data() + sizeof(PacketHeader), notification.c_str(), notification.size());

        std::lock_guard<std::mutex> lock(users_mutex_);
        for (const auto& [id, user] : users_)
        {
            if (id == exclude_user_id) continue;
            auto session = user->GetSession().lock();
            if (session) session->SendMessage(packet.data(), packet.size());
        }
    }

private:
    std::string name_;
    uint32_t max_users_;
    std::unordered_map<uint32_t, std::shared_ptr<User>> users_;
    mutable std::mutex users_mutex_;
};

//=====================
// 서버 클래스
//=====================
class ChatServer
{
public:
    ChatServer(boost::asio::io_context& io_context, short port);

    void OnSessionDisconnected(std::shared_ptr<ChatSession> session)
    {
        uint32_t user_id = session->GetUserId();
        auto user = user_manager_.GetUser(user_id);
        if (user)
        {
            user->SetOnline(false);
            std::lock_guard<std::mutex> lock(rooms_mutex_);
            for (auto& [id, room] : rooms_) room->RemoveUser(user_id);
            std::cout << "[System] User disconnected: " << user->GetUsername() << std::endl;
        }
    }

    MessageDispatcher& GetDispatcher() { return dispatcher_; }
    UserManager& GetUserManager() { return user_manager_; }

    void CreateRoom(uint32_t room_id, const std::string& name, uint32_t max_users)
    {
        std::lock_guard<std::mutex> lock(rooms_mutex_);
        rooms_[room_id] = std::make_shared<ChatRoom>(name, max_users);
    }

    std::shared_ptr<ChatRoom> GetRoom(uint32_t room_id)
    {
        std::lock_guard<std::mutex> lock(rooms_mutex_);
        auto it = rooms_.find(room_id);
        return (it != rooms_.end()) ? it->second : nullptr;
    }

private:
    void do_accept()
    {
        acceptor_.async_accept([this](boost::system::error_code ec, tcp::socket socket) {
            if (!ec) std::make_shared<ChatSession>(std::move(socket), *this)->Start();
            do_accept();
        });
    }

    tcp::acceptor acceptor_;
    MessageDispatcher dispatcher_;
    UserManager user_manager_;
    std::unordered_map<uint32_t, std::shared_ptr<ChatRoom>> rooms_;
    std::mutex rooms_mutex_;
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
    if (is_disconnected_.exchange(true)) return;
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
class LoginHandler : public IMessageHandler
{
public:
    LoginHandler(UserManager& user_manager, ChatServer& server)
        : user_manager_(user_manager), server_(server) {}

    void HandleMessage(std::shared_ptr<ChatSession> session, const char* data, size_t size) override
    {
        if (size < sizeof(LoginRequest)) return;
        const auto& request = *reinterpret_cast<const LoginRequest*>(data);

        LoginResponse response{};
        response.header.message_type = MessageType::LOGIN_RESPONSE;
        response.header.packet_size = sizeof(LoginResponse);

        std::string username = request.username;
        uint64_t pass = 0;
        try { pass = std::stoull(request.password); } catch (...) { pass = 0; }

        auto existing_user = user_manager_.GetUserByUsername(username);

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

            auto lobby = server_.GetRoom(1);
            if (lobby) lobby->AddUser(existing_user);
        }
        else
        {
            response.success = false;
            std::strncpy(response.error_message, "WRONG_PASSWORD", sizeof(response.error_message) - 1);
            std::cout << "[Login Fail] Incorrect password for: " << username << std::endl;
        }

        session->SendMessage(&response, sizeof(LoginResponse));
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

        RegisterResponse response{};
        response.header.message_type = MessageType::REGISTER_RESPONSE;
        response.header.packet_size = sizeof(RegisterResponse);

        std::string username = request.username;
        uint64_t pass = 0;
        try { pass = std::stoull(request.password); } catch (...) { pass = 0; }

        auto new_user = user_manager_.CreateUser(username, pass);
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
    ChatMessageHandler(ChatServer& server) : server_(server) {}

    void HandleMessage(std::shared_ptr<ChatSession> session, const char* data, size_t size) override
    {
        if (!session->IsAuthenticated() || size < sizeof(ChatMessage)) return;
        const auto& message = *reinterpret_cast<const ChatMessage*>(data);

        auto room = server_.GetRoom(message.room_id);
        if (room)
        {
            room->BroadcastMessage(message, session->GetUserId());
        }
    }

private:
    ChatServer& server_;
};

//=====================
// 서버 생성자 (핸들러 바인딩)
//=====================
ChatServer::ChatServer(boost::asio::io_context& io_context, short port)
    : acceptor_(io_context, tcp::endpoint(tcp::v4(), port))
{
    dispatcher_.RegisterHandler(MessageType::LOGIN_REQUEST, std::make_unique<LoginHandler>(user_manager_, *this));
    dispatcher_.RegisterHandler(MessageType::REGISTER_REQUEST, std::make_unique<RegisterHandler>(user_manager_));
    dispatcher_.RegisterHandler(MessageType::CHAT_MESSAGE, std::make_unique<ChatMessageHandler>(*this));
    do_accept();
}

int main()
{
    try
    {
        boost::asio::io_context io_context;
        ChatServer server(io_context, 8080);
        server.CreateRoom(1, "Lobby", 100);

        std::cout << "[Server] Running on port 8080..." << std::endl;
        io_context.run();
    }
    catch (std::exception& e)
    {
        std::cerr << "Exception: " << e.what() << std::endl;
    }
    return 0;
}
