#pragma once

#include "common/Asio.hpp"
#include "protocol/Protocol.hpp"
#include "protocol/PacketSerializer.hpp"
#include <functional>
#include <iostream>
#include <memory>
#include <unordered_map>

class ChatSession;

class MessageDispatcher
{
public:

    template <
        typename T,
        typename HandlerFunc
    >
    void RegisterHandler(
        MessageType type,
        HandlerFunc handler)
    {
        handlers_[type] =
            [handler, type](
                std::shared_ptr<ChatSession> session,
                const char* payload,
                size_t payload_size)
            -> awaitable<void>
            {
                T proto_msg;

                if (
                    !PacketSerializer::ParseProtoStream(
                        payload,
                        payload_size,
                        proto_msg
                    )
                )
                {
                    std::cerr
                        << "[Dispatcher Error] Failed to parse proto stream for MessageType: "
                        << static_cast<uint16_t>(type)
                        << std::endl;

                    co_return;
                }

                co_await handler(
                    session,
                    proto_msg
                );
            };
    }

    template <typename HandlerFunc>
    void RegisterRawHandler(
        MessageType type,
        HandlerFunc handler)
    {
        handlers_[type] =
            [handler](
                std::shared_ptr<ChatSession> session,
                const char*,
                size_t)
            -> awaitable<void>
            {
                co_await handler(
                    session
                );
            };
    }

    awaitable<void>
    DispatchMessageAsync(
        std::shared_ptr<ChatSession> session,
        const PacketHeader& header,
        const char* payload,
        size_t payload_size);

private:

    using InternalAsyncHandler =
        std::function<
            awaitable<void>(
                std::shared_ptr<ChatSession>,
                const char*,
                size_t
            )
        >;

    std::unordered_map<
        MessageType,
        InternalAsyncHandler
    > handlers_;
};
