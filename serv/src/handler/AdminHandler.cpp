#include "handler/AdminHandler.hpp"
#include "server/ChatServer.hpp"
#include "server/ChatSession.hpp"
#include "room/ChatRoom.hpp"
#include "user/User.hpp"
#include "protocol/Protocol.hpp"
#include <chrono>
#include <iostream>
#include <string>

awaitable<void> AdminHandler::HandleKickUser(ChatServer& server, std::shared_ptr<ChatSession> session, const chat::KickUserRequest& req)
    {
        chat::KickUserResponse res;

        if (session->GetRoomId() == 0 || session->GetRoomId() != req.room_id()) {
            res.set_success(false); res.set_error_message("NOT_IN_ROOM");
            session->Send(MessageType::KICK_USER_RESPONSE, res); co_return;
        }

        auto room = co_await server.GetRoomManager().GetRoomAsync(req.room_id());
        if (room && co_await room->KickUserAsync(session->GetUserId(), req.target_user_id())) res.set_success(true);
        else { res.set_success(false); res.set_error_message("KICK_PERMISSION_DENIED_OR_NO_USER"); }
        session->Send(MessageType::KICK_USER_RESPONSE, res);
    }

awaitable<void> AdminHandler::HandleTransferMaster(ChatServer& server, std::shared_ptr<ChatSession> session, const chat::TransferMasterRequest& req)
    {
        chat::TransferMasterResponse res;
        
        if (session->GetRoomId() == 0 || session->GetRoomId() != req.room_id()) {
            res.set_success(false); res.set_error_message("NOT_IN_ROOM");
            session->Send(MessageType::TRANSFER_MASTER_RESPONSE, res); co_return;
        }

        auto room = co_await server.GetRoomManager().GetRoomAsync(req.room_id());
        if (room && co_await room->TransferMasterAsync(session->GetUserId(), req.new_master_id())) res.set_success(true);
        else { res.set_success(false); res.set_error_message("TRANSFER_FAILED_NOT_HOST"); }
        session->Send(MessageType::TRANSFER_MASTER_RESPONSE, res);
    }

