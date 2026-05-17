#include <boost/asio.hpp>
#include <iostream>
#include <thread>
#include <queue>
#include <vector>
#include <string>
#include <mutex>

using boost::asio::ip::tcp;

class ChatClient: public std::enabled_shared_from_this<ChatClient> {
public:
    ChatClient(boost::asio::io_context& io_context, const std::string& host, const std::string& port)
        : io_context_(io_context), socket_(io_context) {
        tcp::resolver resolver(io_context);
        auto endpoints = resolver.resolve(host, port);
        
        // 1. 서버 접속
        boost::asio::async_connect(socket_, endpoints,
            [this](boost::system::error_code ec, tcp::endpoint) {
                if (!ec) {
                    std::cout << "Connected to server!" << std::endl;
                    DoRead(); // 서버로부터 패킷 수신 시작
                } else {
                    std::cout << "Connect failed: " << ec.message() << std::endl;
                }
            });
    }

    // 2. 패킷 전송 함수
    void Send(const void* data, size_t size) {
        auto buf=std::make_shared<std::vector<char>>(static_cast<const char*>(data), static_cast<const char*>(data)+size);
        boost::asio::post(io_context_, [this, buf]() {
            bool write_in_progress = !write_queue_.empty();
            write_queue_.push(*buf);
            if (!write_in_progress) {
                DoWrite();
            }
        });
    }

private:
    void DoConnect(){
        boost::asio::async_connect(socket_, endpoints_, [this](boost::system::error_code ec, tcp::endpoint){
            if(!ec){
                std::cout<<"Connected to server!"<<'\n';
                DoRead()
            }
            else{
                std::cout<<"Connect failed: "<<ec.message()<<'\n';
            }
        });
    }
    void DoRead() {
        socket_.async_read_some(boost::asio::buffer(read_buffer_),
            [this](boost::system::error_code ec, std::size_t length) {
                if (!ec) {
                    packet_buffer_.WriteData(read_buffer_.data(), length);
                    std::vector<char> packet_data;
                    while(packet_buffer_.ReadPacket(packet_data)){
                        ProcessPacket(packet_data.data(), packet_data.size());
                    }
                    DoRead();
                } else {
                    std::cout << "Disconnected from server." << std::endl;
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
        std::cout << "Message: " << std::flush; // 입력 프롬프트 유지
    }

    void DoWrite() {
        boost::asio::async_write(socket_, boost::asio::buffer(write_queue_.front()),
            [this](boost::system::error_code ec, std::size_t) {
                if (!ec) {
                    write_queue_.pop();
                    if (!write_queue_.empty()) DoWrite();
                }
            });
    }

    boost::asio::io_context& io_context_;
    tcp::socket socket_;
    PacketBuffer packet_buffer_;
    std::vector<char> read_buffer_{4096};
    std::queue<std::vector<char>> write_queue_;
};

// ---------------------------------------------------------

int main() {
    boost::asio::io_context io_context;
    auto client = std::make_shared<ChatClient>(io_context, "127.0.0.1", "8080");
    client->Start();

    std::thread t([&io_context]() { io_context.run(); });

    // 1. 로그인 (임시)
    LoginRequest req;
    req.header.packet_size = sizeof(LoginRequest);
    req.header.message_type = MessageType::LOGIN_REQUEST;
    std::strncpy(req.username, "testuser", 31);
    std::strncpy(req.password, "1234", 63);
    client->Send(&req, sizeof(LoginRequest));

    // 2. 입력 루프
    std::string line;
    while (std::getline(std::cin, line)) {
        if (line == "exit") break;
        
        ChatMessage msg;
        msg.header.packet_size = sizeof(ChatMessage);
        msg.header.message_type = MessageType::CHAT_MESSAGE;
        msg.room_id = 1;
        std::strncpy(msg.message, line.c_str(), 511);
        client->Send(&msg, sizeof(ChatMessage));
    }

    io_context.stop();
    t.join();
    return 0;
}
