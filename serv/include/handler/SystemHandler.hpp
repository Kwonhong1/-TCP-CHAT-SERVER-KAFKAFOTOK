#pragma once
#include "common/Asio.hpp"
#include <memory>
class ChatSession;
class SystemHandler { public: static awaitable<void> HandlePing(std::shared_ptr<ChatSession>); };
