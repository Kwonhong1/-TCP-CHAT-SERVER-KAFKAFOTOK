#include "room/RoomLifecycle.hpp"
#include "room/ChatRoom.hpp"
#include "room/RoomManager.hpp"
#include "server/ChatServer.hpp"
#include "repository/ChatRepository.hpp"

#include <iostream>

awaitable<bool> RoomLifecycle::CleanupIfEmptyAsync(
    ChatServer& server,
    std::shared_ptr<ChatRoom> room)
{
    if (!room) {
        co_return false;
    }

    uint32_t room_id = room->GetId();

    bool destroyed = co_await server.GetRoomManager().DestroyRoomIfEmptyAsync(room_id, room);

    if (!destroyed) {
        co_return false;
    }

    auto result = co_await server.GetChatRepository()->DeleteRoomHistoryAsync(room_id);

    if (!result.success) {
        std::cerr << "[RoomLifecycle] Failed to delete room history: "
                  << result.error_msg << '\n';
    }

    co_return true;
}