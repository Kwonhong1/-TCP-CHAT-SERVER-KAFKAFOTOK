#include "room/ChatRoom.hpp"


// Implementations moved out of the header during refactor v2.
    ChatRoom::ChatRoom(
        boost::asio::io_context& io_context,
        uint32_t room_id,
        std::string name,
        uint32_t max_users)
        :
        strand_(
            boost::asio::make_strand(
                io_context
            )
        ),
        room_id_(room_id),
        name_(std::move(name)),
        max_users_(max_users),
        owner_id_(0)
{
    }

//--------------------------------------------------
    // immutable
    //--------------------------------------------------

    uint32_t ChatRoom::GetId() const
{
        return room_id_;
    }

const std::string& ChatRoom::GetName() const
{
        return name_;
    }

uint32_t ChatRoom::GetMaxUsers() const
{
        return max_users_;
    }

//--------------------------------------------------
    // mutable getters
    //--------------------------------------------------

    awaitable<uint32_t> ChatRoom::GetUserCountAsync()
{
        co_await boost::asio::dispatch(
            strand_,
            use_awaitable
        );

        co_return static_cast<uint32_t>(
            users_.size()
        );
    }

awaitable<uint32_t> ChatRoom::GetOwnerIdAsync()
{
        co_await boost::asio::dispatch(
            strand_,
            use_awaitable
        );

        co_return owner_id_;
    }

//--------------------------------------------------
    // RoomInfo snapshot
    //--------------------------------------------------

    awaitable<chat::RoomInfo>
    ChatRoom::GetInfoAsync()
{
        co_await boost::asio::dispatch(
            strand_,
            use_awaitable
        );

        chat::RoomInfo info;

        info.set_room_id(room_id_);
        info.set_room_name(name_);

        info.set_current_users(
            static_cast<uint32_t>(
                users_.size()
            )
        );

        info.set_max_users(
            max_users_
        );

        info.set_owner_id(
            owner_id_
        );

        co_return info;
    }

//--------------------------------------------------
    // AddUser
    //--------------------------------------------------

    awaitable<bool> ChatRoom::AddUserAsync(
        std::shared_ptr<User> user,
        RoomPermission perm =
            RoomPermission::MEMBER)
{
        co_await boost::asio::dispatch(
            strand_,
            use_awaitable
        );

        const uint32_t user_id =
            user->GetId();

        // 이미 들어와 있다면 성공 처리
        if (
            users_.find(user_id) !=
            users_.end()
        )
        {
            co_return true;
        }

        if (
            users_.size() >=
            max_users_
        )
        {
            co_return false;
        }

        users_[user_id] = user;
        permissions_[user_id] = perm;

        if (owner_id_ == 0)
        {
            owner_id_ = user_id;

            permissions_[user_id] =
                RoomPermission::HOST;
        }

        co_return true;
    }

//--------------------------------------------------
    // RemoveUser
    //--------------------------------------------------

    awaitable<bool> ChatRoom::RemoveUserAsync(
        uint32_t user_id)
{
        co_await boost::asio::dispatch(
            strand_,
            use_awaitable
        );

        auto it =
            users_.find(user_id);

        if (it == users_.end())
            co_return false;

        users_.erase(it);
        permissions_.erase(user_id);

        bool master_changed = false;
        uint32_t new_owner_id = 0;

        if (
            owner_id_ == user_id
        )
        {
            if (!users_.empty())
            {
                new_owner_id =
                    users_.begin()->first;

                owner_id_ =
                    new_owner_id;

                permissions_[new_owner_id] =
                    RoomPermission::HOST;

                master_changed = true;
            }
            else
            {
                owner_id_ = 0;
            }
        }

        if (master_changed)
        {
            chat::MasterChangedNotification noti;

            noti.set_room_id(
                room_id_
            );

            noti.set_new_master_id(
                new_owner_id
            );

            // Broadcast 자체가 strand로 안전하게 처리됨
            BroadcastMessage(
                MessageType::MASTER_CHANGED_NOTIFICATION,
                noti
            );
        }

        co_return true;
    }

//--------------------------------------------------
    // HasUser
    //--------------------------------------------------

    awaitable<bool> ChatRoom::HasUserAsync(
        uint32_t user_id)
{
        co_await boost::asio::dispatch(
            strand_,
            use_awaitable
        );

        co_return
            users_.find(user_id) !=
            users_.end();
    }

//--------------------------------------------------
    // Kick
    //--------------------------------------------------

    awaitable<bool> ChatRoom::KickUserAsync(
        uint32_t operator_id,
        uint32_t target_id)
{
        std::shared_ptr<User> target_user;

        {
            co_await boost::asio::dispatch(
                strand_,
                use_awaitable
            );

            auto perm_it =
                permissions_.find(
                    operator_id
                );

            if (
                perm_it ==
                permissions_.end()
            )
            {
                co_return false;
            }

            if (
                !HasPermission(
                    perm_it->second,
                    RoomPermission::KICK_USER
                )
            )
            {
                co_return false;
            }

            auto it =
                users_.find(target_id);

            if (
                it == users_.end()
            )
            {
                co_return false;
            }

            target_user = it->second;

            users_.erase(it);
            permissions_.erase(target_id);
        }

        // User의 session_은 User strand 소유
        auto target_session =
            co_await
                target_user
                    ->GetSessionAsync();

        if (target_session)
        {
            chat::KickedNotification noti;

            noti.set_room_id(
                room_id_
            );

            noti.set_reason(
                "Kicked by room master"
            );

            target_session->Send(
                MessageType::KICKED_NOTIFICATION,
                noti
            );

            // Session mutable state는 Session strand
            co_await
                target_session
                    ->SetRoomIdAsync(0);
        }

        co_return true;
    }

//--------------------------------------------------
    // Transfer Master
    //--------------------------------------------------

    awaitable<bool> ChatRoom::TransferMasterAsync(
        uint32_t operator_id,
        uint32_t new_master_id)
{
        co_await boost::asio::dispatch(
            strand_,
            use_awaitable
        );

        if (
            owner_id_ != operator_id
        )
        {
            co_return false;
        }

        if (
            users_.find(new_master_id) ==
            users_.end()
        )
        {
            co_return false;
        }

        permissions_[owner_id_] =
            RoomPermission::MEMBER;

        owner_id_ =
            new_master_id;

        permissions_[new_master_id] =
            RoomPermission::HOST;

        chat::MasterChangedNotification noti;

        noti.set_room_id(
            room_id_
        );

        noti.set_new_master_id(
            new_master_id
        );

        BroadcastMessage(
            MessageType::MASTER_CHANGED_NOTIFICATION,
            noti
        );

        co_return true;
    }
