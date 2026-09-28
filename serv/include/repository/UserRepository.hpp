#pragma once

#include "common/Asio.hpp"
#include <grpcpp/grpcpp.h>
#include "chat_db.pb.h"
#include "chat_db.grpc.pb.h"
#include <memory>
#include <string>
#include <vector>
#include <type_traits>

struct DBUserData { uint32_t id; std::string username; };

class UserRepository
{
public:

    struct AuthResult
    {
        bool success{false};
        DBUserData user_data;
        std::string reconnect_token;
        std::string error_msg;
    };

    struct RegisterResult
    {
        bool success{false};
        uint32_t assigned_id{0};
        std::string error_msg;
    };

    struct VerifyTokenResult
    {
        bool success{false};
        uint32_t user_id{0};
        std::string username;
        std::string error_msg;
    };
    explicit UserRepository(
            std::shared_ptr<grpc::Channel> channel);
    //==================================================
        // [FIX] gRPC context/request/response lifetime 보장
        //==================================================
    
        awaitable<AuthResult> AuthenticateUserAsync(
            std::string username,
            std::string password);
    awaitable<RegisterResult> RegisterUserAsync(
            std::string username,
            std::string password);
    awaitable<VerifyTokenResult> VerifyTokenAsync(
            std::string token);

private:

    std::unique_ptr<
        chatdb::ChatDBService::Stub
    > stub_;
};
