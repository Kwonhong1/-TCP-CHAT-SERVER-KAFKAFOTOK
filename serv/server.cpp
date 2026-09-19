#include <boost/asio.hpp>
#include <boost/asio/ssl.hpp>
#include <boost/asio/co_spawn.hpp>
#include <boost/asio/detached.hpp>
#include <boost/asio/use_awaitable.hpp>
#include <boost/asio/experimental/channel.hpp>

#include <google/protobuf/io/coded_stream.h>
#include <google/protobuf/io/zero_copy_stream_impl_lite.h>

#include <grpcpp/grpcpp.h>
#include "chat_db.pb.h"
#include "chat_db.grpc.pb.h"

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
#include <type_traits>

using boost::asio::ip::tcp;
using boost::asio::awaitable;
using boost::asio::use_awaitable;
using boost::asio::co_spawn;
using boost::asio::detached;

namespace ssl = boost::asio::ssl;

//==================================================
// 상수
//==================================================

constexpr size_t MAX_PACKET_SIZE = 4 * 1024;

//==================================================
// 메시지 타입
//==================================================

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

//==================================================
// 방 권한
//==================================================

enum class RoomPermission : uint32_t
{
    NONE = 0,

    CHAT = 1 << 0,
    KICK_USER = 1 << 1,
    BAN_USER = 1 << 2,
    CHANGE_CONFIG = 1 << 3,
    DELEGATE_HOST = 1 << 4,

    MEMBER = CHAT,

    HOST =
        CHAT |
        KICK_USER |
        BAN_USER |
        CHANGE_CONFIG |
        DELEGATE_HOST
};

inline RoomPermission operator|(
    RoomPermission a,
    RoomPermission b)
{
    return static_cast<RoomPermission>(
        static_cast<uint32_t>(a) |
        static_cast<uint32_t>(b)
    );
}

inline bool HasPermission(
    RoomPermission user_perm,
    RoomPermission required_perm)
{
    return
        (static_cast<uint32_t>(user_perm) &
         static_cast<uint32_t>(required_perm))
        ==
        static_cast<uint32_t>(required_perm);
}

//==================================================
// 패킷 헤더
//==================================================

#pragma pack(push, 1)

struct PacketHeader
{
    uint16_t packet_size;
    MessageType message_type;
    uint32_t user_id;
    uint32_t sequence_number;
};

#pragma pack(pop)

//==================================================
// DB User
//==================================================

struct DBUserData
{
    uint32_t id;
    std::string username;
};

//==================================================
// 기존 reconnect token 함수 유지
//==================================================

inline std::string GenerateReconnectToken()
{
    static std::random_device rd;
    static std::mt19937 gen(rd());
    static std::uniform_int_distribution<> dis(0, 15);

    const char* hex_digits = "0123456789abcdef";

    std::string token;
    token.reserve(32);

    for (int i = 0; i < 32; ++i)
        token += hex_digits[dis(gen)];

    return token;
}

//==================================================
// PacketSerializer
//==================================================

class PacketSerializer
{
public:

    template <typename T>
    static std::vector<char> Serialize(
        MessageType msg_type,
        uint32_t user_id,
        const T& proto_msg)
    {
        std::string payload;

        proto_msg.SerializeToString(&payload);

        PacketHeader header{};

        header.packet_size =
            static_cast<uint16_t>(
                sizeof(PacketHeader) + payload.size()
            );

        header.message_type = msg_type;
        header.user_id = user_id;
        header.sequence_number = 0;

        std::vector<char> send_buffer(
            header.packet_size
        );

        std::memcpy(
            send_buffer.data(),
            &header,
            sizeof(PacketHeader)
        );

        if (!payload.empty())
        {
            std::memcpy(
                send_buffer.data() + sizeof(PacketHeader),
                payload.data(),
                payload.size()
            );
        }

        return send_buffer;
    }

    template <typename T>
    static bool ParseProtoStream(
        const char* payload,
        size_t payload_size,
        T& out_proto)
    {
        if (!payload && payload_size > 0)
            return false;

        if (payload_size == 0)
            return true;

        google::protobuf::io::ArrayInputStream array_stream(
            payload,
            static_cast<int>(payload_size)
        );

        google::protobuf::io::CodedInputStream coded_stream(
            &array_stream
        );

        coded_stream.SetRecursionLimit(64);

        return out_proto.ParseFromCodedStream(
            &coded_stream
        );
    }
};

class ChatServer;
class ChatSession;
class User;
class ChatRoom;

//==================================================
// RingPacketBuffer
//==================================================

class RingPacketBuffer
{
public:

    explicit RingPacketBuffer(
        size_t capacity = 16 * 1024)
        :
        buffer_(capacity),
        capacity_(capacity),
        head_(0),
        tail_(0),
        size_(0)
    {
    }

    bool WriteData(
        const char* data,
        size_t len)
    {
        if (capacity_ - size_ < len)
            return false;

        size_t first_part =
            std::min(
                len,
                capacity_ - tail_
            );

        std::memcpy(
            &buffer_[tail_],
            data,
            first_part
        );

        size_t second_part =
            len - first_part;

        if (second_part > 0)
        {
            std::memcpy(
                &buffer_[0],
                data + first_part,
                second_part
            );
        }

        tail_ =
            (tail_ + len) % capacity_;

        size_ += len;

        return true;
    }

    int ReadPacket(
        std::vector<char>& out_packet)
    {
        if (size_ < sizeof(PacketHeader))
            return 0;

        PacketHeader header{};

        PeekBytes(
            reinterpret_cast<char*>(&header),
            sizeof(PacketHeader)
        );

        if (
            header.packet_size > MAX_PACKET_SIZE ||
            header.packet_size < sizeof(PacketHeader)
        )
        {
            std::cerr
                << "[Security] Malformed or Oversized Packet size: "
                << header.packet_size
                << std::endl;

            return -1;
        }

        if (size_ < header.packet_size)
            return 0;

        out_packet.resize(
            header.packet_size
        );

        ReadBytes(
            out_packet.data(),
            header.packet_size
        );

        return 1;
    }

private:

    void PeekBytes(
        char* dest,
        size_t len) const
    {
        size_t first_part =
            std::min(
                len,
                capacity_ - head_
            );

        std::memcpy(
            dest,
            &buffer_[head_],
            first_part
        );

        if (len > first_part)
        {
            std::memcpy(
                dest + first_part,
                &buffer_[0],
                len - first_part
            );
        }
    }

    void ReadBytes(
        char* dest,
        size_t len)
    {
        PeekBytes(dest, len);

        head_ =
            (head_ + len) % capacity_;

        size_ -= len;
    }

private:

    std::vector<char> buffer_;

    size_t capacity_;
    size_t head_;
    size_t tail_;
    size_t size_;
};

//==================================================
// UserRepository
//==================================================

class UserRepository
{
public:

    struct AuthResult
    {
        bool success{false};
        DBUserData user_data;
        std::string reconnect_token;
        std::string error_msg;
    };

    struct RegisterResult
    {
        bool success{false};
        uint32_t assigned_id{0};
        std::string error_msg;
    };

    struct VerifyTokenResult
    {
        bool success{false};
        uint32_t user_id{0};
        std::string username;
        std::string error_msg;
    };

    explicit UserRepository(
        std::shared_ptr<grpc::Channel> channel)
        :
        stub_(
            chatdb::ChatDBService::NewStub(channel)
        )
    {
    }

    //==================================================
    // [FIX] gRPC context/request/response lifetime 보장
    //==================================================

    awaitable<AuthResult> AuthenticateUserAsync(
        std::string username,
        std::string password)
    {
        auto executor =
            co_await boost::asio::this_coro::executor;

        co_return co_await boost::asio::async_initiate<
            decltype(use_awaitable),
            void(AuthResult)>(

            [this,
             username = std::move(username),
             password = std::move(password),
             executor](auto handler) mutable
            {
                auto context =
                    std::make_shared<
                        grpc::ClientContext>();

                auto req =
                    std::make_shared<
                        chatdb::AuthRequest>();

                req->set_username(username);
                req->set_password(password);

                auto res =
                    std::make_shared<
                        chatdb::AuthResponse>();

                using Handler =
                    std::decay_t<decltype(handler)>;

                auto handler_ptr =
                    std::make_shared<Handler>(
                        std::move(handler)
                    );

                stub_->async()->AuthenticateUser(
                    context.get(),
                    req.get(),
                    res.get(),

                    [executor,
                     username,
                     context,
                     req,
                     res,
                     handler_ptr]
                    (grpc::Status status) mutable
                    {
                        AuthResult result;

                        if (
                            status.ok() &&
                            res->success()
                        )
                        {
                            result.success = true;

                            result.user_data.id =
                                res->user_id();

                            result.user_data.username =
                                username;

                            result.reconnect_token =
                                res->reconnect_token();
                        }
                        else
                        {
                            result.success = false;

                            result.error_msg =
                                !res->error_message().empty()
                                ? res->error_message()
                                : status.error_message();
                        }

                        boost::asio::post(
                            executor,

                            [handler_ptr,
                             result = std::move(result)]
                            () mutable
                            {
                                (*handler_ptr)(
                                    std::move(result)
                                );
                            }
                        );
                    }
                );
            },

            use_awaitable
        );
    }

