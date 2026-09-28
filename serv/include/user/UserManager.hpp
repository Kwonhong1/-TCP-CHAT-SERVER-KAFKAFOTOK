#pragma once

#include "common/Asio.hpp"
#include "user/User.hpp"
#include <memory>
#include <string>
#include <unordered_map>

class UserManager
{
    public:
    
        explicit UserManager(
            boost::asio::io_context& io_context);
    awaitable<std::shared_ptr<User>>
        GetOrCreateUserAsync(
            uint32_t user_id,
            const std::string& username);
    awaitable<std::shared_ptr<User>>
        GetUserByIdAsync(
            uint32_t user_id);
    awaitable<std::shared_ptr<User>>
        GetUserByNameAsync(
            const std::string& username);

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
