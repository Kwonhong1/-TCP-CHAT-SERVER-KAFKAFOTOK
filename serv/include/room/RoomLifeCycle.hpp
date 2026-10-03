#pragma once

#include "common/Asio.hpp"
#include <memory>

class ChatServer;
class ChatRoom;

class RoomLifecycle {
public:
    static awaitable<bool> CleanupIfEmptyAsync(
        ChatServer& server,
        std::shared_ptr<ChatRoom> room);
};