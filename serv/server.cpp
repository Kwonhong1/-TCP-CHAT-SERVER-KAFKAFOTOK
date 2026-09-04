#include <iostream>
#include <memory>
#include <string>
#include <vector>

#include <grpcpp/grpcpp.h>
#include <boost/asio.hpp>
#include <boost/asio/co_spawn.hpp>
#include <boost/asio/detached.hpp>
#include <boost/asio/use_awaitable.hpp>

// gRPC protoc로 생성된 헤더 파일들
#include "chatdb.grpc.pb.h"
#include "chatdb.pb.h"

using boost::asio::awaitable;
using boost::asio::co_spawn;
using boost::asio::detached;
using boost::asio::use_awaitable;

// ==========================================
// 1. Data Structures
// ==========================================

struct User
{
    uint32_t id{0};
    std::string username;
};

struct AuthResult
{
    bool success{false};
    User user_data;
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

struct ChatHistoryResult
{
    bool success{false};
    std::vector<chatdb::ChatMessage> messages;
    bool has_more{false};
    std::string error_msg;
};

// ==========================================
// 2. UserRepository
// ==========================================

class UserRepository
{
public:
    explicit UserRepository(std::shared_ptr<grpc::Channel> channel)
        : stub_(chatdb::ChatDBService::NewStub(channel)) {}

    awaitable<AuthResult> AuthenticateUserAsync(std::string username, std::string password)
    {
        auto executor = co_await boost::asio::this_coro::executor;

        co_return co_await boost::asio::async_initiate<decltype(use_awaitable), void(AuthResult)>(
            [this, username, password, executor](auto handler) mutable {
                auto context = std::make_shared<grpc::ClientContext>();
                auto req = std::make_shared<chatdb::AuthRequest>();
                req->set_username(username);
                req->set_password(password);

                auto res = std::make_shared<chatdb::AuthResponse>();
                auto handler_ptr = std::make_shared<decltype(handler)>(std::move(handler));

                // [수정] context와 req를 람다 capture에 추가
                stub_->async()->AuthenticateUser(context.get(), req.get(), res.get(),
                    [executor, username, context, req, res, handler_ptr](grpc::Status status) mutable {
                        AuthResult result;
                        if (status.ok() && res->success()) {
                            result.success = true;
                            result.user_data.id = res->user_id();
                            result.user_data.username = username;
                            result.reconnect_token = res->reconnect_token();
                        } else {
                            result.success = false;
                            result.error_msg = !res->error_message().empty() ? res->error_message() : status.error_message();
                        }

                        boost::asio::post(executor, [handler_ptr, result = std::move(result)]() mutable {
                            (*handler_ptr)(result);
                        });
                    });
            },
            use_awaitable
        );
    }

    awaitable<RegisterResult> RegisterUserAsync(std::string username, std::string password)
    {
        auto executor = co_await boost::asio::this_coro::executor;

        co_return co_await boost::asio::async_initiate<decltype(use_awaitable), void(RegisterResult)>(
            [this, username, password, executor](auto handler) mutable {
                auto context = std::make_shared<grpc::ClientContext>();
                auto req = std::make_shared<chatdb::RegisterRequest>();
                req->set_username(username);
                req->set_password(password);

                auto res = std::make_shared<chatdb::RegisterResponse>();
                auto handler_ptr = std::make_shared<decltype(handler)>(std::move(handler));

                // [수정] context와 req를 람다 capture에 추가
                stub_->async()->RegisterUser(context.get(), req.get(), res.get(),
                    [executor, context, req, res, handler_ptr](grpc::Status status) mutable {
                        RegisterResult result;
                        if (status.ok() && res->success()) {
                            result.success = true;
                            result.assigned_id = res->assigned_id();
                        } else {
                            result.success = false;
                            result.error_msg = !res->error_message().empty() ? res->error_message() : status.error_message();
                        }

                        boost::asio::post(executor, [handler_ptr, result = std::move(result)]() mutable {
                            (*handler_ptr)(result);
                        });
                    });
            },
            use_awaitable
        );
    }

