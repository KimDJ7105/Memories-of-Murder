#include "GameData.hpp"

#include <fstream>
#include <stdexcept>

#include <nlohmann/json.hpp>

#ifndef MOM_DATA_DIR
#define MOM_DATA_DIR "data"
#endif

namespace mom {

namespace {

nlohmann::json load_json_file(const std::string& path)
{
    std::ifstream in(path);
    if (!in) throw std::runtime_error("게임 데이터 파일을 열 수 없습니다: " + path);
    nlohmann::json j;
    in >> j;
    return j;
}

MapDef parse_map(const nlohmann::json& j)
{
    MapDef map;
    map.id = j.value("id", "");
    map.name = j.value("name", "");
    if (j.contains("image") && !j.at("image").is_null()) {
        map.image = j.at("image").get<std::string>();
    }

    for (const auto& r : j.at("rooms")) {
        RoomDef room;
        room.id = r.at("id").get<std::string>();
        room.name = r.at("name").get<std::string>();
        map.rooms.push_back(std::move(room));
    }
    for (const auto& e : j.at("edges")) {
        map.edges.emplace_back(e.at(0).get<std::string>(), e.at(1).get<std::string>());
    }
    return map;
}

std::vector<WeaponDef> parse_weapons(const nlohmann::json& j)
{
    std::vector<WeaponDef> weapons;
    for (const auto& w : j) {
        weapons.push_back({w.at("id").get<std::string>(), w.at("name").get<std::string>()});
    }
    return weapons;
}

}

const RoomDef* GameData::find_room(const std::string& id) const
{
    for (const auto& r : map.rooms) {
        if (r.id == id) return &r;
    }
    return nullptr;
}

bool GameData::is_valid_weapon(const std::string& name) const
{
    for (const auto& w : weapons) {
        if (w.name == name) return true;
    }
    return false;
}

const RoomDef* GameData::extract_room_mention(const std::string& text) const
{
    const RoomDef* best = nullptr;
    std::string::size_type best_pos = std::string::npos;
    for (const auto& r : map.rooms) {
        const auto pos = text.find(r.name);
        if (pos != std::string::npos && pos < best_pos) {
            best_pos = pos;
            best = &r;
        }
    }
    return best;
}

GameData GameData::load_default()
{
    const std::string base = MOM_DATA_DIR;
    GameData data;
    data.map = parse_map(load_json_file(base + "/maps/mansion.json"));
    data.weapons = parse_weapons(load_json_file(base + "/weapons.json"));
    return data;
}

}
