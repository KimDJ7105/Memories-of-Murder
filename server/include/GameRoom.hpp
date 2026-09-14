#pragma once

#include <deque>
#include <map>
#include <memory>
#include <random>
#include <string>
#include <vector>

#include <boost/asio/io_context.hpp>
#include <boost/asio/steady_timer.hpp>
#include <nlohmann/json.hpp>

#include "Crime.hpp"
#include "CrimeEvaluator.hpp"
#include "GameData.hpp"
#include "GameTypes.hpp"
#include "GuessJudge.hpp"
#include "MessageSender.hpp"
#include "Player.hpp"

namespace mom {

// Owns one game's worth of state and enforces the state machine from
// docs/DESIGN.md section 5: only messages valid for the current state are
// applied, everything else comes back as an "error" reply. GameRoom knows
// nothing about there being other rooms — RoomManager (Phase 3) owns one
// GameRoom per active room code and routes messages to the right one, so
// nothing here changed to support that.
//
// Every available map (rooms + adjacency + optional image) and the shared
// weapon list are static board data loaded from server/data/*.json (see
// GameData) — never picked by the server itself. A room starts on
// GameData::default_map() and the host can switch it via `select_map`
// any time before start_game. Whichever map is selected, the criminal
// writes the crime entirely as free text, with no location or weapon UI
// selection at all; both are extracted from that text by matching it
// against the selected map's room names / the weapon list
// (GameData::extract_room_mention / extract_weapon_mention — placeholders
// for real AI extraction in a later phase). Both stay secret, like the
// confession text itself, until `round_result` reveals them. Room
// adjacency exists for future evaluators to judge movement plausibility
// (docs/DESIGN.md section 7's "이동 및 실행 가능성"); the mock evaluator
// doesn't use it yet.
//
// Investigation is turn-based: detectives take one guess at a time in a
// rotating queue (`pending_order_`), each guess gets immediate public
// feedback, and the turn advances either when the answering detective
// sends `next_turn` or after a 10s server-side timer fires — whichever
// comes first.
//
// After a round ends, GameRoom sits in GameState::NextRound (rather than
// immediately starting the next round) so players actually have time to
// read round_result's reveal — the host can send `next_round` to move on
// early, or a longer server-side timer does it automatically.
class GameRoom {
public:
    GameRoom(boost::asio::io_context& ioc,
              const GameData& data,
              MessageSender& sender,
              std::unique_ptr<ICrimeEvaluator> evaluator,
              std::unique_ptr<IGuessJudge> judge);

    // Returns the new player's id, or -1 if the room refused the join
    // (already in progress, or full).
    int handle_join(const std::string& name);

    void handle_disconnect(int player_id);

    // Dispatches by msg["type"]. Called only for players that already
    // joined (i.e. have a player id).
    void handle_message(int player_id, const nlohmann::json& msg);

    // Sends the current room_update and board_info to just this one
    // player. handle_join's own broadcast_room_update() fires before
    // GameServer has registered the new session, so the joining player
    // misses it — the caller must send them a snapshot right after
    // registering the session. board_info rides along so a player joining
    // an already-selected (non-default) map sees it immediately too.
    void send_room_snapshot(int player_id);

    // True if nobody who ever joined this room is still connected (or
    // nobody ever joined at all). RoomManager uses this to know when a
    // room can be torn down.
    bool is_empty() const;

private:
    void handle_start_game(int player_id);
    void handle_restart_game(int player_id);
    void handle_select_map(int player_id, const nlohmann::json& msg);
    void handle_submit_crime(int player_id, const nlohmann::json& msg);
    void handle_submit_guess(int player_id, const nlohmann::json& msg);
    void handle_next_turn(int player_id);
    void handle_next_round(int player_id);

    nlohmann::json build_board_info() const;
    void broadcast_board_info();
    void send_board_info(int player_id);
    void start_round();
    void run_ai_judging();
    void start_investigation();
    void begin_next_turn();
    void schedule_turn_advance();

    void finish_round();
    void schedule_next_round_advance();

    nlohmann::json build_room_update() const;
    void broadcast_room_update();
    void send_error(int player_id, const std::string& message);
    Player* find_player(int player_id);

    boost::asio::io_context& ioc_;
    const GameData& data_;
    MessageSender& sender_;
    std::unique_ptr<ICrimeEvaluator> evaluator_;
    std::unique_ptr<IGuessJudge> judge_;
    std::mt19937 rng_;
    boost::asio::steady_timer turn_timer_;

    GameState state_ = GameState::Lobby;
    std::vector<Player> players_;
    int next_player_id_ = 1;
    int host_id_ = -1;
    // Which map this room is playing on. Points into GameData::maps (which
    // outlives every GameRoom), and can change via select_map any time
    // before start_game — the evaluator/judge take it per call for exactly
    // this reason, rather than having it fixed at their construction.
    const MapDef* selected_map_;

    std::vector<int> criminal_queue_;
    int round_number_ = 0;
    int criminal_id_ = -1;
    std::string crime_location_id_;
    std::string crime_location_name_;
    std::string crime_weapon_;
    std::string crime_text_;
    CrimeEvaluation crime_eval_;

    std::deque<int> pending_order_;
    std::map<int, int> attempts_used_;
    std::map<int, int> solved_at_attempt_;
    int current_detective_ = -1;
    // True from the moment a guess is accepted until judge_->judge()'s
    // callback fires — guards against a second submit_guess (e.g. a
    // double-click before the client disables its button) starting a
    // second concurrent judging call for the same turn, which would pop
    // pending_order_ twice and corrupt whose turn it is.
    bool guess_in_flight_ = false;
    // True from when guess_feedback has been shown until next_turn/the
    // 10s timer actually advances the turn.
    bool awaiting_advance_ = false;
    // Bumped whenever the turn/round moves on independently of a pending
    // AI call (begin_next_turn, a new round, or a criminal disconnect
    // aborting the round). An in-flight judge_->judge() callback captures
    // this value at submission time and compares it before touching any
    // state, so a result that arrives after (say) the answering detective
    // disconnected mid-call is discarded instead of corrupting whatever
    // turn/round is active by then.
    int turn_generation_ = 0;
};

}
