#include <boost/asio.hpp>
#include <iostream>
#include <thread>
#include <queue>
#include <vector>
#include <string>
#include <mutex>
#include <memory>
#include <cstring>

using boost::asio::ip::tcp;

// =========================================================
// 서버와 동일하게 맞춘 프로토콜 정의
// =========================================================
enum class MessageType : uint16_t
{
    LOGIN_REQUEST = 1001,
    LOGIN_RESPONSE = 1002,
    LOGOUT_REQUEST = 1003,
    LOGOUT_RESPONSE = 1004,
    CHAT_MESSAGE = 1005,
    JOIN_ROOM = 1006,
    LEAVE_ROOM = 1007,
    ROOM_LIST_REQUEST = 1008,
    ROOM_LIST_RESPONSE = 1009,
    USER_LIST_REQUEST = 1010,
    USER_LIST_RESPONSE = 1011,
    PRIVATE_MESSAGE = 1012,
    SERVER_NOTIFICATION = 1013
};

#pragma pack(push, 1)
struct PacketHeader
{
    uint16_t packet_size;      // 전체 패킷 크기
    MessageType message_type;  // 메시지 타입
    uint32_t user_id;          // 발신자 ID
    uint32_t sequence_number;  // 시퀀스 번호 (서버 헤더와 동기화)
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

// =========================================================
// 패킷 버퍼 클래스 (TCP 스트림 재조립)
// =========================================================
class PacketBuffer
{
public:
    PacketBuffer() : read_pos_(0), write_pos_(0) {}

    bool HasCompletePacket() const
    {
        if (GetReadableSize() < sizeof(PacketHeader))
            return false;
        const PacketHeader* header = reinterpret_cast<const PacketHeader*>(buffer_.data() + read_pos_);
        return GetReadableSize() >= header->packet_size;
    }

    bool ReadPacket(std::vector<char>& packet_data)
    {
        if (!HasCompletePacket())
            return false;
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
    ChatClient(boost::asio::io_context& io_context)
        : io_context_(io_context), socket_(io_context), read_buffer_(4096) {}

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
                    socket_.close();
                }
            });
    }

    void ProcessPacket(const char* data, size_t size)
    {
        if (size < sizeof(PacketHeader)) return;
        const auto& header = *reinterpret_cast<const PacketHeader*>(data);

        switch (header.message_type)
        {
        case MessageType::LOGIN_RESPONSE:
        {
            if (size < sizeof(LoginResponse)) break;
            const auto& res = *reinterpret_cast<const LoginResponse*>(data);
            if (res.success)
            {
                user_id_ = res.assigned_user_id;
                std::cout << "\n[System] Login Success! Assigned User ID: " << user_id_ << std::endl;
            }
            else
            {
                std::cout << "\n[System] Login Failed: " << res.error_message << std::endl;
            }
            break;
        }
        case MessageType::CHAT_MESSAGE:
        {
            if (size < sizeof(ChatMessage)) break;
            const auto& msg = *reinterpret_cast<const ChatMessage*>(data);
            std::cout << "\n[User " << msg.header.user_id << "]: " << msg.message << std::endl;
            break;
        }
        case MessageType::SERVER_NOTIFICATION:
        {
            std::string notice(data + sizeof(PacketHeader), size - sizeof(PacketHeader));
            std::cout << "\n[Notification] " << notice << std::endl;
            break;
        }
        default:
            std::cout << "\n[System] Unhandled packet type: " << static_cast<uint16_t>(header.message_type) << std::endl;
            break;
        }
        std::cout << "Message: " << std::flush;
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
    uint32_t user_id_{0};

    PacketBuffer packet_buffer_;
    std::vector<char> read_buffer_;
    std::queue<std::vector<char>> write_queue_;
};

// =========================================================
// main()
// =========================================================
int main()
{
    boost::asio::io_context io_context;

    auto client = std::make_shared<ChatClient>(io_context);
    client->Start("127.0.0.1", "8080");

    std::thread t([&io_context]() { io_context.run(); });

    // 1. 로그인 요청 패킷 전송
    LoginRequest req{};
    req.header.packet_size = sizeof(LoginRequest);
    req.header.message_type = MessageType::LOGIN_REQUEST;
    req.header.user_id = 0;
    req.header.sequence_number = 1;
    std::strncpy(req.username, "testuser", sizeof(req.username) - 1);
    std::strncpy(req.password, "1234", sizeof(req.password) - 1);

    client->Send(&req, sizeof(LoginRequest));

    // 2. 채팅 메시지 입력 루프
    std::string line;
    while (std::getline(std::cin, line))
    {
        if (line == "exit") break;

        ChatMessage msg{};
        msg.header.packet_size = sizeof(ChatMessage);
        msg.header.message_type = MessageType::CHAT_MESSAGE;
        msg.header.user_id = client->GetUserId(); // 로그인 후 부여받은 ID 사용
        msg.header.sequence_number = 1;
        msg.room_id = 1; // 서버의 기본 로비 방 ID
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
