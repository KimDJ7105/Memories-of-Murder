#pragma once

#include <map>
#include <memory>
#include <random>
#include <set>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "Crime.hpp"
#include "CrimeEvaluator.hpp"
#include "GameTypes.hpp"
#include "GuessJudge.hpp"
#include "MessageSender.hpp"
#include "Player.hpp"

namespace mom {

// Owns one game's worth of state and enforces the state machine from
// docs/DESIGN.md section 5: only messages valid for the current state are
// applied, everything else comes back as an "error" reply. Phase 1 runs a
// single GameRoom directly inside GameServer; a RoomManager can wrap many
// of these later (Phase 3) without GameRoom itself changing.
class GameRoom {
public:
    GameRoom(MessageSender& sender,
              std::unique_ptr<ICrimeEvaluator> evaluator,
              std::unique_ptr<IGuessJudge> judge);

    // Returns the new player's id, or -1 if the room refused the join
    // (already in progress, or full).
    int handle_join(const std::string& name);

    void handle_disconnect(int player_id);

    // Dispatches by msg["type"]. Called only for players that already
    // joined (i.e. have a player id).
    void handle_message(int player_id, const nlohmann::json& msg);

private:
    void handle_start_game(int player_id);
    void handle_submit_crime(int player_id, const nlohmann::json& msg);
    void handle_submit_guess(int player_id, const nlohmann::json& msg);

    void start_round();
    void run_ai_judging();
    void start_investigation_attempt();
    void run_guess_judging();
    void finish_round();

    void broadcast_room_update();
    void send_error(int player_id, const std::string& message);
    Player* find_player(int player_id);

    MessageSender& sender_;
    std::unique_ptr<ICrimeEvaluator> evaluator_;
    std::unique_ptr<IGuessJudge> judge_;
    std::mt19937 rng_;

    GameState state_ = GameState::Lobby;
    std::vector<Player> players_;
    int next_player_id_ = 1;
    int host_id_ = -1;

    std::vector<int> criminal_queue_;
    int round_number_ = 0;
    int criminal_id_ = -1;
    std::string location_;
    std::string weapon_;
    std::string crime_text_;
    CrimeEvaluation crime_eval_;

    int attempt_ = 0;
    std::set<int> pending_detectives_;
    std::map<int, std::string> current_guesses_;
    std::map<int, int> solved_at_attempt_;
};

}
