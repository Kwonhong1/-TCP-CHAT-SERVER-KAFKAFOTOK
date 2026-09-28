#pragma once

#include "common/Asio.hpp"
#include "protocol/Protocol.hpp"
#include "user/User.hpp"
#include "server/ChatSession.hpp"
#include "chat_protocol.pb.h"
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

class ChatRoom :
    public std::enable_shared_from_this<ChatRoom>
{
    public:
    
        ChatRoom(
            boost::asio::io_context& io_context,
            uint32_t room_id,
            std::string name,
            uint32_t max_users);
    //--------------------------------------------------
        // immutable
        //--------------------------------------------------
    
        uint32_t GetId() const;
    const std::string& GetName() const;
    uint32_t GetMaxUsers() const;
    //--------------------------------------------------
        // mutable getters
        //--------------------------------------------------
    
        awaitable<uint32_t> GetUserCountAsync();
    awaitable<uint32_t> GetOwnerIdAsync();
    //--------------------------------------------------
        // RoomInfo snapshot
        //--------------------------------------------------
    
        awaitable<chat::RoomInfo>
        GetInfoAsync();
    //--------------------------------------------------
        // AddUser
        //--------------------------------------------------
    
        awaitable<bool> AddUserAsync(
            std::shared_ptr<User> user,
            RoomPermission perm =
                RoomPermission::MEMBER);
    //--------------------------------------------------
        // RemoveUser
        //--------------------------------------------------
    
        awaitable<bool> RemoveUserAsync(
            uint32_t user_id);
    //--------------------------------------------------
        // HasUser
        //--------------------------------------------------
    
        awaitable<bool> HasUserAsync(
            uint32_t user_id);
    //--------------------------------------------------
        // Kick
        //--------------------------------------------------
    
        awaitable<bool> KickUserAsync(
            uint32_t operator_id,
            uint32_t target_id);
    //--------------------------------------------------
        // Transfer Master
        //--------------------------------------------------
    
        awaitable<bool> TransferMasterAsync(
            uint32_t operator_id,
            uint32_t new_master_id);

    //--------------------------------------------------
    // Broadcast
    //--------------------------------------------------

    template <typename T>
    void BroadcastMessage(
        MessageType msg_type,
        const T& proto_msg)
    {
        auto self = shared_from_this();

        boost::asio::post(
            strand_,

            [self,
             msg_type,
             proto_msg]() mutable
            {
                // users_ 자체는 여기서만 읽는다.
                std::vector<
                    std::shared_ptr<User>
                > users_snapshot;

                users_snapshot.reserve(
                    self->users_.size()
                );

                for (
                    auto& [id, user] :
                    self->users_
                )
                {
                    users_snapshot.push_back(
                        user
                    );
                }

                // User::session_은 User strand 소유이므로
                // 별도 coroutine으로 조회
                for (
                    auto& user :
                    users_snapshot
                )
                {
                    co_spawn(
                        self->strand_,

                        [user,
                         msg_type,
                         proto_msg]()
                        -> awaitable<void>
                        {
                            auto session =
                                co_await
                                    user
                                        ->GetSessionAsync();

                            if (session)
                            {
                                session->Send(
                                    msg_type,
                                    proto_msg
                                );
                            }
                        },

                        detached
                    );
                }
            }
        );
    }

private:

    boost::asio::strand<
        boost::asio::io_context::executor_type
    > strand_;

    // immutable
    const uint32_t room_id_;
    const std::string name_;
    const uint32_t max_users_;

    // strand owned
    uint32_t owner_id_;

    std::unordered_map<
        uint32_t,
        std::shared_ptr<User>
    > users_;

    std::unordered_map<
        uint32_t,
        RoomPermission
    > permissions_;
};
