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
// (참고) 프로토콜 및 버퍼 정의 예시
// =========================================================
enum class MessageType : uint16_t {
    LOGIN_REQUEST = 1,
    LOGIN_RESPONSE = 2,
    CHAT_MESSAGE = 3,
    SERVER_NOTIFICATION = 4
};

#pragma pack(push, 1)
struct PacketHeader {
    uint16_t packet_size;
    MessageType message_type;
    uint32_t user_id;
};

struct LoginRequest {
    PacketHeader header;
    char username[32];
    char password[64];
};

struct LoginResponse {
    PacketHeader header;
    bool success;
    uint32_t assigned_user_id;
    char error_message[128];
};

struct ChatMessage {
    PacketHeader header;
    uint32_t room_id;
    char message[512];
};
#pragma pack(pop)

// 사용자 정의 PacketBuffer 클래스 (실제 구현부 필요)
class PacketBuffer {
public:
    void WriteData(const char* data, size_t size) {}
    bool ReadPacket(std::vector<char>& out) { return false; }
};

// =========================================================
// ChatClient 클래스
// =========================================================
class ChatClient : public std::enabled_shared_from_this<ChatClient> {
public:
    ChatClient(boost::asio::io_context& io_context)
        : io_context_(io_context), socket_(io_context), read_buffer_(4096) {}

    // 1. 서버 접속 시작
    void Start(const std::string& host, const std::string& port) {
        tcp::resolver resolver(io_context_);
        endpoints_ = resolver.resolve(host, port);
        DoConnect();
    }

    // 2. 패킷 전송 함수
    void Send(const void* data, size_t size) {
        auto buf = std::make_shared<std::vector<char>>(
            static_cast<const char*>(data),
            static_cast<const char*>(data) + size
        );

        boost::asio::post(io_context_, [this, self = shared_from_this(), buf]() {
            bool write_in_progress = !write_queue_.empty();
            write_queue_.push(*buf);
            
            // 접속이 완료되어 있고 이전 전송이 진행 중이 아닐 때만 실행
            if (is_connected_ && !write_in_progress) {
                DoWrite();
            }
        });
    }

private:
    void DoConnect() {
        boost::asio::async_connect(socket_, endpoints_,
            [this, self = shared_from_this()](boost::system::error_code ec, tcp::endpoint) {
                if (!ec) {
                    std::cout << "Connected to server!" << std::endl;
                    is_connected_ = true;
                    DoRead(); // 수신 루프 시작

                    // 접속 완료 전 큐에 쌓여 있던 메시지가 있다면 전송 시작
                    if (!write_queue_.empty()) {
                        DoWrite();
                    }
                } else {
                    std::cout << "Connect failed: " << ec.message() << std::endl;
                }
            });
    }

    void DoRead() {
        socket_.async_read_some(boost::asio::buffer(read_buffer_),
            [this, self = shared_from_this()](boost::system::error_code ec, std::size_t length) {
                if (!ec) {
                    packet_buffer_.WriteData(read_buffer_.data(), length);
                    std::vector<char> packet_data;
                    while (packet_buffer_.ReadPacket(packet_data)) {
                        ProcessPacket(packet_data.data(), packet_data.size());
                    }
                    DoRead();
                } else {
                    std::cout << "Disconnected from server." << std::endl;
                    is_connected_ = false;
                    socket_.close();
                }
            });
    }

    void ProcessPacket(const char* data, size_t size) {
        const auto& header = *reinterpret_cast<const PacketHeader*>(data);
        switch (header.message_type) {
        case MessageType::LOGIN_RESPONSE: {
            const auto& res = *reinterpret_cast<const LoginResponse*>(data);
            if (res.success) std::cout << "\n[System] Login Success! ID: " << res.assigned_user_id << std::endl;
            else std::cout << "\n[System] Login Failed: " << res.error_message << std::endl;
            break;
        }
        case MessageType::CHAT_MESSAGE: {
            const auto& msg = *reinterpret_cast<const ChatMessage*>(data);
            std::cout << "\n[User " << msg.header.user_id << "]: " << msg.message << std::endl;
            break;
        }
        case MessageType::SERVER_NOTIFICATION: {
            std::string notice(data + sizeof(PacketHeader), size - sizeof(PacketHeader));
            std::cout << "\n[Notification] " << notice << std::endl;
            break;
        }
        }
        std::cout << "Message: " << std::flush;
    }

    void DoWrite() {
        boost::asio::async_write(socket_, boost::asio::buffer(write_queue_.front()),
            [this, self = shared_from_this()](boost::system::error_code ec, std::size_t) {
                if (!ec) {
                    write_queue_.pop();
                    if (!write_queue_.empty()) {
                        DoWrite();
                    }
                } else {
                    is_connected_ = false;
                    socket_.close();
                }
            });
    }

    boost::asio::io_context& io_context_;
    tcp::socket socket_;
    tcp::resolver::results_type endpoints_;
    bool is_connected_{false};
    
    PacketBuffer packet_buffer_;
    std::vector<char> read_buffer_; // 생성자에서 4096으로 초기화
    std::queue<std::vector<char>> write_queue_;
};

// =========================================================
// main()
// =========================================================
int main() {
    boost::asio::io_context io_context;
    
    auto client = std::make_shared<ChatClient>(io_context);
    client->Start("127.0.0.1", "8080");

    std::thread t([&io_context]() { io_context.run(); });

    // 1. 로그인 요청
    LoginRequest req{};
    req.header.packet_size = sizeof(LoginRequest);
    req.header.message_type = MessageType::LOGIN_REQUEST;
    std::strncpy(req.username, "testuser", sizeof(req.username) - 1);
    std::strncpy(req.password, "1234", sizeof(req.password) - 1);
    client->Send(&req, sizeof(LoginRequest));

    // 2. 입력 루프
    std::string line;
    while (std::getline(std::cin, line)) {
        if (line == "exit") break;
        
        ChatMessage msg{};
        msg.header.packet_size = sizeof(ChatMessage);
        msg.header.message_type = MessageType::CHAT_MESSAGE;
        msg.room_id = 1;
        std::strncpy(msg.message, line.c_str(), sizeof(msg.message) - 1);
        client->Send(&msg, sizeof(ChatMessage));
    }

    io_context.stop();
    if (t.joinable()) {
        t.join();
    }
    return 0;
}
