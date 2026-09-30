#include "user/User.hpp"


// Implementations moved out of the header during refactor v2.
    User::User(
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

    uint32_t User::GetId() const
{
        return id_;
    }

const std::string& User::GetUsername() const
{
        return username_;
    }

//--------------------------------------------------
    // [STRAND] mutable state
    //--------------------------------------------------

    awaitable<void> User::SetOnlineAsync(
        bool online)
{
        co_await boost::asio::dispatch(
            strand_,
            use_awaitable
        );

        is_online_ = online;
    }

awaitable<bool> User::IsOnlineAsync()
{
        co_await boost::asio::dispatch(
            strand_,
            use_awaitable
        );

        co_return is_online_;
    }

awaitable<void> User::SetSessionAsync(
        std::shared_ptr<ChatSession> session)
{
        co_await boost::asio::dispatch(
            strand_,
            use_awaitable
        );

        session_ = session;
    }

awaitable<std::shared_ptr<ChatSession>>
    User::GetSessionAsync()
{
        co_await boost::asio::dispatch(
            strand_,
            use_awaitable
        );

        co_return session_.lock();
    }
