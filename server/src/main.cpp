#include <iostream>

#include "GameServer.hpp"
#include "Listener.hpp"

int main()
{
    const unsigned short port = 9002;

    try {
        boost::asio::io_context ioc{1};
        mom::GameServer server(ioc);

        auto listener = std::make_shared<mom::Listener>(
            ioc, mom::tcp::endpoint(mom::tcp::v4(), port), server);
        listener->run();

        std::cout << "Memories of Murder - Game Server" << std::endl;
        std::cout << "Listening on ws://localhost:" << port << std::endl;

        ioc.run();
    } catch (const std::exception& e) {
        std::cerr << "[game_server] fatal: " << e.what() << std::endl;
        return 1;
    }
}
