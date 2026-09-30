#include "handler/SystemHandler.hpp"
#include "server/ChatServer.hpp"
#include "server/ChatSession.hpp"
#include "room/ChatRoom.hpp"
#include "user/User.hpp"
#include "protocol/Protocol.hpp"
#include <chrono>
#include <iostream>
#include <string>

awaitable<void> SystemHandler::HandlePing(std::shared_ptr<ChatSession> session)
    {
        PacketHeader pong_header{};
        pong_header.packet_size = static_cast<uint16_t>(PACKET_HEADER_SIZE);
        pong_header.message_type = MessageType::PONG;
        pong_header.user_id = session->GetUserId();
        pong_header.sequence_number = 0;

        std::vector<char> pong_packet(PACKET_HEADER_SIZE);
        EncodePacketHeader(pong_header, pong_packet.data());
        session->Send(pong_packet.data(), pong_packet.size());
        co_return;
    }

