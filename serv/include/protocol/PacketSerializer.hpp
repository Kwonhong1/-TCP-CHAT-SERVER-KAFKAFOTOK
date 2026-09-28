#pragma once

#include "protocol/Protocol.hpp"
#include <google/protobuf/io/coded_stream.h>
#include <google/protobuf/io/zero_copy_stream_impl_lite.h>
#include <cstring>
#include <iostream>
#include <limits>
#include <string>
#include <vector>

class PacketSerializer
{
public:
    template <typename T>
    static std::vector<char> Serialize(MessageType msg_type, uint32_t user_id, const T& proto_msg)
    {
        std::string payload;
        if (!proto_msg.SerializeToString(&payload)) {
            std::cerr << "[Serialize Error] Protobuf serialization failed.\n";
            return {};
        }

        const size_t total_size = PACKET_HEADER_SIZE + payload.size();
        if (total_size > MAX_PACKET_SIZE || total_size > std::numeric_limits<uint16_t>::max()) {
            std::cerr << "[Security] Outgoing packet too large: " << total_size << '\n';
            return {};
        }

        PacketHeader header{};
        header.packet_size = static_cast<uint16_t>(total_size);
        header.message_type = msg_type;
        header.user_id = user_id;
        header.sequence_number = 0;

        std::vector<char> send_buffer(total_size);
        EncodePacketHeader(header, send_buffer.data());
        if (!payload.empty()) std::memcpy(send_buffer.data() + PACKET_HEADER_SIZE, payload.data(), payload.size());
        return send_buffer;
    }

    template <typename T>
    static bool ParseProtoStream(const char* payload, size_t payload_size, T& out_proto)
    {
        if (!payload && payload_size > 0) return false;
        if (payload_size == 0) return true;

        google::protobuf::io::ArrayInputStream array_stream(payload, static_cast<int>(payload_size));
        google::protobuf::io::CodedInputStream coded_stream(&array_stream);
        coded_stream.SetRecursionLimit(64);
        return out_proto.ParseFromCodedStream(&coded_stream);
    }
};
