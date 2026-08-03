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

using boost::asio::ip::tcp;

// =========================================================
// 프로토콜 정의
// =========================================================
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

enum class AuthStatus
{
    NONE,
    WAITING,
    SUCCESS,
    FAILED
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

    bool IsConnected() const { return is_connected_; }
    bool IsConnectFailed() const { return connect_failed_; }

private:
    void DoConnect()
    {
        boost::asio::async_connect(socket_, endpoints_,
            [this, self = shared_from_this()](boost::system::error_code ec, tcp::endpoint) {
                if (!ec)
                {
                    std::cout << "[System] Connected to server!" << std::endl;
                    is_connected_ = true;
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
                    auth_status_ = AuthStatus::FAILED;
                    socket_.close();
                }
            });
    }

    void ProcessPacket(const char* data, size_t size)
    {
        if (size < sizeof(PacketHeader)) return;
        const auto& header = *reinterpret_cast<const PacketHeader*>(data);
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
                    socket_.close();
                }
            });
    }

    boost::asio::io_context& io_context_;
    tcp::socket socket_;
    tcp::resolver::results_type endpoints_;
    std::atomic<bool> is_connected_{false};
    std::atomic<bool> connect_failed_{false};
    std::atomic<AuthStatus> auth_status_{AuthStatus::NONE};
    std::atomic<uint32_t> user_id_{0};

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
        std::cout << "\n[User " << msg.header.user_id << "]: " << msg.message << std::endl;
        std::cout << "Message: " << std::flush;
    }
};

class ServerNotificationHandler : public IMessageHandler
{
public:
    void HandleMessage(std::shared_ptr<ChatClient> client, const char* data, size_t size) override
    {
        std::string notice(data + sizeof(PacketHeader), size - sizeof(PacketHeader));
        std::cout << "\n[Notification] " << notice << std::endl;
        std::cout << "Message: " << std::flush;
    }
};

// =========================================================
// ChatClient 생성자 (핸들러 바인딩)
// =========================================================
ChatClient::ChatClient(boost::asio::io_context& io_context)
    : io_context_(io_context), socket_(io_context), read_buffer_(4096)
{
    dispatcher_.RegisterHandler(MessageType::LOGIN_PROMPT, std::make_unique<LoginPromptHandler>());
    dispatcher_.RegisterHandler(MessageType::LOGIN_RESPONSE, std::make_unique<LoginResponseHandler>());
    dispatcher_.RegisterHandler(MessageType::REGISTER_RESPONSE, std::make_unique<RegisterResponseHandler>());
    dispatcher_.RegisterHandler(MessageType::CHAT_MESSAGE, std::make_unique<ChatMessageHandler>());
    dispatcher_.RegisterHandler(MessageType::SERVER_NOTIFICATION, std::make_unique<ServerNotificationHandler>());
}

// =========================================================
// main() - CLI 메뉴 및 동기화 제어
// =========================================================
int main()
{
    boost::asio::io_context io_context;

    auto client = std::make_shared<ChatClient>(io_context);
    
    std::cout << "[System] Connecting to server..." << std::endl;
    client->Start("127.0.0.1", "8080");

    std::thread t([&io_context]() { io_context.run(); });

    // 1. 소켓 연결이 완료될 때까지 대기
    while (!client->IsConnected() && !client->IsConnectFailed())
    {
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }

    if (client->IsConnectFailed())
    {
        std::cout << "[System] Exiting due to connection failure." << std::endl;
        io_context.stop();
        if (t.joinable()) t.join();
        return 0;
    }

    // 2. 연결 완료 후 LOGIN_PROMPT 수신 시간을 위한 짧은 유예 시간
    std::this_thread::sleep_for(std::chrono::milliseconds(100));

    // 3. 로그인 완료될 때까지 메뉴 루프 실행
    while (client->GetAuthStatus() != AuthStatus::SUCCESS)
    {
        std::cout << "\n=========================" << std::endl;
        std::cout << " 1. Register (회원가입)" << std::endl;
        std::cout << " 2. Login (로그인)" << std::endl;
        std::cout << " 3. Exit (종료)" << std::endl;
        std::cout << "Select Menu: ";

        int choice = 0;
        if (!(std::cin >> choice)) break;

        if (choice == 3)
        {
            io_context.stop();
            if (t.joinable()) t.join();
            return 0;
        }

        std::string username, password;
        std::cout << "Username: ";
        std::cin >> username;
        std::cout << "Password: ";
        std::cin >> password;

        if (choice == 1) // 회원가입
        {
            RegisterRequest req{};
            req.header.packet_size = sizeof(RegisterRequest);
            req.header.message_type = MessageType::REGISTER_REQUEST;
            std::strncpy(req.username, username.c_str(), sizeof(req.username) - 1);
            std::strncpy(req.password, password.c_str(), sizeof(req.password) - 1);

            client->SetAuthStatus(AuthStatus::WAITING);
            client->Send(&req, sizeof(RegisterRequest));
        }
        else if (choice == 2) // 로그인
        {
            LoginRequest req{};
            req.header.packet_size = sizeof(LoginRequest);
            req.header.message_type = MessageType::LOGIN_REQUEST;
            std::strncpy(req.username, username.c_str(), sizeof(req.username) - 1);
            std::strncpy(req.password, password.c_str(), sizeof(req.password) - 1);

            client->SetAuthStatus(AuthStatus::WAITING);
            client->Send(&req, sizeof(LoginRequest));
        }

        // 서버 응답 패킷 올 때까지 대기
        while (client->GetAuthStatus() == AuthStatus::WAITING)
        {
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
        }
    }

    // 4. 로그인 성공 후 채팅 입력 루프 시작
    std::cout << "\n--- Entered Chat Room (Lobby) ---" << std::endl;
    std::string line;
    std::cin.ignore(); // 입력 버퍼 잔여 줄바꿈 제거

    while (std::cout << "Message: " && std::getline(std::cin, line))
    {
        if (line == "exit") break;

        ChatMessage msg{};
        msg.header.packet_size = sizeof(ChatMessage);
        msg.header.message_type = MessageType::CHAT_MESSAGE;
        msg.header.user_id = client->GetUserId();
        msg.room_id = 1;
        std::strncpy(msg.message, line.c_str(), sizeof(msg.message) - 1);

        client->Send(&msg, sizeof(ChatMessage));
    }

    io_context.stop();
    if (t.joinable())
    {
        t.join();
    }
    return 0;
}
