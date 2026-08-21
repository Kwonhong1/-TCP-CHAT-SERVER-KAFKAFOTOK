#include <boost/asio.hpp>
#include <boost/asio/co_spawn.hpp>
#include <boost/asio/detached.hpp>
#include <boost/asio/use_awaitable.hpp>
#include <boost/asio/experimental/channel.hpp>

// Boost.Redis
#include <boost/redis/connection.hpp>
#include <boost/redis/src.hpp>

// MySQL Native Driver
#include <mysql/mysql.h>

#include <memory>
#include <iostream>
#include <string>
#include <vector>
#include <unordered_map>
#include <unordered_set>
#include <cstring>
#include <thread>
#include <atomic>
#include <algorithm>
#include <chrono>
#include <optional>
#include <queue>
#include <mutex>
#include <condition_variable>
#include <functional>

using boost::asio::ip::tcp;
using boost::asio::awaitable;
using boost::asio::use_awaitable;
using boost::asio::co_spawn;
using boost::asio::detached;

//=====================
// 방 권한 비트마스크
//=====================
enum class RoomPermission : uint32_t
{
    NONE = 0,
    CHAT = 1 << 0,          // 0x01: 일반 채팅
    KICK_USER = 1 << 1,     // 0x02: 유저 강퇴
    BAN_USER = 1 << 2,      // 0x04: 영구 차단
    CHANGE_CONFIG = 1 << 3, // 0x08: 방 설정 변경
    DELEGATE_HOST = 1 << 4, // 0x10: 방장 위임

    // 역할별 프리셋
    MEMBER = CHAT,
    HOST = CHAT | KICK_USER | BAN_USER | CHANGE_CONFIG | DELEGATE_HOST
};

inline RoomPermission operator|(RoomPermission a, RoomPermission b) {
    return static_cast<RoomPermission>(static_cast<uint32_t>(a) | static_cast<uint32_t>(b));
}

inline bool HasPermission(RoomPermission user_perm, RoomPermission required_perm) {
    return (static_cast<uint32_t>(user_perm) & static_cast<uint32_t>(required_perm)) == static_cast<uint32_t>(required_perm);
}

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
    RECONNECT_RESPONSE = 1022,
    KICK_USER_REQUEST = 1023,
    KICK_USER_RESPONSE = 1024,
    KICKED_NOTIFICATION = 1025,
    TRANSFER_MASTER_REQUEST = 1026,
    TRANSFER_MASTER_RESPONSE = 1027,
    MASTER_CHANGED_NOTIFICATION = 1028
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

struct KickUserRequest
{
    PacketHeader header;
    uint32_t room_id;
    uint32_t target_user_id;
};

struct KickUserResponse
{
    PacketHeader header;
    bool success;
    uint32_t target_user_id;
    char error_message[128];
};

struct KickedNotification
{
    PacketHeader header;
    uint32_t room_id;
    char reason[128];
};

struct TransferMasterRequest
{
    PacketHeader header;
    uint32_t room_id;
    uint32_t target_user_id;
};

struct TransferMasterResponse
{
    PacketHeader header;
    bool success;
    uint32_t target_user_id;
    char error_message[128];
};

struct MasterChangedNotification
{
    PacketHeader header;
    uint32_t room_id;
    uint32_t new_master_id;
    char new_master_username[32];
};
#pragma pack(pop)

struct DBUserData
{
    uint32_t id;
    std::string username;
    std::string password_hash;
};

class ChatServer;
class ChatSession;

//=====================
// MySQL 데이터베이스 매니저 (Worker Thread 전용)
//=====================
class DatabaseManager
{
public:
    DatabaseManager() : conn_(nullptr), stop_(false) {}
    ~DatabaseManager()
    {
        {
            std::lock_guard<std::mutex> lock(queue_mutex_);
            stop_ = true;
        }
        cv_.notify_all();
        if (db_thread_.joinable()) db_thread_.join();
        if (conn_) mysql_close(conn_);
    }

    bool Init(const std::string& host, const std::string& user,
        const std::string& pass, const std::string& dbname, unsigned int port = 3306)
    {
        conn_ = mysql_init(nullptr);
        if (!conn_)
        {
            std::cerr << "[DB Error] mysql_init failed\n";
            return false;
        }

        if (!mysql_real_connect(conn_, host.c_str(), user.c_str(), pass.c_str(),
            dbname.c_str(), port, nullptr, 0))
        {
            std::cerr << "[DB Error] Connection failed: " << mysql_error(conn_) << "\n";
            mysql_close(conn_);
            conn_ = nullptr;
            return false;
        }

        std::cout << "[DB] Successfully connected to MySQL on Dedicated DB Thread." << std::endl;
        db_thread_ = std::thread([this]() { WorkerLoop(); });
        return true;
    }

    void PostTask(std::function<void(MYSQL*)> task)
    {
        {
            std::lock_guard<std::mutex> lock(queue_mutex_);
            tasks_.push(std::move(task));
        }
        cv_.notify_one();
    }

private:
    void WorkerLoop()
    {
        while (true)
        {
            std::function<void(MYSQL*)> task;
            {
                std::unique_lock<std::mutex> lock(queue_mutex_);
                cv_.wait(lock, [this]() { return stop_ || !tasks_.empty(); });

                if (stop_ && tasks_.empty()) break;

                task = std::move(tasks_.front());
                tasks_.pop();
            }

            if (task && conn_)
            {
                task(conn_);
            }
        }
    }

    MYSQL* conn_;
    std::thread db_thread_;
    std::queue<std::function<void(MYSQL*)>> tasks_;
    std::mutex queue_mutex_;
    std::condition_variable cv_;
    bool stop_;
};

//=====================
// Redis 매니저 클래스 (Lock-Free Strand 사용)
//=====================
class RedisManager : public std::enable_shared_from_this<RedisManager>
{
public:
    explicit RedisManager(boost::asio::io_context& io_context)
        : strand_(boost::asio::make_strand(io_context)),
        conn_(std::make_shared<boost::redis::connection>(strand_)) {}

    boost::asio::strand<boost::asio::io_context::executor_type>& GetStrand() { return strand_; }

    awaitable<bool> ConnectAsync(const std::string& host, const std::string& port, const std::string& password = "")
    {
        co_await boost::asio::post(strand_, use_awaitable);

        boost::redis::config cfg;
        cfg.addr.host = host;
        cfg.addr.port = port;
        if (!password.empty()) cfg.password = password;

        co_spawn(strand_, [conn = conn_, cfg]() -> awaitable<void> {
            co_await conn->async_run(cfg, boost::redis::logger{ boost::redis::logger::level::disabled }, use_awaitable);
            co_return;
        }, detached);

        is_connected_ = true;
        std::cout << "[Redis] Connection initialized to " << host << ":" << port << std::endl;
        co_return true;
    }