    awaitable<RegisterResult> RegisterUserAsync(
        std::string username,
        std::string password)
    {
        auto executor =
            co_await boost::asio::this_coro::executor;

        co_return co_await boost::asio::async_initiate<
            decltype(use_awaitable),
            void(RegisterResult)>(

            [this,
             username = std::move(username),
             password = std::move(password),
             executor](auto handler) mutable
            {
                auto context =
                    std::make_shared<
                        grpc::ClientContext>();

                auto req =
                    std::make_shared<
                        chatdb::RegisterRequest>();

                req->set_username(username);
                req->set_password(password);

                auto res =
                    std::make_shared<
                        chatdb::RegisterResponse>();

                using Handler =
                    std::decay_t<decltype(handler)>;

                auto handler_ptr =
                    std::make_shared<Handler>(
                        std::move(handler)
                    );

                stub_->async()->RegisterUser(
                    context.get(),
                    req.get(),
                    res.get(),

                    [executor,
                     context,
                     req,
                     res,
                     handler_ptr]
                    (grpc::Status status) mutable
                    {
                        RegisterResult result;

                        if (
                            status.ok() &&
                            res->success()
                        )
                        {
                            result.success = true;
                            result.assigned_id =
                                res->assigned_id();
                        }
                        else
                        {
                            result.success = false;

                            result.error_msg =
                                !res->error_message().empty()
                                ? res->error_message()
                                : status.error_message();
                        }

                        boost::asio::post(
                            executor,

                            [handler_ptr,
                             result = std::move(result)]
                            () mutable
                            {
                                (*handler_ptr)(
                                    std::move(result)
                                );
                            }
                        );
                    }
                );
            },

            use_awaitable
        );
    }

    awaitable<VerifyTokenResult> VerifyTokenAsync(
        std::string token)
    {
        auto executor =
            co_await boost::asio::this_coro::executor;

        co_return co_await boost::asio::async_initiate<
            decltype(use_awaitable),
            void(VerifyTokenResult)>(

            [this,
             token = std::move(token),
             executor](auto handler) mutable
            {
                auto context =
                    std::make_shared<
                        grpc::ClientContext>();

                auto req =
                    std::make_shared<
                        chatdb::VerifyTokenRequest>();

                req->set_token(token);

                auto res =
                    std::make_shared<
                        chatdb::VerifyTokenResponse>();

                using Handler =
                    std::decay_t<decltype(handler)>;

                auto handler_ptr =
                    std::make_shared<Handler>(
                        std::move(handler)
                    );

                stub_->async()->VerifyToken(
                    context.get(),
                    req.get(),
                    res.get(),

                    [executor,
                     context,
                     req,
                     res,
                     handler_ptr]
                    (grpc::Status status) mutable
                    {
                        VerifyTokenResult result;

                        if (
                            status.ok() &&
                            res->success()
                        )
                        {
                            result.success = true;
                            result.user_id =
                                res->user_id();
                            result.username =
                                res->username();
                        }
                        else
                        {
                            result.success = false;

                            result.error_msg =
                                !res->error_message().empty()
                                ? res->error_message()
                                : status.error_message();
                        }

                        boost::asio::post(
                            executor,

                            [handler_ptr,
                             result = std::move(result)]
                            () mutable
                            {
                                (*handler_ptr)(
                                    std::move(result)
                                );
                            }
                        );
                    }
                );
            },

            use_awaitable
        );
    }

private:

    std::unique_ptr<
        chatdb::ChatDBService::Stub
    > stub_;
};

//==================================================
// SessionRepository
//==================================================

class SessionRepository
{
public:

    explicit SessionRepository(
        std::shared_ptr<grpc::Channel> channel)
        :
        stub_(
            chatdb::ChatDBService::NewStub(channel)
        )
    {
    }

    awaitable<bool> SetUserSessionStateAsync(
        uint32_t user_id,
        const std::string& state,
        int ttl_seconds = 3600)
    {
        auto executor =
            co_await boost::asio::this_coro::executor;

        co_return co_await boost::asio::async_initiate<
            decltype(use_awaitable),
            void(bool)>(

            [this,
             user_id,
             state,
             ttl_seconds,
             executor](auto handler) mutable
            {
                auto context =
                    std::make_shared<
                        grpc::ClientContext>();

                auto req =
                    std::make_shared<
                        chatdb::SessionStateRequest>();

                req->set_user_id(user_id);
                req->set_state(state);
                req->set_ttl_seconds(ttl_seconds);

                auto res =
                    std::make_shared<
                        chatdb::SessionStateResponse>();

                using Handler =
                    std::decay_t<decltype(handler)>;

                auto handler_ptr =
                    std::make_shared<Handler>(
                        std::move(handler)
                    );

                stub_->async()->SetSessionState(
                    context.get(),
                    req.get(),
                    res.get(),

                    [executor,
                     context,
                     req,
                     res,
                     handler_ptr]
                    (grpc::Status status) mutable
                    {
                        bool success =
                            status.ok() &&
                            res->success();

                        boost::asio::post(
                            executor,

                            [handler_ptr,
                             success]() mutable
                            {
                                (*handler_ptr)(
                                    success
                                );
                            }
                        );
                    }
                );
            },

            use_awaitable
        );
    }

private:

    std::unique_ptr<
        chatdb::ChatDBService::Stub
    > stub_;
};

//==================================================
// ChatRepository
//==================================================

class ChatRepository
{
public:

    struct ChatHistoryResult
    {
        bool success{false};

        std::vector<
            chatdb::ChatMessageData
        > messages;

        bool has_more{false};
        std::string error_msg;
    };

    explicit ChatRepository(
        std::shared_ptr<grpc::Channel> channel)
        :
        stub_(
            chatdb::ChatDBService::NewStub(channel)
        )
    {
    }

    awaitable<bool> PublishChatAsync(
        uint32_t room_id,
        uint32_t user_id,
        const std::string& message,
        int64_t timestamp)
    {
        auto executor =
            co_await boost::asio::this_coro::executor;

        co_return co_await boost::asio::async_initiate<
            decltype(use_awaitable),
            void(bool)>(

            [this,
             room_id,
             user_id,
             message,
             timestamp,
             executor](auto handler) mutable
            {
                auto context =
                    std::make_shared<
                        grpc::ClientContext>();

                auto req =
                    std::make_shared<
                        chatdb::ChatPublishRequest>();

                req->set_room_id(room_id);
                req->set_user_id(user_id);
                req->set_message(message);
                req->set_timestamp(timestamp);

                auto res =
                    std::make_shared<
                        chatdb::ChatPublishResponse>();

                using Handler =
                    std::decay_t<decltype(handler)>;

                auto handler_ptr =
                    std::make_shared<Handler>(
                        std::move(handler)
                    );

                stub_->async()->PublishChat(
                    context.get(),
                    req.get(),
                    res.get(),

                    [executor,
                     context,
                     req,
                     res,
                     handler_ptr]
                    (grpc::Status status) mutable
                    {
                        bool success =
                            status.ok() &&
                            res->success();

                        boost::asio::post(
                            executor,

                            [handler_ptr,
                             success]() mutable
                            {
                                (*handler_ptr)(
                                    success
                                );
                            }
                        );
                    }
                );
            },

            use_awaitable
        );
    }

