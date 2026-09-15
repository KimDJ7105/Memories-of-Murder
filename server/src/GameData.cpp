#include "GameData.hpp"

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <stdexcept>

#include <nlohmann/json.hpp>

#ifndef MOM_DATA_DIR
#define MOM_DATA_DIR "data"
#endif

namespace mom {

namespace {

nlohmann::json load_json_file(const std::filesystem::path& path)
{
    std::ifstream in(path);
    if (!in) throw std::runtime_error("게임 데이터 파일을 열 수 없습니다: " + path.string());
    nlohmann::json j;
    in >> j;
    return j;
}

std::string base64_encode(const std::string& bytes)
{
    static const char kAlphabet[] =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string out;
    out.reserve(((bytes.size() + 2) / 3) * 4);

    size_t i = 0;
    while (i + 3 <= bytes.size()) {
        const unsigned n = (static_cast<unsigned char>(bytes[i]) << 16) |
                            (static_cast<unsigned char>(bytes[i + 1]) << 8) |
                            static_cast<unsigned char>(bytes[i + 2]);
        out += kAlphabet[(n >> 18) & 0x3F];
        out += kAlphabet[(n >> 12) & 0x3F];
        out += kAlphabet[(n >> 6) & 0x3F];
        out += kAlphabet[n & 0x3F];
        i += 3;
    }
    const size_t remaining = bytes.size() - i;
    if (remaining == 1) {
        const unsigned n = static_cast<unsigned char>(bytes[i]) << 16;
        out += kAlphabet[(n >> 18) & 0x3F];
        out += kAlphabet[(n >> 12) & 0x3F];
        out += "==";
    } else if (remaining == 2) {
        const unsigned n = (static_cast<unsigned char>(bytes[i]) << 16) |
                            (static_cast<unsigned char>(bytes[i + 1]) << 8);
        out += kAlphabet[(n >> 18) & 0x3F];
        out += kAlphabet[(n >> 12) & 0x3F];
        out += kAlphabet[(n >> 6) & 0x3F];
        out += "=";
    }
    return out;
}

std::string mime_type_for(const std::filesystem::path& path)
{
    std::string ext = path.extension().string();
    std::transform(ext.begin(), ext.end(), ext.begin(), [](unsigned char c) { return std::tolower(c); });
    if (ext == ".svg") return "image/svg+xml";
    if (ext == ".png") return "image/png";
    if (ext == ".jpg" || ext == ".jpeg") return "image/jpeg";
    if (ext == ".webp") return "image/webp";
    return "application/octet-stream";
}

// Reads an image file once at startup and turns it into a self-contained
// `data:` URI, so the server never needs to serve static files over HTTP
// just to hand clients one small picture per map — it rides along inside
// the same board_info WebSocket message as everything else.
std::string load_image_as_data_uri(const std::filesystem::path& path)
{
    std::ifstream in(path, std::ios::binary);
    if (!in) throw std::runtime_error("맵 이미지 파일을 열 수 없습니다: " + path.string());
    std::ostringstream buf;
    buf << in.rdbuf();
    return "data:" + mime_type_for(path) + ";base64," + base64_encode(buf.str());
}

MapDef parse_map(const nlohmann::json& j, const std::filesystem::path& maps_dir)
{
    MapDef map;
    map.id = j.value("id", "");
    map.name = j.value("name", "");
    if (j.contains("image") && !j.at("image").is_null()) {
        const std::string image_file = j.at("image").get<std::string>();
        map.image = load_image_as_data_uri(maps_dir / image_file);
    }
    if (j.contains("surveillance_label") && !j.at("surveillance_label").is_null()) {
        map.surveillance_label = j.at("surveillance_label").get<std::string>();
    }

    for (const auto& r : j.at("rooms")) {
        RoomDef room;
        room.id = r.at("id").get<std::string>();
        room.name = r.at("name").get<std::string>();
        room.surveilled = r.value("surveilled", false);
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

const MapDef* GameData::find_map(const std::string& id) const
{
    for (const auto& m : maps) {
        if (m.id == id) return &m;
    }
    return nullptr;
}

const MapDef& GameData::default_map() const
{
    if (const MapDef* mansion = find_map("mansion")) return *mansion;
    return maps.front();
}

const RoomDef* GameData::find_room(const MapDef& map, const std::string& id) const
{
    for (const auto& r : map.rooms) {
        if (r.id == id) return &r;
    }
    return nullptr;
}

const RoomDef* GameData::extract_room_mention(const MapDef& map, const std::string& text) const
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

const WeaponDef* GameData::extract_weapon_mention(const std::string& text) const
{
    const WeaponDef* best = nullptr;
    std::string::size_type best_pos = std::string::npos;
    for (const auto& w : weapons) {
        const auto pos = text.find(w.name);
        if (pos != std::string::npos && pos < best_pos) {
            best_pos = pos;
            best = &w;
        }
    }
    return best;
}

GameData GameData::load_default()
{
    const std::filesystem::path base = MOM_DATA_DIR;
    const std::filesystem::path maps_dir = base / "maps";

    GameData data;
    for (const auto& entry : std::filesystem::directory_iterator(maps_dir)) {
        if (entry.path().extension() != ".json") continue;
        data.maps.push_back(parse_map(load_json_file(entry.path()), maps_dir));
    }
    if (data.maps.empty()) {
        throw std::runtime_error("사용 가능한 지도가 없습니다: " + maps_dir.string());
    }

    data.weapons = parse_weapons(load_json_file(base / "weapons.json"));
    return data;
}

}
