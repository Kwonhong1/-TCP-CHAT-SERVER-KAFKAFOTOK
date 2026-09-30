#pragma once

#include "protocol/Protocol.hpp"
#include <algorithm>
#include <cstring>
#include <iostream>
#include <vector>

class RingPacketBuffer
{
    public:
    
        explicit RingPacketBuffer(
            size_t capacity = 32 * 1024);
    bool WriteData(
            const char* data,
            size_t len);
    int ReadPacket(
        std::vector<char>& out_packet);
    private:
    
        void PeekBytes(
            char* dest,
            size_t len) const;
    void ReadBytes(
            char* dest,
            size_t len);

private:

    std::vector<char> buffer_;

    size_t capacity_;
    size_t head_;
    size_t tail_;
    size_t size_;
};