    awaitable<ChatHistoryResult>
    GetChatHistoryAsync(
        uint32_t room_id,
        uint64_t last_msg_id,
        uint32_t limit)
    {
        auto executor =
            co_await boost::asio::this_coro::executor;

        co_return co_await boost::asio::async_initiate<
            decltype(use_awaitable),
            void(ChatHistoryResult)>(

            [this,
             room_id,
             last_msg_id,
             limit,
             executor](auto handler) mutable
            {
                auto context =
                    std::make_shared<
                        grpc::ClientContext>();

                auto req =
                    std::make_shared<
                        chatdb::ChatHistoryRequest>();

                req->set_room_id(room_id);
                req->set_last_message_id(
                    last_msg_id
                );
                req->set_limit(limit);

                auto res =
                    std::make_shared<
                        chatdb::ChatHistoryResponse>();

                using Handler =
                    std::decay_t<decltype(handler)>;

                auto handler_ptr =
                    std::make_shared<Handler>(
                        std::move(handler)
                    );

                stub_->async()->GetChatHistory(
                    context.get(),
                    req.get(),
                    res.get(),

                    [executor,
                     context,
                     req,
                     res,
                     handler_ptr]
                    (grpc::Status status) mutable
                    {
                        ChatHistoryResult result;

                        if (
                            status.ok() &&
                            res->success()
                        )
                        {
                            result.success = true;

                            result.has_more =
                                res->messages_size() >=
                                static_cast<int>(
                                    req->limit()
                                );

                            result.messages.reserve(
                                res->messages_size()
                            );

                            for (
                                const auto& msg :
                                res->messages()
                            )
                            {
                                result.messages.push_back(
                                    msg
                                );
                            }
                        }
                        else
                        {
                            result.success = false;

                            result.error_msg =
                                !res->error_message().empty()
                                ? res->error_message()
                                : status.error_message();
                        }

                        boost::asio::post(
                            executor,

                            [handler_ptr,
                             result = std::move(result)]
                            () mutable
                            {
                                (*handler_ptr)(
                                    std::move(result)
                                );
                            }
                        );
                    }
                );
            },

            use_awaitable
        );
    }

private:

    std::unique_ptr<
        chatdb::ChatDBService::Stub
    > stub_;
};

//==================================================
// ChatSession
//==================================================

using MessageChannel =
    boost::asio::experimental::channel<
        void(
            boost::system::error_code,
            std::vector<char>
        )
    >;

class ChatSession :
    public std::enable_shared_from_this<ChatSession>
{
public:

    ChatSession(
        tcp::socket socket,
        ssl::context& ssl_ctx,
        ChatServer& server)
        :
        strand_(
            boost::asio::make_strand(
                socket.get_executor()
            )
        ),
        ssl_socket_(
            std::move(socket),
            ssl_ctx
        ),
        server_(server),
        user_id_(0),
        room_id_(0),
        is_authenticated_(false),
        is_disconnected_(false),
        write_channel_(strand_, 100),
        idle_timer_(strand_)
    {
    }

    ~ChatSession()
    {
        Disconnect();
    }

    boost::asio::strand<
        boost::asio::any_io_executor
    >& GetStrand()
    {
        return strand_;
    }

    void Start()
    {
        auto self = shared_from_this();

        co_spawn(
            strand_,

            [self]() -> awaitable<void>
            {
                try
                {
                    co_await
                        self->ssl_socket_
                            .async_handshake(
                                ssl::stream_base::server,
                                use_awaitable
                            );

                    self->StartIdleTimer();

                    co_spawn(
                        self->strand_,
                        self->WriteLoop(),
                        detached
                    );

                    PacketHeader prompt_header{};

                    prompt_header.packet_size =
                        sizeof(PacketHeader);

                    prompt_header.message_type =
                        MessageType::LOGIN_PROMPT;

                    prompt_header.user_id = 0;
                    prompt_header.sequence_number = 0;

                    self->Send(
                        &prompt_header,
                        sizeof(PacketHeader)
                    );

                    co_await self->ReadLoop();
                }
                catch (
                    const std::exception& e
                )
                {
                    std::cerr
                        << "[SSL Handshake/Start Error] "
                        << e.what()
                        << std::endl;

                    self->Disconnect();
                }
            },

            detached
        );
    }

    //--------------------------------------------------
    // session strand 내부에서 사용하는 빠른 API
    //--------------------------------------------------

    uint32_t GetUserId() const
    {
        return user_id_;
    }

    uint32_t GetRoomId() const
    {
        return room_id_;
    }

    bool IsAuthenticated() const
    {
        return is_authenticated_;
    }

    void SetUserId(uint32_t id)
    {
        user_id_ = id;
    }

    void SetRoomId(uint32_t room_id)
    {
        room_id_ = room_id;
    }

    void SetAuthenticated(bool auth)
    {
        is_authenticated_ = auth;
    }

    //--------------------------------------------------
    // [STRAND] 외부 객체가 Session 상태를 변경할 때 사용
    //--------------------------------------------------

    awaitable<void> SetRoomIdAsync(
        uint32_t room_id)
    {
        co_await boost::asio::dispatch(
            strand_,
            use_awaitable
        );

        room_id_ = room_id;
    }

    //--------------------------------------------------
    // Send는 어느 strand에서도 호출 가능
    //--------------------------------------------------

    template <typename T>
    void Send(
        MessageType msg_type,
        const T& proto_msg)
    {
        // user_id_를 다른 strand에서 읽지 않도록
        // 실제 packet 생성까지 session strand로 넘긴다.

        auto self = shared_from_this();

        boost::asio::post(
            strand_,

            [self,
             msg_type,
             proto_msg]() mutable
            {
                if (
                    self->is_disconnected_.load()
                )
                    return;

                auto packet =
                    PacketSerializer::Serialize(
                        msg_type,
                        self->user_id_,
                        proto_msg
                    );

                self->write_channel_.try_send(
                    boost::system::error_code{},
                    std::move(packet)
                );
            }
        );
    }

    void Send(
        const void* data,
        size_t size)
    {
        SendMessageRaw(
            data,
            size
        );
    }

    void Disconnect();

private:

    void SendMessageRaw(
        const void* data,
        size_t size)
    {
        auto self = shared_from_this();

        std::vector<char> msg_data(
            static_cast<const char*>(data),
            static_cast<const char*>(data) + size
        );

        boost::asio::post(
            strand_,

            [self,
             msg_data = std::move(msg_data)]
            () mutable
            {
                if (
                    self->is_disconnected_.load()
                )
                    return;

                self->write_channel_.try_send(
                    boost::system::error_code{},
                    std::move(msg_data)
                );
            }
        );
    }

    void StartIdleTimer()
    {
        auto self = shared_from_this();

        co_spawn(
            strand_,

            [self]() -> awaitable<void>
            {
                while (
                    !self->is_disconnected_.load()
                )
                {
                    boost::system::error_code ec;

                    self->idle_timer_.expires_after(
                        std::chrono::seconds(45)
                    );

                    co_await
                        self->idle_timer_.async_wait(
                            boost::asio::redirect_error(
                                use_awaitable,
                                ec
                            )
                        );

                    if (!ec)
                    {
                        self->Disconnect();
                        break;
                    }
                }
            },

            detached
        );
    }

    awaitable<void> ReadLoop()
    {
        try
        {
            while (
                !is_disconnected_.load()
            )
            {
                size_t length =
                    co_await
                        ssl_socket_
                            .async_read_some(
                                boost::asio::buffer(
                                    read_buffer_
                                ),
                                use_awaitable
                            );

                idle_timer_.expires_after(
                    std::chrono::seconds(45)
                );

                if (
                    !packet_buffer_.WriteData(
                        read_buffer_.data(),
                        length
                    )
                )
                {
                    Disconnect();
                    co_return;
                }

                std::vector<char> packet_data;

                while (true)
                {
                    int result =
                        packet_buffer_.ReadPacket(
                            packet_data
                        );

                    if (result == 1)
                    {
                        co_await ProcessPacketAsync(
                            packet_data.data(),
                            packet_data.size()
                        );
                    }
                    else if (result == -1)
                    {
                        Disconnect();
                        co_return;
                    }
                    else
                    {
                        break;
                    }
                }
            }
        }
        catch (...)
        {
            Disconnect();
        }
    }

    awaitable<void> WriteLoop()
    {
        try
        {
            while (
                !is_disconnected_.load()
            )
            {
                std::vector<char> msg =
                    co_await
                        write_channel_
                            .async_receive(
                                use_awaitable
                            );

                if (
                    is_disconnected_.load()
                )
                    break;

                co_await boost::asio::async_write(
                    ssl_socket_,
                    boost::asio::buffer(
                        msg.data(),
                        msg.size()
                    ),
                    use_awaitable
                );
            }
        }
        catch (...)
        {
            Disconnect();
        }
    }

    awaitable<void> ProcessPacketAsync(
        const char* data,
        size_t size);

private:

    boost::asio::strand<
        boost::asio::any_io_executor
    > strand_;

    ssl::stream<tcp::socket> ssl_socket_;

    ChatServer& server_;

    // [STRAND OWNED]
    uint32_t user_id_;
    uint32_t room_id_;
    bool is_authenticated_;

    std::atomic<bool> is_disconnected_;

    MessageChannel write_channel_;

    boost::asio::steady_timer idle_timer_;

    std::vector<char> read_buffer_ =
        std::vector<char>(4096);

    RingPacketBuffer packet_buffer_;
};

//==================================================
// User
//==================================================

