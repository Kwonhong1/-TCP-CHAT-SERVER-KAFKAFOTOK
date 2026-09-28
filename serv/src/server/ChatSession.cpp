#include "server/ChatSession.hpp"
#include "server/ChatServer.hpp"
#include "room/ChatRoom.hpp"
#include "user/User.hpp"
#include <iostream>

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

    if (cur_user_id !=0)
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
                size < PACKET_HEADER_SIZE
            )
            {
                co_return;
            }
        
            // Little Endian Wire Header
            //          ↓
            // 현재 CPU Native Header
            PacketHeader header =
                DecodePacketHeader(
                    data
                );
            
            // 실제 전달받은 패킷 크기와
            // 헤더에 기록된 크기가 일치하는지 확인
            if (
                header.packet_size != size
            )
            {
                std::cerr
                    << "[Security] Packet size mismatch. Header: "
                    << header.packet_size
                    << ", Actual: "
                    << size
                    << std::endl;
            
                co_return;
            }
        
            const char* payload =
                data + PACKET_HEADER_SIZE;
        
            size_t payload_size =
                size - PACKET_HEADER_SIZE;
        
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



// Implementations moved out of the header during refactor v2.
    ChatSession::ChatSession(
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

ChatSession::~ChatSession()
{
        Disconnect();
    }

boost::asio::strand<
        boost::asio::any_io_executor
    >& ChatSession::GetStrand()
{
        return strand_;
    }

void ChatSession::Start()
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
                        static_cast<uint16_t>(
                            PACKET_HEADER_SIZE
                        );
                    
                    prompt_header.message_type =
                        MessageType::LOGIN_PROMPT;
                    
                    prompt_header.user_id = 0;
                    prompt_header.sequence_number = 0;
                    
                    
                    // Native Header
                    //      ↓
                    // Little Endian Wire Header
                    std::vector<char> prompt_packet(
                        PACKET_HEADER_SIZE
                    );

                    EncodePacketHeader(
                        prompt_header,
                        prompt_packet.data()
                    );

                    self->Send(
                        prompt_packet.data(),
                        prompt_packet.size()
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

    uint32_t ChatSession::GetUserId() const
{
        return user_id_;
    }

uint32_t ChatSession::GetRoomId() const
{
        return room_id_;
    }

bool ChatSession::IsAuthenticated() const
{
        return is_authenticated_;
    }

void ChatSession::SetUserId(uint32_t id)
{
        user_id_ = id;
    }

void ChatSession::SetRoomId(uint32_t room_id)
{
        room_id_ = room_id;
    }

void ChatSession::SetAuthenticated(bool auth)
{
        is_authenticated_ = auth;
    }

//--------------------------------------------------
    // [STRAND] 외부 객체가 Session 상태를 변경할 때 사용
    //--------------------------------------------------

    awaitable<void> ChatSession::SetRoomIdAsync(
        uint32_t room_id)
{
        co_await boost::asio::dispatch(
            strand_,
            use_awaitable
        );

        room_id_ = room_id;
    }
    void ChatSession::SendMessageRaw(
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

void ChatSession::StartIdleTimer()
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

awaitable<void> ChatSession::ReadLoop()
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

awaitable<void> ChatSession::WriteLoop()
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
