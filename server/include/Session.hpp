#pragma once

#include <deque>
#include <memory>
#include <optional>
#include <string>

#include <boost/asio.hpp>
#include <boost/beast/core.hpp>
#include <boost/beast/websocket.hpp>
#include <nlohmann/json.hpp>

namespace mom {

class GameServer;

namespace beast = boost::beast;
namespace websocket = beast::websocket;
namespace net = boost::asio;
using tcp = net::ip::tcp;

// One WebSocket connection. All I/O is async so many sessions can be live
// at once on a single io_context thread; every callback that touches game
// state routes through GameServer, so state mutation still happens
// serially on that one thread (see docs/DESIGN.md section 4).
class Session : public std::enable_shared_from_this<Session> {
public:
    Session(tcp::socket socket, GameServer& server);

    void run();
    void send(const nlohmann::json& msg);
    void close();

    std::optional<int> player_id() const { return player_id_; }
    void set_player_id(int id) { player_id_ = id; }

private:
    void on_accept(beast::error_code ec);
    void do_read();
    void on_read(beast::error_code ec, std::size_t bytes_transferred);
    void do_write();
    void on_write(beast::error_code ec, std::size_t bytes_transferred);

    websocket::stream<tcp::socket> ws_;
    beast::flat_buffer buffer_;
    GameServer& server_;
    std::optional<int> player_id_;
    std::deque<std::string> write_queue_;
    bool writing_ = false;
};

}