class User :
    public std::enable_shared_from_this<User>
{
public:

    User(
        boost::asio::io_context& io_context,
        uint32_t id,
        std::string username)
        :
        strand_(
            boost::asio::make_strand(
                io_context
            )
        ),
        id_(id),
        username_(std::move(username)),
        is_online_(false)
    {
    }

    //--------------------------------------------------
    // immutable
    //--------------------------------------------------

    uint32_t GetId() const
    {
        return id_;
    }

    const std::string& GetUsername() const
    {
        return username_;
    }

    //--------------------------------------------------
    // [STRAND] mutable state
    //--------------------------------------------------

    awaitable<void> SetOnlineAsync(
        bool online)
    {
        co_await boost::asio::dispatch(
            strand_,
            use_awaitable
        );

        is_online_ = online;
    }

    awaitable<bool> IsOnlineAsync()
    {
        co_await boost::asio::dispatch(
            strand_,
            use_awaitable
        );

        co_return is_online_;
    }

    awaitable<void> SetSessionAsync(
        std::shared_ptr<ChatSession> session)
    {
        co_await boost::asio::dispatch(
            strand_,
            use_awaitable
        );

        session_ = session;
    }

    awaitable<std::shared_ptr<ChatSession>>
    GetSessionAsync()
    {
        co_await boost::asio::dispatch(
            strand_,
            use_awaitable
        );

        co_return session_.lock();
    }

private:

    boost::asio::strand<
        boost::asio::io_context::executor_type
    > strand_;

    // immutable
    const uint32_t id_;
    const std::string username_;

    // strand owned
    bool is_online_;

    std::weak_ptr<ChatSession> session_;
};

//==================================================
// ChatRoom
//==================================================

class ChatRoom :
    public std::enable_shared_from_this<ChatRoom>
{
public:

    ChatRoom(
        boost::asio::io_context& io_context,
        uint32_t room_id,
        std::string name,
        uint32_t max_users)
        :
        strand_(
            boost::asio::make_strand(
                io_context
            )
        ),
        room_id_(room_id),
        name_(std::move(name)),
        max_users_(max_users),
        owner_id_(0)
    {
    }

    //--------------------------------------------------
    // immutable
    //--------------------------------------------------

    uint32_t GetId() const
    {
        return room_id_;
    }

    const std::string& GetName() const
    {
        return name_;
    }

    uint32_t GetMaxUsers() const
    {
        return max_users_;
    }

    //--------------------------------------------------
    // mutable getters
    //--------------------------------------------------

    awaitable<uint32_t> GetUserCountAsync()
    {
        co_await boost::asio::dispatch(
            strand_,
            use_awaitable
        );

        co_return static_cast<uint32_t>(
            users_.size()
        );
    }

    awaitable<uint32_t> GetOwnerIdAsync()
    {
        co_await boost::asio::dispatch(
            strand_,
            use_awaitable
        );

        co_return owner_id_;
    }

    //--------------------------------------------------
    // RoomInfo snapshot
    //--------------------------------------------------

    awaitable<chat::RoomInfo>
    GetInfoAsync()
    {
        co_await boost::asio::dispatch(
            strand_,
            use_awaitable
        );

        chat::RoomInfo info;

        info.set_room_id(room_id_);
        info.set_room_name(name_);

        info.set_current_users(
            static_cast<uint32_t>(
                users_.size()
            )
        );

        info.set_max_users(
            max_users_
        );

        info.set_owner_id(
            owner_id_
        );

        co_return info;
    }

    //--------------------------------------------------
    // AddUser
    //--------------------------------------------------

    awaitable<bool> AddUserAsync(
        std::shared_ptr<User> user,
        RoomPermission perm =
            RoomPermission::MEMBER)
    {
        co_await boost::asio::dispatch(
            strand_,
            use_awaitable
        );

        const uint32_t user_id =
            user->GetId();

        // 이미 들어와 있다면 성공 처리
        if (
            users_.find(user_id) !=
            users_.end()
        )
        {
            co_return true;
        }

        if (
            users_.size() >=
            max_users_
        )
        {
            co_return false;
        }

        users_[user_id] = user;
        permissions_[user_id] = perm;

        if (owner_id_ == 0)
        {
            owner_id_ = user_id;

            permissions_[user_id] =
                RoomPermission::HOST;
        }

        co_return true;
    }

    //--------------------------------------------------
    // RemoveUser
    //--------------------------------------------------

    awaitable<bool> RemoveUserAsync(
        uint32_t user_id)
    {
        co_await boost::asio::dispatch(
            strand_,
            use_awaitable
        );

        auto it =
            users_.find(user_id);

        if (it == users_.end())
            co_return false;

        users_.erase(it);
        permissions_.erase(user_id);

        bool master_changed = false;
        uint32_t new_owner_id = 0;

        if (
            owner_id_ == user_id
        )
        {
            if (!users_.empty())
            {
                new_owner_id =
                    users_.begin()->first;

                owner_id_ =
                    new_owner_id;

                permissions_[new_owner_id] =
                    RoomPermission::HOST;

                master_changed = true;
            }
            else
            {
                owner_id_ = 0;
            }
        }

        if (master_changed)
        {
            chat::MasterChangedNotification noti;

            noti.set_room_id(
                room_id_
            );

            noti.set_new_master_id(
                new_owner_id
            );

            // Broadcast 자체가 strand로 안전하게 처리됨
            BroadcastMessage(
                MessageType::MASTER_CHANGED_NOTIFICATION,
                noti
            );
        }

        co_return true;
    }

    //--------------------------------------------------
    // HasUser
    //--------------------------------------------------

    awaitable<bool> HasUserAsync(
        uint32_t user_id)
    {
        co_await boost::asio::dispatch(
            strand_,
            use_awaitable
        );

        co_return
            users_.find(user_id) !=
            users_.end();
    }

    //--------------------------------------------------
    // Kick
    //--------------------------------------------------

    awaitable<bool> KickUserAsync(
        uint32_t operator_id,
        uint32_t target_id)
    {
        std::shared_ptr<User> target_user;

        {
            co_await boost::asio::dispatch(
                strand_,
                use_awaitable
            );

            auto perm_it =
                permissions_.find(
                    operator_id
                );

            if (
                perm_it ==
                permissions_.end()
            )
            {
                co_return false;
            }

            if (
                !HasPermission(
                    perm_it->second,
                    RoomPermission::KICK_USER
                )
            )
            {
                co_return false;
            }

            auto it =
                users_.find(target_id);

            if (
                it == users_.end()
            )
            {
                co_return false;
            }

            target_user = it->second;

            users_.erase(it);
            permissions_.erase(target_id);
        }

        // User의 session_은 User strand 소유
        auto target_session =
            co_await
                target_user
                    ->GetSessionAsync();

        if (target_session)
        {
            chat::KickedNotification noti;

            noti.set_room_id(
                room_id_
            );

            noti.set_reason(
                "Kicked by room master"
            );

            target_session->Send(
                MessageType::KICKED_NOTIFICATION,
                noti
            );

            // Session mutable state는 Session strand
            co_await
                target_session
                    ->SetRoomIdAsync(0);
        }

        co_return true;
    }

    //--------------------------------------------------
    // Transfer Master
    //--------------------------------------------------

    awaitable<bool> TransferMasterAsync(
        uint32_t operator_id,
        uint32_t new_master_id)
    {
        co_await boost::asio::dispatch(
            strand_,
            use_awaitable
        );

        if (
            owner_id_ != operator_id
        )
        {
            co_return false;
        }

        if (
            users_.find(new_master_id) ==
            users_.end()
        )
        {
            co_return false;
        }

        permissions_[owner_id_] =
            RoomPermission::MEMBER;

        owner_id_ =
            new_master_id;

        permissions_[new_master_id] =
            RoomPermission::HOST;

        chat::MasterChangedNotification noti;

        noti.set_room_id(
            room_id_
        );

        noti.set_new_master_id(
            new_master_id
        );

        BroadcastMessage(
            MessageType::MASTER_CHANGED_NOTIFICATION,
            noti
        );

        co_return true;
    }

    //--------------------------------------------------
    // Broadcast
    //--------------------------------------------------

    template <typename T>
    void BroadcastMessage(
        MessageType msg_type,
        const T& proto_msg)
    {
        auto self = shared_from_this();

        boost::asio::post(
            strand_,

            [self,
             msg_type,
             proto_msg]() mutable
            {
                // users_ 자체는 여기서만 읽는다.
                std::vector<
                    std::shared_ptr<User>
                > users_snapshot;

                users_snapshot.reserve(
                    self->users_.size()
                );

                for (
                    auto& [id, user] :
                    self->users_
                )
                {
                    users_snapshot.push_back(
                        user
                    );
                }

                // User::session_은 User strand 소유이므로
                // 별도 coroutine으로 조회
                for (
                    auto& user :
                    users_snapshot
                )
                {
                    co_spawn(
                        self->strand_,

                        [user,
                         msg_type,
                         proto_msg]()
                        -> awaitable<void>
                        {
                            auto session =
                                co_await
                                    user
                                        ->GetSessionAsync();

                            if (session)
                            {
                                session->Send(
                                    msg_type,
                                    proto_msg
                                );
                            }
                        },

                        detached
                    );
                }
            }
        );
    }

private:

    boost::asio::strand<
        boost::asio::io_context::executor_type
    > strand_;

    // immutable
    const uint32_t room_id_;
    const std::string name_;
    const uint32_t max_users_;

    // strand owned
    uint32_t owner_id_;

    std::unordered_map<
        uint32_t,
        std::shared_ptr<User>
    > users_;

    std::unordered_map<
        uint32_t,
        RoomPermission
    > permissions_;
};