    awaitable<bool> SetAsync(const std::string& key, const std::string& value, int ttl_seconds = 0)
    {
        co_await boost::asio::post(strand_, use_awaitable);
        if (!is_connected_) co_return false;

        boost::redis::request req;
        if (ttl_seconds > 0)
            req.push("SET", key, value, "EX", std::to_string(ttl_seconds));
        else
            req.push("SET", key, value);

        boost::redis::response<std::string> resp;
        boost::system::error_code ec;

        co_await conn_->async_exec(req, resp, boost::asio::redirect_error(use_awaitable, ec));
        co_return !ec;
    }

    awaitable<std::string> GetAsync(const std::string& key)
    {
        co_await boost::asio::post(strand_, use_awaitable);
        if (!is_connected_) co_return "";

        boost::redis::request req;
        req.push("GET", key);

        boost::redis::response<std::string> resp;
        boost::system::error_code ec;

        co_await conn_->async_exec(req, resp, boost::asio::redirect_error(use_awaitable, ec));
        if (ec) co_return "";

        co_return std::get<0>(resp).value();
    }

    awaitable<bool> PublishAsync(const std::string& channel, const std::string& message)
    {
        co_await boost::asio::post(strand_, use_awaitable);
        if (!is_connected_) co_return false;

        boost::redis::request req;
        req.push("PUBLISH", channel, message);

        boost::redis::response<int> resp;
        boost::system::error_code ec;

        co_await conn_->async_exec(req, resp, boost::asio::redirect_error(use_awaitable, ec));
        co_return !ec;
    }

    template <typename MessageCallback>
    void StartSubscribeLoop(const std::string& channel, MessageCallback&& on_message)
    {
        co_spawn(strand_, [this, self = shared_from_this(), channel, cb = std::forward<MessageCallback>(on_message)]() mutable -> awaitable<void> {
            boost::redis::request req;
            req.push("SUBSCRIBE", channel);

            boost::redis::response<boost::redis::ignore_t> resp;
            boost::system::error_code ec;

            co_await conn_->async_exec(req, resp, boost::asio::redirect_error(use_awaitable, ec));
            if (ec)
            {
                std::cerr << "[Redis] Subscribe failed on channel: " << channel << std::endl;
                co_return;
            }

            while (is_connected_)
            {
                boost::redis::response<std::string, std::string, std::string> sub_msg;
                conn_->set_receive_response(sub_msg);
                co_await conn_->async_receive(boost::asio::redirect_error(use_awaitable, ec));
                if (ec) break;

                auto [cmd, ch, msg] = sub_msg;
                if (ch.has_value() && msg.has_value()) {
                    cb(ch.value(), msg.value());
                }
            }
        }, detached);
    }

private:
    boost::asio::strand<boost::asio::io_context::executor_type> strand_;
    std::shared_ptr<boost::redis::connection> conn_;
    bool is_connected_{ false };
};

//=====================
// Repository 레이어
//=====================
class IUserRepository
{
public:
    virtual ~IUserRepository() = default;

    struct AuthResult {
        bool success{ false };
        DBUserData user_data;
    };

    struct RegisterResult {
        bool success{ false };
        uint32_t assigned_id{ 0 };
    };

    virtual awaitable<AuthResult> AuthenticateUserAsync(std::string username, std::string password) = 0;
    virtual awaitable<RegisterResult> RegisterUserAsync(std::string username, std::string password) = 0;
};

class MySqlUserRepository : public IUserRepository
{
public:
    explicit MySqlUserRepository(std::shared_ptr<DatabaseManager> db_mgr)
        : db_mgr_(db_mgr) {}

    awaitable<AuthResult> AuthenticateUserAsync(std::string username, std::string password) override
    {
        auto executor = co_await boost::asio::this_coro::executor;
        auto result_ptr = std::make_shared<AuthResult>();

        co_await boost::asio::async_initiate<decltype(use_awaitable), void(boost::system::error_code)>(
            [this, username, password, result_ptr, executor](auto handler) {
                auto shared_handler = std::make_shared<decltype(handler)>(std::move(handler));

                db_mgr_->PostTask([username, password, result_ptr, executor, shared_handler](MYSQL* conn) {
                    std::string query = "SELECT id, username, password_hash FROM users WHERE username = '" + username + "' LIMIT 1;";
                    if (mysql_query(conn, query.c_str()) == 0)
                    {
                        MYSQL_RES* res = mysql_store_result(conn);
                        if (res)
                        {
                            if (MYSQL_ROW row = mysql_fetch_row(res))
                            {
                                if (std::string(row[2]) == password)
                                {
                                    result_ptr->success = true;
                                    result_ptr->user_data.id = static_cast<uint32_t>(std::stoul(row[0]));
                                    result_ptr->user_data.username = row[1];
                                }
                            }
                            mysql_free_result(res);
                        }
                    }

                    boost::asio::post(executor, [shared_handler]() mutable {
                        (*shared_handler)(boost::system::error_code{});
                    });
                });
            },
            use_awaitable
        );

        co_return *result_ptr;
    }

    awaitable<RegisterResult> RegisterUserAsync(std::string username, std::string password) override
    {
        auto executor = co_await boost::asio::this_coro::executor;
        auto result_ptr = std::make_shared<RegisterResult>();

        co_await boost::asio::async_initiate<decltype(use_awaitable), void(boost::system::error_code)>(
            [this, username, password, result_ptr, executor](auto handler) {
                auto shared_handler = std::make_shared<decltype(handler)>(std::move(handler));

                db_mgr_->PostTask([username, password, result_ptr, executor, shared_handler](MYSQL* conn) {
                    std::string query = "INSERT INTO users (username, password_hash) VALUES ('" + username + "', '" + password + "');";
                    if (mysql_query(conn, query.c_str()) == 0)
                    {
                        result_ptr->success = true;
                        result_ptr->assigned_id = static_cast<uint32_t>(mysql_insert_id(conn));
                    }

                    boost::asio::post(executor, [shared_handler]() mutable {
                        (*shared_handler)(boost::system::error_code{});
                    });
                });
            },
            use_awaitable
        );

        co_return *result_ptr;
    }

private:
    std::shared_ptr<DatabaseManager> db_mgr_;
};

class ISessionRepository
{
public:
    virtual ~ISessionRepository() = default;

