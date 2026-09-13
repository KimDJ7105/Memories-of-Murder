#include "RoomManager.hpp"

#include "MessageSender.hpp"

namespace mom {

namespace {

// Reaches only the sessions of one room. Holds a reference into that
// room's own Room::sessions map rather than owning a copy, so it stays in
// sync as players join/leave without any extra bookkeeping.
class RoomSender : public MessageSender {
public:
    explicit RoomSender(std::map<int, std::shared_ptr<Session>>& sessions) : sessions_(sessions) {}

    void send(int player_id, const nlohmann::json& msg) override
    {
        auto it = sessions_.find(player_id);
        if (it != sessions_.end()) it->second->send(msg);
    }

    void broadcast(const nlohmann::json& msg) override
    {
        for (auto& [id, session] : sessions_) session->send(msg);
    }

private:
    std::map<int, std::shared_ptr<Session>>& sessions_;
};

}

// Declaration order matters here: members are destroyed in reverse, and
// game_room holds a reference to *sender (which holds a reference to
// sessions), so game_room must be destroyed first, then sender, then
// sessions. That's the reverse of this declaration order.
struct RoomManager::Room {
    std::map<int, std::shared_ptr<Session>> sessions;
    std::unique_ptr<RoomSender> sender;
    std::unique_ptr<GameRoom> game_room;
};

RoomManager::RoomManager(boost::asio::io_context& ioc,
                          const GameData& data,
                          EvaluatorFactory make_evaluator,
                          JudgeFactory make_judge)
    : ioc_(ioc)
    , data_(data)
    , make_evaluator_(std::move(make_evaluator))
    , make_judge_(std::move(make_judge))
    , rng_(std::random_device{}())
{
}

RoomManager::~RoomManager() = default;

std::string RoomManager::generate_unique_code()
{
    std::uniform_int_distribution<int> dist(0, 9999);
    std::string code;
    do {
        code = std::to_string(dist(rng_));
        code.insert(code.begin(), 4 - code.size(), '0');
    } while (rooms_.count(code));
    return code;
}

std::pair<std::string, int> RoomManager::create_room(const std::shared_ptr<Session>& session, const std::string& name)
{
    auto room = std::make_unique<Room>();
    room->sender = std::make_unique<RoomSender>(room->sessions);
    room->game_room = std::make_unique<GameRoom>(ioc_, data_, *room->sender, make_evaluator_(ioc_), make_judge_(ioc_));

    const int player_id = room->game_room->handle_join(name);
    room->sessions[player_id] = session;

    const std::string code = generate_unique_code();
    rooms_[code] = std::move(room);
    return {code, player_id};
}

int RoomManager::join_room(const std::string& room_code, const std::shared_ptr<Session>& session, const std::string& name)
{
    auto it = rooms_.find(room_code);
    if (it == rooms_.end()) return -1;

    const int player_id = it->second->game_room->handle_join(name);
    if (player_id == -1) return -1;

    it->second->sessions[player_id] = session;
    return player_id;
}

void RoomManager::handle_message(const std::string& room_code, int player_id, const nlohmann::json& msg)
{
    auto it = rooms_.find(room_code);
    if (it == rooms_.end()) return;
    it->second->game_room->handle_message(player_id, msg);
}

void RoomManager::handle_disconnect(const std::string& room_code, int player_id)
{
    auto it = rooms_.find(room_code);
    if (it == rooms_.end()) return;

    it->second->sessions.erase(player_id);
    it->second->game_room->handle_disconnect(player_id);

    if (it->second->game_room->is_empty()) {
        rooms_.erase(it);
    }
}

void RoomManager::send_room_snapshot(const std::string& room_code, int player_id)
{
    auto it = rooms_.find(room_code);
    if (it == rooms_.end()) return;
    it->second->game_room->send_room_snapshot(player_id);
}

}
