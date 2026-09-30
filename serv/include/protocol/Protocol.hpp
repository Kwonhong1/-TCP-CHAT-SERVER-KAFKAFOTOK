#pragma once

#include <cstddef>
#include <cstdint>

constexpr std::size_t MAX_PACKET_SIZE = 20 * 1024;
constexpr std::size_t PACKET_HEADER_SIZE = 12;

enum class MessageType : uint16_t {
    LOGIN_PROMPT = 1000, LOGIN_REQUEST = 1001, LOGIN_RESPONSE = 1002,
    LOGOUT_REQUEST = 1003, LOGOUT_RESPONSE = 1004, CHAT_MESSAGE = 1005,
    JOIN_ROOM = 1006, LEAVE_ROOM = 1007, CREATE_ROOM_REQUEST = 1008,
    CREATE_ROOM_RESPONSE = 1009, ROOM_LIST_REQUEST = 1010, ROOM_LIST_RESPONSE = 1011,
    CHAT_HISTORY_REQUEST = 1012, CHAT_HISTORY_RESPONSE = 1013, SERVER_NOTIFICATION = 1014,
    REGISTER_REQUEST = 1015, REGISTER_RESPONSE = 1016, JOIN_ROOM_RESPONSE = 1017,
    LEAVE_ROOM_RESPONSE = 1018, WHISPER_REQUEST = 1019, WHISPER_RESPONSE = 1020,
    WHISPER_NOTIFICATION = 1021, KICK_USER_REQUEST = 1023, KICK_USER_RESPONSE = 1024,
    KICKED_NOTIFICATION = 1025, TRANSFER_MASTER_REQUEST = 1026,
    TRANSFER_MASTER_RESPONSE = 1027, MASTER_CHANGED_NOTIFICATION = 1028,
    PING = 1029, PONG = 1030
};

enum class RoomPermission : uint32_t {
    NONE = 0, CHAT = 1 << 0, KICK_USER = 1 << 1, BAN_USER = 1 << 2,
    CHANGE_CONFIG = 1 << 3, DELEGATE_HOST = 1 << 4, MEMBER = CHAT,
    HOST = CHAT | KICK_USER | BAN_USER | CHANGE_CONFIG | DELEGATE_HOST
};

RoomPermission operator|(RoomPermission a, RoomPermission b);
bool HasPermission(RoomPermission user_perm, RoomPermission required_perm);

struct PacketHeader {
    uint16_t packet_size = 0;
    MessageType message_type{};
    uint32_t user_id = 0;
    uint32_t sequence_number = 0;
};

void EncodePacketHeader(const PacketHeader& header, char* dst);
PacketHeader DecodePacketHeader(const char* src);
