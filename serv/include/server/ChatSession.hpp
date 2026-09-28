#pragma once

#include "common/Asio.hpp"
#include "protocol/Protocol.hpp"
#include "protocol/PacketSerializer.hpp"
#include "protocol/RingPacketBuffer.hpp"
#include <boost/asio/experimental/channel.hpp>
#include <array>
#include <atomic>
#include <chrono>
#include <memory>
#include <vector>

class ChatServer;

using MessageChannel = boost::asio::experimental::channel<void(boost::system::error_code, std::vector<char>)>;

class ChatSession :
    public std::enable_shared_from_this<ChatSession>
{
    public:
    
        ChatSession(
            tcp::socket socket,
            ssl::context& ssl_ctx,
            ChatServer& server);
    ~ChatSession();
    boost::asio::strand<
            boost::asio::any_io_executor
        >& GetStrand();
    void Start();
    //--------------------------------------------------
        // session strand 내부에서 사용하는 빠른 API
        //--------------------------------------------------
    
        uint32_t GetUserId() const;
    uint32_t GetRoomId() const;
    bool IsAuthenticated() const;
    void SetUserId(uint32_t id);
    void SetRoomId(uint32_t room_id);
    void SetAuthenticated(bool auth);
    //--------------------------------------------------
        // [STRAND] 외부 객체가 Session 상태를 변경할 때 사용
        //--------------------------------------------------
    
        awaitable<void> SetRoomIdAsync(
            uint32_t room_id);

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

                auto packet = PacketSerializer::Serialize(msg_type, self->user_id_, proto_msg);
                if (packet.empty()) return;
                self->write_channel_.try_send(boost::system::error_code{}, std::move(packet));
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
            size_t size);
    void StartIdleTimer();
    awaitable<void> ReadLoop();
    awaitable<void> WriteLoop();

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