    virtual awaitable<bool> SetUserSessionStateAsync(uint32_t user_id, const std::string& state, int ttl_seconds = 3600) = 0;
    virtual awaitable<bool> PublishChatMessageAsync(const ChatMessage& msg) = 0;
};

class RedisSessionRepository : public ISessionRepository
{
public:
    explicit RedisSessionRepository(std::shared_ptr<RedisManager> redis_mgr)
        : redis_mgr_(redis_mgr) {}

    awaitable<bool> SetUserSessionStateAsync(uint32_t user_id, const std::string& state, int ttl_seconds = 3600) override
    {
        std::string key = "user:session:" + std::to_string(user_id);
        co_return co_await redis_mgr_->SetAsync(key, state, ttl_seconds);
    }

    awaitable<bool> PublishChatMessageAsync(const ChatMessage& msg) override
    {
        std::string payload(reinterpret_cast<const char*>(&msg), sizeof(ChatMessage));
        co_return co_await redis_mgr_->PublishAsync("chat_broadcast", payload);
    }

    template <typename MessageCallback>
    void SubscribeChatBroadcast(MessageCallback&& on_message)
    {
        redis_mgr_->StartSubscribeLoop("chat_broadcast", std::forward<MessageCallback>(on_message));
    }

private:
    std::shared_ptr<RedisManager> redis_mgr_;
};

//=====================
// Ring Buffer 패킷 버퍼
//=====================
class RingPacketBuffer
{
public:
    explicit RingPacketBuffer(size_t capacity = 16384)
        : buffer_(capacity), capacity_(capacity), head_(0), tail_(0), size_(0) {}

    bool WriteData(const char* data, size_t len)
    {
        if (capacity_ - size_ < len) return false;

        size_t first_part = std::min(len, capacity_ - tail_);
        std::memcpy(&buffer_[tail_], data, first_part);
        size_t second_part = len - first_part;
        if (second_part > 0)
        {
            std::memcpy(&buffer_[0], data + first_part, second_part);
        }

        tail_ = (tail_ + len) % capacity_;
        size_ += len;
        return true;
    }

    bool HasCompletePacket() const
    {
        if (size_ < sizeof(PacketHeader)) return false;

        PacketHeader header;
        PeekBytes(reinterpret_cast<char*>(&header), sizeof(PacketHeader));
        return size_ >= header.packet_size;
    }

    bool ReadPacket(std::vector<char>& out_packet)
    {
        if (!HasCompletePacket()) return false;

        PacketHeader header;
        PeekBytes(reinterpret_cast<char*>(&header), sizeof(PacketHeader));

        out_packet.resize(header.packet_size);
        ReadBytes(out_packet.data(), header.packet_size);
        return true;
    }

private:
    void PeekBytes(char* dest, size_t len) const
    {
        size_t first_part = std::min(len, capacity_ - head_);
        std::memcpy(dest, &buffer_[head_], first_part);
        if (len > first_part)
        {
            std::memcpy(dest + first_part, &buffer_[0], len - first_part);
        }
    }

    void ReadBytes(char* dest, size_t len)
    {
        PeekBytes(dest, len);
        head_ = (head_ + len) % capacity_;
        size_ -= len;
    }

    std::vector<char> buffer_;
    size_t capacity_;
    size_t head_;
    size_t tail_;
    size_t size_;
};

//=====================
// 세션 클래스 (Lock-Free Strand 기반)
//=====================
using MessageChannel = boost::asio::experimental::channel<void(boost::system::error_code, std::vector<char>)>;

class ChatSession : public std::enable_shared_from_this<ChatSession>
{
public:
    ChatSession(tcp::socket socket, ChatServer& server)
        : strand_(boost::asio::make_strand(socket.get_executor())),
        socket_(std::move(socket)), server_(server), user_id_(0),
        is_authenticated_(false), is_disconnected_(false),
        write_channel_(strand_, 100), idle_timer_(strand_) {}

    ~ChatSession()
    {
        boost::system::error_code ec;
        idle_timer_.cancel(ec);
        socket_.close(ec);
    }

    boost::asio::strand<boost::asio::any_io_executor>& GetStrand() { return strand_; }

