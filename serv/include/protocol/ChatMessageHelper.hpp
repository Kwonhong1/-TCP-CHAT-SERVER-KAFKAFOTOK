#pragma once

#include "chat_db.pb.h"
#include "chat_protocol.pb.h"

namespace ChatMessageMapper {

inline chat::ChatMessage ToProto(const chatdb::ChatMessage& db_msg)
{
    chat::ChatMessage msg;

    msg.set_message_id(db_msg.message_id());
    msg.set_room_id(db_msg.room_id());
    msg.set_sender_id(db_msg.sender_id());
    msg.set_sender_username(db_msg.sender_name());
    msg.set_message(db_msg.message());
    msg.set_timestamp(db_msg.timestamp());

    return msg;
}

}