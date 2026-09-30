#include "protocol/Protocol.hpp"
#include <boost/endian/conversion.hpp>
#include <cstring>

RoomPermission operator|(RoomPermission a, RoomPermission b) {
    return static_cast<RoomPermission>(static_cast<uint32_t>(a) | static_cast<uint32_t>(b));
}

bool HasPermission(RoomPermission user_perm, RoomPermission required_perm) {
    return (static_cast<uint32_t>(user_perm) & static_cast<uint32_t>(required_perm)) == static_cast<uint32_t>(required_perm);
}

void EncodePacketHeader(const PacketHeader& header, char* dst) {
    uint16_t packet_size = boost::endian::native_to_little(header.packet_size);
    uint16_t message_type = boost::endian::native_to_little(static_cast<uint16_t>(header.message_type));
    uint32_t user_id = boost::endian::native_to_little(header.user_id);
    uint32_t sequence_number = boost::endian::native_to_little(header.sequence_number);
    std::memcpy(dst + 0, &packet_size, sizeof(packet_size));
    std::memcpy(dst + 2, &message_type, sizeof(message_type));
    std::memcpy(dst + 4, &user_id, sizeof(user_id));
    std::memcpy(dst + 8, &sequence_number, sizeof(sequence_number));
}

PacketHeader DecodePacketHeader(const char* src) {
    uint16_t packet_size, message_type; uint32_t user_id, sequence_number;
    std::memcpy(&packet_size, src + 0, sizeof(packet_size));
    std::memcpy(&message_type, src + 2, sizeof(message_type));
    std::memcpy(&user_id, src + 4, sizeof(user_id));
    std::memcpy(&sequence_number, src + 8, sizeof(sequence_number));
    PacketHeader header{};
    header.packet_size = boost::endian::little_to_native(packet_size);
    header.message_type = static_cast<MessageType>(boost::endian::little_to_native(message_type));
    header.user_id = boost::endian::little_to_native(user_id);
    header.sequence_number = boost::endian::little_to_native(sequence_number);
    return header;
}
