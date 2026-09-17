#pragma once

#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace mom {

struct RoomDef {
    std::string id;
    std::string name;
    // Always-on, fixed-location watch (CCTV, a stationed guard, etc. —
    // see MapDef::surveillance_label for how this map names it). No time
    // window or patrol path: those were considered and rejected, since a
    // criminal's free-text narration can't be checked against either
    // (docs/DESIGN.md section 15-3's brainstorm ran into this — "the
    // guard happened to be elsewhere" is exactly as unverifiable as a
    // claimed time of night).
    bool surveilled = false;
};

struct WeaponDef {
    std::string id;
    std::string name;
};

struct MapDef {
    std::string id;
    std::string name;
    // A ready-to-use `data:` URI (e.g. "data:image/svg+xml;base64,...."),
    // already read from the file named in the map's JSON and encoded once
    // at load time — nullopt if the map has no image yet. Clients can
    // drop this straight into an <img src="...">.
    std::optional<std::string> image;
    // What to call this map's surveilled room(s) in the UI and in the AI
    // prompt ("CCTV", "경비병 상주 구역", ...) — nullopt means this map has
    // no surveillance system at all, in which case no RoomDef on it should
    // have `surveilled` set.
    std::optional<std::string> surveillance_label;
    std::vector<RoomDef> rooms;
    std::vector<std::pair<std::string, std::string>> edges;
    // The subset of GameData::weapons available on this map, so a school
    // doesn't offer a gun and a modern cruise ship doesn't offer a
    // candlestick — thematic fit, decided per map's own JSON ("weapons":
    // [id, ...]). A map that omits the field gets every globally-defined
    // weapon (see GameData::load_default), so existing/new maps don't have
    // to opt in explicitly.
    std::vector<WeaponDef> weapons;
};

// All game content that isn't per-round state: every available map (rooms
// + adjacency + optional image + its own weapon subset) and the full
// weapon pool each map's subset is drawn from. Loaded once from
// server/data/*.json instead of being hardcoded, so adding a new map or
// weapon set later is a new file, not a recompile (see docs/DESIGN.md
// section 15-3, "범행 유형/테마").
struct GameData {
    std::vector<MapDef> maps;
    // Canonical id->name definitions for every weapon that exists in the
    // game. Individual maps only ever expose a subset of these (see
    // MapDef::weapons) — this list exists so map JSON files can reference
    // weapons by a short id instead of repeating {id, name} everywhere.
    std::vector<WeaponDef> weapons;

    const MapDef* find_map(const std::string& id) const;
    // The map a new room starts with before its host picks one.
    const MapDef& default_map() const;

    const RoomDef* find_room(const MapDef& map, const std::string& id) const;

    // Placeholder for real language understanding: returns the room/weapon
    // whose name appears earliest in the free-text crime, or nullptr if
    // none of the map's rooms/the weapon list are mentioned at all. Both
    // location and weapon are extracted this way — the criminal only ever
    // writes free text, with no separate location or weapon UI selection
    // (docs/DESIGN.md section 6: crime writing is free-text only). Phase
    // 2's AI evaluator should eventually take over this extraction
    // (docs/DESIGN.md section 8).
    const RoomDef* extract_room_mention(const MapDef& map, const std::string& text) const;
    const WeaponDef* extract_weapon_mention(const MapDef& map, const std::string& text) const;

    static GameData load_default();
};

}
