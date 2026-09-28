#pragma once
#include "common/Asio.hpp"
#include "chat_protocol.pb.h"
#include <memory>
class ChatServer; class ChatSession;
class AdminHandler { public:
    static awaitable<void> HandleKickUser(ChatServer&, std::shared_ptr<ChatSession>, const chat::KickUserRequest&);
    static awaitable<void> HandleTransferMaster(ChatServer&, std::shared_ptr<ChatSession>, const chat::TransferMasterRequest&);
};