//==================================================
// RoomManager
//==================================================

class RoomManager
{
public:

    explicit RoomManager(
        boost::asio::io_context& io_context)
        :
        io_context_(io_context),
        strand_(
            boost::asio::make_strand(
                io_context
            )
        ),
        next_room_id_(1)
    {
    }

    //--------------------------------------------------
    // Create
    //--------------------------------------------------

    awaitable<std::shared_ptr<ChatRoom>>
    CreateRoomAsync(
        const std::string& name,
        uint32_t max_users)
    {
        co_await boost::asio::dispatch(
            strand_,
            use_awaitable
        );

        uint32_t id =
            next_room_id_++;

        auto room =
            std::make_shared<ChatRoom>(
                io_context_,
                id,
                name,
                max_users
            );

        rooms_[id] = room;

        co_return room;
    }

    //--------------------------------------------------
    // Get
    //--------------------------------------------------

    awaitable<std::shared_ptr<ChatRoom>>
    GetRoomAsync(
        uint32_t room_id)
    {
        co_await boost::asio::dispatch(
            strand_,
            use_awaitable
        );

        auto it =
            rooms_.find(room_id);

        if (
            it == rooms_.end()
        )
        {
            co_return nullptr;
        }

        co_return it->second;
    }

    //--------------------------------------------------
    // Destroy
    //--------------------------------------------------

    awaitable<void> DestroyRoomAsync(
        uint32_t room_id)
    {
        co_await boost::asio::dispatch(
            strand_,
            use_awaitable
        );

        rooms_.erase(room_id);
    }

    //--------------------------------------------------
    // 방 목록
    //
    // Manager strand에서는 rooms_의 shared_ptr만 snapshot.
    // 각 Room의 mutable state는 Room strand에서 조회.
    //--------------------------------------------------

    awaitable<std::vector<chat::RoomInfo>>
    GetRoomListAsync()
    {
        std::vector<
            std::shared_ptr<ChatRoom>
        > room_snapshot;

        co_await boost::asio::dispatch(
            strand_,
            use_awaitable
        );

        room_snapshot.reserve(
            std::min<size_t>(
                rooms_.size(),
                16
            )
        );

        for (
            auto& [id, room] :
            rooms_
        )
        {
            room_snapshot.push_back(
                room
            );

            if (
                room_snapshot.size() >= 16
            )
            {
                break;
            }
        }

        std::vector<chat::RoomInfo> list;

        list.reserve(
            room_snapshot.size()
        );

        for (
            auto& room :
            room_snapshot
        )
        {
            list.push_back(
                co_await
                    room->GetInfoAsync()
            );
        }

        co_return list;
    }

    //--------------------------------------------------
    // 빈 방 정리
    //
    // shared_ptr identity까지 확인해서
    // 오래된 ChatRoom 객체가 새 room을 지우지 못하게 함.
    //--------------------------------------------------

    awaitable<bool> DestroyRoomIfEmptyAsync(
        uint32_t room_id,
        std::shared_ptr<ChatRoom> expected_room)
    {
        if (!expected_room)
            co_return false;

        uint32_t count =
            co_await
                expected_room
                    ->GetUserCountAsync();

        if (count != 0)
            co_return false;

        co_await boost::asio::dispatch(
            strand_,
            use_awaitable
        );

        auto it =
            rooms_.find(room_id);

        if (
            it == rooms_.end()
        )
        {
            co_return false;
        }

        if (
            it->second != expected_room
        )
        {
            co_return false;
        }

        // Manager 확인 후 Room이 다시 채워졌을 가능성을
        // 줄이기 위해 한 번 더 확인한다.
        //
        // 완전한 lifecycle atomicity는 이후
        // room closing state를 도입하면 더 강화 가능하다.

        uint32_t final_count =
            co_await
                expected_room
                    ->GetUserCountAsync();

        if (final_count != 0)
            co_return false;

        // 같은 room인지 다시 확인
        auto final_it =
            rooms_.find(room_id);

        if (
            final_it == rooms_.end() ||
            final_it->second != expected_room
        )
        {
            co_return false;
        }

        rooms_.erase(final_it);

        co_return true;
    }

private:

    boost::asio::io_context& io_context_;

    // [STRAND OWNED]
    boost::asio::strand<
        boost::asio::io_context::executor_type
    > strand_;

    // manager strand에서만 접근하므로 atomic 불필요
    uint32_t next_room_id_;

    std::unordered_map<
        uint32_t,
        std::shared_ptr<ChatRoom>
    > rooms_;
};

//==================================================
// UserManager
//==================================================

class UserManager
{
public:

    explicit UserManager(
        boost::asio::io_context& io_context)
        :
        io_context_(io_context),
        strand_(
            boost::asio::make_strand(
                io_context
            )
        )
    {
    }

    awaitable<std::shared_ptr<User>>
    GetOrCreateUserAsync(
        uint32_t user_id,
        const std::string& username)
    {
        co_await boost::asio::dispatch(
            strand_,
            use_awaitable
        );

        auto it =
            users_by_id_.find(
                user_id
            );

        if (
            it != users_by_id_.end()
        )
        {
            co_return it->second;
        }

        auto user =
            std::make_shared<User>(
                io_context_,
                user_id,
                username
            );

        users_by_id_[user_id] =
            user;

        users_by_name_[username] =
            user;

        co_return user;
    }

    awaitable<std::shared_ptr<User>>
    GetUserByIdAsync(
        uint32_t user_id)
    {
        co_await boost::asio::dispatch(
            strand_,
            use_awaitable
        );

        auto it =
            users_by_id_.find(
                user_id
            );

        if (
            it == users_by_id_.end()
        )
        {
            co_return nullptr;
        }

        co_return it->second;
    }

    awaitable<std::shared_ptr<User>>
    GetUserByNameAsync(
        const std::string& username)
    {
        co_await boost::asio::dispatch(
            strand_,
            use_awaitable
        );

        auto it =
            users_by_name_.find(
                username
            );

        if (
            it == users_by_name_.end()
        )
        {
            co_return nullptr;
        }

        co_return it->second;
    }

private:

    boost::asio::io_context& io_context_;

    boost::asio::strand<
        boost::asio::io_context::executor_type
    > strand_;

    std::unordered_map<
        uint32_t,
        std::shared_ptr<User>
    > users_by_id_;

    std::unordered_map<
        std::string,
        std::shared_ptr<User>
    > users_by_name_;
};

//==================================================
// MessageDispatcher
//==================================================

class MessageDispatcher
{
public:

    template <
        typename T,
        typename HandlerFunc
    >
    void RegisterHandler(
        MessageType type,
        HandlerFunc handler)
    {
        handlers_[type] =
            [handler, type](
                std::shared_ptr<ChatSession> session,
                const char* payload,
                size_t payload_size)
            -> awaitable<void>
            {
                T proto_msg;

                if (
                    !PacketSerializer::ParseProtoStream(
                        payload,
                        payload_size,
                        proto_msg
                    )
                )
                {
                    std::cerr
                        << "[Dispatcher Error] Failed to parse proto stream for MessageType: "
                        << static_cast<uint16_t>(type)
                        << std::endl;

                    co_return;
                }

                co_await handler(
                    session,
                    proto_msg
                );
            };
    }

    template <typename HandlerFunc>
    void RegisterRawHandler(
        MessageType type,
        HandlerFunc handler)
    {
        handlers_[type] =
            [handler](
                std::shared_ptr<ChatSession> session,
                const char*,
                size_t)
            -> awaitable<void>
            {
                co_await handler(
                    session
                );
            };
    }

    awaitable<void>
    DispatchMessageAsync(
        std::shared_ptr<ChatSession> session,
        const PacketHeader& header,
        const char* payload,
        size_t payload_size)
    {
        auto it =
            handlers_.find(
                header.message_type
            );

        if (
            it != handlers_.end()
        )
        {
            co_await it->second(
                session,
                payload,
                payload_size
            );
        }
        else
        {
            std::cerr
                << "[Dispatcher Error] Unhandled MessageType: "
                << static_cast<uint16_t>(
                    header.message_type
                )
                << std::endl;
        }
    }

private:

