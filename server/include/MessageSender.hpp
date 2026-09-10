#pragma once

#include <nlohmann/json.hpp>

namespace mom {

// Lets GameRoom push messages to clients without knowing anything about
// WebSocket sessions. GameServer implements this; GameRoom only ever sees
// the interface, keeping game logic and the network layer separable (see
// docs/DESIGN.md section 16, principle 4).
class MessageSender {
public:
    virtual ~MessageSender() = default;
    virtual void send(int player_id, const nlohmann::json& msg) = 0;
    virtual void broadcast(const nlohmann::json& msg) = 0;
};

}
