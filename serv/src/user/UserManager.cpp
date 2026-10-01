#include "user/UserManager.hpp"


// Implementations moved out of the header during refactor v2.
    UserManager::UserManager(
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
    UserManager::GetOrCreateUserAsync(
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
    UserManager::GetUserByIdAsync(
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
    UserManager::GetUserByNameAsync(
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
//--------------------------------------------------
// Disconnect cleanup
//--------------------------------------------------

awaitable<void>
UserManager::SetUserOfflineAsync(
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
        co_return;
    }

    auto user =
        it->second;

    co_await user->SetOnlineAsync(
        false
    );
}