    awaitable<VerifyTokenResult> VerifyTokenAsync(std::string token)
    {
        auto executor = co_await boost::asio::this_coro::executor;

        co_return co_await boost::asio::async_initiate<decltype(use_awaitable), void(VerifyTokenResult)>(
            [this, token, executor](auto handler) mutable {
                auto context = std::make_shared<grpc::ClientContext>();
                auto req = std::make_shared<chatdb::VerifyTokenRequest>();
                req->set_token(token);

                auto res = std::make_shared<chatdb::VerifyTokenResponse>();
                auto handler_ptr = std::make_shared<decltype(handler)>(std::move(handler));

                // [수정] context와 req를 람다 capture에 추가
                stub_->async()->VerifyToken(context.get(), req.get(), res.get(),
                    [executor, context, req, res, handler_ptr](grpc::Status status) mutable {
                        VerifyTokenResult result;
                        if (status.ok() && res->success()) {
                            result.success = true;
                            result.user_id = res->user_id();
                            result.username = res->username();
                        } else {
                            result.success = false;
                            result.error_msg = !res->error_message().empty() ? res->error_message() : status.error_message();
                        }

                        boost::asio::post(executor, [handler_ptr, result = std::move(result)]() mutable {
                            (*handler_ptr)(result);
                        });
                    });
            },
            use_awaitable
        );
    }

private:
    std::unique_ptr<chatdb::ChatDBService::Stub> stub_;
};

// ==========================================
// 3. SessionRepository
// ==========================================

class SessionRepository
{
public:
    explicit SessionRepository(std::shared_ptr<grpc::Channel> channel)
        : stub_(chatdb::ChatDBService::NewStub(channel)) {}

    awaitable<bool> SetUserSessionStateAsync(uint32_t user_id, const std::string& state, int ttl_seconds = 3600)
    {
        auto executor = co_await boost::asio::this_coro::executor;

        co_return co_await boost::asio::async_initiate<decltype(use_awaitable), void(bool)>(
            [this, user_id, state, ttl_seconds, executor](auto handler) mutable {
                auto context = std::make_shared<grpc::ClientContext>();
                auto req = std::make_shared<chatdb::SessionStateRequest>();
                req->set_user_id(user_id);
                req->set_state(state);
                req->set_ttl_seconds(ttl_seconds);

                auto res = std::make_shared<chatdb::SessionStateResponse>();
                auto handler_ptr = std::make_shared<decltype(handler)>(std::move(handler));

                // [수정] context와 req를 람다 capture에 추가
                stub_->async()->SetSessionState(context.get(), req.get(), res.get(),
                    [executor, context, req, res, handler_ptr](grpc::Status status) mutable {
                        bool success = status.ok() && res->success();
                        boost::asio::post(executor, [handler_ptr, success]() mutable {
                            (*handler_ptr)(success);
                        });
                    });
            },
            use_awaitable
        );
    }

private:
    std::unique_ptr<chatdb::ChatDBService::Stub> stub_;
};

// ==========================================
// 4. ChatRepository
// ==========================================

class ChatRepository
{
public:
    explicit ChatRepository(std::shared_ptr<grpc::Channel> channel)
        : stub_(chatdb::ChatDBService::NewStub(channel)) {}

    awaitable<bool> PublishChatAsync(uint32_t room_id, uint32_t user_id, const std::string& message, int64_t timestamp)
    {
        auto executor = co_await boost::asio::this_coro::executor;

        co_return co_await boost::asio::async_initiate<decltype(use_awaitable), void(bool)>(
            [this, room_id, user_id, message, timestamp, executor](auto handler) mutable {
                auto context = std::make_shared<grpc::ClientContext>();
                auto req = std::make_shared<chatdb::ChatPublishRequest>();
                req->set_room_id(room_id);
                req->set_user_id(user_id);
                req->set_message(message);
                req->set_timestamp(timestamp);

                auto res = std::make_shared<chatdb::ChatPublishResponse>();
                auto handler_ptr = std::make_shared<decltype(handler)>(std::move(handler));

                // [수정] context와 req를 람다 capture에 추가
                stub_->async()->PublishChat(context.get(), req.get(), res.get(),
                    [executor, context, req, res, handler_ptr](grpc::Status status) mutable {
                        bool success = status.ok() && res->success();
                        boost::asio::post(executor, [handler_ptr, success]() mutable {
                            (*handler_ptr)(success);
                        });
                    });
            },
            use_awaitable
        );
    }

