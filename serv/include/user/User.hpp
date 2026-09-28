#pragma once

#include "common/Asio.hpp"
#include <memory>
#include <string>

class ChatSession;

class User :
    public std::enable_shared_from_this<User>
{
    public:
    
        User(
            boost::asio::io_context& io_context,
            uint32_t id,
            std::string username);
    //--------------------------------------------------
        // immutable
        //--------------------------------------------------
    
        uint32_t GetId() const;
    const std::string& GetUsername() const;
    //--------------------------------------------------
        // [STRAND] mutable state
        //--------------------------------------------------
    
        awaitable<void> SetOnlineAsync(
            bool online);
    awaitable<bool> IsOnlineAsync();
    awaitable<void> SetSessionAsync(
            std::shared_ptr<ChatSession> session);
    awaitable<std::shared_ptr<ChatSession>>
        GetSessionAsync();

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
