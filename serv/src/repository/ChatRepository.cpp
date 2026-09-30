#include "repository/ChatRepository.hpp"


// Implementations moved out of the header during refactor v2.
ChatRepository::ChatRepository(
        std::shared_ptr<grpc::Channel> channel)
        :
        stub_(
            chatdb::ChatDBService::NewStub(channel)
        )
{
    }

awaitable<bool> ChatRepository::PublishChatAsync(
        uint32_t room_id,
        uint32_t user_id,
        const std::string& message,
        int64_t timestamp)
{
        auto executor =
            co_await boost::asio::this_coro::executor;

        co_return co_await boost::asio::async_initiate<
            decltype(use_awaitable),
            void(bool)>(

            [this,
             room_id,
             user_id,
             message,
             timestamp,
             executor](auto handler) mutable
            {
                auto context =
                    std::make_shared<
                        grpc::ClientContext>();

                auto req =
                    std::make_shared<
                        chatdb::ChatPublishRequest>();

                req->set_room_id(room_id);
                req->set_user_id(user_id);
                req->set_message(message);
                req->set_timestamp(timestamp);

                auto res =
                    std::make_shared<
                        chatdb::ChatPublishResponse>();

                using Handler =
                    std::decay_t<decltype(handler)>;

                auto handler_ptr =
                    std::make_shared<Handler>(
                        std::move(handler)
                    );

                stub_->async()->PublishChat(
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
                        bool success =
                            status.ok() &&
                            res->success();

                        boost::asio::post(
                            executor,

                            [handler_ptr,
                             success]() mutable
                            {
                                (*handler_ptr)(
                                    success
                                );
                            }
                        );
                    }
                );
            },

            use_awaitable
        );
    }

awaitable<ChatRepository::ChatHistoryResult>
    ChatRepository::GetChatHistoryAsync(
        uint32_t room_id,
        uint64_t last_msg_id,
        uint32_t limit)
{
        auto executor =
            co_await boost::asio::this_coro::executor;

        co_return co_await boost::asio::async_initiate<
            decltype(use_awaitable),
            void(ChatHistoryResult)>(

            [this,
             room_id,
             last_msg_id,
             limit,
             executor](auto handler) mutable
            {
                auto context =
                    std::make_shared<
                        grpc::ClientContext>();

                auto req =
                    std::make_shared<
                        chatdb::ChatHistoryRequest>();

                req->set_room_id(room_id);
                req->set_last_message_id(
                    last_msg_id
                );
                req->set_limit(limit);

                auto res =
                    std::make_shared<
                        chatdb::ChatHistoryResponse>();

                using Handler =
                    std::decay_t<decltype(handler)>;

                auto handler_ptr =
                    std::make_shared<Handler>(
                        std::move(handler)
                    );

                stub_->async()->GetChatHistory(
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
                        ChatHistoryResult result;

                        if (
                            status.ok() &&
                            res->success()
                        )
                        {
                            result.success = true;

                            result.has_more =
                                res->messages_size() >=
                                static_cast<int>(
                                    req->limit()
                                );

                            result.messages.reserve(
                                res->messages_size()
                            );

                            for (
                                const auto& msg :
                                res->messages()
                            )
                            {
                                result.messages.push_back(
                                    msg
                                );
                            }
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
