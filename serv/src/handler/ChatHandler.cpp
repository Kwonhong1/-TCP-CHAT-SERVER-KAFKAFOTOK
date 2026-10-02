#include "handler/ChatHandler.hpp"
#include "server/ChatServer.hpp"
#include "server/ChatSession.hpp"
#include "room/ChatRoom.hpp"
#include "user/User.hpp"
#include "protocol/Protocol.hpp"
#include "protocol/ChatMessageMapper.hpp"
#include <chrono>
#include <iostream>
#include <string>

awaitable<void> ChatHandler::HandleChatMessage(ChatServer& server, std::shared_ptr<ChatSession> session, const chat::ChatMessage& msg_param)
    {
        uint32_t user_id = session->GetUserId();
        chat::ChatMessage msg = msg_param;
        if (session->GetRoomId() == 0 || session->GetRoomId() != msg.room_id()) co_return;

        auto room = co_await server.GetRoomManager().GetRoomAsync(msg.room_id());
        auto user = co_await server.GetUserManager().GetUserByIdAsync(user_id);
        if (room && user && co_await room->HasUserAsync(user_id)) {
            msg.set_sender_id(user_id); msg.set_sender_username(user->GetUsername());
            room->BroadcastMessage(MessageType::CHAT_MESSAGE, msg);

            uint32_t room_id = msg.room_id();
            std::string text = msg.message();
            int64_t timestamp = msg.timestamp();
            co_spawn(server.GetIOContext(), [&server, room_id, user_id, text = std::move(text), timestamp]() -> awaitable<void> {
                co_await server.GetChatRepository()->PublishChatAsync(room_id, user_id, text, timestamp);
            }, detached);
        }
    }

awaitable<void> ChatHandler::HandleChatHistory(ChatServer& server, std::shared_ptr<ChatSession> session, const chat::ChatHistoryRequest& req)
    {
        chat::ChatHistoryResponse res;
        res.set_room_id(req.room_id());
        
        if (session->GetRoomId() == 0 || session->GetRoomId() != req.room_id()) {
            res.set_success(false); res.set_error_message("NOT_IN_ROOM");
            session->Send(MessageType::CHAT_HISTORY_RESPONSE, res); co_return;
        }

        auto room = co_await server.GetRoomManager().GetRoomAsync(req.room_id());
        if (!room || !co_await room->HasUserAsync(session->GetUserId())) {
            res.set_success(false); res.set_error_message("NOT_IN_ROOM");
            session->Send(MessageType::CHAT_HISTORY_RESPONSE, res); co_return;
        }

        uint32_t count = req.count();
        if (count == 0) count = 20;
        if (count > 100) count = 100;
        auto result = co_await server.GetChatRepository()->GetChatHistoryAsync(req.room_id(), req.last_message_id(), count);
        if (!result.success) {
            res.set_success(false); res.set_error_message(result.error_msg);
            session->Send(MessageType::CHAT_HISTORY_RESPONSE, res); co_return;
        }

        res.set_success(true); res.set_has_more(result.has_more);
        for (const auto& db_msg : result.messages) {
        *res.add_messages() = ChatMessageMapper::ToProto(db_msg);

        if (PACKET_HEADER_SIZE + res.ByteSizeLong() > MAX_PACKET_SIZE) {
            res.mutable_messages()->RemoveLast();
            res.set_has_more(true);
            break;
            }
        }
        session->Send(MessageType::CHAT_HISTORY_RESPONSE, res);
    }

awaitable<void> ChatHandler::HandleWhisper(ChatServer& server, std::shared_ptr<ChatSession> session, const chat::WhisperRequest& req)
    {
        chat::WhisperResponse res;
        
        if (req.room_id() == 0 || session->GetRoomId() != req.room_id()) {
            res.set_success(false); res.set_error_message("INVALID_ROOM");
            session->Send(MessageType::WHISPER_RESPONSE, res); co_return;
        }

        auto room = co_await server.GetRoomManager().GetRoomAsync(req.room_id());
        if (!room || !co_await room->HasUserAsync(session->GetUserId())) {
            res.set_success(false); res.set_error_message("NOT_IN_ROOM");
            session->Send(MessageType::WHISPER_RESPONSE, res); co_return;
        }

        auto sender = co_await server.GetUserManager().GetUserByIdAsync(session->GetUserId());
        auto target = co_await server.GetUserManager().GetUserByNameAsync(req.target_username());
        if (!sender || !target) {
            res.set_success(false); res.set_error_message("TARGET_NOT_FOUND");
            session->Send(MessageType::WHISPER_RESPONSE, res); co_return;
        }
        if (!co_await room->HasUserAsync(target->GetId())) {
            res.set_success(false); res.set_error_message("TARGET_NOT_IN_ROOM");
            session->Send(MessageType::WHISPER_RESPONSE, res); co_return;
        }

        auto target_session = co_await target->GetSessionAsync();
        if (!target_session) {
            res.set_success(false); res.set_error_message("USER_OFFLINE");
            session->Send(MessageType::WHISPER_RESPONSE, res); co_return;
        }

        chat::WhisperNotification noti;
        noti.set_sender_username(sender->GetUsername()); noti.set_message(req.message());
        target_session->Send(MessageType::WHISPER_NOTIFICATION, noti);
        res.set_success(true);
        session->Send(MessageType::WHISPER_RESPONSE, res);
    }

