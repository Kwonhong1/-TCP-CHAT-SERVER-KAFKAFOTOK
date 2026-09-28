#pragma once
#include "common/Asio.hpp"
#include "chat_protocol.pb.h"
#include <memory>
class ChatServer; class ChatSession;
class ChatHandler { public:
    static awaitable<void> HandleChatMessage(ChatServer&, std::shared_ptr<ChatSession>, const chat::ChatMessage&);
    static awaitable<void> HandleChatHistory(ChatServer&, std::shared_ptr<ChatSession>, const chat::ChatHistoryRequest&);
    static awaitable<void> HandleWhisper(ChatServer&, std::shared_ptr<ChatSession>, const chat::WhisperRequest&);
};
