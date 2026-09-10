#pragma once

#include <map>
#include <memory>

#include <boost/asio/io_context.hpp>

#include "GameRoom.hpp"
#include "MessageSender.hpp"
#include "Session.hpp"

namespace mom {

// Ties the network layer to game logic. Phase 1 owns exactly one GameRoom;
// a RoomManager can later hand GameServer a room per connection/lobby code
// without changing how Session or GameRoom work (docs/DESIGN.md Phase 3).
class GameServer : public MessageSender {
public:
    explicit GameServer(boost::asio::io_context& ioc);

    void on_message(const std::shared_ptr<Session>& session, const std::string& text);
    void on_disconnect(const std::shared_ptr<Session>& session);

    void send(int player_id, const nlohmann::json& msg) override;
    void broadcast(const nlohmann::json& msg) override;

private:
    std::map<int, std::shared_ptr<Session>> sessions_;
    GameRoom room_;
};

}