    awaitable<ChatHistoryResult> GetChatHistoryAsync(uint32_t room_id, uint64_t last_msg_id, uint32_t limit)
    {
        auto executor = co_await boost::asio::this_coro::executor;

        co_return co_await boost::asio::async_initiate<decltype(use_awaitable), void(ChatHistoryResult)>(
            [this, room_id, last_msg_id, limit, executor](auto handler) mutable {
                auto context = std::make_shared<grpc::ClientContext>();
                auto req = std::make_shared<chatdb::ChatHistoryRequest>();
                req->set_room_id(room_id);
                req->set_last_message_id(last_msg_id);
                req->set_limit(limit);

                auto res = std::make_shared<chatdb::ChatHistoryResponse>();
                auto handler_ptr = std::make_shared<decltype(handler)>(std::move(handler));

                // [수정] context를 람다 capture에 추가 (req는 기존에 존재)
                stub_->async()->GetChatHistory(context.get(), req.get(), res.get(),
                    [executor, context, res, handler_ptr, req](grpc::Status status) mutable {
                        ChatHistoryResult result;
                        if (status.ok() && res->success()) {
                            result.success = true;
                            result.has_more = (res->messages_size() >= static_cast<int>(req->limit()));
                            for (const auto& msg : res->messages()) {
                                result.messages.push_back(msg);
                            }
                        } else {
                            result.success = false;
                            result.error_msg = status.error_message();
                        }

                        boost::asio::post(executor, [handler_ptr, result = std::move(result)]() mutable {
                            (*handler_ptr)(result);
                        });
                    });
            },
            use_awaitable
        );
    }

private:
    std::unique_ptr<chatdb::ChatDBService::Stub> stub_;
};

// ==========================================
// 5. Coroutine Example Usage (Main)
// ==========================================

awaitable<void> RunClientTests(std::shared_ptr<grpc::Channel> channel)
{
    UserRepository user_repo(channel);
    SessionRepository session_repo(channel);
    ChatRepository chat_repo(channel);

    std::cout << "[Test 1] Register User..." << std::endl;
    auto reg_res = co_await user_repo.RegisterUserAsync("alice", "password123");
    if (reg_res.success) {
        std::cout << " -> Success! Assigned ID: " << reg_res.assigned_id << std::endl;
    } else {
        std::cout << " -> Failed: " << reg_res.error_msg << std::endl;
    }

    std::cout << "\n[Test 2] Authenticate User..." << std::endl;
    auto auth_res = co_await user_repo.AuthenticateUserAsync("alice", "password123");
    if (auth_res.success) {
        std::cout << " -> Success! User ID: " << auth_res.user_data.id 
                  << ", Token: " << auth_res.reconnect_token << std::endl;

        std::cout << "\n[Test 3] Set Session State..." << std::endl;
        bool state_ok = co_await session_repo.SetUserSessionStateAsync(auth_res.user_data.id, "ONLINE");
        std::cout << " -> Session state updated: " << (state_ok ? "OK" : "FAILED") << std::endl;

        std::cout << "\n[Test 4] Verify Token..." << std::endl;
        auto verify_res = co_await user_repo.VerifyTokenAsync(auth_res.reconnect_token);
        std::cout << " -> Token Validated: " << (verify_res.success ? "YES" : "NO") << std::endl;

        std::cout << "\n[Test 5] Publish Chat Message..." << std::endl;
        bool pub_ok = co_await chat_repo.PublishChatAsync(101, auth_res.user_data.id, "Hello, Boost.Asio & gRPC!", 1234567890);
        std::cout << " -> Message Published: " << (pub_ok ? "OK" : "FAILED") << std::endl;

        std::cout << "\n[Test 6] Get Chat History..." << std::endl;
        auto history_res = co_await chat_repo.GetChatHistoryAsync(101, 0, 10);
        if (history_res.success) {
            std::cout << " -> Fetched " << history_res.messages.size() << " messages." << std::endl;
        } else {
            std::cout << " -> Fetch History Failed: " << history_res.error_msg << std::endl;
        }
    } else {
        std::cout << " -> Auth Failed: " << auth_res.error_msg << std::endl;
    }
}

int main()
{
    try {
        boost::asio::io_context io_context;

        // gRPC 채널 생성 (localhost:50051 가정)
        auto channel = grpc::CreateChannel("localhost:50051", grpc::InsecureChannelCredentials());

        // Coroutine 실행
        co_spawn(io_context, RunClientTests(channel), detached);

        std::cout << "Starting Boost.Asio IO Event Loop..." << std::endl;
        io_context.run();
        std::cout << "IO Event Loop Finished." << std::endl;

    } catch (const std::exception& e) {
        std::cerr << "Exception: " << e.what() << std::endl;
    }

    return 0;
}

