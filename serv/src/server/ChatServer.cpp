#include "server/ChatServer.hpp"
#include "handler/AuthHandler.hpp"
#include "handler/RoomHandler.hpp"
#include "handler/ChatHandler.hpp"
#include "handler/AdminHandler.hpp"
#include "handler/SystemHandler.hpp"

void ChatServer::InitHandlers()
{
    dispatcher_.RegisterHandler<chat::LoginRequest>(
        MessageType::LOGIN_REQUEST,
        AuthPolicy::PUBLIC,
        [this](auto s, const auto& req)
        {
            return AuthHandler::HandleLogin(*this, s, req);
        }
    );

    dispatcher_.RegisterHandler<chat::RegisterRequest>(
        MessageType::REGISTER_REQUEST,
        AuthPolicy::PUBLIC,
        [this](auto s, const auto& req)
        {
            return AuthHandler::HandleRegister(*this, s, req);
        }
    );

    dispatcher_.RegisterHandler<chat::CreateRoomRequest>(
        MessageType::CREATE_ROOM_REQUEST,
        AuthPolicy::AUTHENTICATED,
        [this](auto s, const auto& req)
        {
            return RoomHandler::HandleCreateRoom(*this, s, req);
        }
    );

    dispatcher_.RegisterHandler<chat::RoomListRequest>(
        MessageType::ROOM_LIST_REQUEST,
        AuthPolicy::AUTHENTICATED,
        [this](auto s, const auto& req)
        {
            return RoomHandler::HandleRoomList(*this, s, req);
        }
    );

    dispatcher_.RegisterHandler<chat::JoinRoomRequest>(
        MessageType::JOIN_ROOM,
        AuthPolicy::AUTHENTICATED,
        [this](auto s, const auto& req)
        {
            return RoomHandler::HandleJoinRoom(*this, s, req);
        }
    );

    dispatcher_.RegisterHandler<chat::LeaveRoomRequest>(
        MessageType::LEAVE_ROOM,
        AuthPolicy::AUTHENTICATED,
        [this](auto s, const auto& req)
        {
            return RoomHandler::HandleLeaveRoom(*this, s, req);
        }
    );

    dispatcher_.RegisterHandler<chat::ChatMessage>(
        MessageType::CHAT_MESSAGE,
        AuthPolicy::AUTHENTICATED,
        [this](auto s, const auto& req)
        {
            return ChatHandler::HandleChatMessage(*this, s, req);
        }
    );

    dispatcher_.RegisterHandler<chat::ChatHistoryRequest>(
        MessageType::CHAT_HISTORY_REQUEST,
        AuthPolicy::AUTHENTICATED,
        [this](auto s, const auto& req)
        {
            return ChatHandler::HandleChatHistory(*this, s, req);
        }
    );

    dispatcher_.RegisterHandler<chat::WhisperRequest>(
        MessageType::WHISPER_REQUEST,
        AuthPolicy::AUTHENTICATED,
        [this](auto s, const auto& req)
        {
            return ChatHandler::HandleWhisper(*this, s, req);
        }
    );

    dispatcher_.RegisterHandler<chat::KickUserRequest>(
        MessageType::KICK_USER_REQUEST,
        AuthPolicy::AUTHENTICATED,
        [this](auto s, const auto& req)
        {
            return AdminHandler::HandleKickUser(*this, s, req);
        }
    );

    dispatcher_.RegisterHandler<chat::TransferMasterRequest>(
        MessageType::TRANSFER_MASTER_REQUEST,
        AuthPolicy::AUTHENTICATED,
        [this](auto s, const auto& req)
        {
            return AdminHandler::HandleTransferMaster(*this, s, req);
        }
    );

    dispatcher_.RegisterRawHandler(
        MessageType::PING,
        AuthPolicy::PUBLIC,
        [](auto s)
        {
            return SystemHandler::HandlePing(s);
        }
    );
}









// Implementations moved out of the header during refactor v2.
    ChatServer::ChatServer(
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
    ChatServer::GetIOContext()
{
        return io_context_;
    }

MessageDispatcher&
    ChatServer::GetDispatcher()
{
        return dispatcher_;
    }

UserManager&
    ChatServer::GetUserManager()
{
        return *user_manager_;
    }

RoomManager&
    ChatServer::GetRoomManager()
{
        return *room_manager_;
    }

std::shared_ptr<UserRepository>
    ChatServer::GetUserRepository()
{
        return user_repository_;
    }

std::shared_ptr<SessionRepository>
    ChatServer::GetSessionRepository()
{
        return session_repository_;
    }

std::shared_ptr<ChatRepository>
    ChatServer::GetChatRepository()
{
        return chat_repository_;
    }

void ChatServer::StartAccept()
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
