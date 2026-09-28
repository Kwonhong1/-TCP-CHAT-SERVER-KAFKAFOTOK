#include "repository/UserRepository.hpp"


// Implementations moved out of the header during refactor v2.
UserRepository::UserRepository(
        std::shared_ptr<grpc::Channel> channel)
        :
        stub_(
            chatdb::ChatDBService::NewStub(channel)
        )
{
    }

//==================================================
    // [FIX] gRPC context/request/response lifetime 보장
    //==================================================

    awaitable<AuthResult> UserRepository::AuthenticateUserAsync(
        std::string username,
        std::string password)
{
        auto executor =
            co_await boost::asio::this_coro::executor;

        co_return co_await boost::asio::async_initiate<
            decltype(use_awaitable),
            void(AuthResult)>(

            [this,
             username = std::move(username),
             password = std::move(password),
             executor](auto handler) mutable
            {
                auto context =
                    std::make_shared<
                        grpc::ClientContext>();

                auto req =
                    std::make_shared<
                        chatdb::AuthRequest>();

                req->set_username(username);
                req->set_password(password);

                auto res =
                    std::make_shared<
                        chatdb::AuthResponse>();

                using Handler =
                    std::decay_t<decltype(handler)>;

                auto handler_ptr =
                    std::make_shared<Handler>(
                        std::move(handler)
                    );

                stub_->async()->AuthenticateUser(
                    context.get(),
                    req.get(),
                    res.get(),

                    [executor,
                     username,
                     context,
                     req,
                     res,
                     handler_ptr]
                    (grpc::Status status) mutable
                    {
                        AuthResult result;

                        if (
                            status.ok() &&
                            res->success()
                        )
                        {
                            result.success = true;

                            result.user_data.id =
                                res->user_id();

                            result.user_data.username =
                                username;

                            result.reconnect_token =
                                res->reconnect_token();
                        }
                        else
                        {
                            result.success = false;

                            result.error_msg =
                                !res->error_message().empty()
                                ? res->error_message()
                                : status.error_message();
                        }

                        boost::asio::post(
                            executor,

                            [handler_ptr,
                             result = std::move(result)]
                            () mutable
                            {
                                (*handler_ptr)(
                                    std::move(result)
                                );
                            }
                        );
                    }
                );
            },

            use_awaitable
        );
    }

awaitable<RegisterResult> UserRepository::RegisterUserAsync(
        std::string username,
        std::string password)
{
        auto executor =
            co_await boost::asio::this_coro::executor;

        co_return co_await boost::asio::async_initiate<
            decltype(use_awaitable),
            void(RegisterResult)>(

            [this,
             username = std::move(username),
             password = std::move(password),
             executor](auto handler) mutable
            {
                auto context =
                    std::make_shared<
                        grpc::ClientContext>();

                auto req =
                    std::make_shared<
                        chatdb::RegisterRequest>();

                req->set_username(username);
                req->set_password(password);

                auto res =
                    std::make_shared<
                        chatdb::RegisterResponse>();

                using Handler =
                    std::decay_t<decltype(handler)>;

                auto handler_ptr =
                    std::make_shared<Handler>(
                        std::move(handler)
                    );

                stub_->async()->RegisterUser(
                    context.get(),
                    req.get(),
                    res.get(),

                    [executor,
                     context,
                     req,
                     res,
                     handler_ptr]
                    (grpc::Status status) mutable
                    {
                        RegisterResult result;

                        if (
                            status.ok() &&
                            res->success()
                        )
                        {
                            result.success = true;
                            result.assigned_id =
                                res->assigned_id();
                        }
                        else
                        {
                            result.success = false;

                            result.error_msg =
                                !res->error_message().empty()
                                ? res->error_message()
                                : status.error_message();
                        }

                        boost::asio::post(
                            executor,

                            [handler_ptr,
                             result = std::move(result)]
                            () mutable
                            {
                                (*handler_ptr)(
                                    std::move(result)
                                );
                            }
                        );
                    }
                );
            },

            use_awaitable
        );
    }

awaitable<VerifyTokenResult> UserRepository::VerifyTokenAsync(
        std::string token)
{
        auto executor =
            co_await boost::asio::this_coro::executor;

        co_return co_await boost::asio::async_initiate<
            decltype(use_awaitable),
            void(VerifyTokenResult)>(

            [this,
             token = std::move(token),
             executor](auto handler) mutable
            {
                auto context =
                    std::make_shared<
                        grpc::ClientContext>();

                auto req =
                    std::make_shared<
                        chatdb::VerifyTokenRequest>();

                req->set_token(token);

                auto res =
                    std::make_shared<
                        chatdb::VerifyTokenResponse>();

                using Handler =
                    std::decay_t<decltype(handler)>;

                auto handler_ptr =
                    std::make_shared<Handler>(
                        std::move(handler)
                    );

                stub_->async()->VerifyToken(
                    context.get(),
                    req.get(),
                    res.get(),

                    [executor,
                     context,
                     req,
                     res,
                     handler_ptr]
                    (grpc::Status status) mutable
                    {
                        VerifyTokenResult result;

                        if (
                            status.ok() &&
                            res->success()
                        )
                        {
                            result.success = true;
                            result.user_id =
                                res->user_id();
                            result.username =
                                res->username();
                        }
                        else
                        {
                            result.success = false;

                            result.error_msg =
                                !res->error_message().empty()
                                ? res->error_message()
                                : status.error_message();
                        }

                        boost::asio::post(
                            executor,

                            [handler_ptr,
                             result = std::move(result)]
                            () mutable
                            {
                                (*handler_ptr)(
                                    std::move(result)
                                );
                            }
                        );
                    }
                );
            },

            use_awaitable
        );
    }
