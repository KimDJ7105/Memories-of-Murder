#include <boost/asio.hpp>
#include <boost/beast/core.hpp>
#include <boost/beast/websocket.hpp>
#include <iostream>
#include <string>

namespace beast = boost::beast;
namespace websocket = beast::websocket;
namespace net = boost::asio;
using tcp = net::ip::tcp;

// Environment-verification server: accepts one WebSocket connection at a
// time and echoes every message back. This is not the game server itself —
// it exists to prove the MSVC + Ninja + vcpkg + Boost.Beast toolchain works
// end to end before any game logic is written.
void handle_session(tcp::socket socket)
{
    try
    {
        websocket::stream<tcp::socket> ws(std::move(socket));
        ws.accept();

        std::cout << "[game_server] client connected" << std::endl;

        for (;;)
        {
            beast::flat_buffer buffer;
            ws.read(buffer);
            ws.text(ws.got_text());
            ws.write(buffer.data());
        }
    }
    catch (beast::system_error const& se)
    {
        if (se.code() != websocket::error::closed)
            std::cerr << "[game_server] session error: " << se.code().message() << std::endl;
        std::cout << "[game_server] client disconnected" << std::endl;
    }
    catch (std::exception const& e)
    {
        std::cerr << "[game_server] session error: " << e.what() << std::endl;
    }
}

int main()
{
    const unsigned short port = 9002;

    try
    {
        net::io_context ioc{1};
        tcp::acceptor acceptor{ioc, tcp::endpoint(tcp::v4(), port)};

        std::cout << "Memories of Murder - Game Server" << std::endl;
        std::cout << "Listening on ws://localhost:" << port << std::endl;

        for (;;)
        {
            tcp::socket socket{ioc};
            acceptor.accept(socket);
            handle_session(std::move(socket));
        }
    }
    catch (std::exception const& e)
    {
        std::cerr << "[game_server] fatal: " << e.what() << std::endl;
        return 1;
    }
}
