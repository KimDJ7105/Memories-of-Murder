#include "Listener.hpp"
#include "Session.hpp"

namespace mom {

Listener::Listener(net::io_context& ioc, tcp::endpoint endpoint, GameServer& server)
    : ioc_(ioc)
    , acceptor_(ioc, endpoint)
    , server_(server)
{
}

void Listener::run()
{
    do_accept();
}

void Listener::do_accept()
{
    acceptor_.async_accept([self = shared_from_this()](boost::beast::error_code ec, tcp::socket socket) {
        self->on_accept(ec, std::move(socket));
    });
}

void Listener::on_accept(boost::beast::error_code ec, tcp::socket socket)
{
    if (!ec) {
        std::make_shared<Session>(std::move(socket), server_)->run();
    }
    do_accept();
}

}
