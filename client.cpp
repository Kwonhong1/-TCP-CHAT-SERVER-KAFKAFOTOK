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

using boost::asio::ip::tcp;

// =========================================================
// 프로토콜 정의 (LOGIN_PROMPT = 1000 추가)
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
    void SetLoggedIn(bool status) { is_logged_in_ = status; }
    bool IsLoggedIn() const { return is_logged_in_; }

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
                    is_logged_in_ = false;
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
    bool is_connected_{false};
    std::atomic<bool> is_logged_in_{false};
    uint32_t user_id_{0};

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
        std::cout << "[Server] Please log in to proceed." << std::endl;
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
            client->SetLoggedIn(true);
            std::cout << "\n[System] Login Success! Assigned User ID: " << res.assigned_user_id << std::endl;
            std::cout << "Message: " << std::flush;
        }
        else
        {
            std::cout << "\n[System] Login Failed: " << res.error_message << std::endl;
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
    dispatcher_.RegisterHandler(MessageType::CHAT_MESSAGE, std::make_unique<ChatMessageHandler>());
    dispatcher_.RegisterHandler(MessageType::SERVER_NOTIFICATION, std::make_unique<ServerNotificationHandler>());
}

// =========================================================
// main() - 동기화 제어 추가
// =========================================================
int main()
{
    boost::asio::io_context io_context;

    auto client = std::make_shared<ChatClient>(io_context);
    client->Start("127.0.0.1", "8080");

    std::thread t([&io_context]() { io_context.run(); });

    // 1. 로그인 요청 전송
    LoginRequest req{};
    req.header.packet_size = sizeof(LoginRequest);
    req.header.message_type = MessageType::LOGIN_REQUEST;
    req.header.user_id = 0;
    req.header.sequence_number = 1;
    std::strncpy(req.username, "testuser", sizeof(req.username) - 1);
    std::strncpy(req.password, "1234", sizeof(req.password) - 1);

    client->Send(&req, sizeof(LoginRequest));

    // 2. 로그인 완료까지 대기 (간단한 동기화)
    std::cout << "[System] Waiting for login response..." << std::endl;
    while (!client->IsLoggedIn())
    {
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }

    // 3. 로그인 성공 후에만 채팅 입력 루프 실행
    std::string line;
    while (std::getline(std::cin, line))
    {
        if (line == "exit") break;

        ChatMessage msg{};
        msg.header.packet_size = sizeof(ChatMessage);
        msg.header.message_type = MessageType::CHAT_MESSAGE;
        msg.header.user_id = client->GetUserId();
        msg.header.sequence_number = 1;
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
