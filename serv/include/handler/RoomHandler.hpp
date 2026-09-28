#pragma once
#include "common/Asio.hpp"
#include "chat_protocol.pb.h"
#include <memory>
class ChatServer; class ChatSession;
class RoomHandler { public:
    static awaitable<void> HandleCreateRoom(ChatServer&, std::shared_ptr<ChatSession>, const chat::CreateRoomRequest&);
    static awaitable<void> HandleRoomList(ChatServer&, std::shared_ptr<ChatSession>, const chat::RoomListRequest&);
    static awaitable<void> HandleJoinRoom(ChatServer&, std::shared_ptr<ChatSession>, const chat::JoinRoomRequest&);
    static awaitable<void> HandleLeaveRoom(ChatServer&, std::shared_ptr<ChatSession>, const chat::LeaveRoomRequest&);
};
