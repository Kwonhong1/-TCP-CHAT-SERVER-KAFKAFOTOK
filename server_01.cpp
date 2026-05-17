#include <boost/asio.hpp>
#include <boost/bind/bind.hpp>
#include <memory>
#include <iostream>
#include <string>
#include <vector>
#include <unordered_map>
#include <unordered_set>
#include <mutex>
#include <atomic>
#include <queue>

using boost::asio::ip::tcp;

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

// 패킷 헤더 구조체
#pragma pack(push, 1)
struct PacketHeader
{
    uint16_t packet_size;     // 전체 패킷 크기
    MessageType message_type; // 메시지 타입
    uint32_t user_id;         // 발신자 ID
    uint32_t sequence_number; // 시퀀스 번호
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

struct JoinRoomRequest
{
    PacketHeader header;
    uint32_t room_id;
    char room_name[64];
};
#pragma pack(pop)


#include <boost/asio.hpp>
#include <boost/bind/bind.hpp>
#include <memory>
#include <iostream>
#include <string>
#include <vector>
#include <unordered_map>
#include <unordered_set>
#include <mutex>
#include <atomic>
#include <queue>

using boost::asio::ip::tcp;

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

// 패킷 헤더 구조체
#pragma pack(push, 1)
struct PacketHeader
{
    uint16_t packet_size;     // 전체 패킷 크기
    MessageType message_type; // 메시지 타입
    uint32_t user_id;         // 발신자 ID
    uint32_t sequence_number; // 시퀀스 번호
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

struct JoinRoomRequest
{
    PacketHeader header;
    uint32_t room_id;
    char room_name[64];
};
#pragma pack(pop)
11.1.2 기본 클래스 구조 설계
// 전방 선언
class ChatServer;
class ChatRoom;
class ChatSession;

// 사용자 정보 클래스











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






//==============================
//네트워크 부분
class ChatServer {
public:
    ChatServer(boost::asio::io_context& io_context, short port)
        : acceptor_(io_context, tcp::endpoint(tcp::v4(), port)) {
        dispatcher_.RegisterHandler(MessageType::LOGIN_REQUEST, std::make_unique<LoginHandler>(*this));
        dispatcher_.RegisterHandler(MessageType::CHAT_MESSAGE, std::make_unique<ChatMessageHandler>(*this));
        do_accept();

    }
    

    void OnSessionDisconnected(std::shared_ptr<ChatSession> session){
    usint32_t user_id= session->GetUserId();
    auto user=user_manager.GetUser(user_id);
    if(user){
        user->Setonline(false);

        std::lock_guard<std::mutex> lock(rooms_mutex_);
        for(auto&[id, room]: rooms_){
            room->RemoveUser(user_id);
            }
        std::cout<<"[System] Session disconnected."<<'\n';    
        }
    
    }  
    void OnSessionDisconnected(std::shared_ptr<ChatSession> session){
    usint32_t user_id= session->GetUserId();
    auto user=user_manager.GetUser(user_id);
    if(user){
        user->Setonline(false);

        std::lock_guard<std::mutex> lock(rooms_mutex_);
        for(auto&[id, room]: rooms_){
                room->RemoveUser(user_id);
            }
        std::cout<<"[System] Session disconnected."<<'\n';    
        }
        
    }


    MessageDispatcher& GetDispatcher(){return dispatcher_;}
    UserManager& GetUserManager(){return user_manager_;}
private:
    //dispatch management for memory and mainstream

    MessageDispatcher dispatcher_;
    UserManager user_manager_;



    void do_accept() {
        acceptor_.async_accept([this](boost::system::error_code ec, tcp::socket socket) {
            if (!ec) std::make_shared<ChatSession>(std::move(socket), *this)->Start();
            do_accept();
        });
    }


