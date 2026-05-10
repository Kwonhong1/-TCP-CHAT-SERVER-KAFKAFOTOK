#pragma pack(push, 1)
struct PacketHeader
{
    uint16_t packet_id;
    uint16_t packet_size;
    uint32_t sequence_number;
};

struct LoginPacket
{
    PacketHeader header;
    char user_id[32];
    char password[64];
};

struct ChatPacket
{
    PacketHeader header;
    char message[256];
};
#pragma pack(pop)

// 채팅 메시지 타입 정의
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

using boost::asio::ip::tcp;




//=====================
//패킷 부분
class PacketBuffer
{
public:
    PacketBuffer() : read_pos_(0), write_pos_(0) {}
    
    void Clear()
    {
        read_pos_ = 0;
        write_pos_ = 0;
    }
    
    bool HasCompletePacket() const
    {
        if (GetReadableSize() < sizeof(PacketHeader))
            return false;
            
        const PacketHeader* header = reinterpret_cast<const PacketHeader*>(
            buffer_.data() + read_pos_);
        return GetReadableSize() >= header->packet_size;
    }
    
    bool ReadPacket(std::vector<char>& packet_data)
    {
        if (!HasCompletePacket())
            return false;
            
        const PacketHeader* header = reinterpret_cast<const PacketHeader*>(
            buffer_.data() + read_pos_);
        
        packet_data.resize(header->packet_size);
        std::memcpy(packet_data.data(), buffer_.data() + read_pos_, 
                   header->packet_size);
        
        read_pos_ += header->packet_size;
        
        // 버퍼 최적화
        if (read_pos_ > buffer_.size() / 2)
        {
            std::memmove(buffer_.data(), buffer_.data() + read_pos_, 
                        GetReadableSize());
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
    
    char* GetWriteBuffer() { return buffer_.data() + write_pos_; } 
    size_t GetWritableSize() const { return buffer_.size() - write_pos_; }
    size_t GetReadableSize() const { return write_pos_ - read_pos_; }
    void AdvanceWritePos(size_t size) { write_pos_ += size; }
    
private:
    std::vector<char> buffer_{8192}; // 초기 버퍼 크기
    size_t read_pos_;
    size_t write_pos_;
};

//==============================
//네트워크 부분
class ChatServer {
public:
    ChatServer(boost::asio::io_context& io_context, short port)
        : acceptor_(io_context, tcp::endpoint(tcp::v4(), port)) {
        do_accept();
    }
private:
    void do_accept() {
        acceptor_.async_accept([this](boost::system::error_code ec, tcp::socket socket) {
            if (!ec) std::make_shared<ChatSession>(std::move(socket))->start();
            do_accept();
        });
    }
    tcp::acceptor acceptor_;
};



//==========================
//세션 부분
// ChatSession 클래스의 상세 구현
class ChatSession::public std::enabled_shared_from_this<ChatSession>
{
public:
    ChatSession(tcp::socket socket, ChatServer& server)
    : socket_(std::move(socket)), server_(server), 
      user_id_(0), is_authenticated_(false), is_disconnected_(false){}

    ~ChatSession(){
        Disconnect();
    }

    Start(){
        auto self=shared_from_this();
        Do_read();
    }

    void SendMessage(const void* data, size_t size)
    {
        if (is_disconnected_.load())
            return;

        std::vector<char> message(static_cast<const char*>(data), 
                                 static_cast<const char*>(data) + size);

        {
            std::lock_guard<std::mutex> lock(write_mutex_);
            bool write_in_progress = !write_queue_.empty();
            write_queue_.push(std::move(message));

            if (!write_in_progress)
            {
                Do_write();
            }
        }
    }

private:
    
    void Do_read()
    {
        auto self = shared_from_this();
        socket_.async_read_some(
            boost::asio::buffer(read_buffer_),
            [this, self](boost::system::error_code ec, std::size_t length)
            {
                if (!ec)
                {
                    ProcessPacket(read_buffer_.data(), length);
                    DoRead();
                }
                else
                {
                    std::cout << "Read error: " << ec.message() << std::endl;
                    Disconnect();
                }
            });
    }
    void Do_write()
    {
        auto self = shared_from_this();
        
        std::lock_guard<std::mutex> lock(write_mutex_);
        if (write_queue_.empty())
            return;

        auto& front_message = write_queue_.front();
        boost::asio::async_write(
            socket_,
            boost::asio::buffer(front_message),
            [this, self](boost::system::error_code ec, std::size_t length)
            {
                if (!ec)
                {
                    std::lock_guard<std::mutex> lock(write_mutex_);
                    write_queue_.pop();
                    if (!write_queue_.empty())
                    {
                        DoWrite();
                    }
                }
                else
                {
                    std::cout << "Write error: " << ec.message() << std::endl;
                    Disconnect();
                }
            });
    }

    
    
    void Disconnect()
    {
        if (is_disconnected_.exchange(true))
            return; // 이미 연결 해제됨

        boost::system::error_code ec;
        socket_.close(ec);

        // 서버에 연결 해제 알림
        server_.OnSessionDisconnected(shared_from_this());
    }

    void ProcessPacket(const char* data, size_t size)
    {
        if (size < sizeof(PacketHeader))
            return;

        const auto& header = *reinterpret_cast<const PacketHeader*>(data);

        // 패킷 크기 검증
        if (header.packet_size != size || header.packet_size > 1024)
            return;

        // 메시지 타입별 처리
        server_.GetMessageDispatcher().DispatchMessage(
            shared_from_this(), header, data, size);
    }
        //변수 정의 부분
    tcp::socket socket_;
    ChatServer& server_;
    uint32_t user_id_;
    std::atomic<bool> is_authenticated_;
    std::atomic<bool> is_disconnected_;
    std::mutex write_mutex_;

};




//===============
//패킷 디스패처 & 핸들러 부분
// 메시지 처리를 위한 핸들러 인터페이스
class IMessageHandler
{
public:
    virtual ~IMessageHandler() = default;
    virtual void HandleMessage(std::shared_ptr<ChatSession> session, 
                              const char* data, size_t size) = 0;
};

// 메시지 디스패처 클래스
class MessageDispatcher
{
public:
    void RegisterHandler(MessageType type, std::unique_ptr<IMessageHandler> handler)
    {
        handlers_[type] = std::move(handler);
    }

    void DispatchMessage(std::shared_ptr<ChatSession> session, 
                        const PacketHeader& header, 
                        const char* data, size_t size)
    {
        auto it = handlers_.find(header.message_type);
        if (it != handlers_.end())
        {
            it->second->HandleMessage(session, data, size);
        }
        else
        {
            std::cout << "Unknown message type: " 
                     << static_cast<uint16_t>(header.message_type) << std::endl;
        }
    }

private:
    std::unordered_map<MessageType, std::unique_ptr<IMessageHandler>> handlers_;
};

// 로그인 처리 핸들러
class LoginHandler : public IMessageHandler
{
public:
    LoginHandler(ChatServer& server) : server_(server) {}

    void HandleMessage(std::shared_ptr<ChatSession> session, 
                      const char* data, size_t size) override
    {
        if (size < sizeof(LoginRequest))
            return;

        const auto& request = *reinterpret_cast<const LoginRequest*>(data);
        server_.ProcessLogin(session, request);
    }

private:
    ChatServer& server_;
};

// 채팅 메시지 처리 핸들러
class ChatMessageHandler : public IMessageHandler
{
public:
    ChatMessageHandler(ChatServer& server) : server_(server) {}

    void HandleMessage(std::shared_ptr<ChatSession> session, 
                      const char* data, size_t size) override
    {
        if (!session->IsAuthenticated())
            return;

        if (size < sizeof(ChatMessage))
            return;

        const auto& message = *reinterpret_cast<const ChatMessage*>(data);
        server_.ProcessChatMessage(session, message);
    }

private:
    ChatServer& server_;
};
