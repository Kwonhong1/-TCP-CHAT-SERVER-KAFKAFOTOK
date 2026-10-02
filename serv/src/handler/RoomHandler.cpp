#include "handler/RoomHandler.hpp"
#include "server/ChatServer.hpp"
#include "server/ChatSession.hpp"
#include "room/ChatRoom.hpp"
#include "user/User.hpp"
#include "protocol/Protocol.hpp"
#include <chrono>
#include <iostream>
#include <string>

awaitable<void> RoomHandler::HandleCreateRoom(ChatServer& server, std::shared_ptr<ChatSession> session, const chat::CreateRoomRequest& req)
    {
        chat::CreateRoomResponse res;
        
        if (session->GetRoomId() != 0) {
            res.set_success(false); res.set_error_message("ALREADY_IN_ROOM");
            session->Send(MessageType::CREATE_ROOM_RESPONSE, res); co_return;
        }

        uint32_t user_id = session->GetUserId();
        auto room = co_await server.GetRoomManager().CreateRoomAsync(req.room_name(), req.max_users());
        auto user = co_await server.GetUserManager().GetUserByIdAsync(user_id);

        if (room && user && co_await room->AddUserAsync(user, RoomPermission::HOST)) {
            session->SetRoomId(room->GetId());
            res.set_success(true); res.set_created_room_id(room->GetId()); res.set_owner_id(user->GetId());
        } else {
            if (room) co_await server.GetRoomManager().DestroyRoomAsync(room->GetId());
            res.set_success(false); res.set_error_message("ROOM_CREATE_FAILED");
        }
        session->Send(MessageType::CREATE_ROOM_RESPONSE, res);
    }

awaitable<void> RoomHandler::HandleRoomList(
        ChatServer& server,
        std::shared_ptr<ChatSession> session,
        const chat::RoomListRequest&)
    {

        auto rooms =
            co_await
                server
                    .GetRoomManager()
                    .GetRoomListAsync();

        chat::RoomListResponse res;

        for (
            const auto& room_info :
            rooms
        )
        {
            *res.add_rooms() =
                room_info;
        }

        session->Send(
            MessageType::ROOM_LIST_RESPONSE,
            res
        );
    }

awaitable<void> RoomHandler::HandleJoinRoom(ChatServer& server, std::shared_ptr<ChatSession> session, const chat::JoinRoomRequest& req)
    {
        chat::JoinRoomResponse res;
        
        if (session->GetRoomId() != 0) {
            res.set_success(false); res.set_error_message("ALREADY_IN_ROOM");
            session->Send(MessageType::JOIN_ROOM_RESPONSE, res); co_return;
        }

        uint32_t user_id = session->GetUserId();
        auto room = co_await server.GetRoomManager().GetRoomAsync(req.room_id());
        auto user = co_await server.GetUserManager().GetUserByIdAsync(user_id);

        if (!room || !user || !co_await room->AddUserAsync(user, RoomPermission::MEMBER)) {
            res.set_success(false); res.set_error_message("JOIN_FAILED_OR_FULL");
            session->Send(MessageType::JOIN_ROOM_RESPONSE, res); co_return;
        }

        session->SetRoomId(room->GetId());
        res.set_success(true); res.set_room_id(room->GetId()); res.set_owner_id(co_await room->GetOwnerIdAsync());

        auto history = co_await server.GetChatRepository()->GetChatHistoryAsync(room->GetId(), 0, 20);
        if (history.success) {
            for (auto it = history.messages.rbegin(); it != history.messages.rend(); ++it) {
                const auto& db_msg = *it;
                auto* msg = res.add_recent_messages();
                msg->set_message_id(db_msg.message_id()); msg->set_room_id(db_msg.room_id());
                msg->set_sender_id(db_msg.sender_id()); msg->set_sender_username(db_msg.sender_name());
                msg->set_message(db_msg.message()); msg->set_timestamp(db_msg.timestamp());
                if (PACKET_HEADER_SIZE + res.ByteSizeLong() > MAX_PACKET_SIZE) {
                    res.mutable_recent_messages()->RemoveLast(); break;
                }
            }
        }
        session->Send(MessageType::JOIN_ROOM_RESPONSE, res);
    }

awaitable<void> RoomHandler::HandleLeaveRoom(ChatServer& server, std::shared_ptr<ChatSession> session, const chat::LeaveRoomRequest& req)
    {
        chat::LeaveRoomResponse res;
        
        if (session->GetRoomId() == 0 || session->GetRoomId() != req.room_id()) {
            res.set_success(false); res.set_error_message("INVALID_ROOM");
            session->Send(MessageType::LEAVE_ROOM_RESPONSE, res); co_return;
        }

        auto room = co_await server.GetRoomManager().GetRoomAsync(req.room_id());
        if (!room || !co_await room->RemoveUserAsync(session->GetUserId())) {
            res.set_success(false); res.set_error_message("LEAVE_FAILED");
            session->Send(MessageType::LEAVE_ROOM_RESPONSE, res); co_return;
        }

        session->SetRoomId(0);
        co_await server.GetRoomManager().DestroyRoomIfEmptyAsync(room->GetId(), room);
        res.set_success(true);
        session->Send(MessageType::LEAVE_ROOM_RESPONSE, res);
    }

