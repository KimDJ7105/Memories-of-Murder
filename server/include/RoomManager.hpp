#pragma once

#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <random>
#include <string>

#include <boost/asio/io_context.hpp>
#include <nlohmann/json.hpp>

#include "CrimeEvaluator.hpp"
#include "GameData.hpp"
#include "GameRoom.hpp"
#include "GuessJudge.hpp"
#include "Session.hpp"

namespace mom {

using EvaluatorFactory = std::function<std::unique_ptr<ICrimeEvaluator>(boost::asio::io_context&)>;
using JudgeFactory = std::function<std::unique_ptr<IGuessJudge>(boost::asio::io_context&)>;

// Owns every active GameRoom, keyed by a short room code players share with
// each other to play together (docs/DESIGN.md Phase 3's "방 생성 및 입장").
// GameServer stays a thin router: it only needs to know which room code and
// player id a session belongs to, and hands everything else off here.
//
// Each room gets its own MessageSender (RoomSender, defined in the .cpp)
// scoped to just that room's sessions, so a broadcast from one room's
// GameRoom can never reach a different room's players — GameRoom itself
// still has no idea other rooms exist.
// Returned by create_room/join_room on success — the token rides along so
// GameServer can hand it to that player alone (see
// docs/RECONNECT_DESIGN.md). Never broadcast anywhere; only ever sent to
// the one session it was issued to.
struct JoinResult {
    std::string room_code;
    int player_id;
    std::string token;
};

class RoomManager {
public:
    RoomManager(boost::asio::io_context& ioc,
                const GameData& data,
                EvaluatorFactory make_evaluator,
                JudgeFactory make_judge);
    ~RoomManager();

    // Creates a fresh room and joins `session` as its first player (always
    // succeeds — a brand-new room is always empty and in Lobby).
    JoinResult create_room(const std::shared_ptr<Session>& session, const std::string& name);

    // nullopt if the code doesn't exist or the room refused the join (in
    // progress, or full).
    std::optional<JoinResult> join_room(const std::string& room_code, const std::shared_ptr<Session>& session, const std::string& name);

    // Re-links `session` to an existing player_id after verifying `token`
    // matches what that player was issued at join time. On success,
    // evicts any still-live older session for that player_id (e.g. a
    // second tab) before taking over. See docs/RECONNECT_DESIGN.md.
    bool rejoin_room(const std::string& room_code, int player_id, const std::string& token,
                      const std::shared_ptr<Session>& session);

    void handle_message(const std::string& room_code, int player_id, const nlohmann::json& msg);

    // Also tears the room down once it has no connected players left.
    void handle_disconnect(const std::string& room_code, int player_id);

    void send_room_snapshot(const std::string& room_code, int player_id);
    void send_game_state_sync(const std::string& room_code, int player_id);

private:
    struct Room;

    std::string generate_unique_code();

    boost::asio::io_context& ioc_;
    const GameData& data_;
    EvaluatorFactory make_evaluator_;
    JudgeFactory make_judge_;
    std::mt19937 rng_;
    std::map<std::string, std::unique_ptr<Room>> rooms_;
};

}
