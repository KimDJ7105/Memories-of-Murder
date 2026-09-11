#pragma once

#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace mom {

struct RoomDef {
    std::string id;
    std::string name;
};

struct WeaponDef {
    std::string id;
    std::string name;
};

struct MapDef {
    std::string id;
    std::string name;
    std::optional<std::string> image;  // background image path; null for now
    std::vector<RoomDef> rooms;
    std::vector<std::pair<std::string, std::string>> edges;
};

// All game content that isn't per-round state: the map (rooms + adjacency
// + optional background image) and the weapon pool. Loaded once from
// server/data/*.json instead of being hardcoded, so adding a new map or
// weapon set later is a new file, not a recompile (see docs/DESIGN.md
// section 15-3, "범행 유형/테마").
struct GameData {
    MapDef map;
    std::vector<WeaponDef> weapons;

    const RoomDef* find_room(const std::string& id) const;

    // Placeholder for real language understanding: returns the room/weapon
    // whose name appears earliest in the free-text crime, or nullptr if
    // none of the map's rooms/the weapon list are mentioned at all. Both
    // location and weapon are extracted this way — the criminal only ever
    // writes free text, with no separate location or weapon UI selection
    // (docs/DESIGN.md section 6: crime writing is free-text only). Phase
    // 2's AI evaluator should eventually take over this extraction
    // (docs/DESIGN.md section 8).
    const RoomDef* extract_room_mention(const std::string& text) const;
    const WeaponDef* extract_weapon_mention(const std::string& text) const;

    static GameData load_default();
};

}
