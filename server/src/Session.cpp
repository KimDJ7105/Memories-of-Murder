#include "Session.hpp"
#include "GameServer.hpp"

#include <iostream>

namespace mom {

Session::Session(tcp::socket socket, GameServer& server)
    : ws_(std::move(socket))
    , server_(server)
{
}

void Session::run()
{
    ws_.async_accept([self = shared_from_this()](beast::error_code ec) {
        self->on_accept(ec);
    });
}

void Session::on_accept(beast::error_code ec)
{
    if (ec) return;
    do_read();
}

void Session::do_read()
{
    buffer_.consume(buffer_.size());
    ws_.async_read(buffer_, [self = shared_from_this()](beast::error_code ec, std::size_t bytes) {
        self->on_read(ec, bytes);
    });
}

void Session::on_read(beast::error_code ec, std::size_t)
{
    if (ec) {
        server_.on_disconnect(shared_from_this());
        return;
    }

    const std::string text = beast::buffers_to_string(buffer_.data());
    server_.on_message(shared_from_this(), text);
    do_read();
}

void Session::send(const nlohmann::json& msg)
{
    write_queue_.push_back(msg.dump());
    if (!writing_) do_write();
}

void Session::do_write()
{
    if (write_queue_.empty()) {
        writing_ = false;
        return;
    }
    writing_ = true;
    ws_.text(true);
    ws_.async_write(net::buffer(write_queue_.front()),
                     [self = shared_from_this()](beast::error_code ec, std::size_t bytes) {
                         self->on_write(ec, bytes);
                     });
}

void Session::on_write(beast::error_code ec, std::size_t)
{
    if (ec) {
        writing_ = false;
        return;
    }
    write_queue_.pop_front();
    do_write();
}

void Session::close()
{
    ws_.async_close(websocket::close_code::normal, [self = shared_from_this()](beast::error_code) {});
}

}
