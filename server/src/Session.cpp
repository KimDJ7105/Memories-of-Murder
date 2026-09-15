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
    // error_handler_t::replace, not the (throwing) default: a string that
    // reaches here isn't guaranteed to be valid UTF-8 in every case — most
    // notably, boost::system::error_code::message() on Windows returns OS
    // error text in the system's ANSI codepage (e.g. Korean text in CP949
    // on a Korean-locale machine), not UTF-8, and that text can flow into
    // a fallback CrimeEvaluation's `evaluation` field when Ollama is
    // unreachable. dump()'s default UTF-8 validation throws on that, and
    // an uncaught throw here unwinds out of the io_context and kills the
    // whole server process — reproduced live, not hypothetical. Replacing
    // invalid sequences with U+FFFD keeps every other room's connection
    // alive instead.
    write_queue_.push_back(msg.dump(-1, ' ', false, nlohmann::json::error_handler_t::replace));
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
