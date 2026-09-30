#pragma once

#include "common/Asio.hpp"
#include <grpcpp/grpcpp.h>
#include "chat_db.pb.h"
#include "chat_db.grpc.pb.h"
#include <memory>
#include <string>
#include <vector>
#include <type_traits>

class ChatRepository
{
public:

    struct ChatHistoryResult
    {
        bool success{false};

        std::vector<
            chatdb::ChatMessageData
        > messages;

        bool has_more{false};
        std::string error_msg;
    };
    explicit ChatRepository(
            std::shared_ptr<grpc::Channel> channel);
    awaitable<bool> PublishChatAsync(
            uint32_t room_id,
            uint32_t user_id,
            const std::string& message,
            int64_t timestamp);
    awaitable<ChatHistoryResult>
        GetChatHistoryAsync(
            uint32_t room_id,
            uint64_t last_msg_id,
            uint32_t limit);

private:

    std::unique_ptr<
        chatdb::ChatDBService::Stub
    > stub_;
};
