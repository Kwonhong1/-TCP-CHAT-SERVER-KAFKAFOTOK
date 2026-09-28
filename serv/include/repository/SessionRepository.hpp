#pragma once

#include "common/Asio.hpp"
#include <grpcpp/grpcpp.h>
#include "chat_db.pb.h"
#include "chat_db.grpc.pb.h"
#include <memory>
#include <string>
#include <vector>
#include <type_traits>

class SessionRepository
{
    public:
    
        explicit SessionRepository(
            std::shared_ptr<grpc::Channel> channel);
    awaitable<bool> SetUserSessionStateAsync(
            uint32_t user_id,
            const std::string& state,
            int ttl_seconds = 3600);

private:

    std::unique_ptr<
        chatdb::ChatDBService::Stub
    > stub_;
};