    using InternalAsyncHandler =
        std::function<
            awaitable<void>(
                std::shared_ptr<ChatSession>,
                const char*,
                size_t
            )
        >;

    std::unordered_map<
        MessageType,
        InternalAsyncHandler
    > handlers_;
};

//==================================================
// ChatServer
//==================================================

class ChatServer :
    public std::enable_shared_from_this<ChatServer>
{
public:

    ChatServer(
        boost::asio::io_context& io_context,
        ssl::context& ssl_ctx,
        short port,
        const std::string& go_grpc_addr)
        :
        io_context_(io_context),
        ssl_ctx_(ssl_ctx),

        acceptor_(
            io_context,
            tcp::endpoint(
                tcp::v4(),
                port
            )
        ),

        user_manager_(
            std::make_shared<UserManager>(
                io_context
            )
        ),

        room_manager_(
            std::make_shared<RoomManager>(
                io_context
            )
        )
    {
        auto channel =
            grpc::CreateChannel(
                go_grpc_addr,
                grpc::InsecureChannelCredentials()
            );

        user_repository_ =
            std::make_shared<UserRepository>(
                channel
            );

        session_repository_ =
            std::make_shared<SessionRepository>(
                channel
            );

        chat_repository_ =
            std::make_shared<ChatRepository>(
                channel
            );

        InitHandlers();
    }

    boost::asio::io_context&
    GetIOContext()
    {
        return io_context_;
    }

    MessageDispatcher&
    GetDispatcher()
    {
        return dispatcher_;
    }

    UserManager&
    GetUserManager()
    {
        return *user_manager_;
    }

    RoomManager&
    GetRoomManager()
    {
        return *room_manager_;
    }

    std::shared_ptr<UserRepository>
    GetUserRepository()
    {
        return user_repository_;
    }

    std::shared_ptr<SessionRepository>
    GetSessionRepository()
    {
        return session_repository_;
    }

    std::shared_ptr<ChatRepository>
    GetChatRepository()
    {
        return chat_repository_;
    }

    void StartAccept()
    {
        auto self =
            shared_from_this();

        co_spawn(
            acceptor_.get_executor(),

            [self]() -> awaitable<void>
            {
                while (true)
                {
                    tcp::socket socket =
                        co_await
                            self->acceptor_
                                .async_accept(
                                    use_awaitable
                                );

                    auto session =
                        std::make_shared<
                            ChatSession
                        >(
                            std::move(socket),
                            self->ssl_ctx_,
                            *self
                        );

                    session->Start();
                }
            },

            detached
        );
    }

private:

    void InitHandlers();

private:

    boost::asio::io_context& io_context_;

    ssl::context& ssl_ctx_;

    tcp::acceptor acceptor_;

    MessageDispatcher dispatcher_;

    std::shared_ptr<UserManager>
        user_manager_;

    std::shared_ptr<RoomManager>
        room_manager_;

    std::shared_ptr<UserRepository>
        user_repository_;

    std::shared_ptr<SessionRepository>
        session_repository_;

    std::shared_ptr<ChatRepository>
        chat_repository_;
};

//==================================================
// ChatSession::Disconnect
//==================================================

void ChatSession::Disconnect()
{
    if (
        is_disconnected_.exchange(true)
    )
    {
        return;
    }

    // Disconnect는 여러 경로에서 들어올 수 있으므로
    // session 상태 정리는 session strand로 넘긴다.
    //
    // destructor 경로에서는 shared_from_this()가 위험할 수 있으므로
    // 여기서는 기존 구조와 동일하게 atomic guard를 유지한다.

    uint32_t cur_user_id =
        user_id_;

    uint32_t cur_room_id =
        room_id_;

    user_id_ = 0;
    room_id_ = 0;
    is_authenticated_ = false;

    if (cur_user_id != 0)
    {
        co_spawn(
            server_.GetIOContext(),

            [server_ptr = &server_,
             cur_user_id,
             cur_room_id]()
            -> awaitable<void>
            {
                auto user =
                    co_await
                        server_ptr
                            ->GetUserManager()
                            .GetUserByIdAsync(
                                cur_user_id
                            );

                if (user)
                {
                    co_await
                        user->SetOnlineAsync(
                            false
                        );
                }

                if (cur_room_id != 0)
                {
                    auto room =
                        co_await
                            server_ptr
                                ->GetRoomManager()
                                .GetRoomAsync(
                                    cur_room_id
                                );

                    if (room)
                    {
                        co_await
                            room->RemoveUserAsync(
                                cur_user_id
                            );

                        bool destroyed =
                            co_await
                                server_ptr
                                    ->GetRoomManager()
                                    .DestroyRoomIfEmptyAsync(
                                        cur_room_id,
                                        room
                                    );

                        if (destroyed)
                        {
                            std::cout
                                << "[Room Cleanup] #"
                                << cur_room_id
                                << "번 방의 모든 유저가 나갔으므로 방을 파괴했습니다.\n";
                        }
                    }
                }

                co_await
                    server_ptr
                        ->GetSessionRepository()
                        ->SetUserSessionStateAsync(
                            cur_user_id,
                            "OFFLINE"
                        );
            },

            detached
        );
    }

    boost::system::error_code ec;

    idle_timer_.cancel(ec);

    write_channel_.close();

    ssl_socket_
        .lowest_layer()
        .close(ec);
}

//==================================================
// ProcessPacket
//==================================================

awaitable<void>
ChatSession::ProcessPacketAsync(
    const char* data,
    size_t size)
{
    if (
        size < sizeof(PacketHeader)
    )
    {
        co_return;
    }

    PacketHeader header{};

    std::memcpy(
        &header,
        data,
        sizeof(PacketHeader)
    );

    const char* payload =
        data + sizeof(PacketHeader);

    size_t payload_size =
        size - sizeof(PacketHeader);

    co_await
        server_
            .GetDispatcher()
            .DispatchMessageAsync(
                shared_from_this(),
                header,
                payload,
                payload_size
            );
}

//==================================================
// ChatHandlers
//==================================================

class ChatHandlers
{
public:

    //--------------------------------------------------
    // Login
    //--------------------------------------------------

    static awaitable<void> HandleLogin(
        ChatServer& server,
        std::shared_ptr<ChatSession> session,
        const chat::LoginRequest& req)
    {
        chat::LoginResponse res;

        uint32_t user_id = 0;

        //--------------------------------------------------
        // reconnect token
        //--------------------------------------------------

        if (
            !req.reconnect_token().empty()
        )
        {
            auto verify_res =
                co_await
                    server
                        .GetUserRepository()
                        ->VerifyTokenAsync(
                            req.reconnect_token()
                        );

            if (verify_res.success)
            {
                user_id =
                    verify_res.user_id;

                auto user =
                    co_await
                        server
                            .GetUserManager()
                            .GetOrCreateUserAsync(
                                user_id,
                                verify_res.username
                            );

                co_await
                    user->SetSessionAsync(
                        session
                    );

                co_await
                    user->SetOnlineAsync(
                        true
                    );

                // 현재 handler는 session packet processing
                // coroutine에서 실행된다.
                session->SetUserId(
                    user_id
                );

                session->SetAuthenticated(
                    true
                );

                // [FIX]
                // 기존 reconnect 성공 경로에서
                // Redis session ONLINE 갱신이 빠져 있었다.
                co_await
                    server
                        .GetSessionRepository()
                        ->SetUserSessionStateAsync(
                            user_id,
                            "ONLINE"
                        );

                res.set_success(true);

                res.set_assigned_user_id(
                    user_id
                );

                res.set_reconnect_token(
                    req.reconnect_token()
                );

                session->Send(
                    MessageType::LOGIN_RESPONSE,
                    res
                );

                co_return;
            }
        }

        //--------------------------------------------------
        // username/password auth
        //--------------------------------------------------

        auto auth_result =
            co_await
                server
                    .GetUserRepository()
                    ->AuthenticateUserAsync(
                        req.username(),
                        req.password()
                    );

        if (auth_result.success)
        {
            user_id =
                auth_result.user_data.id;

            auto user =
                co_await
                    server
                        .GetUserManager()
                        .GetOrCreateUserAsync(
                            user_id,
                            auth_result
                                .user_data
                                .username
                        );

            co_await
                user->SetSessionAsync(
                    session
                );

            co_await
                user->SetOnlineAsync(
                    true
                );

            session->SetUserId(
                user_id
            );

            session->SetAuthenticated(
                true
            );

            co_await
                server
                    .GetSessionRepository()
                    ->SetUserSessionStateAsync(
                        user_id,
                        "ONLINE"
                    );

            res.set_success(true);

            res.set_assigned_user_id(
                user_id
            );

            res.set_reconnect_token(
                auth_result.reconnect_token
            );
        }
        else
        {
            res.set_success(false);

            res.set_error_message(
                auth_result.error_msg
            );
        }

        session->Send(
            MessageType::LOGIN_RESPONSE,
            res
        );
    }

