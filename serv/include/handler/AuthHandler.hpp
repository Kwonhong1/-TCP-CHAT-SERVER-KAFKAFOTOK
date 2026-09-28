#pragma once
#include "common/Asio.hpp"
#include "chat_protocol.pb.h"
#include <memory>
class ChatServer; class ChatSession;
class AuthHandler { public:
    static awaitable<void> HandleLogin(ChatServer&, std::shared_ptr<ChatSession>, const chat::LoginRequest&);
    static awaitable<void> HandleRegister(ChatServer&, std::shared_ptr<ChatSession>, const chat::RegisterRequest&);
};
