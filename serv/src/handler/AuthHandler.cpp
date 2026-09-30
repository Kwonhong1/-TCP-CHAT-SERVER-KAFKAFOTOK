#include "handler/AuthHandler.hpp"
#include "server/ChatServer.hpp"
#include "server/ChatSession.hpp"
#include "room/ChatRoom.hpp"
#include "user/User.hpp"
#include "protocol/Protocol.hpp"
#include <chrono>
#include <iostream>
#include <string>

awaitable<void> AuthHandler::HandleLogin(
        ChatServer& server,
        std::shared_ptr<ChatSession> session,
        const chat::LoginRequest& req)
    {
        chat::LoginResponse res;

        uint32_t user_id = 0;

        //--------------------------------------------------
        // reconnect token
        //--------------------------------------------------

        if (
            !req.reconnect_token().empty()
        )
        {
            auto verify_res =
                co_await
                    server
                        .GetUserRepository()
                        ->VerifyTokenAsync(
                            req.reconnect_token()
                        );

            if (verify_res.success)
            {
                user_id =
                    verify_res.user_id;

                auto user =
                    co_await
                        server
                            .GetUserManager()
                            .GetOrCreateUserAsync(
                                user_id,
                                verify_res.username
                            );

                co_await
                    user->SetSessionAsync(
                        session
                    );

                co_await
                    user->SetOnlineAsync(
                        true
                    );

                // 현재 handler는 session packet processing
                // coroutine에서 실행된다.
                session->SetUserId(
                    user_id
                );

                session->SetAuthenticated(
                    true
                );

                
                co_await
                    server
                        .GetSessionRepository()
                        ->SetUserSessionStateAsync(
                            user_id,
                            "ONLINE"
                        );

                res.set_success(true);

                res.set_assigned_user_id(
                    user_id
                );

                res.set_reconnect_token(
                    req.reconnect_token()
                );

                session->Send(
                    MessageType::LOGIN_RESPONSE,
                    res
                );

                co_return;
            }
        }

        //--------------------------------------------------
        // username/password auth
        //--------------------------------------------------

        auto auth_result =
            co_await
                server
                    .GetUserRepository()
                    ->AuthenticateUserAsync(
                        req.username(),
                        req.password()
                    );

        if (auth_result.success)
        {
            user_id =
                auth_result.user_data.id;

            auto user =
                co_await
                    server
                        .GetUserManager()
                        .GetOrCreateUserAsync(
                            user_id,
                            auth_result
                                .user_data
                                .username
                        );

            co_await
                user->SetSessionAsync(
                    session
                );

            co_await
                user->SetOnlineAsync(
                    true
                );

            session->SetUserId(
                user_id
            );

            session->SetAuthenticated(
                true
            );

            co_await
                server
                    .GetSessionRepository()
                    ->SetUserSessionStateAsync(
                        user_id,
                        "ONLINE"
                    );

            res.set_success(true);

            res.set_assigned_user_id(
                user_id
            );

            res.set_reconnect_token(
                auth_result.reconnect_token
            );
        }
        else
        {
            res.set_success(false);

            res.set_error_message(
                auth_result.error_msg
            );
        }

        session->Send(
            MessageType::LOGIN_RESPONSE,
            res
        );
    }

awaitable<void> AuthHandler::HandleRegister(
        ChatServer& server,
        std::shared_ptr<ChatSession> session,
        const chat::RegisterRequest& req)
    {
        auto reg_result =
            co_await
                server
                    .GetUserRepository()
                    ->RegisterUserAsync(
                        req.username(),
                        req.password()
                    );

        chat::RegisterResponse res;

        if (reg_result.success)
        {
            res.set_success(true);

            res.set_assigned_user_id(
                reg_result.assigned_id
            );
        }
        else
        {
            res.set_success(false);

            res.set_error_message(
                reg_result.error_msg
            );
        }

        session->Send(
            MessageType::REGISTER_RESPONSE,
            res
        );
    }

