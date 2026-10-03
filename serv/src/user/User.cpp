#include "user/User.hpp"

//--------------------------------------------------
// constructor
//--------------------------------------------------

User::User(boost::asio::io_context& io_context, uint32_t id, std::string username)
    : strand_(boost::asio::make_strand(io_context)),
      id_(id),
      username_(std::move(username)),
      is_online_(false),
      room_id_(0),
      reconnect_generation_(0),
      reconnect_timer_(io_context)
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

awaitable<void> User::SetOnlineAsync(bool online)
{
    co_await boost::asio::dispatch(strand_, use_awaitable);
    is_online_ = online;
}

awaitable<bool> User::IsOnlineAsync()
{
    co_await boost::asio::dispatch(strand_, use_awaitable);
    co_return is_online_;
}

awaitable<void> User::SetSessionAsync(std::shared_ptr<ChatSession> session)
{
    co_await boost::asio::dispatch(strand_, use_awaitable);
    session_ = session;
}

awaitable<std::shared_ptr<ChatSession>> User::GetSessionAsync()
{
    co_await boost::asio::dispatch(strand_, use_awaitable);
    co_return session_.lock();
}

awaitable<void> User::SetRoomIdAsync(uint32_t room_id)
{
    co_await boost::asio::dispatch(strand_, use_awaitable);
    room_id_ = room_id;
}

awaitable<uint32_t> User::GetRoomIdAsync()
{
    co_await boost::asio::dispatch(strand_, use_awaitable);
    co_return room_id_;
}

//--------------------------------------------------
// reconnect lifecycle
//--------------------------------------------------

awaitable<uint64_t> User::BeginReconnectGraceAsync(std::chrono::seconds timeout)
{
    co_await boost::asio::dispatch(strand_, use_awaitable);

    is_online_ = false;

    ++reconnect_generation_;

    reconnect_timer_.expires_after(timeout);

    co_return reconnect_generation_;
}

awaitable<bool> User::WaitReconnectGraceAsync()
{
    co_await boost::asio::dispatch(strand_, use_awaitable);

    boost::system::error_code ec;

    co_await reconnect_timer_.async_wait(
        boost::asio::redirect_error(use_awaitable, ec)
    );

    if (ec == boost::asio::error::operation_aborted) {
        co_return false;
    }

    if (ec) {
        co_return false;
    }

    co_return true;
}

awaitable<void> User::CompleteReconnectAsync(std::shared_ptr<ChatSession> session)
{
    co_await boost::asio::dispatch(strand_, use_awaitable);

    ++reconnect_generation_;

    boost::system::error_code ec;
    reconnect_timer_.cancel(ec);

    session_ = session;
    is_online_ = true;
}

awaitable<bool> User::TryExpireReconnectAsync(uint64_t generation)
{
    co_await boost::asio::dispatch(strand_, use_awaitable);

    if (reconnect_generation_ != generation) {
        co_return false;
    }

    if (is_online_) {
        co_return false;
    }

    ++reconnect_generation_;

    co_return true;
}