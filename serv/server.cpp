#include <boost/asio.hpp>
#include <boost/asio/ssl.hpp>
#include <boost/asio/co_spawn.hpp>
#include <boost/asio/detached.hpp>
#include <boost/asio/use_awaitable.hpp>
#include <boost/asio/experimental/channel.hpp>

// Protobuf 스트림 보안 헤더
#include <google/protobuf/io/coded_stream.h>
#include <google/protobuf/io/zero_copy_stream_impl_lite.h>

// gRPC DB 서버 통신용 헤더
#include <grpcpp/grpcpp.h>
#include "chat_db.pb.h"
#include "chat_db.grpc.pb.h"

// Protobuf 패킷 헤더
#include "chat_protocol.pb.h"

#include <memory>
#include <iostream>
#include <string>
#include <vector>
#include <unordered_map>
#include <cstring>
#include <thread>
#include <atomic>
#include <algorithm>
#include <chrono>
#include <random>
#include <functional>

using boost::asio::ip::tcp;
using boost::asio::awaitable;
using boost::asio::use_awaitable;
using boost::asio::co_spawn;
using boost::asio::detached;
namespace ssl = boost::asio::ssl;

//=====================
// 상수 및 보안 설정
//=====================
constexpr size_t MAX_PACKET_SIZE = 4 * 1024; // 4KB 제한

//=====================
// 메시지 타입 및 패킷 헤더
//=====================
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
    CHAT_HISTORY_REQUEST = 1012,
    CHAT_HISTORY_RESPONSE = 1013,
    SERVER_NOTIFICATION = 1014,
    REGISTER_REQUEST = 1015,
    REGISTER_RESPONSE = 1016,
    JOIN_ROOM_RESPONSE = 1017,
    LEAVE_ROOM_RESPONSE = 1018,
    WHISPER_REQUEST = 1019,
    WHISPER_RESPONSE = 1020,
    WHISPER_NOTIFICATION = 1021,
    KICK_USER_REQUEST = 1023,
    KICK_USER_RESPONSE = 1024,
    KICKED_NOTIFICATION = 1025,
    TRANSFER_MASTER_REQUEST = 1026,
    TRANSFER_MASTER_RESPONSE = 1027,
    MASTER_CHANGED_NOTIFICATION = 1028,
    PING = 1029,
    PONG = 1030
};

enum class RoomPermission : uint32_t
{
    NONE = 0,
    CHAT = 1 << 0,
    KICK_USER = 1 << 1,
    BAN_USER = 1 << 2,
    CHANGE_CONFIG = 1 << 3,
    DELEGATE_HOST = 1 << 4,

    MEMBER = CHAT,
    HOST = CHAT | KICK_USER | BAN_USER | CHANGE_CONFIG | DELEGATE_HOST
};

inline RoomPermission operator|(RoomPermission a, RoomPermission b) {
    return static_cast<RoomPermission>(static_cast<uint32_t>(a) | static_cast<uint32_t>(b));
}

inline bool HasPermission(RoomPermission user_perm, RoomPermission required_perm) {
    return (static_cast<uint32_t>(user_perm) & static_cast<uint32_t>(required_perm)) == static_cast<uint32_t>(required_perm);
}

#pragma pack(push, 1)
struct PacketHeader
{
    uint16_t packet_size;
    MessageType message_type;
    uint32_t user_id;
    uint32_t sequence_number;
};
#pragma pack(pop)

struct DBUserData
{
    uint32_t id;
    std::string username;
};

inline std::string GenerateReconnectToken() {
    static std::random_device rd;
    static std::mt19937 gen(rd());
    static std::uniform_int_distribution<> dis(0, 15);
    const char* hex_digits = "0123456789abcdef";

    std::string token;
    token.reserve(32);
    for (int i = 0; i < 32; ++i) token += hex_digits[dis(gen)];
    return token;
}

//=====================
// 패킷 직렬화 / 디코딩 헬퍼
//=====================
class PacketSerializer
{
public:
    template <typename T>
    static std::vector<char> Serialize(MessageType msg_type, uint32_t user_id, const T& proto_msg) {
        std::string payload;
        proto_msg.SerializeToString(&payload);

        PacketHeader header{};
        header.packet_size = static_cast<uint16_t>(sizeof(PacketHeader) + payload.size());
        header.message_type = msg_type;
        header.user_id = user_id;

        std::vector<char> send_buffer(header.packet_size);
        std::memcpy(send_buffer.data(), &header, sizeof(PacketHeader));
        if (!payload.empty()) {
            std::memcpy(send_buffer.data() + sizeof(PacketHeader), payload.data(), payload.size());
        }

        return send_buffer;
    }

    template <typename T>
    static bool ParseProtoStream(const char* payload, size_t payload_size, T& out_proto) {
        if (!payload && payload_size > 0) return false;
        if (payload_size == 0) return true;

        google::protobuf::io::ArrayInputStream array_stream(payload, static_cast<int>(payload_size));
        google::protobuf::io::CodedInputStream coded_stream(&array_stream);

        coded_stream.SetRecursionLimit(64);
        return out_proto.ParseFromCodedStream(&coded_stream);
    }
};

class ChatServer;

//=====================
// Ring Buffer 패킷 버퍼
//=====================
class RingPacketBuffer
{
public:
    explicit RingPacketBuffer(size_t capacity = 16 * 1024)
        : buffer_(capacity), capacity_(capacity), head_(0), tail_(0), size_(0) {}

    bool WriteData(const char* data, size_t len)
    {
        if (capacity_ - size_ < len) return false;
        size_t first_part = std::min(len, capacity_ - tail_);
        std::memcpy(&buffer_[tail_], data, first_part);
        size_t second_part = len - first_part;
        if (second_part > 0) {
            std::memcpy(&buffer_[0], data + first_part, second_part);
        }
        tail_ = (tail_ + len) % capacity_;
        size_ += len;
        return true;
    }