    void ProcessLogin(shared_ptr<ChatSession> session, LoginRequest& request){
        auto user = user_manager_.GetAuthority(request.username, std::stoull(request.password));

        LoginResponse response;
        response.header.message_type=MessageType::LOGIN_RESPONSE;
        response.header.packet_size=sizeof(LoginResponse);
        if(user){
            user->SetSession(session);
            session->SetUserId(user->GetId());
            session->SetAuthenticated(true);

            response.success = true;
            response.assigned_user_id = user->GetId();
            std::cout << "[Login] User " << request.username << " authenticated." << std::endl;
        
            // 성공 시 로비(기본 방)에 자동 입장시키기 (옵션)
            auto lobby = GetRoom(1); // ID 1번이 로비라고 가정
            if (lobby) lobby->AddUser(user);
        } 
        else {
        // 로그인 실패
        response.success = false;
        response.assigned_user_id = 0;
        std::strcpy(response.error_message, "Invalid username or password.");
        }

        session->SendMessage(&response, sizeof(LoginResponse));

        

    }
    void CreateRoom(uint32_t room_id, const std::string& name, uint32_t max_users){
        std::lock_guard<std::mutex> lock(rooms_mutex_);
        rooms_[room_id]=std::make_shared<ChatRoom>(name, max_users);
        std::cout<<"[System] Room Created: "  << name<< " (ID: "<<room_id<<")"<<'\n';
        }

    void ProcessChatMessage(shared_ptr<ChatSession> session, const ChatMessage& message){
        if(!session->IsAuthenticated()) return;
        
        auto room=GetRoom(message.room_id);
        if(room){
            room->BroadcastMessage(message, session->GetUserId());
        }


    }    
    std::shared_ptr<ChatSession> session_;
    tcp::acceptor acceptor_;

    std::shared_ptr<ChatRoom> GetRoom(uint32_t room_id){
        std::lock_guard<std::mutex> lock(rooms_mutex_);
        auto it=rooms.find(room_id);
        return (it!=rooms.end()) ? it->second: nullptr;

    }




    std::unordered_map<uint32_t, std::shared_ptr<ChatRoom>> rooms_;
    std::mutex rooms_mutex_;
};













    //==========================
    //세션 부분
    // ChatSession 클래스의 상세 구현
class ChatSession: public std::enabled_shared_from_this<ChatSession>
{
public:
    ChatSession(tcp::socket socket, ChatServer& server)
    : socket_(std::move(socket)), server_(server), 
    user_id_(0), is_authenticated_(false), is_disconnected_(false){}

    ~ChatSession(){
        Disconnect();
    }

    void Start(){
        auto self=shared_from_this();
        Do_read();
    }

