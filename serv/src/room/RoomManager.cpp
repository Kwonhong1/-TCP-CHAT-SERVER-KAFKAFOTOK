#include "room/RoomManager.hpp"


// Implementations moved out of the header during refactor v2.
    RoomManager::RoomManager(
        boost::asio::io_context& io_context)
        :
        io_context_(io_context),
        strand_(
            boost::asio::make_strand(
                io_context
            )
        ),
        next_room_id_(1)
{
    }

//--------------------------------------------------
    // Create
    //--------------------------------------------------

    awaitable<std::shared_ptr<ChatRoom>>
    RoomManager::CreateRoomAsync(
        const std::string& name,
        uint32_t max_users)
{
        co_await boost::asio::dispatch(
            strand_,
            use_awaitable
        );

        uint32_t id =
            next_room_id_++;

        auto room =
            std::make_shared<ChatRoom>(
                io_context_,
                id,
                name,
                max_users
            );

        rooms_[id] = room;

        co_return room;
    }

//--------------------------------------------------
    // Get
    //--------------------------------------------------

    awaitable<std::shared_ptr<ChatRoom>>
    RoomManager::GetRoomAsync(
        uint32_t room_id)
{
        co_await boost::asio::dispatch(
            strand_,
            use_awaitable
        );

        auto it =
            rooms_.find(room_id);

        if (
            it == rooms_.end()
        )
        {
            co_return nullptr;
        }

        co_return it->second;
    }

//--------------------------------------------------
    // Destroy
    //--------------------------------------------------

    awaitable<void> RoomManager::DestroyRoomAsync(
        uint32_t room_id)
{
        co_await boost::asio::dispatch(
            strand_,
            use_awaitable
        );

        rooms_.erase(room_id);
    }

//--------------------------------------------------
    // 방 목록
    //
    // Manager strand에서는 rooms_의 shared_ptr만 snapshot.
    // 각 Room의 mutable state는 Room strand에서 조회.
    //--------------------------------------------------

    awaitable<std::vector<chat::RoomInfo>>
    RoomManager::GetRoomListAsync()
{
        std::vector<
            std::shared_ptr<ChatRoom>
        > room_snapshot;

        co_await boost::asio::dispatch(
            strand_,
            use_awaitable
        );

        room_snapshot.reserve(
            std::min<size_t>(
                rooms_.size(),
                16
            )
        );

        for (
            auto& [id, room] :
            rooms_
        )
        {
            room_snapshot.push_back(
                room
            );

            if (
                room_snapshot.size() >= 16
            )
            {
                break;
            }
        }

        std::vector<chat::RoomInfo> list;

        list.reserve(
            room_snapshot.size()
        );

        for (
            auto& room :
            room_snapshot
        )
        {
            list.push_back(
                co_await
                    room->GetInfoAsync()
            );
        }

        co_return list;
    }

//--------------------------------------------------
    // 빈 방 정리
    //
    // shared_ptr identity까지 확인해서
    // 오래된 ChatRoom 객체가 새 room을 지우지 못하게 함.
    //--------------------------------------------------

    awaitable<bool> RoomManager::DestroyRoomIfEmptyAsync(
        uint32_t room_id,
        std::shared_ptr<ChatRoom> expected_room)
{
        if (!expected_room)
            co_return false;

        uint32_t count =
            co_await
                expected_room
                    ->GetUserCountAsync();

        if (count != 0)
            co_return false;

        co_await boost::asio::dispatch(
            strand_,
            use_awaitable
        );

        auto it =
            rooms_.find(room_id);

        if (
            it == rooms_.end()
        )
        {
            co_return false;
        }

        if (
            it->second != expected_room
        )
        {
            co_return false;
        }

        // Manager 확인 후 Room이 다시 채워졌을 가능성을
        // 줄이기 위해 한 번 더 확인한다.
        //
        // 완전한 lifecycle atomicity는 이후
        // room closing state를 도입하면 더 강화 가능하다.

        uint32_t final_count =
            co_await
                expected_room
                    ->GetUserCountAsync();

        if (final_count != 0)
            co_return false;

        // 같은 room인지 다시 확인
        auto final_it =
            rooms_.find(room_id);

        if (
            final_it == rooms_.end() ||
            final_it->second != expected_room
        )
        {
            co_return false;
        }

        rooms_.erase(final_it);

        co_return true;
    }
//--------------------------------------------------
// Disconnect cleanup
//--------------------------------------------------

awaitable<bool>
RoomManager::RemoveUserAndCleanupRoomAsync(
    uint32_t room_id,
    uint32_t user_id)
{
    auto room =
        co_await GetRoomAsync(
            room_id
        );

    if (!room)
        co_return false;

    co_await room->RemoveUserAsync(
        user_id
    );

    bool destroyed =
        co_await DestroyRoomIfEmptyAsync(
            room_id,
            room
        );

    co_return destroyed;
}