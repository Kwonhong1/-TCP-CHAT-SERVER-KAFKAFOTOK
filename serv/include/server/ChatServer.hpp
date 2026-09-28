#pragma once

#include "common/Asio.hpp"
#include "handler/MessageDispatcher.hpp"
#include "repository/UserRepository.hpp"
#include "repository/SessionRepository.hpp"
#include "repository/ChatRepository.hpp"
#include "room/RoomManager.hpp"
#include "user/UserManager.hpp"
#include "server/ChatSession.hpp"
#include <grpcpp/grpcpp.h>
#include <memory>
#include <string>

class ChatServer :
    public std::enable_shared_from_this<ChatServer>
{
    public:
    
        ChatServer(
            boost::asio::io_context& io_context,
            ssl::context& ssl_ctx,
            short port,
            const std::string& go_grpc_addr);
    boost::asio::io_context&
        GetIOContext();
    MessageDispatcher&
        GetDispatcher();
    UserManager&
        GetUserManager();
    RoomManager&
        GetRoomManager();
    std::shared_ptr<UserRepository>
        GetUserRepository();
    std::shared_ptr<SessionRepository>
        GetSessionRepository();
    std::shared_ptr<ChatRepository>
        GetChatRepository();
    void StartAccept();

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
