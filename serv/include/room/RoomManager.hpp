#pragma once

#include "common/Asio.hpp"
#include "room/ChatRoom.hpp"
#include "chat_protocol.pb.h"
#include <algorithm>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

class RoomManager
{
    public:
    
        explicit RoomManager(
            boost::asio::io_context& io_context);
    //--------------------------------------------------
        // Create
        //--------------------------------------------------
    
        awaitable<std::shared_ptr<ChatRoom>>
        CreateRoomAsync(
            const std::string& name,
            uint32_t max_users);
    //--------------------------------------------------
        // Get
        //--------------------------------------------------
    
        awaitable<std::shared_ptr<ChatRoom>>
        GetRoomAsync(
            uint32_t room_id);
    //--------------------------------------------------
        // Destroy
        //--------------------------------------------------
    
        awaitable<void> DestroyRoomAsync(
            uint32_t room_id);
    //--------------------------------------------------
        // 방 목록
        //
        // Manager strand에서는 rooms_의 shared_ptr만 snapshot.
        // 각 Room의 mutable state는 Room strand에서 조회.
        //--------------------------------------------------
    
        awaitable<std::vector<chat::RoomInfo>>
        GetRoomListAsync();
    //--------------------------------------------------
        // 빈 방 정리
        //
        // shared_ptr identity까지 확인해서
        // 오래된 ChatRoom 객체가 새 room을 지우지 못하게 함.
        //--------------------------------------------------
    
        awaitable<bool> DestroyRoomIfEmptyAsync(
            uint32_t room_id,
            std::shared_ptr<ChatRoom> expected_room);

private:

    boost::asio::io_context& io_context_;

    // [STRAND OWNED]
    boost::asio::strand<
        boost::asio::io_context::executor_type
    > strand_;

    // manager strand에서만 접근하므로 atomic 불필요
    uint32_t next_room_id_;

    std::unordered_map<
        uint32_t,
        std::shared_ptr<ChatRoom>
    > rooms_;
};
