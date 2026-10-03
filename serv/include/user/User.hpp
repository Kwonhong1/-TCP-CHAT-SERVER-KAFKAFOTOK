#pragma once

#include "common/Asio.hpp"
#include <chrono>
#include <memory>
#include <string>

class ChatSession;

class User
{
public:

    User(boost::asio::io_context& io_context, uint32_t id, std::string username);

    uint32_t GetId() const;
    const std::string& GetUsername() const;

    

    awaitable<void> SetOnlineAsync(bool online);
    awaitable<bool> IsOnlineAsync();

    awaitable<void> SetSessionAsync(std::shared_ptr<ChatSession> session);
    awaitable<std::shared_ptr<ChatSession>> GetSessionAsync();

    awaitable<void> SetRoomIdAsync(uint32_t room_id);
    awaitable<uint32_t> GetRoomIdAsync();

        awaitable<uint64_t> BeginReconnectGraceAsync(std::chrono::seconds timeout);
    awaitable<bool> WaitReconnectGraceAsync();
    awaitable<void> CompleteReconnectAsync(std::shared_ptr<ChatSession> session);
    awaitable<bool> TryExpireReconnectAsync(uint64_t generation);
private:

    boost::asio::strand<boost::asio::io_context::executor_type> strand_;

    const uint32_t id_;
    const std::string username_;


    bool is_online_;
    uint32_t room_id_;
    uint64_t reconnect_generation_;
    boost::asio::steady_timer reconnect_timer_;
     
    std::weak_ptr<ChatSession> session_;
    
};