    //--------------------------------------------------
    // Register
    //--------------------------------------------------

    static awaitable<void> HandleRegister(
        ChatServer& server,
        std::shared_ptr<ChatSession> session,
        const chat::RegisterRequest& req)
    {
        auto reg_result =
            co_await
                server
                    .GetUserRepository()
                    ->RegisterUserAsync(
                        req.username(),
                        req.password()
                    );

        chat::RegisterResponse res;

        if (reg_result.success)
        {
            res.set_success(true);

            res.set_assigned_user_id(
                reg_result.assigned_id
            );
        }
        else
        {
            res.set_success(false);

            res.set_error_message(
                reg_result.error_msg
            );
        }

        session->Send(
            MessageType::REGISTER_RESPONSE,
            res
        );
    }

    //--------------------------------------------------
    // Create Room
    //--------------------------------------------------

    static awaitable<void> HandleCreateRoom(
        ChatServer& server,
        std::shared_ptr<ChatSession> session,
        const chat::CreateRoomRequest& req)
    {
        if (
            !session->IsAuthenticated()
        )
        {
            co_return;
        }

        uint32_t user_id =
            session->GetUserId();

        auto room =
            co_await
                server
                    .GetRoomManager()
                    .CreateRoomAsync(
                        req.room_name(),
                        req.max_users()
                    );

        auto user =
            co_await
                server
                    .GetUserManager()
                    .GetUserByIdAsync(
                        user_id
                    );

        chat::CreateRoomResponse res;

        if (room && user)
        {
            bool added =
                co_await
                    room->AddUserAsync(
                        user,
                        RoomPermission::HOST
                    );

            if (added)
            {
                session->SetRoomId(
                    room->GetId()
                );

                res.set_success(true);

                res.set_created_room_id(
                    room->GetId()
                );

                res.set_owner_id(
                    user->GetId()
                );
            }
            else
            {
                co_await
                    server
                        .GetRoomManager()
                        .DestroyRoomAsync(
                            room->GetId()
                        );

                res.set_success(false);

                res.set_error_message(
                    "ROOM_CREATE_FAILED"
                );
            }
        }
        else
        {
            if (room)
            {
                co_await
                    server
                        .GetRoomManager()
                        .DestroyRoomAsync(
                            room->GetId()
                        );
            }

            res.set_success(false);

            res.set_error_message(
                "ROOM_CREATE_FAILED"
            );
        }

        session->Send(
            MessageType::CREATE_ROOM_RESPONSE,
            res
        );
    }

    //--------------------------------------------------
    // Room List
    //--------------------------------------------------

    static awaitable<void> HandleRoomList(
        ChatServer& server,
        std::shared_ptr<ChatSession> session,
        const chat::RoomListRequest&)
    {
        if (
            !session->IsAuthenticated()
        )
        {
            co_return;
        }

        auto rooms =
            co_await
                server
                    .GetRoomManager()
                    .GetRoomListAsync();

        chat::RoomListResponse res;

        for (
            const auto& room_info :
            rooms
        )
        {
            *res.add_rooms() =
                room_info;
        }

        session->Send(
            MessageType::ROOM_LIST_RESPONSE,
            res
        );
    }

    //--------------------------------------------------
    // Join
    //--------------------------------------------------

    static awaitable<void> HandleJoinRoom(
        ChatServer& server,
        std::shared_ptr<ChatSession> session,
        const chat::JoinRoomRequest& req)
    {
        if (
            !session->IsAuthenticated()
        )
        {
            co_return;
        }

        uint32_t user_id =
            session->GetUserId();

        auto room =
            co_await
                server
                    .GetRoomManager()
                    .GetRoomAsync(
                        req.room_id()
                    );

        auto user =
            co_await
                server
                    .GetUserManager()
                    .GetUserByIdAsync(
                        user_id
                    );

        chat::JoinRoomResponse res;

        if (
            room &&
            user &&
            co_await room->AddUserAsync(
                user,
                RoomPermission::MEMBER
            )
        )
        {
            session->SetRoomId(
                room->GetId()
            );

            res.set_success(true);

            res.set_room_id(
                room->GetId()
            );

            res.set_owner_id(
                co_await
                    room->GetOwnerIdAsync()
            );
        }
        else
        {
            res.set_success(false);

            res.set_error_message(
                "JOIN_FAILED_OR_FULL"
            );
        }

        session->Send(
            MessageType::JOIN_ROOM_RESPONSE,
            res
        );
    }

    //--------------------------------------------------
    // Leave
    //--------------------------------------------------

    static awaitable<void> HandleLeaveRoom(
        ChatServer& server,
        std::shared_ptr<ChatSession> session,
        const chat::LeaveRoomRequest& req)
    {
        if (
            !session->IsAuthenticated()
        )
        {
            co_return;
        }

        auto room =
            co_await
                server
                    .GetRoomManager()
                    .GetRoomAsync(
                        req.room_id()
                    );

        chat::LeaveRoomResponse res;

        if (
            room &&
            co_await
                room->RemoveUserAsync(
                    session->GetUserId()
                )
        )
        {
            session->SetRoomId(0);

            co_await
                server
                    .GetRoomManager()
                    .DestroyRoomIfEmptyAsync(
                        room->GetId(),
                        room
                    );

            res.set_success(true);
        }
        else
        {
            res.set_success(false);

            res.set_error_message(
                "LEAVE_FAILED"
            );
        }

        session->Send(
            MessageType::LEAVE_ROOM_RESPONSE,
            res
        );
    }

    //--------------------------------------------------
    // Chat
    //--------------------------------------------------

    static awaitable<void> HandleChatMessage(
        ChatServer& server,
        std::shared_ptr<ChatSession> session,
        const chat::ChatMessage& msg_param)
    {
        if (
            !session->IsAuthenticated()
        )
        {
            co_return;
        }

        uint32_t user_id =
            session->GetUserId();

        chat::ChatMessage msg =
            msg_param;

        auto room =
            co_await
                server
                    .GetRoomManager()
                    .GetRoomAsync(
                        msg.room_id()
                    );

        auto user =
            co_await
                server
                    .GetUserManager()
                    .GetUserByIdAsync(
                        user_id
                    );

        if (
            room &&
            user &&
            co_await
                room->HasUserAsync(
                    user_id
                )
        )
        {
            msg.set_sender_id(
                user_id
            );

            msg.set_sender_username(
                user->GetUsername()
            );

            room->BroadcastMessage(
                MessageType::CHAT_MESSAGE,
                msg
            );

            uint32_t room_id =
                msg.room_id();

            std::string text =
                msg.message();

            int64_t timestamp =
                msg.timestamp();

            co_spawn(
                server.GetIOContext(),

                [&server,
                 room_id,
                 user_id,
                 text = std::move(text),
                 timestamp]()
                -> awaitable<void>
                {
                    co_await
                        server
                            .GetChatRepository()
                            ->PublishChatAsync(
                                room_id,
                                user_id,
                                text,
                                timestamp
                            );
                },

                detached
            );
        }
    }

    //--------------------------------------------------
    // History
    //--------------------------------------------------

    static awaitable<void> HandleChatHistory(
        ChatServer& server,
        std::shared_ptr<ChatSession> session,
        const chat::ChatHistoryRequest& req)
    {
        if (
            !session->IsAuthenticated()
        )
        {
            co_return;
        }

        auto result =
            co_await
                server
                    .GetChatRepository()
                    ->GetChatHistoryAsync(
                        req.room_id(),
                        req.last_message_id(),
                        req.count()
                    );

        chat::ChatHistoryResponse res;

        res.set_room_id(
            req.room_id()
        );

        if (result.success)
        {
            res.set_success(true);

            res.set_has_more(
                result.has_more
            );

            for (
                const auto& db_msg :
                result.messages
            )
            {
                auto* msg =
                    res.add_messages();

                msg->set_message_id(
                    db_msg.message_id()
                );

                msg->set_room_id(
                    db_msg.room_id()
                );

                msg->set_sender_id(
                    db_msg.sender_id()
                );

                msg->set_sender_username(
                    db_msg.sender_name()
                );

                msg->set_message(
                    db_msg.message()
                );

                msg->set_timestamp(
                    db_msg.timestamp()
                );
            }
        }
        else
        {
            res.set_success(false);

            res.set_error_message(
                result.error_msg
            );
        }

        session->Send(
            MessageType::CHAT_HISTORY_RESPONSE,
            res
        );
    }

    //--------------------------------------------------
    // Whisper
    //--------------------------------------------------

