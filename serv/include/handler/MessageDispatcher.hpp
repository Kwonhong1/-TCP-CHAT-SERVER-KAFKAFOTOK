#pragma once

#include "common/Asio.hpp"
#include "protocol/Protocol.hpp"
#include "protocol/PacketSerializer.hpp"
#include "server/ChatSession.hpp"

#include <functional>
#include <memory>
#include <unordered_map>

enum class AuthPolicy {
    PUBLIC,
    AUTHENTICATED
};

class MessageDispatcher {
public:

    template <typename ProtoMessage, typename Handler>
    void RegisterHandler(MessageType type, AuthPolicy auth_policy, Handler&& handler)
    {
        HandlerEntry entry;
        entry.auth_policy = auth_policy;

        entry.handler =
            [handler = std::forward<Handler>(handler)](
                std::shared_ptr<ChatSession> session,
                const char* payload,
                size_t payload_size) -> awaitable<void>
            {
                ProtoMessage message;

                if (!message.ParseFromArray(payload, static_cast<int>(payload_size)))
                {
                    co_return;
                }

                co_await handler(session, message);
            };

        handlers_[type] = std::move(entry);
    }

    template <typename Handler>
    void RegisterRawHandler(MessageType type, AuthPolicy auth_policy, Handler&& handler)
    {
        HandlerEntry entry;
        entry.auth_policy = auth_policy;

        entry.handler =
            [handler = std::forward<Handler>(handler)](
                std::shared_ptr<ChatSession> session,
                const char*,
                size_t) -> awaitable<void>
            {
                co_await handler(session);
            };

        handlers_[type] = std::move(entry);
    }

    awaitable<void> DispatchMessageAsync(
        std::shared_ptr<ChatSession> session,
        const PacketHeader& header,
        const char* payload,
        size_t payload_size)
    {
        auto it = handlers_.find(header.message_type);

        if (it == handlers_.end())
        {
            std::cerr
                << "[Dispatcher] Unhandled MessageType: "
                << static_cast<uint16_t>(header.message_type)
                << '\n';

            co_return;
        }

        const auto& entry = it->second;

        if (entry.auth_policy == AuthPolicy::AUTHENTICATED && !session->IsAuthenticated())
        {
            std::cerr
                << "[Security] Unauthenticated request: "
                << static_cast<uint16_t>(header.message_type)
                << '\n';

            co_return;
        }

        co_await entry.handler(session, payload, payload_size);
    }

private:

    using InternalAsyncHandler =
        std::function<
            awaitable<void>(
                std::shared_ptr<ChatSession>,
                const char*,
                size_t
            )
        >;

    struct HandlerEntry {
        AuthPolicy auth_policy = AuthPolicy::AUTHENTICATED;
        InternalAsyncHandler handler;
    };

    std::unordered_map<MessageType, HandlerEntry> handlers_;
};