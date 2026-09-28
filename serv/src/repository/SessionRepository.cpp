#include "repository/SessionRepository.hpp"


// Implementations moved out of the header during refactor v2.
    SessionRepository::SessionRepository(
        std::shared_ptr<grpc::Channel> channel)
        :
        stub_(
            chatdb::ChatDBService::NewStub(channel)
        )
{
    }

awaitable<bool> SessionRepository::SetUserSessionStateAsync(
        uint32_t user_id,
        const std::string& state,
        int ttl_seconds)
{
        auto executor =
            co_await boost::asio::this_coro::executor;

        co_return co_await boost::asio::async_initiate<
            decltype(use_awaitable),
            void(bool)>(

            [this,
             user_id,
             state,
             ttl_seconds,
             executor](auto handler) mutable
            {
                auto context =
                    std::make_shared<
                        grpc::ClientContext>();

                auto req =
                    std::make_shared<
                        chatdb::SessionStateRequest>();

                req->set_user_id(user_id);
                req->set_state(state);
                req->set_ttl_seconds(ttl_seconds);

                auto res =
                    std::make_shared<
                        chatdb::SessionStateResponse>();

                using Handler =
                    std::decay_t<decltype(handler)>;

                auto handler_ptr =
                    std::make_shared<Handler>(
                        std::move(handler)
                    );

                stub_->async()->SetSessionState(
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