    void SendMessage(const void* data, size_t size)
    {
        if (is_disconnected_.load())
            return;

        std::vector<char> message(static_cast<const char*>(data), static_cast<const char*>(data) + size);

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

    void SetUserId(uint32_t id){ user_id_=id;   }
    uint32_t GetUserId() const {return user_id_;}

    void SetAuthenticated(bool auth){is_authenticated_=auth;}
    bool IsAuthenticated() const {return is_authenticated_;}
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
                    packet_buffer_.WriteData(read_buffer_.data(), length());
                    std::vector<char> packet_data;
                    while(packet_buffer_.ReadPacket(packet_data)){
                        ProcessPacket(packet_data.data(), packet_data.size());
                    }
                    Do_read();
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
            boost::asio::buffer(front_message.data(), front_message.size()),
            [this, self](boost::system::error_code ec, std::size_t length)
            {
                if (!ec)
                {
                    std::lock_guard<std::mutex> lock(write_mutex_);
                    write_queue_.pop();
                    if (!write_queue_.empty())
                    {
                        Do_write();
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

        server_.GetDisaptcher().DispatchMessage(shared_from_this(), header, data, size);
    }
        //변수 정의 부분
    tcp::socket socket_;
    ChatServer& server_;
    uint32_t user_id_;
    std::atomic<bool> is_authenticated_;
    std::atomic<bool> is_disconnected_;
    std::mutex write_mutex_;

    //버퍼
    std::queue<std::vector<char>> write_queue_;
    std::vector<char> read_buffer_(4096);
    PacketBuffer packet_buffer_;
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









//=====================================
//유저 부분

class User
{
public:
    User(uint32_t id, const std::string& username)
        : id_(id), username_(username), is_online_(false)
    {
    }
    void SetPassword(uint64_t password) {password_=password;}
    uint32_t GetId() const { return id_; }
    uint64_t GetPassword() const {return password_;}
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

    // 사용자 생성
    std::shared_ptr<User> CreateUser(const std::string& username);

    // 사용자 인증 및 온라인 상태 전환
    std::shared_ptr<User> GetAuthority(const std::string& username, uint64_t password);

    // ID로 사용자 조회
    std::shared_ptr<User> GetUser(uint32_t user_id);

    // 이름으로 사용자 조회
    std::shared_ptr<User> GetUserByName(const std::string& username);

    // 사용자 제거
    bool RemoveUser(uint32_t user_id);

    // 현재 온라인인 사용자 목록 반환
    std::vector<std::shared_ptr<User>> GetOnlineUsers();

    // 전체 사용자 수 반환
    size_t GetUserCount() const;

private:
    std::unordered_map<uint32_t, std::shared_ptr<User>> users_;
    mutable std::mutex users_mutex_;
    std::atomic<uint32_t> next_user_id_;
};



std::shared_ptr<User> UserManager::CreateUser(const std::string& username) {
    std::lock_guard<std::mutex> lock(users_mutex_);

    // 중복 사용자명 확인
    for (const auto& [id, user] : users_) {
        if (user->GetUsername() == username) {
            return nullptr; // 이미 존재하는 사용자명
        }
    }

    uint32_t user_id = next_user_id_++;
    auto user = std::make_shared<User>(user_id, username);
    users_[user_id] = user;

    std::cout << "User created: " << username << " (ID: " << user_id << ")" << std::endl;
    return user;
}

std::shared_ptr<User> UserManager::GetAuthority(const std::string& username, uint64_t password) {
    std::lock_guard<std::mutex> lock(users_mutex_);

    for (const auto& [id, user] : users_) {
        if (user->GetUsername() == username) {
            if (user->GetPassword() == password) {
                user->SetOnline(true);
                return user;
            }
            break; // 이름은 맞지만 비번이 틀린 경우 루프 종료
        }
    }
    return nullptr;
}

std::shared_ptr<User> UserManager::GetUser(uint32_t user_id) {
    std::lock_guard<std::mutex> lock(users_mutex_);
    auto it = users_.find(user_id);
    return (it != users_.end()) ? it->second : nullptr;
}

std::shared_ptr<User> UserManager::GetUserByName(const std::string& username) {
    std::lock_guard<std::mutex> lock(users_mutex_);
    for (const auto& [id, user] : users_) {
        if (user->GetUsername() == username)
            return user;
    }
    return nullptr;
}

bool UserManager::RemoveUser(uint32_t user_id) {
    std::lock_guard<std::mutex> lock(users_mutex_);
    auto it = users_.find(user_id);
    if (it != users_.end()) {
        std::cout << "User removed: " << it->second->GetUsername() 
                  << " (ID: " << user_id << ")" << std::endl;
        users_.erase(it);
        return true;
    }
    return false;
}

std::vector<std::shared_ptr<User>> UserManager::GetOnlineUsers() {
    std::lock_guard<std::mutex> lock(users_mutex_);
    std::vector<std::shared_ptr<User>> online_users;
    for (const auto& [id, user] : users_) {
        if (user->IsOnline()) {
            online_users.push_back(user);
        }
    }
    return online_users;
}

size_t UserManager::GetUserCount() const {
    std::lock_guard<std::mutex> lock(users_mutex_);
    return users_.size();
}









    //===================
//채팅방 부분
// ChatRoom 클래스의 구현
class ChatRoom {
public:
    ChatRoom(std::string name, uint32_t max_users) 
        : name_(name), max_users_(max_users) {}

    // 주요 멤버 함수 선언
    bool AddUser(std::shared_ptr<User> user);
    bool RemoveUser(uint32_t user_id);
    void BroadcastMessage(const ChatMessage& message, uint32_t sender_id);
    void BroadcastNotification(const std::string& notification, uint32_t exclude_user_id);
    std::vector<std::shared_ptr<User>> GetUserList() const;

private:
    std::string name_;
    uint32_t max_users_;
    std::unordered_map<uint32_t, std::shared_ptr<User>> users_;
    mutable std::mutex users_mutex_;
};

bool ChatRoom::AddUser(std::shared_ptr<User> user)
{
    std::lock_guard<std::mutex> lock(users_mutex_);
    
    if (users_.size() >= max_users_)
        return false;
    
    if (users_.find(user->GetId()) != users_.end())
        return false; // 이미 방에 있음
    
    users_[user->GetId()] = user;
    
    // 입장 알림 메시지 브로드캐스트
    std::string notification = user->GetUsername() + " joined the room.";
    BroadcastNotification(notification, user->GetId());
    
    std::cout << "User " << user->GetUsername() 
             << " joined room " << name_ 
             << " (Users: " << users_.size() << ")" << std::endl;
    
    return true;
}

bool ChatRoom::RemoveUser(uint32_t user_id)
{
    std::lock_guard<std::mutex> lock(users_mutex_);
    
    auto it = users_.find(user_id);
    if (it == users_.end())
        return false;
    
    std::string username = it->second->GetUsername();
    users_.erase(it);
    
    // 퇴장 알림 메시지 브로드캐스트
    std::string notification = username + " left the room.";
    BroadcastNotification(notification, user_id);
    
    std::cout << "User " << username 
             << " left room " << name_ 
             << " (Users: " << users_.size() << ")" << std::endl;
    
    return true;
}

void ChatRoom::BroadcastMessage(const ChatMessage& message, uint32_t sender_id)
{
    std::lock_guard<std::mutex> lock(users_mutex_);
    
    for (const auto& pair : users_)
    {
        // 발신자에게는 메시지를 보내지 않음 (선택사항)
        if (pair.first == sender_id)
            continue;
            
        auto user = pair.second;
        auto session = user->GetSession().lock();
        if (session)
        {
            session->SendMessage(&message, sizeof(ChatMessage));
        }
    }
}

void ChatRoom::BroadcastNotification(const std::string& notification, uint32_t exclude_user_id)
{
    PacketHeader header;
    header.packet_size = sizeof(PacketHeader) + static_cast<uint16_t>(notification.size());
    header.message_type = MessageType::SERVER_NOTIFICATION;
    header.user_id = 0; // 서버 메시지
    header.sequence_number = 0;

    std::vector<char> packet(header.packet_size);
    std::memcpy(packet.data(), &header, sizeof(PacketHeader));
    std::memcpy(packet.data() + sizeof(PacketHeader), 
               notification.c_str(), notification.size());

    std::lock_guard<std::mutex> lock(users_mutex_);
    for (const auto& pair : users_)
    {
        if (pair.first == exclude_user_id)
            continue;
            
        auto user = pair.second;
        auto session = user->GetSession().lock();
        if (session)
        {
            session->SendMessage(packet.data(), packet.size());
        }
    }
}

std::vector<std::shared_ptr<User>> ChatRoom::GetUserList() const
{
    std::lock_guard<std::mutex> lock(users_mutex_);
    std::vector<std::shared_ptr<User>> user_list;
    
    for (const auto& pair : users_)
    {
        user_list.push_back(pair.second);
    }
    
    return user_list;
}




int main() {
    try {
        boost::asio::io_context io_context;
        ChatServer server(io_context, 8080);

        // 1. 테스트 유저 생성
        auto test_user = server.GetUserManager().CreateUser("testuser");
        if (test_user) test_user->SetPassword(1234);

        // 2. 기본 로비 방 생성 (ID: 1)
        // 이 코드가 있어야 클라이언트의 'msg.room_id = 1' 요청이 처리됩니다.
        server.CreateRoom(1, "Lobby", 100); 

        std::cout << "Server is running..." << std::endl;
        io_context.run();
    } catch (std::exception& e) {
        std::cerr << e.what() << std::endl;
    }
    return 0;
}
