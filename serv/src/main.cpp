#include "server/ChatServer.hpp"
#include "common/Asio.hpp"
#include <iostream>
#include <memory>
#include <thread>
#include <vector>

int main()
{
    try
    {
        boost::asio::io_context io_context;

        auto work_guard =
            boost::asio::make_work_guard(
                io_context
            );

        ssl::context ssl_ctx(
            ssl::context::tlsv12_server
        );

        ssl_ctx.set_options(
            ssl::context::default_workarounds |
            ssl::context::no_sslv2 |
            ssl::context::single_dh_use
        );

        ssl_ctx.use_certificate_chain_file(
            "server.crt"
        );

        ssl_ctx.use_private_key_file(
            "server.key",
            ssl::context::pem
        );

        auto server =
            std::make_shared<ChatServer>(
                io_context,
                ssl_ctx,
                8080,
                "127.0.0.1:50051"
            );

        server->StartAccept();

        std::cout
            << "[C++ SSL Chat Server] "
            << "Listening on port 8080 "
            << "(TLS 1.2 Encrypted)...\n";

        unsigned int threads_count =
            std::thread::hardware_concurrency();

        if (threads_count == 0)
            threads_count = 4;

        std::vector<std::thread>
            thread_pool;

        thread_pool.reserve(
            threads_count
        );

        for (
            unsigned int i = 0;
            i < threads_count;
            ++i
        )
        {
            thread_pool.emplace_back(
                [&io_context]()
                {
                    io_context.run();
                }
            );
        }

        std::cout
            << "[Unified Thread Pool] Total Workers: "
            << threads_count
            << "\n";

        for (
            auto& t :
            thread_pool
        )
        {
            if (t.joinable())
                t.join();
        }
    }
    catch (
        const std::exception& e
    )
    {
        std::cerr
            << "Exception: "
            << e.what()
            << std::endl;
    }

    return 0;
}