    int ReadPacket(std::vector<char>& out_packet)
    {
        if (size_ < sizeof(PacketHeader)) return 0;

        PacketHeader header;
        PeekBytes(reinterpret_cast<char*>(&header), sizeof(PacketHeader));

        if (header.packet_size > MAX_PACKET_SIZE || header.packet_size < sizeof(PacketHeader)) {
            std::cerr << "[Security] Malformed or Oversized Packet size: " << header.packet_size << std::endl;
            return -1;
        }

        if (size_ < header.packet_size) return 0;

        out_packet.resize(header.packet_size);
        ReadBytes(out_packet.data(), header.packet_size);
        return 1;
    }

private:
    void PeekBytes(char* dest, size_t len) const
    {
        size_t first_part = std::min(len, capacity_ - head_);
        std::memcpy(dest, &buffer_[head_], first_part);
        if (len > first_part) {
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
    size_t capacity_, head_, tail_, size_;
};

//=====================
// gRPC Repositories
//=====================
class UserRepository
{
public:
    struct AuthResult {
        bool success{ false };
        DBUserData user_data;
        std::string reconnect_token;
        std::string error_msg;
    };

    struct RegisterResult {
        bool success{ false };
        uint32_t assigned_id{ 0 };
        std::string error_msg;
    };

    struct VerifyTokenResult {
        bool success{ false };
        uint32_t user_id{ 0 };
        std::string username;
        std::string error_msg;
    };

    explicit UserRepository(std::shared_ptr<grpc::Channel> channel)
        : stub_(chatdb::ChatDBService::NewStub(channel)) {}

    awaitable<AuthResult> AuthenticateUserAsync(std::string username, std::string password)
    {
        auto executor = co_await boost::asio::this_coro::executor;

        co_return co_await boost::asio::async_initiate<decltype(use_awaitable), void(AuthResult)>(
            [this, username, password, executor](auto handler) mutable {
                auto context = std::make_shared<grpc::ClientContext>();
                auto req = std::make_shared<chatdb::AuthRequest>();
                req->set_username(username);
                req->set_password(password);

                auto res = std::make_shared<chatdb::AuthResponse>();
                auto handler_ptr = std::make_shared<decltype(handler)>(std::move(handler));

                stub_->async()->AuthenticateUser(context.get(), req.get(), res.get(),
                    [executor, username, res, handler_ptr](grpc::Status status) mutable {
                        AuthResult result;
                        if (status.ok() && res->success()) {
                            result.success = true;
                            result.user_data.id = res->user_id();
                            result.user_data.username = username;
                            result.reconnect_token = res->reconnect_token();
                        } else {
                            result.success = false;
                            result.error_msg = !res->error_message().empty() ? res->error_message() : status.error_message();
                        }

                        boost::asio::post(executor, [handler_ptr, result = std::move(result)]() mutable {
                            (*handler_ptr)(result);
                        });
                    });
            },
            use_awaitable
        );
    }

    awaitable<RegisterResult> RegisterUserAsync(std::string username, std::string password)
    {
        auto executor = co_await boost::asio::this_coro::executor;

        co_return co_await boost::asio::async_initiate<decltype(use_awaitable), void(RegisterResult)>(
            [this, username, password, executor](auto handler) mutable {
                auto context = std::make_shared<grpc::ClientContext>();
                auto req = std::make_shared<chatdb::RegisterRequest>();
                req->set_username(username);
                req->set_password(password);

                auto res = std::make_shared<chatdb::RegisterResponse>();
                auto handler_ptr = std::make_shared<decltype(handler)>(std::move(handler));

                stub_->async()->RegisterUser(context.get(), req.get(), res.get(),
                    [executor, res, handler_ptr](grpc::Status status) mutable {
                        RegisterResult result;
                        if (status.ok() && res->success()) {
                            result.success = true;
                            result.assigned_id = res->assigned_id();
                        } else {
                            result.success = false;
                            result.error_msg = !res->error_message().empty() ? res->error_message() : status.error_message();
                        }

                        boost::asio::post(executor, [handler_ptr, result = std::move(result)]() mutable {
                            (*handler_ptr)(result);
                        });
                    });
            },
            use_awaitable
        );
    }

    awaitable<VerifyTokenResult> VerifyTokenAsync(std::string token)
    {
        auto executor = co_await boost::asio::this_coro::executor;

        co_return co_await boost::asio::async_initiate<decltype(use_awaitable), void(VerifyTokenResult)>(
            [this, token, executor](auto handler) mutable {
                auto context = std::make_shared<grpc::ClientContext>();
                auto req = std::make_shared<chatdb::VerifyTokenRequest>();
                req->set_token(token);

                auto res = std::make_shared<chatdb::VerifyTokenResponse>();
                auto handler_ptr = std::make_shared<decltype(handler)>(std::move(handler));

                stub_->async()->VerifyToken(context.get(), req.get(), res.get(),
                    [executor, res, handler_ptr](grpc::Status status) mutable {
                        VerifyTokenResult result;
                        if (status.ok() && res->success()) {
                            result.success = true;
                            result.user_id = res->user_id();
                            result.username = res->username();
                        } else {
                            result.success = false;
                            result.error_msg = !res->error_message().empty() ? res->error_message() : status.error_message();
                        }

                        boost::asio::post(executor, [handler_ptr, result = std::move(result)]() mutable {
                            (*handler_ptr)(result);
                        });
                    });
            },
            use_awaitable
        );
    }

private:
    std::unique_ptr<chatdb::ChatDBService::Stub> stub_;
};

class SessionRepository
{
public:
    explicit SessionRepository(std::shared_ptr<grpc::Channel> channel)
        : stub_(chatdb::ChatDBService::NewStub(channel)) {}

    awaitable<bool> SetUserSessionStateAsync(uint32_t user_id, const std::string& state, int ttl_seconds = 3600)
    {
        auto executor = co_await boost::asio::this_coro::executor;

        co_return co_await boost::asio::async_initiate<decltype(use_awaitable), void(bool)>(
            [this, user_id, state, ttl_seconds, executor](auto handler) mutable {
                auto context = std::make_shared<grpc::ClientContext>();
                auto req = std::make_shared<chatdb::SessionStateRequest>();
                req->set_user_id(user_id);
                req->set_state(state);
                req->set_ttl_seconds(ttl_seconds);

                auto res = std::make_shared<chatdb::SessionStateResponse>();
                auto handler_ptr = std::make_shared<decltype(handler)>(std::move(handler));

                stub_->async()->SetSessionState(context.get(), req.get(), res.get(),
                    [executor, res, handler_ptr](grpc::Status status) mutable {
                        bool success = status.ok() && res->success();
                        boost::asio::post(executor, [handler_ptr, success]() mutable {
                            (*handler_ptr)(success);
                        });
                    });
            },
            use_awaitable
        );
    }

private:
    std::unique_ptr<chatdb::ChatDBService::Stub> stub_;
};

class ChatRepository
{
public:
    struct ChatHistoryResult {
        bool success{ false };
        std::vector<chatdb::ChatMessageData> messages;
        bool has_more{ false };
        std::string error_msg;
    };

    explicit ChatRepository(std::shared_ptr<grpc::Channel> channel)
        : stub_(chatdb::ChatDBService::NewStub(channel)) {}

    awaitable<bool> PublishChatAsync(uint32_t room_id, uint32_t user_id, const std::string& message, int64_t timestamp)
    {
        auto executor = co_await boost::asio::this_coro::executor;

        co_return co_await boost::asio::async_initiate<decltype(use_awaitable), void(bool)>(
            [this, room_id, user_id, message, timestamp, executor](auto handler) mutable {
                auto context = std::make_shared<grpc::ClientContext>();
                auto req = std::make_shared<chatdb::ChatPublishRequest>();
                req->set_room_id(room_id);
                req->set_user_id(user_id);
                req->set_message(message);
                req->set_timestamp(timestamp);

                auto res = std::make_shared<chatdb::ChatPublishResponse>();
                auto handler_ptr = std::make_shared<decltype(handler)>(std::move(handler));

                stub_->async()->PublishChat(context.get(), req.get(), res.get(),
                    [executor, res, handler_ptr](grpc::Status status) mutable {
                        bool success = status.ok() && res->success();
                        boost::asio::post(executor, [handler_ptr, success]() mutable {
                            (*handler_ptr)(success);
                        });
                    });
            },
            use_awaitable
        );
    }

    awaitable<ChatHistoryResult> GetChatHistoryAsync(uint32_t room_id, uint64_t last_msg_id, uint32_t limit)
    {
        auto executor = co_await boost::asio::this_coro::executor;

        co_return co_await boost::asio::async_initiate<decltype(use_awaitable), void(ChatHistoryResult)>(
            [this, room_id, last_msg_id, limit, executor](auto handler) mutable {
                auto context = std::make_shared<grpc::ClientContext>();
                auto req = std::make_shared<chatdb::ChatHistoryRequest>();
                req->set_room_id(room_id);
                req->set_last_message_id(last_msg_id);
                req->set_limit(limit);

                auto res = std::make_shared<chatdb::ChatHistoryResponse>();
                auto handler_ptr = std::make_shared<decltype(handler)>(std::move(handler));

                stub_->async()->GetChatHistory(context.get(), req.get(), res.get(),
                    [executor, req, res, handler_ptr](grpc::Status status) mutable {
                        ChatHistoryResult result;
                        if (status.ok() && res->success()) {
                            result.success = true;
                            result.has_more = (res->messages_size() >= static_cast<int>(req->limit()));
                            for (const auto& msg : res->messages()) {
                                result.messages.push_back(msg);
                            }
                        } else {
                            result.success = false;
                            result.error_msg = status.error_message();
                        }

                        boost::asio::post(executor, [handler_ptr, result = std::move(result)]() mutable {
                            (*handler_ptr)(result);
                        });
                    });
            },
            use_awaitable
        );
    }

private:
    std::unique_ptr<chatdb::ChatDBService::Stub> stub_;
};

//=====================
// 세션 클래스
//=====================
using MessageChannel = boost::asio::experimental::channel<void(boost::system::error_code, std::vector<char>)>;

class ChatSession : public std::enable_shared_from_this<ChatSession>
{
public:
    ChatSession(tcp::socket socket, ssl::context& ssl_ctx, ChatServer& server)
        : strand_(boost::asio::make_strand(socket.get_executor())),
          ssl_socket_(std::move(socket), ssl_ctx), server_(server), user_id_(0), room_id_(0),
          is_authenticated_(false), is_disconnected_(false),
          write_channel_(strand_, 100), idle_timer_(strand_) {}

    ~ChatSession() { Disconnect(); }

    boost::asio::strand<boost::asio::any_io_executor>& GetStrand() { return strand_; }

    void Start()
    {
        co_spawn(strand_, [this, self = shared_from_this()]() -> awaitable<void> {
            try {
                co_await ssl_socket_.async_handshake(ssl::stream_base::server, use_awaitable);
                StartIdleTimer();
                co_spawn(strand_, WriteLoop(), detached);

                PacketHeader prompt_header{};
                prompt_header.packet_size = sizeof(PacketHeader);
                prompt_header.message_type = MessageType::LOGIN_PROMPT;
                Send(&prompt_header, sizeof(PacketHeader));

                co_await ReadLoop();
            } catch (const std::exception& e) {
                std::cerr << "[SSL Handshake/Start Error] " << e.what() << std::endl;
                Disconnect();
            }
        }, detached);
    }

    template <typename T>
    void Send(MessageType msg_type, const T& proto_msg) {
        auto packet = PacketSerializer::Serialize(msg_type, user_id_, proto_msg);
        SendMessageRaw(packet.data(), packet.size());
    }

    void Send(const void* data, size_t size) {
        SendMessageRaw(data, size);
    }

    void SetUserId(uint32_t id) { user_id_ = id; }
    uint32_t GetUserId() const { return user_id_; }
    void SetRoomId(uint32_t room_id) { room_id_ = room_id; }
    uint32_t GetRoomId() const { return room_id_; }
    void SetAuthenticated(bool auth) { is_authenticated_ = auth; }
    bool IsAuthenticated() const { return is_authenticated_; }
    void Disconnect();

private:
    void SendMessageRaw(const void* data, size_t size)
    {
        boost::asio::post(strand_, [this, self = shared_from_this(), msg_data = std::vector<char>(static_cast<const char*>(data), static_cast<const char*>(data) + size)]() mutable {
            if (is_disconnected_.load()) return;
            write_channel_.try_send(boost::system::error_code{}, std::move(msg_data));
        });
    }

    void StartIdleTimer()
    {
        co_spawn(strand_, [this, self = shared_from_this()]() -> awaitable<void> {
            while (!is_disconnected_.load()) {
                boost::system::error_code ec;
                idle_timer_.expires_after(std::chrono::seconds(45));
                co_await idle_timer_.async_wait(boost::asio::redirect_error(use_awaitable, ec));
                if (!ec) {
                    Disconnect();
                    break;
                }
            }
        }, detached);
    }

    awaitable<void> ReadLoop()
    {
        try {
            while (!is_disconnected_.load()) {
                size_t length = co_await ssl_socket_.async_read_some(boost::asio::buffer(read_buffer_), use_awaitable);
                idle_timer_.expires_after(std::chrono::seconds(45));

                if (!packet_buffer_.WriteData(read_buffer_.data(), length)) {
                    Disconnect();
                    co_return;
                }

                std::vector<char> packet_data;
                while (true) {
                    int result = packet_buffer_.ReadPacket(packet_data);
                    if (result == 1) {
                        co_await ProcessPacketAsync(packet_data.data(), packet_data.size());
                    } else if (result == -1) {
                        Disconnect();
                        co_return;
                    } else {
                        break;
                    }
                }
            }
        } catch (...) {
            Disconnect();
        }
    }

    awaitable<void> WriteLoop()
    {
        try {
            while (!is_disconnected_.load()) {
                std::vector<char> msg = co_await write_channel_.async_receive(use_awaitable);
                if (is_disconnected_.load()) break;
                co_await boost::asio::async_write(ssl_socket_, boost::asio::buffer(msg.data(), msg.size()), use_awaitable);
            }
        } catch (...) {
            Disconnect();
        }
    }

    awaitable<void> ProcessPacketAsync(const char* data, size_t size);

    boost::asio::strand<boost::asio::any_io_executor> strand_;
    ssl::stream<tcp::socket> ssl_socket_;
    ChatServer& server_;
    uint32_t user_id_;
    uint32_t room_id_;
    bool is_authenticated_;
    std::atomic<bool> is_disconnected_{false};
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
        : strand_(boost::asio::make_strand(io_context)), id_(id), username_(username), is_online_(false) {}

    uint32_t GetId() const { return id_; }
    const std::string& GetUsername() const { return username_; }
    void SetOnline(bool online) { is_online_ = online; }
    bool IsOnline() const { return is_online_; }
    void SetSession(std::shared_ptr<ChatSession> session) { session_ = session; }
    std::weak_ptr<ChatSession> GetSession() const { return session_; }

private:
    boost::asio::strand<boost::asio::io_context::executor_type> strand_;
    uint32_t id_;
    std::string username_;
    bool is_online_;
    std::weak_ptr<ChatSession> session_;
};

//=====================
// 채팅방 (ChatRoom)
//=====================
class ChatRoom : public std::enable_shared_from_this<ChatRoom>
{
public:
    ChatRoom(boost::asio::io_context& io_context, uint32_t room_id, std::string name, uint32_t max_users)
        : strand_(boost::asio::make_strand(io_context)), room_id_(room_id), name_(std::move(name)), max_users_(max_users), owner_id_(0) {}

    uint32_t GetId() const { return room_id_; }
    std::string GetName() const { return name_; }
    uint32_t GetUserCount() const { return static_cast<uint32_t>(users_.size()); }
    uint32_t GetMaxUsers() const { return max_users_; }
    uint32_t GetOwnerId() const { return owner_id_; }

    awaitable<bool> AddUserAsync(std::shared_ptr<User> user, RoomPermission perm = RoomPermission::MEMBER) {
        co_await boost::asio::post(strand_, use_awaitable);
        if (users_.size() >= max_users_) co_return false;

        users_[user->GetId()] = user;
        permissions_[user->GetId()] = perm;

        if (owner_id_ == 0) {
            owner_id_ = user->GetId();
            permissions_[user->GetId()] = RoomPermission::HOST;
        }
        co_return true;
    }

    awaitable<bool> RemoveUserAsync(uint32_t user_id) {
        co_await boost::asio::post(strand_, use_awaitable);
        users_.erase(user_id);
        permissions_.erase(user_id);

        if (owner_id_ == user_id && !users_.empty()) {
            uint32_t new_owner_id = users_.begin()->first;
            owner_id_ = new_owner_id;
            permissions_[new_owner_id] = RoomPermission::HOST;

            chat::MasterChangedNotification noti;
            noti.set_room_id(room_id_);
            noti.set_new_master_id(new_owner_id);
            BroadcastMessage(MessageType::MASTER_CHANGED_NOTIFICATION, noti);
        }
        co_return true;
    }

    awaitable<bool> HasUserAsync(uint32_t user_id) {
        co_await boost::asio::post(strand_, use_awaitable);
        co_return users_.find(user_id) != users_.end();
    }

    awaitable<bool> KickUserAsync(uint32_t operator_id, uint32_t target_id) {
        co_await boost::asio::post(strand_, use_awaitable);
        auto op_perm = permissions_[operator_id];
        if (!HasPermission(op_perm, RoomPermission::KICK_USER)) co_return false;

        auto it = users_.find(target_id);
        if (it != users_.end()) {
            if (auto session = it->second->GetSession().lock()) {
                chat::KickedNotification noti;
                noti.set_room_id(room_id_);
                noti.set_reason("Kicked by room master");
                session->Send(MessageType::KICKED_NOTIFICATION, noti);
                session->SetRoomId(0);
            }
            users_.erase(it);
            permissions_.erase(target_id);
            co_return true;
        }
        co_return false;
    }

    awaitable<bool> TransferMasterAsync(uint32_t operator_id, uint32_t new_master_id) {
        co_await boost::asio::post(strand_, use_awaitable);
        if (owner_id_ != operator_id) co_return false;
        if (users_.find(new_master_id) == users_.end()) co_return false;

        permissions_[owner_id_] = RoomPermission::MEMBER;
        owner_id_ = new_master_id;
        permissions_[new_master_id] = RoomPermission::HOST;

        chat::MasterChangedNotification noti;
        noti.set_room_id(room_id_);
        noti.set_new_master_id(new_master_id);

        BroadcastMessage(MessageType::MASTER_CHANGED_NOTIFICATION, noti);
        co_return true;
    }

    template <typename T>
    void BroadcastMessage(MessageType msg_type, const T& proto_msg) {
        boost::asio::post(strand_, [this, self = shared_from_this(), msg_type, proto_msg]() {
            for (auto& [id, user] : users_) {
                if (auto session = user->GetSession().lock()) {
                    session->Send(msg_type, proto_msg);
                }
            }
        });
    }

private:
    boost::asio::strand<boost::asio::io_context::executor_type> strand_;
    uint32_t room_id_;
    std::string name_;
    uint32_t max_users_;
    uint32_t owner_id_;
    std::unordered_map<uint32_t, std::shared_ptr<User>> users_;
    std::unordered_map<uint32_t, RoomPermission> permissions_;
};

//=====================
// 매니저 클래스들
//=====================
class RoomManager
{
public:
    explicit RoomManager(boost::asio::io_context& io_context)
        : io_context_(io_context), next_room_id_(1) {}

    std::shared_ptr<ChatRoom> CreateRoom(const std::string& name, uint32_t max_users) {
        uint32_t id = next_room_id_++;
        auto room = std::make_shared<ChatRoom>(io_context_, id, name, max_users);
        rooms_[id] = room;
        return room;
    }

    std::shared_ptr<ChatRoom> GetRoom(uint32_t room_id) {
        auto it = rooms_.find(room_id);
        return (it != rooms_.end()) ? it->second : nullptr;
    }

    void DestroyRoom(uint32_t room_id) {
        rooms_.erase(room_id);
    }

    std::vector<chat::RoomInfo> GetRoomList() {
        std::vector<chat::RoomInfo> list;
        for (auto& [id, room] : rooms_) {
            chat::RoomInfo info;
            info.set_room_id(room->GetId());
            info.set_room_name(room->GetName());
            info.set_current_users(room->GetUserCount());
            info.set_max_users(room->GetMaxUsers());
            info.set_owner_id(room->GetOwnerId());
            list.push_back(info);
            if (list.size() >= 16) break;
        }
        return list;
    }

private:
    boost::asio::io_context& io_context_;
    std::atomic<uint32_t> next_room_id_;
    std::unordered_map<uint32_t, std::shared_ptr<ChatRoom>> rooms_;
};

class UserManager
{
public:
    explicit UserManager(boost::asio::io_context& io_context) : io_context_(io_context) {}

    std::shared_ptr<User> GetOrCreateUser(uint32_t user_id, const std::string& username) {
        auto it = users_by_id_.find(user_id);
        if (it != users_by_id_.end()) return it->second;

        auto user = std::make_shared<User>(io_context_, user_id, username);
        users_by_id_[user_id] = user;
        users_by_name_[username] = user;
        return user;
    }

    std::shared_ptr<User> GetUserById(uint32_t user_id) {
        auto it = users_by_id_.find(user_id);
        return (it != users_by_id_.end()) ? it->second : nullptr;
    }

    std::shared_ptr<User> GetUserByName(const std::string& username) {
        auto it = users_by_name_.find(username);
        return (it != users_by_name_.end()) ? it->second : nullptr;
    }

private:
    boost::asio::io_context& io_context_;
    std::unordered_map<uint32_t, std::shared_ptr<User>> users_by_id_;
    std::unordered_map<std::string, std::shared_ptr<User>> users_by_name_;
};

//=====================
// 디스패처
//=====================
class MessageDispatcher
{
public:
    template <typename T, typename HandlerFunc>
    void RegisterHandler(MessageType type, HandlerFunc handler) {
        handlers_[type] = [handler, type](std::shared_ptr<ChatSession> session, const char* payload, size_t payload_size) -> awaitable<void> {
            T proto_msg;

            if (!PacketSerializer::ParseProtoStream(payload, payload_size, proto_msg)) {
                std::cerr << "[Dispatcher Error] Failed to parse proto stream for MessageType: " 
                          << static_cast<uint16_t>(type) << std::endl;
                co_return;
            }

            co_await handler(session, proto_msg);
        };
    }

    template <typename HandlerFunc>
    void RegisterRawHandler(MessageType type, HandlerFunc handler) {
        handlers_[type] = [handler](std::shared_ptr<ChatSession> session, const char* /*payload*/, size_t /*payload_size*/) -> awaitable<void> {
            co_await handler(session);
        };
    }

    awaitable<void> DispatchMessageAsync(std::shared_ptr<ChatSession> session, const PacketHeader& header, const char* payload, size_t payload_size) {
        auto it = handlers_.find(header.message_type);
        if (it != handlers_.end()) {
            co_await it->second(session, payload, payload_size);
        } else {
            std::cerr << "[Dispatcher Error] Unhandled MessageType: " 
                      << static_cast<uint16_t>(header.message_type) << std::endl;
        }
    }

private:
    using InternalAsyncHandler = std::function<awaitable<void>(std::shared_ptr<ChatSession>, const char*, size_t)>;
    std::unordered_map<MessageType, InternalAsyncHandler> handlers_;
};

//=====================
// ChatServer 클래스
//=====================
class ChatServer : public std::enable_shared_from_this<ChatServer>
{
public:
    ChatServer(boost::asio::io_context& io_context, ssl::context& ssl_ctx, short port, const std::string& go_grpc_addr)
        : io_context_(io_context),
          ssl_ctx_(ssl_ctx),
          acceptor_(io_context, tcp::endpoint(tcp::v4(), port)),
          user_manager_(std::make_shared<UserManager>(io_context)),
          room_manager_(std::make_shared<RoomManager>(io_context))
    {
        auto channel = grpc::CreateChannel(go_grpc_addr, grpc::InsecureChannelCredentials());
        user_repository_ = std::make_shared<UserRepository>(channel);
        session_repository_ = std::make_shared<SessionRepository>(channel);
        chat_repository_ = std::make_shared<ChatRepository>(channel);

        InitHandlers();
    }

    boost::asio::io_context& GetIOContext() { return io_context_; }
    MessageDispatcher& GetDispatcher() { return dispatcher_; }
    UserManager& GetUserManager() { return *user_manager_; }
    RoomManager& GetRoomManager() { return *room_manager_; }
    std::shared_ptr<UserRepository> GetUserRepository() { return user_repository_; }
    std::shared_ptr<SessionRepository> GetSessionRepository() { return session_repository_; }
    std::shared_ptr<ChatRepository> GetChatRepository() { return chat_repository_; }

    void StartAccept()
    {
        co_spawn(acceptor_.get_executor(), [this, self = shared_from_this()]() -> awaitable<void> {
            while (true) {
                tcp::socket socket = co_await acceptor_.async_accept(use_awaitable);
                auto session = std::make_shared<ChatSession>(std::move(socket), ssl_ctx_, *this);
                session->Start();
            }
        }, detached);
    }

private:
    void InitHandlers();

    boost::asio::io_context& io_context_;
    ssl::context& ssl_ctx_;
    tcp::acceptor acceptor_;
    MessageDispatcher dispatcher_;
    std::shared_ptr<UserManager> user_manager_;
    std::shared_ptr<RoomManager> room_manager_;
    std::shared_ptr<UserRepository> user_repository_;
    std::shared_ptr<SessionRepository> session_repository_;
    std::shared_ptr<ChatRepository> chat_repository_;
};

void ChatSession::Disconnect()
{
    if (is_disconnected_.exchange(true)) return;

    if (user_id_ != 0) {
        uint32_t cur_user_id = user_id_;
        uint32_t cur_room_id = room_id_;
        user_id_ = 0;
        room_id_ = 0;

        co_spawn(server_.GetIOContext(), [server_ptr = &server_, cur_user_id, cur_room_id]() -> awaitable<void> {
            if (auto user = server_ptr->GetUserManager().GetUserById(cur_user_id)) {
                user->SetOnline(false);
            }

            if (cur_room_id != 0) {
                auto room = server_ptr->GetRoomManager().GetRoom(cur_room_id);
                if (room) {
                    co_await room->RemoveUserAsync(cur_user_id);
                    if (room->GetUserCount() == 0) {
                        server_ptr->GetRoomManager().DestroyRoom(cur_room_id);
                        std::cout << "[Room Cleanup] #" << cur_room_id << "번 방의 모든 유저가 나갔으므로 방을 파괴했습니다.\n";
                    }
                }
            }

            co_await server_ptr->GetSessionRepository()->SetUserSessionStateAsync(cur_user_id, "OFFLINE");
        }, detached);
    }

    boost::system::error_code ec;
    idle_timer_.cancel(ec);
    write_channel_.close();
    ssl_socket_.lowest_layer().close(ec);
}

awaitable<void> ChatSession::ProcessPacketAsync(const char* data, size_t size)
{
    if (size < sizeof(PacketHeader)) co_return;

    PacketHeader header;
    std::memcpy(&header, data, sizeof(PacketHeader));

    const char* payload = data + sizeof(PacketHeader);
    size_t payload_size = size - sizeof(PacketHeader);

    co_await server_.GetDispatcher().DispatchMessageAsync(shared_from_this(), header, payload, payload_size);
}

//=====================
// 핸들러 모음
//=====================
class ChatHandlers
{
public: 
    static awaitable<void> HandleLogin(ChatServer& server, std::shared_ptr<ChatSession> session, const chat::LoginRequest& req)
    {
        chat::LoginResponse res;
        uint32_t user_id = 0;

        if (!req.reconnect_token().empty()) {
            auto verify_res = co_await server.GetUserRepository()->VerifyTokenAsync(req.reconnect_token());

            if (verify_res.success) {
                user_id = verify_res.user_id;
                auto user = server.GetUserManager().GetOrCreateUser(user_id, verify_res.username);
                user->SetSession(session);
                user->SetOnline(true);
                session->SetUserId(user_id);
                session->SetAuthenticated(true);

                res.set_success(true);
                res.set_assigned_user_id(user_id);
                res.set_reconnect_token(req.reconnect_token());
                session->Send(MessageType::LOGIN_RESPONSE, res);
                co_return;
            }
        }

        auto auth_result = co_await server.GetUserRepository()->AuthenticateUserAsync(req.username(), req.password());

        if (auth_result.success) {
            user_id = auth_result.user_data.id;
            auto user = server.GetUserManager().GetOrCreateUser(user_id, req.username());
            user->SetSession(session);
            user->SetOnline(true);
            session->SetUserId(user_id);
            session->SetAuthenticated(true);

            co_await server.GetSessionRepository()->SetUserSessionStateAsync(user_id, "ONLINE");

            res.set_success(true);
            res.set_assigned_user_id(user_id);
            res.set_reconnect_token(auth_result.reconnect_token);
        } else {
            res.set_success(false);
            res.set_error_message(auth_result.error_msg);
        }

        session->Send(MessageType::LOGIN_RESPONSE, res);
    }

    static awaitable<void> HandleRegister(ChatServer& server, std::shared_ptr<ChatSession> session, const chat::RegisterRequest& req)
    {
        auto reg_result = co_await server.GetUserRepository()->RegisterUserAsync(req.username(), req.password());

        chat::RegisterResponse res;
        if (reg_result.success) {
            res.set_success(true);
            res.set_assigned_user_id(reg_result.assigned_id);
        } else {
            res.set_success(false);
            res.set_error_message(reg_result.error_msg);
        }

        session->Send(MessageType::REGISTER_RESPONSE, res);
    }

    static awaitable<void> HandleCreateRoom(ChatServer& server, std::shared_ptr<ChatSession> session, const chat::CreateRoomRequest& req)
    {
        if (!session->IsAuthenticated()) co_return;

        auto room = server.GetRoomManager().CreateRoom(req.room_name(), req.max_users());
        auto user = server.GetUserManager().GetUserById(session->GetUserId());

        chat::CreateRoomResponse res;
        if (room && user) {
            co_await room->AddUserAsync(user, RoomPermission::HOST);
            session->SetRoomId(room->GetId());
            res.set_success(true);
            res.set_created_room_id(room->GetId());
            res.set_owner_id(user->GetId());
        } else {
            res.set_success(false);
            res.set_error_message("ROOM_CREATE_FAILED");
        }

        session->Send(MessageType::CREATE_ROOM_RESPONSE, res);
    }

    static awaitable<void> HandleRoomList(ChatServer& server, std::shared_ptr<ChatSession> session, const chat::RoomListRequest& /*req*/)
    {
        if (!session->IsAuthenticated()) co_return;

        auto rooms = server.GetRoomManager().GetRoomList();

        chat::RoomListResponse res;
        for (const auto& room_info : rooms) {
            *res.add_rooms() = room_info;
        }

        session->Send(MessageType::ROOM_LIST_RESPONSE, res);
    }

    static awaitable<void> HandleJoinRoom(ChatServer& server, std::shared_ptr<ChatSession> session, const chat::JoinRoomRequest& req)
    {
        if (!session->IsAuthenticated()) co_return;

        auto room = server.GetRoomManager().GetRoom(req.room_id());
        auto user = server.GetUserManager().GetUserById(session->GetUserId());

        chat::JoinRoomResponse res;
        if (room && user && co_await room->AddUserAsync(user, RoomPermission::MEMBER)) {
            session->SetRoomId(room->GetId());
            res.set_success(true);
            res.set_room_id(room->GetId());
            res.set_owner_id(room->GetOwnerId());
        } else {
            res.set_success(false);
            res.set_error_message("JOIN_FAILED_OR_FULL");
        }

        session->Send(MessageType::JOIN_ROOM_RESPONSE, res);
    }

    static awaitable<void> HandleLeaveRoom(ChatServer& server, std::shared_ptr<ChatSession> session, const chat::LeaveRoomRequest& req)
    {
        if (!session->IsAuthenticated()) co_return;

        auto room = server.GetRoomManager().GetRoom(req.room_id());

        chat::LeaveRoomResponse res;
        if (room && co_await room->RemoveUserAsync(session->GetUserId())) {
            session->SetRoomId(0);
            if (room->GetUserCount() == 0) {
                server.GetRoomManager().DestroyRoom(room->GetId());
            }
            res.set_success(true);
        } else {
            res.set_success(false);
            res.set_error_message("LEAVE_FAILED");
        }

        session->Send(MessageType::LEAVE_ROOM_RESPONSE, res);
    }

    static awaitable<void> HandleChatMessage(ChatServer& server, std::shared_ptr<ChatSession> session, const chat::ChatMessage& msg_param)
    {
        if (!session->IsAuthenticated()) co_return;

        chat::ChatMessage msg = msg_param;
        auto room = server.GetRoomManager().GetRoom(msg.room_id());
        auto user = server.GetUserManager().GetUserById(session->GetUserId());

        if (room && user && co_await room->HasUserAsync(session->GetUserId())) {
            msg.set_sender_id(session->GetUserId());
            msg.set_sender_username(user->GetUsername());
            room->BroadcastMessage(MessageType::CHAT_MESSAGE, msg);

            co_spawn(session->GetStrand(), [server_ptr = &server, room_id = msg.room_id(), user_id = session->GetUserId(), text = msg.message(), ts = msg.timestamp()]() -> awaitable<void> {
                co_await server_ptr->GetChatRepository()->PublishChatAsync(room_id, user_id, text, ts);
            }, detached);
        }
    }

    static awaitable<void> HandleChatHistory(ChatServer& server, std::shared_ptr<ChatSession> session, const chat::ChatHistoryRequest& req)
    {
        if (!session->IsAuthenticated()) co_return;

        auto result = co_await server.GetChatRepository()->GetChatHistoryAsync(req.room_id(), req.last_message_id(), req.count());

        chat::ChatHistoryResponse res;
        res.set_room_id(req.room_id());

        if (result.success) {
            res.set_success(true);
            res.set_has_more(result.has_more);

            for (const auto& db_msg : result.messages) {
                auto* msg = res.add_messages();
                msg->set_message_id(db_msg.message_id());
                msg->set_room_id(db_msg.room_id());
                msg->set_sender_id(db_msg.sender_id());
                msg->set_sender_username(db_msg.sender_name());
                msg->set_message(db_msg.message());
                msg->set_timestamp(db_msg.timestamp());
            }
        } else {
            res.set_success(false);
            res.set_error_message(result.error_msg);
        }

        session->Send(MessageType::CHAT_HISTORY_RESPONSE, res);
    }

    static awaitable<void> HandleWhisper(ChatServer& server, std::shared_ptr<ChatSession> session, const chat::WhisperRequest& req)
    {
        if (!session->IsAuthenticated()) co_return;

        auto sender = server.GetUserManager().GetUserById(session->GetUserId());
        auto target = server.GetUserManager().GetUserByName(req.target_username());

        chat::WhisperResponse res;
        if (sender && target) {
            if (auto target_session = target->GetSession().lock()) {
                chat::WhisperNotification noti;
                noti.set_sender_username(sender->GetUsername());
                noti.set_message(req.message());

                target_session->Send(MessageType::WHISPER_NOTIFICATION, noti);
                res.set_success(true);
            } else {
                res.set_success(false);
                res.set_error_message("USER_OFFLINE");
            }
        } else {
            res.set_success(false);
            res.set_error_message("TARGET_NOT_FOUND");
        }

        session->Send(MessageType::WHISPER_RESPONSE, res);
    }

    static awaitable<void> HandleKickUser(ChatServer& server, std::shared_ptr<ChatSession> session, const chat::KickUserRequest& req)
    {
        if (!session->IsAuthenticated()) co_return;

        auto room = server.GetRoomManager().GetRoom(req.room_id());

        chat::KickUserResponse res;
        if (room && co_await room->KickUserAsync(session->GetUserId(), req.target_user_id())) {
            res.set_success(true);
        } else {
            res.set_success(false);
            res.set_error_message("KICK_PERMISSION_DENIED_OR_NO_USER");
        }

        session->Send(MessageType::KICK_USER_RESPONSE, res);
    }

    static awaitable<void> HandleTransferMaster(ChatServer& server, std::shared_ptr<ChatSession> session, const chat::TransferMasterRequest& req)
    {
        if (!session->IsAuthenticated()) co_return;

        auto room = server.GetRoomManager().GetRoom(req.room_id());

        chat::TransferMasterResponse res;
        if (room && co_await room->TransferMasterAsync(session->GetUserId(), req.new_master_id())) {
            res.set_success(true);
        } else {
            res.set_success(false);
            res.set_error_message("TRANSFER_FAILED_NOT_HOST");
        }

        session->Send(MessageType::TRANSFER_MASTER_RESPONSE, res);
    }

    static awaitable<void> HandlePing(std::shared_ptr<ChatSession> session)
    {
        PacketHeader pong_header{};
        pong_header.packet_size = sizeof(PacketHeader);
        pong_header.message_type = MessageType::PONG;
        pong_header.user_id = session->GetUserId();
        pong_header.sequence_number = 0;

        session->Send(&pong_header, sizeof(PacketHeader));
        co_return;
    }
};

void ChatServer::InitHandlers()
{
    dispatcher_.RegisterHandler<chat::LoginRequest>(MessageType::LOGIN_REQUEST, 
        [this](auto s, const auto& req) { return ChatHandlers::HandleLogin(*this, s, req); });

    dispatcher_.RegisterHandler<chat::RegisterRequest>(MessageType::REGISTER_REQUEST, 
        [this](auto s, const auto& req) { return ChatHandlers::HandleRegister(*this, s, req); });

    dispatcher_.RegisterHandler<chat::CreateRoomRequest>(MessageType::CREATE_ROOM_REQUEST, 
        [this](auto s, const auto& req) { return ChatHandlers::HandleCreateRoom(*this, s, req); });

    dispatcher_.RegisterHandler<chat::RoomListRequest>(MessageType::ROOM_LIST_REQUEST, 
        [this](auto s, const auto& req) { return ChatHandlers::HandleRoomList(*this, s, req); });

    dispatcher_.RegisterHandler<chat::JoinRoomRequest>(MessageType::JOIN_ROOM, 
        [this](auto s, const auto& req) { return ChatHandlers::HandleJoinRoom(*this, s, req); });

    dispatcher_.RegisterHandler<chat::LeaveRoomRequest>(MessageType::LEAVE_ROOM, 
        [this](auto s, const auto& req) { return ChatHandlers::HandleLeaveRoom(*this, s, req); });

    dispatcher_.RegisterHandler<chat::ChatMessage>(MessageType::CHAT_MESSAGE, 
        [this](auto s, const auto& req) { return ChatHandlers::HandleChatMessage(*this, s, req); });

    dispatcher_.RegisterHandler<chat::ChatHistoryRequest>(MessageType::CHAT_HISTORY_REQUEST, 
        [this](auto s, const auto& req) { return ChatHandlers::HandleChatHistory(*this, s, req); });

    dispatcher_.RegisterHandler<chat::WhisperRequest>(MessageType::WHISPER_REQUEST, 
        [this](auto s, const auto& req) { return ChatHandlers::HandleWhisper(*this, s, req); });

    dispatcher_.RegisterHandler<chat::KickUserRequest>(MessageType::KICK_USER_REQUEST, 
        [this](auto s, const auto& req) { return ChatHandlers::HandleKickUser(*this, s, req); });

    dispatcher_.RegisterHandler<chat::TransferMasterRequest>(MessageType::TRANSFER_MASTER_REQUEST, 
        [this](auto s, const auto& req) { return ChatHandlers::HandleTransferMaster(*this, s, req); });

    dispatcher_.RegisterRawHandler(MessageType::PING, 
        [](auto s) { return ChatHandlers::HandlePing(s); });
}

//=====================
// 메인 진입점
//=====================
int main()
{
    try {
        boost::asio::io_context io_context;

        // io_context의 이벤트 루프가 끊기지 않도록 Work Guard 등록
        auto work_guard = boost::asio::make_work_guard(io_context);

        ssl::context ssl_ctx(ssl::context::tlsv12_server);
        ssl_ctx.set_options(ssl::context::default_workarounds | ssl::context::no_sslv2 | ssl::context::single_dh_use);
        ssl_ctx.use_certificate_chain_file("server.crt");
        ssl_ctx.use_private_key_file("server.key", ssl::context::pem);

        auto server = std::make_shared<ChatServer>(io_context, ssl_ctx, 8080, "127.0.0.1:50051");
        server->StartAccept();

        std::cout << "[C++ SSL Chat Server] Listening on port 8080 (TLS 1.2 Encrypted)...\n";

        unsigned int threads_count = std::thread::hardware_concurrency();
        if (threads_count == 0) threads_count = 4;

        std::vector<std::thread> thread_pool;
        for (unsigned int i = 0; i < threads_count; ++i) {
            thread_pool.emplace_back([&io_context]() {
                io_context.run();
            });
        }

        std::cout << "[Unified Thread Pool] Total Workers: " << threads_count << "\n";

        for (auto& t : thread_pool) {
            if (t.joinable()) t.join();
        }
    } catch (const std::exception& e) {
        std::cerr << "Exception: " << e.what() << std::endl;
    }
    return 0;
}

