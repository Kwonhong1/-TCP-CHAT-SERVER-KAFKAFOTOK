#include "protocol/RingPacketBuffer.hpp"


// Implementations moved out of the header during refactor v2.
    RingPacketBuffer::RingPacketBuffer(
        size_t capacity)
        :
        buffer_(capacity),
        capacity_(capacity),
        head_(0),
        tail_(0),
        size_(0)
{
    }

bool RingPacketBuffer::WriteData(
        const char* data,
        size_t len)
{
        if (capacity_ - size_ < len)
            return false;

        size_t first_part =
            std::min(
                len,
                capacity_ - tail_
            );

        std::memcpy(
            &buffer_[tail_],
            data,
            first_part
        );

        size_t second_part =
            len - first_part;

        if (second_part > 0)
        {
            std::memcpy(
                &buffer_[0],
                data + first_part,
                second_part
            );
        }

        tail_ =
            (tail_ + len) % capacity_;

        size_ += len;

        return true;
    }

int RingPacketBuffer::ReadPacket(
    std::vector<char>& out_packet)
{
        // 아직 헤더 12바이트조차 안 들어왔으면 대기
        if (size_ < PACKET_HEADER_SIZE)
            return 0;

        // Ring Buffer는 메모리가 중간에서 wrap될 수 있으므로
        // 우선 wire header 12바이트를 연속된 임시 버퍼로 복사
        char header_buffer[PACKET_HEADER_SIZE];

        PeekBytes(
            header_buffer,
            PACKET_HEADER_SIZE
        );

        // Little Endian Wire Header
        //        ↓
        // 현재 CPU의 Native Endian
        PacketHeader header =
            DecodePacketHeader(
                header_buffer
            );

        // 비정상 패킷 크기 검사
        if (
            header.packet_size > MAX_PACKET_SIZE ||
            header.packet_size < PACKET_HEADER_SIZE
        )
        {
            std::cerr
                << "[Security] Malformed or Oversized Packet size: "
                << header.packet_size
                << std::endl;

            return -1;
        }

        // 전체 패킷이 아직 도착하지 않았으면 대기
        if (size_ < header.packet_size)
            return 0;

        // 완전한 패킷이 도착했으므로
        // wire format 그대로 꺼낸다.
        out_packet.resize(
            header.packet_size
        );

        ReadBytes(
            out_packet.data(),
            header.packet_size
        );

        return 1;
    }
    void RingPacketBuffer::PeekBytes(
        char* dest,
        size_t len) const
{
        size_t first_part =
            std::min(
                len,
                capacity_ - head_
            );

        std::memcpy(
            dest,
            &buffer_[head_],
            first_part
        );

        if (len > first_part)
        {
            std::memcpy(
                dest + first_part,
                &buffer_[0],
                len - first_part
            );
        }
    }

void RingPacketBuffer::ReadBytes(
        char* dest,
        size_t len)
{
        PeekBytes(dest, len);

        head_ =
            (head_ + len) % capacity_;

        size_ -= len;
    }
