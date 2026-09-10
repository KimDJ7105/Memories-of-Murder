#pragma once

#include <memory>

#include <boost/asio.hpp>
#include <boost/beast/core.hpp>

namespace mom {

class GameServer;

namespace net = boost::asio;
using tcp = net::ip::tcp;

// Accepts raw TCP connections and hands each one to a new Session. Runs on
// the same single-threaded io_context as every Session, so no
// synchronization is needed between accept and per-connection I/O.
class Listener : public std::enable_shared_from_this<Listener> {
public:
    Listener(net::io_context& ioc, tcp::endpoint endpoint, GameServer& server);
    void run();

private:
    void do_accept();
    void on_accept(boost::beast::error_code ec, tcp::socket socket);

    net::io_context& ioc_;
    tcp::acceptor acceptor_;
    GameServer& server_;
};

}