    void Start()
    {
        co_spawn(strand_, [this, self = shared_from_this()]() -> awaitable<void> {
            StartIdleTimer();
            co_spawn(strand_, WriteLoop(), detached);

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
        boost::asio::post(strand_, [this, self = shared_from_this(), msg_data = std::vector<char>(static_cast<const char*>(data), static_cast<const char*>(data) + size)]() mutable {
            if (is_disconnected_) return;
            write_channel_.try_send(boost::system::error_code{}, std::move(msg_data));
        });
    }

    void SetUserId(uint32_t id) { user_id_ = id; }
    uint32_t GetUserId() const { return user_id_; }
    void SetAuthenticated(bool auth) { is_authenticated_ = auth; }
    bool IsAuthenticated() const { return is_authenticated_; }
    bool IsDisconnected() const { return is_disconnected_; }

    void Disconnect();

    void Kick()
    {
        boost::asio::post(strand_, [this, self = shared_from_this()]() {
            Disconnect();
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
                if (!packet_buffer_.WriteData(read_buffer_.data(), length))
                {
                    std::cerr << "[Error] Ring Buffer Overflow! Closing Session." << std::endl;
                    Disconnect();
                    co_return;
                }

                std::vector<char> packet_data;
                while (packet_buffer_.ReadPacket(packet_data))
                {
                    co_await ProcessPacketAsync(packet_data.data(), packet_data.size());
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
            while (!is_disconnected_)
            {
                std::vector<char> msg = co_await write_channel_.async_receive(use_awaitable);
                if (is_disconnected_) break;

                co_await boost::asio::async_write(socket_, boost::asio::buffer(msg.data(), msg.size()), use_awaitable);
            }
        }
        catch (const boost::system::system_error&)
        {
            Disconnect();
        }
    }

    awaitable<void> ProcessPacketAsync(const char* data, size_t size);

    boost::asio::strand<boost::asio::any_io_executor> strand_;
    tcp::socket socket_;
    ChatServer& server_;
    uint32_t user_id_;
    bool is_authenticated_;
    bool is_disconnected_;
    MessageChannel write_channel_;
    boost::asio::steady_timer idle_timer_;
    std::vector<char> read_buffer_ = std::vector<char>(4096);
    RingPacketBuffer packet_buffer_;
};

//=====================
// 유저 클래스
//=====================
class User : public std::enable_shared_from_this<User>
{
public:
    User(boost::asio::io_context& io_context, uint32_t id, const std::string& username)
        : strand_(boost::asio::make_strand(io_context)),
        id_(id), password_(0), username_(username), is_online_(false),
        disconnect_timer_(strand_) {}

    boost::asio::strand<boost::asio::io_context::executor_type>& GetStrand() { return strand_; }

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
// 메시지 핸들러 인터페이스
//=====================
class IMessageHandler
{
public:
    virtual ~IMessageHandler() = default;
    virtual awaitable<void> HandleMessageAsync(std::shared_ptr<ChatSession> session, const char* data, size_t size) = 0;
};

class MessageDispatcher
{
public:
    void RegisterHandler(MessageType type, std::unique_ptr<IMessageHandler> handler)
    {
        handlers_[type] = std::move(handler);
    }

    awaitable<void> DispatchMessageAsync(std::shared_ptr<ChatSession> session, const PacketHeader& header, const char* data, size_t size)
    {
        auto it = handlers_.find(header.message_type);
        if (it != handlers_.end())
        {
            co_await it->second->HandleMessageAsync(session, data, size);
        }
        else
        {
            std::cout << "[Server] Unknown message type: " << static_cast<uint16_t>(header.message_type) << std::endl;
        }
    }

private:
    std::unordered_map<MessageType, std::unique_ptr<IMessageHandler>> handlers_;
};

//=====================
// 유저 매니저 (Lock-Free Strand 기반)
//=====================
class UserManager : public std::enable_shared_from_this<UserManager>
{
public:
    explicit UserManager(boost::asio::io_context& io_context)
        : io_context_(io_context), strand_(boost::asio::make_strand(io_context)) {}

    boost::asio::strand<boost::asio::io_context::executor_type>& GetStrand() { return strand_; }

    awaitable<std::shared_ptr<User>> GetOrCreateMemoryUserAsync(uint32_t user_id, const std::string& username)
    {
        co_await boost::asio::post(strand_, use_awaitable);

        auto it = users_.find(user_id);
        if (it != users_.end())
        {
            co_return it->second;
        }

        auto user = std::make_shared<User>(io_context_, user_id, username);
        users_[user_id] = user;
        co_return user;
    }

    awaitable<std::shared_ptr<User>> GetUserByUsernameAsync(std::string username)
    {
        co_await boost::asio::post(strand_, use_awaitable);

        for (const auto& [id, user] : users_) {
            if (user->GetUsername() == username) {
                co_return user;
            }
        }
        co_return nullptr;
    }

    awaitable<std::shared_ptr<User>> GetUserAsync(uint32_t user_id)
    {
        co_await boost::asio::post(strand_, use_awaitable);

        auto it = users_.find(user_id);
        co_return (it != users_.end()) ? it->second : nullptr;
    }

    awaitable<std::pair<bool, std::string>> TryReconnectAsync(uint32_t user_id, std::string token, std::shared_ptr<ChatSession> new_session)
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
};

//=====================
// 채팅방 (Lock-Free Strand 기반)
//=====================
class ChatRoom : public std::enable_shared_from_this<ChatRoom>
{
public:
    ChatRoom(boost::asio::io_context& io_context, uint32_t room_id, std::string name, uint32_t max_users)
        : strand_(boost::asio::make_strand(io_context)), room_id_(room_id), name_(name), max_users_(max_users) {}

    boost::asio::strand<boost::asio::io_context::executor_type>& GetStrand() { return strand_; }

    uint32_t GetId() const { return room_id_; }
    std::string GetName() const { return name_; }
    uint32_t GetMaxUsers() const { return max_users_; }
    uint32_t GetCurrentUsers() const { return static_cast<uint32_t>(users_.size()); }

    awaitable<std::pair<bool, std::string>> AddUserAsync(std::shared_ptr<User> user, RoomPermission perm = RoomPermission::MEMBER)
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
        user_permissions_[user->GetId()] = perm;

        BroadcastNotification(user->GetUsername() + " joined the room.", user->GetId());
        co_return std::make_pair(true, "");
    }

    awaitable<std::pair<bool, std::string>> RemoveUserAsync(uint32_t user_id)
    {
        co_await boost::asio::post(strand_, use_awaitable);

        auto it = users_.find(user_id);
        if (it == users_.end()) {
            co_return std::make_pair(false, "USER_NOT_IN_ROOM");
        }

        std::string username = it->second->GetUsername();
        bool is_host = HasPermission(user_permissions_[user_id], RoomPermission::DELEGATE_HOST);

        users_.erase(it);
        user_permissions_.erase(user_id);

        BroadcastNotification(username + " left the room.", user_id);

        // 방장이 퇴장했고 남은 유저가 있는 경우 방장 권한 자동 위임
        if (is_host && !users_.empty())
        {
            auto next_master_it = users_.begin();
            uint32_t next_master_id = next_master_it->first;
            std::string next_master_name = next_master_it->second->GetUsername();

            user_permissions_[next_master_id] = RoomPermission::HOST;

            MasterChangedNotification notif{};
            notif.header.packet_size = sizeof(MasterChangedNotification);
            notif.header.message_type = MessageType::MASTER_CHANGED_NOTIFICATION;
            notif.room_id = room_id_;
            notif.new_master_id = next_master_id;
            std::strncpy(notif.new_master_username, next_master_name.c_str(), sizeof(notif.new_master_username) - 1);

            BroadcastRawData(&notif, sizeof(MasterChangedNotification));
            BroadcastNotification(next_master_name + " is now the room host.");
        }

        co_return std::make_pair(true, "");
    }

    awaitable<bool> CheckPermissionAsync(uint32_t user_id, RoomPermission required_perm)
    {
        co_await boost::asio::post(strand_, use_awaitable);
        auto it = user_permissions_.find(user_id);
        if (it == user_permissions_.end()) co_return false;

        co_return HasPermission(it->second, required_perm);
    }

    awaitable<std::pair<bool, std::string>> KickUserAsync(uint32_t operator_id, uint32_t target_id)
    {
        co_await boost::asio::post(strand_, use_awaitable);

        auto perm_it = user_permissions_.find(operator_id);
        if (perm_it == user_permissions_.end() || !HasPermission(perm_it->second, RoomPermission::KICK_USER)) {
            co_return std::make_pair(false, "PERMISSION_DENIED");
        }

        auto user_it = users_.find(target_id);
        if (user_it == users_.end()) {
            co_return std::make_pair(false, "TARGET_NOT_IN_ROOM");
        }

        if (operator_id == target_id) {
            co_return std::make_pair(false, "CANNOT_KICK_SELF");
        }

        auto target_user = user_it->second;

        if (auto session = target_user->GetSession().lock())
        {
            KickedNotification notif{};
            notif.header.packet_size = sizeof(KickedNotification);
            notif.header.message_type = MessageType::KICKED_NOTIFICATION;
            notif.room_id = room_id_;
            std::strncpy(notif.reason, "Kicked by room host.", sizeof(notif.reason) - 1);
            session->SendMessage(&notif, sizeof(KickedNotification));
        }

        users_.erase(user_it);
        user_permissions_.erase(target_id);

        BroadcastNotification(target_user->GetUsername() + " has been kicked.", 0);
        co_return std::make_pair(true, "");
    }

    awaitable<std::pair<bool, std::string>> TransferMasterAsync(uint32_t operator_id, uint32_t target_id)
    {
        co_await boost::asio::post(strand_, use_awaitable);

        auto perm_it = user_permissions_.find(operator_id);
        if (perm_it == user_permissions_.end() || !HasPermission(perm_it->second, RoomPermission::DELEGATE_HOST)) {
            co_return std::make_pair(false, "PERMISSION_DENIED");
        }

        auto target_it = users_.find(target_id);
        if (target_it == users_.end()) {
            co_return std::make_pair(false, "TARGET_NOT_IN_ROOM");
        }

        if (operator_id == target_id) {
            co_return std::make_pair(false, "CANNOT_TRANSFER_SELF");
        }

        user_permissions_[operator_id] = RoomPermission::MEMBER;
        user_permissions_[target_id] = RoomPermission::HOST;

        std::string new_master_name = target_it->second->GetUsername();

        MasterChangedNotification notif{};
        notif.header.packet_size = sizeof(MasterChangedNotification);
        notif.header.message_type = MessageType::MASTER_CHANGED_NOTIFICATION;
        notif.room_id = room_id_;
        notif.new_master_id = target_id;
        std::strncpy(notif.new_master_username, new_master_name.c_str(), sizeof(notif.new_master_username) - 1);

        BroadcastRawData(&notif, sizeof(MasterChangedNotification));
        BroadcastNotification(new_master_name + " is now the new room host.");

        co_return std::make_pair(true, "");
    }

    void BroadcastMessage(const ChatMessage& msg, uint32_t sender_id);
    void BroadcastNotification(const std::string& notification_text, uint32_t except_user_id = 0);
    void BroadcastRawData(const void* data, size_t size, uint32_t except_user_id = 0);

private:
    boost::asio::strand<boost::asio::io_context::executor_type> strand_;
    uint32_t room_id_;
    std::string name_;
    uint32_t max_users_;
    std::unordered_map<uint32_t, std::shared_ptr<User>> users_;
    std::unordered_map<uint32_t, RoomPermission> user_permissions_;
};

//=====================
// ChatRoom 브로드캐스트 구현
//=====================
void ChatRoom::BroadcastMessage(const ChatMessage& msg, uint32_t sender_id)
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

void ChatRoom::BroadcastNotification(const std::string& notification_text, uint32_t except_user_id)
{
    boost::asio::post(strand_, [this, self = shared_from_this(), notification_text, except_user_id]() {
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
    });
}

void ChatRoom::BroadcastRawData(const void* data, size_t size, uint32_t except_user_id)
{
    boost::asio::post(strand_, [this, self = shared_from_this(), data_vec = std::vector<char>(static_cast<const char*>(data), static_cast<const char*>(data) + size), except_user_id]() {
        for (auto& [id, user] : users_)
        {
            if (id == except_user_id) continue;
            if (auto session = user->GetSession().lock())
            {
                session->SendMessage(data_vec.data(), data_vec.size());
            }
        }
    });
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
    std::shared_ptr<RedisManager> GetRedisManager() { return redis_manager_; }
    std::shared_ptr<DatabaseManager> GetDBManager() { return db_manager_; }
    std::shared_ptr<IUserRepository> GetUserRepository() { return user_repository_; }
    std::shared_ptr<ISessionRepository> GetSessionRepository() { return session_repository_; }

    bool InitDB(const std::string& host, const std::string& user, const std::string& pass, const std::string& dbname)
    {
        return db_manager_->Init(host, user, pass, dbname);
    }

    awaitable<void> InitRedisAsync(const std::string& host, const std::string& port, const std::string& password = "")
    {
        co_await redis_manager_->ConnectAsync(host, port, password);

        session_repository_->SubscribeChatBroadcast([this](const std::string& channel, const std::string& payload) {
            if (payload.size() < sizeof(ChatMessage)) return;

            ChatMessage msg{};
            std::memcpy(&msg, payload.data(), sizeof(ChatMessage));

            co_spawn(strand_, [this, msg]() -> awaitable<void> {
                auto room = co_await GetRoomAsync(msg.room_id);
                if (room)
                {
                    room->BroadcastMessage(msg, msg.header.user_id);
                }
            }, detached);
        });
    }

    void AddSession(std::shared_ptr<ChatSession> session)
    {
        boost::asio::post(strand_, [this, self = shared_from_this(), session]() {
            sessions_.insert(session);
            std::cout << "[Server] Client connected. Active FD/Sessions: " << sessions_.size() << std::endl;
        });
    }

    void RemoveSession(std::shared_ptr<ChatSession> session)
    {
        boost::asio::post(strand_, [this, self = shared_from_this(), session]() {
            sessions_.erase(session);
            std::cout << "[Server] Client disconnected. Active FD/Sessions: " << sessions_.size() << std::endl;
        });
    }

    void OnSessionDisconnected(std::shared_ptr<ChatSession> session)
    {
        RemoveSession(session);

        uint32_t user_id = session->GetUserId();
        if (user_id == 0) return;

        co_spawn(strand_, [this, self = shared_from_this(), user_id]() -> awaitable<void> {
            auto user = co_await user_manager_->GetUserAsync(user_id);
            if (user)
            {
                user->SetOnline(false);
                std::cout << "[System] User connection lost (Grace period 60s started). User ID: " << user_id << std::endl;

                user->StartDisconnectTimer([this, self, user_id]() {
                    co_spawn(strand_, [this, self, user_id]() -> awaitable<void> {
                        for (auto& [id, room] : rooms_) {
                            co_await room->RemoveUserAsync(user_id);
                        }
                        std::cout << "[System] User disconnect grace period expired. Removed from all rooms ID: " << user_id << std::endl;
                    }, detached);
                });
            }
        }, detached);
    }

    awaitable<uint32_t> CreateRoomAsync(std::string name, uint32_t max_users)
    {
        co_await boost::asio::post(strand_, use_awaitable);

        uint32_t new_room_id = next_room_id_++;
        auto new_room = std::make_shared<ChatRoom>(io_context_, new_room_id, name, max_users);
        rooms_[new_room_id] = new_room;
        std::cout << "[Server] Room Created: " << name << " (ID: " << new_room_id << ")" << std::endl;
        co_return new_room_id;
    }

    awaitable<std::vector<RoomInfo>> GetRoomListAsync()
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

    awaitable<std::shared_ptr<ChatRoom>> GetRoomAsync(uint32_t room_id)
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
                auto session = std::make_shared<ChatSession>(std::move(socket), *this);
                AddSession(session);
                session->Start();
            }
        }, detached);
    }

private:
    boost::asio::strand<boost::asio::io_context::executor_type> strand_;
    tcp::acceptor acceptor_;
    MessageDispatcher dispatcher_;
    std::shared_ptr<UserManager> user_manager_;
    std::shared_ptr<RedisManager> redis_manager_;
    std::shared_ptr<DatabaseManager> db_manager_;
    std::shared_ptr<IUserRepository> user_repository_;
    std::shared_ptr<RedisSessionRepository> session_repository_;

