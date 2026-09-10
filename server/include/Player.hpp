#pragma once

#include <string>

namespace mom {

struct Player {
    int id = 0;
    std::string name;
    bool connected = true;
    int score = 0;
    bool has_been_criminal = false;
};

}
