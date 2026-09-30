#pragma once

#include <boost/asio.hpp>
#include <boost/asio/ssl.hpp>
#include <boost/asio/co_spawn.hpp>
#include <boost/asio/detached.hpp>
#include <boost/asio/use_awaitable.hpp>

using boost::asio::ip::tcp;
using boost::asio::awaitable;
using boost::asio::use_awaitable;
using boost::asio::co_spawn;
using boost::asio::detached;
namespace ssl = boost::asio::ssl;