    std::unordered_map<uint32_t, std::shared_ptr<ChatRoom>> rooms_;
    std::unordered_set<std::shared_ptr<ChatSession>> sessions_;
    uint32_t next_room_id_ = 1;
};

void ChatSession::Disconnect()
{
    if (is_disconnected_) return;
    is_disconnected_ = true;

    boost::system::error_code ec;
    idle_timer_.cancel(ec);
    write_channel_.close();
    socket_.close(ec);
    server_.OnSessionDisconnected(shared_from_this());
}

awaitable<void> ChatSession::ProcessPacketAsync(const char* data, size_t size)
{
    if (size < sizeof(PacketHeader)) co_return;
    const auto& header = *reinterpret_cast<const PacketHeader*>(data);
    co_await server_.GetDispatcher().DispatchMessageAsync(shared_from_this(), header, data, size);
}

//=====================
// 패킷 핸들러 구현부
//=====================

class LoginHandler : public IMessageHandler
{
public:
    LoginHandler(UserManager& user_manager, ChatServer& server)
        : user_manager_(user_manager), server_(server) {}

    awaitable<void> HandleMessageAsync(std::shared_ptr<ChatSession> session, const char* data, size_t size) override
    {
        if (size < sizeof(LoginRequest)) co_return;
        const auto& request = *reinterpret_cast<const LoginRequest*>(data);

        std::string username = request.username;
        std::string password = request.password;

        auto auth_result = co_await server_.GetUserRepository()->AuthenticateUserAsync(username, password);

        if (session->IsDisconnected()) co_return;

        LoginResponse response{};
        response.header.message_type = MessageType::LOGIN_RESPONSE;
        response.header.packet_size = sizeof(LoginResponse);

        if (!auth_result.success)
        {
            response.success = false;
            std::strncpy(response.error_message, "INVALID_CREDENTIALS", sizeof(response.error_message) - 1);
            session->SendMessage(&response, sizeof(LoginResponse));
            co_return;
        }

        uint32_t user_id = auth_result.user_data.id;
        auto user = co_await user_manager_.GetOrCreateMemoryUserAsync(user_id, username);

        if (user->IsOnline())
        {
            if (auto old_session = user->GetSession().lock())
            {
                ServerNotification kick_notif{};
                kick_notif.header.packet_size = sizeof(ServerNotification);
                kick_notif.header.message_type = MessageType::SERVER_NOTIFICATION;
                std::strncpy(kick_notif.message, "Logged in from another location.", sizeof(kick_notif.message) - 1);
                old_session->SendMessage(&kick_notif, sizeof(ServerNotification));
                old_session->Kick();
            }
            user->CancelDisconnectTimer();
        }

        user->SetSession(session);
        user->SetOnline(true);

        std::string token = "TOKEN_" + std::to_string(user_id) + "_SECRET";
        user->SetReconnectToken(token);
        std::strncpy(response.reconnect_token, token.c_str(), sizeof(response.reconnect_token) - 1);

        session->SetUserId(user_id);
        session->SetAuthenticated(true);

        co_await server_.GetSessionRepository()->SetUserSessionStateAsync(user_id, "ONLINE", 3600);

        response.success = true;
        response.assigned_user_id = user_id;

        auto lobby = co_await server_.GetRoomAsync(1);
        if (lobby && !session->IsDisconnected())
        {
            co_await lobby->AddUserAsync(user, RoomPermission::MEMBER);
        }

        if (!session->IsDisconnected())
        {
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
    RegisterHandler(UserManager& user_manager, ChatServer& server)
        : user_manager_(user_manager), server_(server) {}

    awaitable<void> HandleMessageAsync(std::shared_ptr<ChatSession> session, const char* data, size_t size) override
    {
        if (size < sizeof(RegisterRequest)) co_return;
        const auto& request = *reinterpret_cast<const RegisterRequest*>(data);

        std::string username = request.username;
        std::string password = request.password;

        auto reg_result = co_await server_.GetUserRepository()->RegisterUserAsync(username, password);

        if (session->IsDisconnected()) co_return;

        RegisterResponse response{};
        response.header.message_type = MessageType::REGISTER_RESPONSE;
        response.header.packet_size = sizeof(RegisterResponse);

        if (reg_result.success)
        {
            response.success = true;
            response.assigned_user_id = reg_result.assigned_id;
        }
        else
        {
            response.success = false;
            std::strncpy(response.error_message, "REGISTER_FAILED_OR_DUPLICATE", sizeof(response.error_message) - 1);
        }

        session->SendMessage(&response, sizeof(RegisterResponse));
    }

private:
    UserManager& user_manager_;
    ChatServer& server_;
};

class ReconnectHandler : public IMessageHandler
{
public:
    ReconnectHandler(UserManager& user_manager, ChatServer& server)
        : user_manager_(user_manager), server_(server) {}

    awaitable<void> HandleMessageAsync(std::shared_ptr<ChatSession> session, const char* data, size_t size) override
    {
        if (size < sizeof(ReconnectRequest)) co_return;
        const auto& req = *reinterpret_cast<const ReconnectRequest*>(data);

        auto [success, err] = co_await user_manager_.TryReconnectAsync(req.user_id, req.reconnect_token, session);

        ReconnectResponse res{};
        res.header.message_type = MessageType::RECONNECT_RESPONSE;
        res.header.packet_size = sizeof(ReconnectResponse);

        if (!success || session->IsDisconnected())
        {
            res.success = false;
            std::strncpy(res.error_message, err.c_str(), sizeof(res.error_message) - 1);
            session->SendMessage(&res, sizeof(ReconnectResponse));
            co_return;
        }

        auto user = co_await user_manager_.GetUserAsync(req.user_id);
        if (!user || session->IsDisconnected()) co_return;

        user->SetOnline(true);
        session->SetUserId(user->GetId());
        session->SetAuthenticated(true);

        auto room = co_await server_.GetRoomAsync(req.last_room_id);
        if (room && !session->IsDisconnected())
        {
            auto [joined, join_err] = co_await room->AddUserAsync(user, RoomPermission::MEMBER);
            res.success = true;
            res.restored_room_id = joined ? req.last_room_id : 0;
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

    awaitable<void> HandleMessageAsync(std::shared_ptr<ChatSession> session, const char* data, size_t size) override
    {
        if (!session->IsAuthenticated() || size < sizeof(CreateRoomRequest)) co_return;
        const auto& req = *reinterpret_cast<const CreateRoomRequest*>(data);

        uint32_t created_room_id = co_await server_.CreateRoomAsync(req.room_name, req.max_users);
        auto room = co_await server_.GetRoomAsync(created_room_id);
        auto user = co_await server_.GetUserManager().GetUserAsync(session->GetUserId());

        if (session->IsDisconnected()) co_return;

        CreateRoomResponse res{};
        res.header.message_type = MessageType::CREATE_ROOM_RESPONSE;
        res.header.packet_size = sizeof(CreateRoomResponse);

        if (room && user)
        {
            co_await room->AddUserAsync(user, RoomPermission::HOST);
            res.success = true;
            res.created_room_id = created_room_id;
        }
        else
        {
            res.success = false;
            std::strncpy(res.error_message, "ROOM_CREATION_FAILED", sizeof(res.error_message) - 1);
        }

        session->SendMessage(&res, sizeof(CreateRoomResponse));
    }

private:
    ChatServer& server_;
};

class KickUserHandler : public IMessageHandler
{
public:
    explicit KickUserHandler(ChatServer& server) : server_(server) {}

    awaitable<void> HandleMessageAsync(std::shared_ptr<ChatSession> session, const char* data, size_t size) override
    {
        if (!session->IsAuthenticated() || size < sizeof(KickUserRequest)) co_return;
        const auto& req = *reinterpret_cast<const KickUserRequest*>(data);

        auto room = co_await server_.GetRoomAsync(req.room_id);

        KickUserResponse res{};
        res.header.message_type = MessageType::KICK_USER_RESPONSE;
        res.header.packet_size = sizeof(KickUserResponse);
        res.target_user_id = req.target_user_id;

        if (!room)
        {
            res.success = false;
            std::strncpy(res.error_message, "ROOM_NOT_FOUND", sizeof(res.error_message) - 1);
            session->SendMessage(&res, sizeof(KickUserResponse));
            co_return;
        }

        auto [success, err] = co_await room->KickUserAsync(session->GetUserId(), req.target_user_id);
        res.success = success;
        if (!success) {
            std::strncpy(res.error_message, err.c_str(), sizeof(res.error_message) - 1);
        }

        if (!session->IsDisconnected()) {
            session->SendMessage(&res, sizeof(KickUserResponse));
        }
    }

private:
    ChatServer& server_;
};

class TransferMasterHandler : public IMessageHandler
{
public:
    explicit TransferMasterHandler(ChatServer& server) : server_(server) {}

    awaitable<void> HandleMessageAsync(std::shared_ptr<ChatSession> session, const char* data, size_t size) override
    {
        if (!session->IsAuthenticated() || size < sizeof(TransferMasterRequest)) co_return;
        const auto& req = *reinterpret_cast<const TransferMasterRequest*>(data);

        auto room = co_await server_.GetRoomAsync(req.room_id);

        TransferMasterResponse res{};
        res.header.message_type = MessageType::TRANSFER_MASTER_RESPONSE;
        res.header.packet_size = sizeof(TransferMasterResponse);
        res.target_user_id = req.target_user_id;

        if (!room)
        {
            res.success = false;
            std::strncpy(res.error_message, "ROOM_NOT_FOUND", sizeof(res.error_message) - 1);
            session->SendMessage(&res, sizeof(TransferMasterResponse));
            co_return;
        }

        auto [success, err] = co_await room->TransferMasterAsync(session->GetUserId(), req.target_user_id);
        res.success = success;
        if (!success) {
            std::strncpy(res.error_message, err.c_str(), sizeof(res.error_message) - 1);
        }

        if (!session->IsDisconnected()) {
            session->SendMessage(&res, sizeof(TransferMasterResponse));
        }
    }

private:
    ChatServer& server_;
};

class RoomListHandler : public IMessageHandler
{
public:
    explicit RoomListHandler(ChatServer& server) : server_(server) {}

    awaitable<void> HandleMessageAsync(std::shared_ptr<ChatSession> session, const char* data, size_t size) override
    {
        if (!session->IsAuthenticated() || size < sizeof(RoomListRequest)) co_return;

        auto list = co_await server_.GetRoomListAsync();

        if (session->IsDisconnected()) co_return;

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

class ChatMessageHandler : public IMessageHandler
{
public:
    explicit ChatMessageHandler(ChatServer& server) : server_(server) {}

    awaitable<void> HandleMessageAsync(std::shared_ptr<ChatSession> session, const char* data, size_t size) override
    {
        if (!session->IsAuthenticated() || size < sizeof(ChatMessage)) co_return;
        const auto& message = *reinterpret_cast<const ChatMessage*>(data);

        co_await server_.GetSessionRepository()->PublishChatMessageAsync(message);
    }

private:
    ChatServer& server_;
};

class JoinRoomHandler : public IMessageHandler
{
public:
    JoinRoomHandler(UserManager& user_manager, ChatServer& server)
        : user_manager_(user_manager), server_(server) {}

    awaitable<void> HandleMessageAsync(std::shared_ptr<ChatSession> session, const char* data, size_t size) override
    {
        if (!session->IsAuthenticated() || size < sizeof(JoinRoomRequest)) co_return;
        const auto& req = *reinterpret_cast<const JoinRoomRequest*>(data);

        auto user = co_await user_manager_.GetUserAsync(session->GetUserId());
        auto room = co_await server_.GetRoomAsync(req.room_id);

        if (session->IsDisconnected()) co_return;

        JoinRoomResponse res{};
        res.header.message_type = MessageType::JOIN_ROOM_RESPONSE;
        res.header.packet_size = sizeof(JoinRoomResponse);
        res.room_id = req.room_id;

        if (!room || !user)
        {
            res.success = false;
            std::strncpy(res.error_message, "ROOM_OR_USER_NOT_FOUND", sizeof(res.error_message) - 1);
            session->SendMessage(&res, sizeof(JoinRoomResponse));
            co_return;
        }

        auto [success, err] = co_await room->AddUserAsync(user, RoomPermission::MEMBER);
        res.success = success;
        if (!success) std::strncpy(res.error_message, err.c_str(), sizeof(res.error_message) - 1);

        if (!session->IsDisconnected())
        {
            session->SendMessage(&res, sizeof(JoinRoomResponse));
        }
    }

private:
    UserManager& user_manager_;
    ChatServer& server_;
};

class LeaveRoomHandler : public IMessageHandler
{
public:
    explicit LeaveRoomHandler(ChatServer& server) : server_(server) {}

    awaitable<void> HandleMessageAsync(std::shared_ptr<ChatSession> session, const char* data, size_t size) override
    {
        if (!session->IsAuthenticated() || size < sizeof(LeaveRoomRequest)) co_return;
        const auto& req = *reinterpret_cast<const LeaveRoomRequest*>(data);

        auto room = co_await server_.GetRoomAsync(req.room_id);

        if (session->IsDisconnected()) co_return;

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

        auto [success, err] = co_await room->RemoveUserAsync(session->GetUserId());
        res.success = success;
        if (!success) std::strncpy(res.error_message, err.c_str(), sizeof(res.error_message) - 1);

        if (!session->IsDisconnected())
        {
            session->SendMessage(&res, sizeof(LeaveRoomResponse));
        }
    }

private:
    ChatServer& server_;
};

class WhisperHandler : public IMessageHandler
{
public:
    WhisperHandler(UserManager& user_manager, ChatServer& server)
        : user_manager_(user_manager), server_(server) {}

    awaitable<void> HandleMessageAsync(std::shared_ptr<ChatSession> session, const char* data, size_t size) override
    {
        if (!session->IsAuthenticated() || size < sizeof(WhisperRequest)) co_return;
        const auto& req = *reinterpret_cast<const WhisperRequest*>(data);

        WhisperResponse res{};
        res.header.message_type = MessageType::WHISPER_RESPONSE;
        res.header.packet_size = sizeof(WhisperResponse);

        auto sender_user = co_await user_manager_.GetUserAsync(session->GetUserId());
        auto target_user = co_await user_manager_.GetUserByUsernameAsync(req.target_username);

        if (session->IsDisconnected()) co_return;

        if (!sender_user || !target_user)
        {
            res.success = false;
            std::strncpy(res.error_message, "USER_NOT_FOUND", sizeof(res.error_message) - 1);
            session->SendMessage(&res, sizeof(WhisperResponse));
            co_return;
        }

        if (sender_user->GetUsername() == req.target_username)
        {
            res.success = false;
            std::strncpy(res.error_message, "CANNOT_WHISPER_SELF", sizeof(res.error_message) - 1);
            session->SendMessage(&res, sizeof(WhisperResponse));
            co_return;
        }

        if (auto target_session = target_user->GetSession().lock())
        {
            WhisperNotification notif{};
            notif.header.packet_size = sizeof(WhisperNotification);
            notif.header.message_type = MessageType::WHISPER_NOTIFICATION;
            std::strncpy(notif.sender_username, sender_user->GetUsername().c_str(), sizeof(notif.sender_username) - 1);
            std::strncpy(notif.message, req.message, sizeof(notif.message) - 1);

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
ChatServer::ChatServer(boost::asio::io_context& io_context, short port)
    : io_context_(io_context),
    strand_(boost::asio::make_strand(io_context)),
    acceptor_(io_context, tcp::endpoint(tcp::v4(), port)),
    user_manager_(std::make_shared<UserManager>(io_context)),
    redis_manager_(std::make_shared<RedisManager>(io_context)),
    db_manager_(std::make_shared<DatabaseManager>()),
    user_repository_(std::make_shared<MySqlUserRepository>(db_manager_)),
    session_repository_(std::make_shared<RedisSessionRepository>(redis_manager_))
{
    dispatcher_.RegisterHandler(MessageType::LOGIN_REQUEST, std::make_unique<LoginHandler>(*user_manager_, *this));
    dispatcher_.RegisterHandler(MessageType::REGISTER_REQUEST, std::make_unique<RegisterHandler>(*user_manager_, *this));
    dispatcher_.RegisterHandler(MessageType::CHAT_MESSAGE, std::make_unique<ChatMessageHandler>(*this));
    dispatcher_.RegisterHandler(MessageType::CREATE_ROOM_REQUEST, std::make_unique<CreateRoomHandler>(*this));
    dispatcher_.RegisterHandler(MessageType::ROOM_LIST_REQUEST, std::make_unique<RoomListHandler>(*this));
    dispatcher_.RegisterHandler(MessageType::JOIN_ROOM, std::make_unique<JoinRoomHandler>(*user_manager_, *this));
    dispatcher_.RegisterHandler(MessageType::LEAVE_ROOM, std::make_unique<LeaveRoomHandler>(*this));
    dispatcher_.RegisterHandler(MessageType::WHISPER_REQUEST, std::make_unique<WhisperHandler>(*user_manager_, *this));
    dispatcher_.RegisterHandler(MessageType::RECONNECT_REQUEST, std::make_unique<ReconnectHandler>(*user_manager_, *this));
    dispatcher_.RegisterHandler(MessageType::KICK_USER_REQUEST, std::make_unique<KickUserHandler>(*this));
    dispatcher_.RegisterHandler(MessageType::TRANSFER_MASTER_REQUEST, std::make_unique<TransferMasterHandler>(*this));
}

//=====================
// 메인 진입점
//=====================
int main()
{
    try
    {
        boost::asio::io_context io_context;
        auto server = std::make_shared<ChatServer>(io_context, 8080);

        if (!server->InitDB("172.31.43.246", "game_user", "rnjsghd123@", "game_db"))
        {
            std::cerr << "[Server] Failed to initialize Database. Exiting..." << std::endl;
            return 1;
        }

        co_spawn(io_context, server->InitRedisAsync("172.31.43.246", "6379"), detached);

        server->StartAccept();

        co_spawn(io_context, server->CreateRoomAsync("Lobby", 100), detached);

        std::cout << "[Server] Running on port 8080 (3 Asio Threads + 1 DB Thread)..." << std::endl;

        std::vector<std::thread> threads;
        for (int i = 0; i < 3; ++i)
        {
            threads.emplace_back([&io_context, i]() {
                std::cout << "[Asio Worker " << i + 1 << "] Started. Thread ID: "
                    << std::this_thread::get_id() << std::endl;
                io_context.run();
            });
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
