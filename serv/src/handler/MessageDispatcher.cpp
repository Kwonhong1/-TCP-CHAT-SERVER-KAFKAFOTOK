#include "handler/MessageDispatcher.hpp"

awaitable<void>
    MessageDispatcher::DispatchMessageAsync(
        std::shared_ptr<ChatSession> session,
        const PacketHeader& header,
        const char* payload,
        size_t payload_size)
{
        auto it =
            handlers_.find(
                header.message_type
            );

        if (
            it != handlers_.end()
        )
        {
            co_await it->second(
                session,
                payload,
                payload_size
            );
        }
        else
        {
            std::cerr
                << "[Dispatcher Error] Unhandled MessageType: "
                << static_cast<uint16_t>(
                    header.message_type
                )
                << std::endl;
        }
    }