    static awaitable<void> HandleWhisper(
        ChatServer& server,
        std::shared_ptr<ChatSession> session,
        const chat::WhisperRequest& req)
    {
        if (
            !session->IsAuthenticated()
        )
        {
            co_return;
        }

        auto sender =
            co_await
                server
                    .GetUserManager()
                    .GetUserByIdAsync(
                        session->GetUserId()
                    );

        auto target =
            co_await
                server
                    .GetUserManager()
                    .GetUserByNameAsync(
                        req.target_username()
                    );

        chat::WhisperResponse res;

        if (sender && target)
        {
            auto target_session =
                co_await
                    target
                        ->GetSessionAsync();

            if (target_session)
            {
                chat::WhisperNotification noti;

                noti.set_sender_username(
                    sender->GetUsername()
                );

                noti.set_message(
                    req.message()
                );

                target_session->Send(
                    MessageType::WHISPER_NOTIFICATION,
                    noti
                );

                res.set_success(true);
            }
            else
            {
                res.set_success(false);

                res.set_error_message(
                    "USER_OFFLINE"
                );
            }
        }
        else
        {
            res.set_success(false);

            res.set_error_message(
                "TARGET_NOT_FOUND"
            );
        }

        session->Send(
            MessageType::WHISPER_RESPONSE,
            res
        );
    }

    //--------------------------------------------------
    // Kick
    //--------------------------------------------------

    static awaitable<void> HandleKickUser(
        ChatServer& server,
        std::shared_ptr<ChatSession> session,
        const chat::KickUserRequest& req)
    {
        if (
            !session->IsAuthenticated()
        )
        {
            co_return;
        }

        auto room =
            co_await
                server
                    .GetRoomManager()
                    .GetRoomAsync(
                        req.room_id()
                    );

        chat::KickUserResponse res;

        if (
            room &&
            co_await
                room->KickUserAsync(
                    session->GetUserId(),
                    req.target_user_id()
                )
        )
        {
            res.set_success(true);
        }
        else
        {
            res.set_success(false);

            res.set_error_message(
                "KICK_PERMISSION_DENIED_OR_NO_USER"
            );
        }

        session->Send(
            MessageType::KICK_USER_RESPONSE,
            res
        );
    }

    //--------------------------------------------------
    // Transfer master
    //--------------------------------------------------

    static awaitable<void> HandleTransferMaster(
        ChatServer& server,
        std::shared_ptr<ChatSession> session,
        const chat::TransferMasterRequest& req)
    {
        if (
            !session->IsAuthenticated()
        )
        {
            co_return;
        }

        auto room =
            co_await
                server
                    .GetRoomManager()
                    .GetRoomAsync(
                        req.room_id()
                    );

        chat::TransferMasterResponse res;

        if (
            room &&
            co_await
                room->TransferMasterAsync(
                    session->GetUserId(),
                    req.new_master_id()
                )
        )
        {
            res.set_success(true);
        }
        else
        {
            res.set_success(false);

            res.set_error_message(
                "TRANSFER_FAILED_NOT_HOST"
            );
        }

        session->Send(
            MessageType::TRANSFER_MASTER_RESPONSE,
            res
        );
    }

    //--------------------------------------------------
    // Ping
    //--------------------------------------------------

    static awaitable<void> HandlePing(
        std::shared_ptr<ChatSession> session)
    {
        PacketHeader pong_header{};

        pong_header.packet_size =
            sizeof(PacketHeader);

        pong_header.message_type =
            MessageType::PONG;

        pong_header.user_id =
            session->GetUserId();

        pong_header.sequence_number = 0;

        session->Send(
            &pong_header,
            sizeof(PacketHeader)
        );

        co_return;
    }
};

//==================================================
// Handler 등록
//==================================================

void ChatServer::InitHandlers()
{
    dispatcher_.RegisterHandler<
        chat::LoginRequest
    >(
        MessageType::LOGIN_REQUEST,

        [this](
            auto s,
            const auto& req)
        {
            return ChatHandlers::HandleLogin(
                *this,
                s,
                req
            );
        }
    );

    dispatcher_.RegisterHandler<
        chat::RegisterRequest
    >(
        MessageType::REGISTER_REQUEST,

        [this](
            auto s,
            const auto& req)
        {
            return ChatHandlers::HandleRegister(
                *this,
                s,
                req
            );
        }
    );

    dispatcher_.RegisterHandler<
        chat::CreateRoomRequest
    >(
        MessageType::CREATE_ROOM_REQUEST,

        [this](
            auto s,
            const auto& req)
        {
            return ChatHandlers::HandleCreateRoom(
                *this,
                s,
                req
            );
        }
    );

    dispatcher_.RegisterHandler<
        chat::RoomListRequest
    >(
        MessageType::ROOM_LIST_REQUEST,

        [this](
            auto s,
            const auto& req)
        {
            return ChatHandlers::HandleRoomList(
                *this,
                s,
                req
            );
        }
    );

    dispatcher_.RegisterHandler<
        chat::JoinRoomRequest
    >(
        MessageType::JOIN_ROOM,

        [this](
            auto s,
            const auto& req)
        {
            return ChatHandlers::HandleJoinRoom(
                *this,
                s,
                req
            );
        }
    );

    dispatcher_.RegisterHandler<
        chat::LeaveRoomRequest
    >(
        MessageType::LEAVE_ROOM,

        [this](
            auto s,
            const auto& req)
        {
            return ChatHandlers::HandleLeaveRoom(
                *this,
                s,
                req
            );
        }
    );

    dispatcher_.RegisterHandler<
        chat::ChatMessage
    >(
        MessageType::CHAT_MESSAGE,

        [this](
            auto s,
            const auto& req)
        {
            return ChatHandlers::HandleChatMessage(
                *this,
                s,
                req
            );
        }
    );

    dispatcher_.RegisterHandler<
        chat::ChatHistoryRequest
    >(
        MessageType::CHAT_HISTORY_REQUEST,

        [this](
            auto s,
            const auto& req)
        {
            return ChatHandlers::HandleChatHistory(
                *this,
                s,
                req
            );
        }
    );

    dispatcher_.RegisterHandler<
        chat::WhisperRequest
    >(
        MessageType::WHISPER_REQUEST,

        [this](
            auto s,
            const auto& req)
        {
            return ChatHandlers::HandleWhisper(
                *this,
                s,
                req
            );
        }
    );

    dispatcher_.RegisterHandler<
        chat::KickUserRequest
    >(
        MessageType::KICK_USER_REQUEST,

        [this](
            auto s,
            const auto& req)
        {
            return ChatHandlers::HandleKickUser(
                *this,
                s,
                req
            );
        }
    );

    dispatcher_.RegisterHandler<
        chat::TransferMasterRequest
    >(
        MessageType::TRANSFER_MASTER_REQUEST,

        [this](
            auto s,
            const auto& req)
        {
            return ChatHandlers::HandleTransferMaster(
                *this,
                s,
                req
            );
        }
    );

    dispatcher_.RegisterRawHandler(
        MessageType::PING,

        [](auto s)
        {
            return ChatHandlers::HandlePing(
                s
            );
        }
    );
}

//==================================================
// main
//==================================================

int main()
{
    try
    {
        boost::asio::io_context io_context;

        auto work_guard =
            boost::asio::make_work_guard(
                io_context
            );

        ssl::context ssl_ctx(
            ssl::context::tlsv12_server
        );

        ssl_ctx.set_options(
            ssl::context::default_workarounds |
            ssl::context::no_sslv2 |
            ssl::context::single_dh_use
        );

        ssl_ctx.use_certificate_chain_file(
            "server.crt"
        );

        ssl_ctx.use_private_key_file(
            "server.key",
            ssl::context::pem
        );

        auto server =
            std::make_shared<ChatServer>(
                io_context,
                ssl_ctx,
                8080,
                "127.0.0.1:50051"
            );

        server->StartAccept();

        std::cout
            << "[C++ SSL Chat Server] "
            << "Listening on port 8080 "
            << "(TLS 1.2 Encrypted)...\n";

        unsigned int threads_count =
            std::thread::hardware_concurrency();

        if (threads_count == 0)
            threads_count = 4;

        std::vector<std::thread>
            thread_pool;

        thread_pool.reserve(
            threads_count
        );

        for (
            unsigned int i = 0;
            i < threads_count;
            ++i
        )
        {
            thread_pool.emplace_back(
                [&io_context]()
                {
                    io_context.run();
                }
            );
        }

        std::cout
            << "[Unified Thread Pool] Total Workers: "
            << threads_count
            << "\n";

        for (
            auto& t :
            thread_pool
        )
        {
            if (t.joinable())
                t.join();
        }
    }
    catch (
        const std::exception& e
    )
    {
        std::cerr
            << "Exception: "
            << e.what()
            << std::endl;
    }

    return 0;
}