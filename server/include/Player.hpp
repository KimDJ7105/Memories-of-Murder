#pragma once

#include <string>

namespace mom {

struct Player {
    int id = 0;
    std::string name;
    bool connected = true;
    int score = 0;
    bool has_been_criminal = false;
    // Secret, issued once at join time (see docs/RECONNECT_DESIGN.md) and
    // handed to this player alone in their room_created/joined response.
    // Proves a later `rejoin` claiming this id is actually the same
    // player, not just someone guessing a small player_id. Never include
    // this in any broadcast (build_room_update() etc.) — only ever send
    // it to the player it belongs to.
    std::string token;
};

}
