#include "GameRoom.hpp"

#include <algorithm>
#include <chrono>

#include <boost/system/error_code.hpp>

#include "AiTestLog.hpp"

namespace mom {

namespace {

constexpr int kMinPlayers = 3;
constexpr int kMaxPlayers = 6;
constexpr int kMaxAttempts = 3;
constexpr double kAttemptMultipliers[kMaxAttempts] = {1.0, 0.9, 0.8};
constexpr int kCorrectBonus = 10;
// Time limits on the two active composition phases (see the class
// comment for why these exist but turn/round advancement doesn't have
// timers of its own anymore).
constexpr int kCrimeWritingSeconds = 90;
constexpr int kGuessSeconds = 45;

// 32 hex characters (128 bits) — see docs/RECONNECT_DESIGN.md for what
// this token is and isn't meant to defend against. Massive overkill for
// the actual threat model (a friend guessing a small player_id), but
// generating a few extra random hex digits costs nothing.
std::string generate_token(std::mt19937& rng)
{
    static const char kHex[] = "0123456789abcdef";
    std::uniform_int_distribution<int> dist(0, 15);
    std::string token;
    token.reserve(32);
    for (int i = 0; i < 32; ++i) token += kHex[dist(rng)];
    return token;
}

const Player* find_player_const(const std::vector<Player>& players, int player_id)
{
    for (const auto& p : players) {
        if (p.id == player_id) return &p;
    }
    return nullptr;
}

}

GameRoom::GameRoom(boost::asio::io_context& ioc,
                    const GameData& data,
                    MessageSender& sender,
                    std::unique_ptr<ICrimeEvaluator> evaluator,
                    std::unique_ptr<IGuessJudge> judge)
    : ioc_(ioc)
    , data_(data)
    , sender_(sender)
    , evaluator_(std::move(evaluator))
    , judge_(std::move(judge))
    , rng_(std::random_device{}())
    , phase_timer_(ioc)
    , selected_map_(&data.default_map())
{
}

Player* GameRoom::find_player(int player_id)
{
    for (auto& p : players_) {
        if (p.id == player_id) return &p;
    }
    return nullptr;
}

void GameRoom::send_error(int player_id, const std::string& message)
{
    sender_.send(player_id, {{"type", "error"}, {"message", message}});
}

int GameRoom::handle_join(const std::string& name)
{
    if (state_ != GameState::Lobby) return -1;
    if (static_cast<int>(players_.size()) >= kMaxPlayers) return -1;

    Player p;
    p.id = next_player_id_++;
    p.name = name.empty() ? ("Player" + std::to_string(p.id)) : name;
    p.token = generate_token(rng_);
    players_.push_back(p);

    if (host_id_ == -1) host_id_ = p.id;

    broadcast_room_update();
    return p.id;
}

std::string GameRoom::player_token(int player_id) const
{
    const Player* p = find_player_const(players_, player_id);
    return p ? p->token : std::string();
}

bool GameRoom::verify_rejoin(int player_id, const std::string& token) const
{
    const Player* p = find_player_const(players_, player_id);
    return p && !token.empty() && p->token == token;
}

void GameRoom::handle_reconnect(int player_id)
{
    Player* p = find_player(player_id);
    if (!p) return;
    p->connected = true;
    broadcast_room_update();
}

void GameRoom::handle_disconnect(int player_id)
{
    Player* p = find_player(player_id);
    if (!p) return;

    if (state_ == GameState::Lobby) {
        players_.erase(
            std::remove_if(players_.begin(), players_.end(),
                            [player_id](const Player& pl) { return pl.id == player_id; }),
            players_.end());
        if (host_id_ == player_id) {
            host_id_ = players_.empty() ? -1 : players_.front().id;
        }
        broadcast_room_update();
        return;
    }

    p->connected = false;

    // Host succession applies here too, not just in Lobby: without it, a
    // host who disconnects mid-game leaves host_id_ pointing at a session
    // that will never come back (there's no reconnect feature), which
    // permanently locks out host-only actions for whoever's left —
    // restart_game and next_round both only ever fire on an explicit host
    // message now (no auto-advance timer backs either up), so a stuck
    // host_id_ would strand the room for good.
    if (host_id_ == player_id) {
        auto it = std::find_if(players_.begin(), players_.end(),
                                [](const Player& pl) { return pl.connected; });
        host_id_ = (it != players_.end()) ? it->id : -1;
    }

    const bool round_in_progress =
        state_ == GameState::CrimeWriting || state_ == GameState::AIJudging ||
        state_ == GameState::Investigation;

    if (player_id == criminal_id_ && round_in_progress) {
        sender_.broadcast({{"type", "error"}, {"message", "범인의 연결이 끊겨 라운드를 종료합니다."}});
        phase_timer_.cancel();  // whichever of the crime-writing/guess timers might be pending
        awaiting_advance_ = false;
        guess_in_flight_ = false;
        turn_generation_++;
        pending_order_.clear();
        crime_eval_ = CrimeEvaluation{};
        solved_at_attempt_.clear();
        finish_round();
        return;
    }

    if (state_ == GameState::Investigation) {
        const bool was_current = (player_id == current_detective_);
        pending_order_.erase(std::remove(pending_order_.begin(), pending_order_.end(), player_id),
                              pending_order_.end());

        if (was_current) {
            phase_timer_.cancel();  // that detective's guess timer, if it was still pending
            awaiting_advance_ = false;
            guess_in_flight_ = false;
            begin_next_turn();
            return;
        }
        broadcast_room_update();
        return;
    }

    broadcast_room_update();
}

void GameRoom::handle_message(int player_id, const nlohmann::json& msg)
{
    const std::string type = msg.value("type", "");

    if (type == "start_game") {
        handle_start_game(player_id);
    } else if (type == "restart_game") {
        handle_restart_game(player_id);
    } else if (type == "select_map") {
        handle_select_map(player_id, msg);
    } else if (type == "submit_crime") {
        handle_submit_crime(player_id, msg);
    } else if (type == "submit_guess") {
        handle_submit_guess(player_id, msg);
    } else if (type == "next_turn") {
        handle_next_turn(player_id);
    } else if (type == "next_round") {
        handle_next_round(player_id);
    } else {
        send_error(player_id, "알 수 없는 메시지 유형입니다: " + type);
    }
}

void GameRoom::handle_start_game(int player_id)
{
    if (state_ != GameState::Lobby) {
        send_error(player_id, "게임이 이미 시작되었습니다.");
        return;
    }
    if (player_id != host_id_) {
        send_error(player_id, "방장만 게임을 시작할 수 있습니다.");
        return;
    }
    const int count = static_cast<int>(players_.size());
    if (count < kMinPlayers || count > kMaxPlayers) {
        send_error(player_id, "3~6명이 모여야 시작할 수 있습니다.");
        return;
    }

    criminal_queue_.clear();
    for (const auto& p : players_) criminal_queue_.push_back(p.id);
    std::shuffle(criminal_queue_.begin(), criminal_queue_.end(), rng_);

    round_number_ = 0;
    broadcast_board_info();
    start_round();
}

void GameRoom::handle_select_map(int player_id, const nlohmann::json& msg)
{
    if (state_ != GameState::Lobby) {
        send_error(player_id, "게임이 시작되기 전에만 지도를 바꿀 수 있습니다.");
        return;
    }
    if (player_id != host_id_) {
        send_error(player_id, "방장만 지도를 바꿀 수 있습니다.");
        return;
    }
    const std::string map_id = msg.value("map_id", "");
    const MapDef* map = data_.find_map(map_id);
    if (!map) {
        send_error(player_id, "존재하지 않는 지도입니다.");
        return;
    }

    selected_map_ = map;
    broadcast_board_info();
}

void GameRoom::handle_restart_game(int player_id)
{
    if (state_ != GameState::GameOver) {
        send_error(player_id, "게임이 끝난 후에만 다시 시작할 수 있습니다.");
        return;
    }
    if (player_id != host_id_) {
        send_error(player_id, "방장만 다시 시작할 수 있습니다.");
        return;
    }

    // Drop anyone who disconnected during the last game so they don't
    // count toward the 3~6 player check for the next one.
    players_.erase(
        std::remove_if(players_.begin(), players_.end(), [](const Player& p) { return !p.connected; }),
        players_.end());

    for (auto& p : players_) {
        p.score = 0;
        p.has_been_criminal = false;
    }

    criminal_queue_.clear();
    round_number_ = 0;
    criminal_id_ = -1;
    crime_location_id_.clear();
    crime_location_name_.clear();
    crime_weapon_.clear();
    crime_text_.clear();
    crime_eval_ = CrimeEvaluation{};
    crime_score_revealed_ = false;
    last_round_result_ = nullptr;
    pending_order_.clear();
    attempts_used_.clear();
    solved_at_attempt_.clear();
    current_detective_ = -1;
    guess_in_flight_ = false;
    awaiting_advance_ = false;

    state_ = GameState::Lobby;
    broadcast_room_update();
}

nlohmann::json GameRoom::build_board_info() const
{
    const MapDef& map = *selected_map_;

    nlohmann::json rooms = nlohmann::json::array();
    for (const auto& r : map.rooms) rooms.push_back({{"id", r.id}, {"name", r.name}, {"surveilled", r.surveilled}});

    nlohmann::json edges = nlohmann::json::array();
    for (const auto& [a, b] : map.edges) edges.push_back({a, b});

    nlohmann::json weapons = nlohmann::json::array();
    for (const auto& w : map.weapons) weapons.push_back({{"id", w.id}, {"name", w.name}});

    nlohmann::json available_maps = nlohmann::json::array();
    for (const auto& m : data_.maps) available_maps.push_back({{"id", m.id}, {"name", m.name}});

    return {{"type", "board_info"},
            {"available_maps", available_maps},
            {"map", {{"id", map.id}, {"name", map.name},
                     {"image", map.image ? nlohmann::json(*map.image) : nlohmann::json(nullptr)},
                     {"surveillance_label", map.surveillance_label ? nlohmann::json(*map.surveillance_label) : nlohmann::json(nullptr)}}},
            {"rooms", rooms},
            {"edges", edges},
            {"weapons", weapons}};
}

void GameRoom::broadcast_board_info()
{
    // The map (rooms + adjacency + image) and the weapon list only change
    // when the host picks a different map in Lobby (select_map) — this
    // gets called on every such change, plus once at start_game as a
    // final sync.
    sender_.broadcast(build_board_info());
}

void GameRoom::send_board_info(int player_id)
{
    sender_.send(player_id, build_board_info());
}

void GameRoom::start_round()
{
    state_ = GameState::RoleAssignment;

    criminal_id_ = criminal_queue_.front();
    criminal_queue_.erase(criminal_queue_.begin());
    // has_been_criminal is set once this player's round as criminal is
    // actually over (in finish_round), not here — setting it this early
    // showed the "범인 완료" badge on a player who was still in the
    // middle of writing their crime.

    round_number_++;

    crime_location_id_.clear();
    crime_location_name_.clear();
    crime_weapon_.clear();
    crime_text_.clear();
    crime_eval_ = CrimeEvaluation{};
    crime_score_revealed_ = false;
    last_round_result_ = nullptr;
    pending_order_.clear();
    attempts_used_.clear();
    solved_at_attempt_.clear();
    current_detective_ = -1;
    guess_in_flight_ = false;
    awaiting_advance_ = false;

    state_ = GameState::CrimeWriting;

    sender_.broadcast({{"type", "round_start"},
                        {"round", round_number_},
                        {"crime_writing_seconds", kCrimeWritingSeconds}});

    for (const auto& p : players_) {
        if (p.id == criminal_id_) {
            sender_.send(p.id, {{"type", "your_role"}, {"role", "criminal"}});
        } else {
            sender_.send(p.id, {{"type", "your_role"}, {"role", "detective"}});
        }
    }

    schedule_crime_writing_timeout();

    broadcast_room_update();
}

void GameRoom::schedule_crime_writing_timeout()
{
    phase_timer_.expires_after(std::chrono::seconds(kCrimeWritingSeconds));
    std::weak_ptr<char> alive = alive_;
    phase_timer_.async_wait([this, alive](const boost::system::error_code& ec) {
        if (ec) return;  // cancelled - the crime was submitted (or the round otherwise ended) in time
        if (alive.expired()) return;  // room was torn down while this was pending
        if (state_ != GameState::CrimeWriting) return;  // extra safety, shouldn't be reachable given cancel() above
        handle_crime_writing_timeout();
    });
}

void GameRoom::handle_crime_writing_timeout()
{
    sender_.broadcast({{"type", "error"}, {"message", "범인이 시간 내에 범행을 작성하지 못해 라운드를 종료합니다."}});
    crime_eval_ = CrimeEvaluation{};
    crime_eval_.evaluation = "범인이 시간 내에 범행을 작성하지 못했습니다.";
    finish_round();
}

void GameRoom::handle_submit_crime(int player_id, const nlohmann::json& msg)
{
    if (state_ != GameState::CrimeWriting) {
        send_error(player_id, "지금은 범행을 제출할 수 없습니다.");
        return;
    }
    if (player_id != criminal_id_) {
        send_error(player_id, "범인만 범행을 제출할 수 있습니다.");
        return;
    }
    const std::string text = msg.value("text", "");
    if (text.empty()) {
        send_error(player_id, "범행 내용을 입력하세요.");
        return;
    }
    // Neither location nor weapon is a separate field — the criminal only
    // ever writes free text, and both are extracted from it by matching
    // against the map's room names / the weapon list. This also keeps
    // them consistent with the narrative by construction: there's no way
    // for the "official" weapon to disagree with what the text actually
    // describes, since it's read from the same text.
    const WeaponDef* weapon = data_.extract_weapon_mention(*selected_map_, text);
    if (!weapon) {
        send_error(player_id, "범행 내용에 사용 가능한 흉기 이름을 포함해 주세요.");
        return;
    }
    const RoomDef* room = data_.extract_room_mention(*selected_map_, text);
    if (!room) {
        send_error(player_id, "범행 내용에 지도에 있는 구체적인 장소를 포함해 주세요.");
        return;
    }

    phase_timer_.cancel();  // submitted in time, no need for the crime-writing timeout anymore

    crime_text_ = text;
    crime_weapon_ = weapon->name;
    crime_location_id_ = room->id;
    crime_location_name_ = room->name;
    state_ = GameState::AIJudging;
    broadcast_room_update();
    run_ai_judging();
}

void GameRoom::run_ai_judging()
{
    Crime crime{crime_location_name_, crime_weapon_, crime_text_};
    const int round = round_number_;
    const int criminal_id = criminal_id_;
    const int generation = turn_generation_;
    std::weak_ptr<char> alive = alive_;
    evaluator_->evaluate(crime, *selected_map_, [this, crime, round, criminal_id, generation, alive](CrimeEvaluation eval) {
        if (alive.expired()) return;  // room was torn down while this call was in flight
        if (generation != turn_generation_) return;  // round was aborted (e.g. criminal disconnected) before this arrived
        log_ai_test_event({{"type", "crime_evaluation"},
                            {"round", round},
                            {"criminal_id", criminal_id},
                            {"location", crime.location},
                            {"weapon", crime.weapon},
                            {"crime_text", crime.text},
                            {"score", eval.score},
                            {"evaluation", eval.evaluation},
                            {"key_facts", eval.key_facts}});
        crime_eval_ = std::move(eval);
        crime_score_revealed_ = true;
        // The score alone (not the reasoning or key_facts, which could
        // hint at the answer) is revealed to everyone as soon as it's
        // known, rather than waiting for round_result — both the
        // criminal and the detectives use it to gauge the stakes before
        // investigation starts.
        sender_.broadcast({{"type", "crime_score_revealed"}, {"score", crime_eval_.score}});

        // The criminal alone gets the full breakdown: playtesting found
        // that even the criminal often couldn't tell why a detective's
        // guess was judged "유사" instead of "일치" on some aspect without
        // seeing how the AI itself read their own crime. Sent privately
        // (sender_.send, not broadcast) so detectives never see it —
        // otherwise this would just hand them the answer.
        nlohmann::json answer_key_json = nlohmann::json::array();
        for (const auto& a : crime_eval_.answer_key) {
            answer_key_json.push_back({{"aspect", a.aspect}, {"answer", a.answer}});
        }
        sender_.send(criminal_id_, {{"type", "crime_answer_key"},
                                     {"answer_key", answer_key_json},
                                     {"score_breakdown", build_score_breakdown_json()}});

        start_investigation();
    });
}

void GameRoom::start_investigation()
{
    pending_order_.clear();
    attempts_used_.clear();
    solved_at_attempt_.clear();

    std::vector<int> detective_ids;
    for (const auto& p : players_) {
        if (p.id != criminal_id_ && p.connected) detective_ids.push_back(p.id);
    }
    std::shuffle(detective_ids.begin(), detective_ids.end(), rng_);
    for (int id : detective_ids) pending_order_.push_back(id);

    state_ = GameState::Investigation;
    begin_next_turn();
}

void GameRoom::begin_next_turn()
{
    turn_generation_++;

    if (pending_order_.empty()) {
        finish_round();
        return;
    }

    current_detective_ = pending_order_.front();
    const int attempt_number = attempts_used_[current_detective_] + 1;

    sender_.broadcast({{"type", "investigation_turn_start"},
               {"detective_id", current_detective_},
               {"attempt", attempt_number},
               {"guess_seconds", kGuessSeconds}});
    broadcast_room_update();
    schedule_guess_timeout();
}

void GameRoom::handle_submit_guess(int player_id, const nlohmann::json& msg)
{
    if (state_ != GameState::Investigation) {
        send_error(player_id, "지금은 추리를 제출할 수 없습니다.");
        return;
    }
    if (player_id != current_detective_) {
        send_error(player_id, "지금은 당신의 차례가 아닙니다.");
        return;
    }
    if (awaiting_advance_ || guess_in_flight_) {
        send_error(player_id, "이미 이번 차례에 제출했습니다.");
        return;
    }
    const std::string text = msg.value("text", "");
    if (text.empty()) {
        send_error(player_id, "추리 내용을 입력하세요.");
        return;
    }

    // Set synchronously, before the async AI call starts — a second
    // submit_guess arriving while this one is still in flight (e.g. a
    // double-click before the client can disable its button) must be
    // rejected by the check above, not allowed to start a second judging
    // call for the same turn.
    guess_in_flight_ = true;
    phase_timer_.cancel();  // submitted in time, no need for the guess timeout anymore
    sender_.send(player_id, {{"type", "guess_pending"}});
    const int generation = turn_generation_;
    std::weak_ptr<char> alive = alive_;

    Crime crime{crime_location_name_, crime_weapon_, crime_text_};
    judge_->judge(crime, *selected_map_, text, [this, player_id, text, crime, generation, alive](GuessFeedback fb) {
        if (alive.expired()) return;  // room was torn down while this call was in flight

        if (generation != turn_generation_) {
            // The turn (or round) already moved on without this result —
            // e.g. this detective disconnected while the AI was still
            // thinking. Applying it now would touch state that belongs
            // to a different turn/round, so just drop it.
            return;
        }

        guess_in_flight_ = false;
        attempts_used_[player_id] = attempts_used_[player_id] + 1;
        pending_order_.pop_front();

        if (fb.correct) {
            solved_at_attempt_[player_id] = attempts_used_[player_id];
        } else if (attempts_used_[player_id] < kMaxAttempts) {
            pending_order_.push_back(player_id);
        }

        nlohmann::json aspects_json = nlohmann::json::array();
        for (const auto& a : fb.aspects) {
            aspects_json.push_back({{"aspect", a.aspect}, {"verdict", a.verdict}});
        }

        log_ai_test_event({{"type", "guess_judging"},
                            {"round", round_number_},
                            {"player_id", player_id},
                            {"attempt", attempts_used_[player_id]},
                            {"guess_text", text},
                            {"crime_text", crime.text},
                            {"location", crime.location},
                            {"weapon", crime.weapon},
                            {"correct", fb.correct},
                            {"aspects", aspects_json}});

        sender_.broadcast({{"type", "guess_feedback"},
                   {"player_id", player_id},
                   {"guess_text", text},
                   {"correct", fb.correct},
                   {"attempt", attempts_used_[player_id]},
                   {"aspects", aspects_json}});

        // No auto-advance timer: the answering detective (or the host)
        // sends next_turn explicitly whenever they're ready.
        awaiting_advance_ = true;
    });
}

void GameRoom::schedule_guess_timeout()
{
    phase_timer_.expires_after(std::chrono::seconds(kGuessSeconds));
    const int generation = turn_generation_;
    std::weak_ptr<char> alive = alive_;
    phase_timer_.async_wait([this, generation, alive](const boost::system::error_code& ec) {
        if (ec) return;  // cancelled - a guess was submitted (or the round moved on) in time
        if (alive.expired()) return;  // room was torn down while this was pending
        if (generation != turn_generation_) return;  // turn already moved on for some other reason
        if (guess_in_flight_ || awaiting_advance_) return;  // already submitted/being judged/already answered
        handle_guess_timeout();
    });
}

void GameRoom::handle_guess_timeout()
{
    const int player_id = current_detective_;
    attempts_used_[player_id] = attempts_used_[player_id] + 1;
    pending_order_.pop_front();
    if (attempts_used_[player_id] < kMaxAttempts) {
        pending_order_.push_back(player_id);
    }

    // Synthesized exactly as a real "no content in the guess" verdict
    // would come back from the judge (see OllamaGuessJudge's own rule for
    // that case) — timing out is just an extreme case of submitting
    // nothing.
    const nlohmann::json aspects_json = nlohmann::json::array({
        {{"aspect", "장소"}, {"verdict", "불일치"}},
        {{"aspect", "무기"}, {"verdict", "불일치"}},
        {{"aspect", "살해 방법"}, {"verdict", "불일치"}},
        {{"aspect", "은닉 장소"}, {"verdict", "불일치"}},
        {{"aspect", "은닉 방법"}, {"verdict", "불일치"}},
    });

    log_ai_test_event({{"type", "guess_timeout"},
                        {"round", round_number_},
                        {"player_id", player_id},
                        {"attempt", attempts_used_[player_id]}});

    sender_.broadcast({{"type", "guess_feedback"},
               {"player_id", player_id},
               {"guess_text", ""},
               {"timed_out", true},
               {"correct", false},
               {"attempt", attempts_used_[player_id]},
               {"aspects", aspects_json}});

    // Same manual-only advance as a real answer — see the class comment.
    // The absent detective won't be the one to click it, but the host can.
    awaiting_advance_ = true;
}

void GameRoom::handle_next_turn(int player_id)
{
    if (state_ != GameState::Investigation || !awaiting_advance_ ||
        (player_id != current_detective_ && player_id != host_id_)) {
        send_error(player_id, "지금은 다음 차례로 넘길 수 없습니다.");
        return;
    }
    awaiting_advance_ = false;
    begin_next_turn();
}

void GameRoom::finish_round()
{
    state_ = GameState::Result;

    if (Player* criminal = find_player(criminal_id_)) criminal->has_been_criminal = true;

    std::map<int, int> gained;
    const bool anyone_solved = !solved_at_attempt_.empty();
    gained[criminal_id_] = anyone_solved ? 0 : crime_eval_.score;

    for (const auto& [id, attempt] : solved_at_attempt_) {
        const int base_reward = 100 - crime_eval_.score;
        const double multiplier = kAttemptMultipliers[std::clamp(attempt, 1, kMaxAttempts) - 1];
        gained[id] = static_cast<int>(base_reward * multiplier) + kCorrectBonus;
    }

    for (const auto& [id, amount] : gained) {
        if (Player* p = find_player(id)) p->score += amount;
    }

    nlohmann::json total_scores = nlohmann::json::object();
    for (const auto& p : players_) total_scores[std::to_string(p.id)] = p.score;
    nlohmann::json gained_json = nlohmann::json::object();
    for (const auto& [id, amount] : gained) gained_json[std::to_string(id)] = amount;

    // The per-category breakdown (like `evaluation` and `key_facts`) is
    // withheld from everyone but the criminal until round_result — see
    // run_ai_judging's private crime_answer_key message for why the
    // criminal already saw it earlier.
    //
    // Cached (not just broadcast) because round_result is a one-shot
    // event: a player who reconnects during Result/NextRound would
    // otherwise have no way to ever see this round's reveal, since it
    // already went out once to whoever was connected at the time. See
    // build_game_state_sync / docs/RECONNECT_DESIGN.md.
    last_round_result_ = {{"type", "round_result"},
               {"round", round_number_},
               {"criminal_id", criminal_id_},
               {"crime_text", crime_text_},
               {"weapon", crime_weapon_},
               {"location", crime_location_name_},
               {"location_id", crime_location_id_},
               {"crime_score", crime_eval_.score},
               {"score_breakdown", build_score_breakdown_json()},
               {"evaluation", crime_eval_.evaluation},
               {"key_facts", crime_eval_.key_facts},
               {"scores_gained", gained_json},
               {"total_scores", total_scores}};
    sender_.broadcast(last_round_result_);

    if (criminal_queue_.empty()) {
        state_ = GameState::GameOver;
        int winner_id = -1;
        int best = -1;
        for (const auto& p : players_) {
            if (p.score > best) { best = p.score; winner_id = p.id; }
        }
        sender_.broadcast({{"type", "game_over"}, {"total_scores", total_scores}, {"winner_id", winner_id}});
        broadcast_room_update();
        return;
    }

    // Wait here instead of immediately starting the next round, so players
    // actually have time to read round_result's reveal (the crime, the
    // score, who solved it) rather than it flashing by as the next
    // round's round_start/your_role messages instantly hide it. No
    // auto-advance timer — only the host's explicit next_round moves on
    // (see the class comment for why).
    state_ = GameState::NextRound;
    broadcast_room_update();
}

void GameRoom::handle_next_round(int player_id)
{
    if (state_ != GameState::NextRound) {
        send_error(player_id, "지금은 다음 라운드로 넘길 수 없습니다.");
        return;
    }
    if (player_id != host_id_) {
        send_error(player_id, "방장만 다음 라운드로 넘길 수 있습니다.");
        return;
    }
    start_round();
}

nlohmann::json GameRoom::build_score_breakdown_json() const
{
    nlohmann::json breakdown_json = nlohmann::json::array();
    for (const auto& item : crime_eval_.breakdown) {
        breakdown_json.push_back({{"category", item.category}, {"score", item.score}, {"max", item.max}});
    }
    return breakdown_json;
}

nlohmann::json GameRoom::build_game_state_sync(int player_id) const
{
    const bool round_in_progress =
        state_ == GameState::RoleAssignment || state_ == GameState::CrimeWriting ||
        state_ == GameState::AIJudging || state_ == GameState::Investigation ||
        state_ == GameState::Result || state_ == GameState::NextRound;

    nlohmann::json sync = {
        {"type", "game_state_sync"},
        {"state", to_string(state_)},
        {"round", round_number_},
        {"role", round_in_progress ? nlohmann::json(player_id == criminal_id_ ? "criminal" : "detective") : nlohmann::json(nullptr)},
        {"crime_score", crime_score_revealed_ ? nlohmann::json(crime_eval_.score) : nlohmann::json(nullptr)},
        {"current_detective_id", nullptr},
        {"current_attempt", nullptr},
        {"answer_key", nullptr},
        {"score_breakdown", nullptr},
        // Result/NextRound already had their one-shot round_result
        // broadcast go out to whoever was connected at the time — a
        // reconnecting player missed it, so hand them the same payload
        // again here rather than leaving them with nothing to look at.
        {"round_result", (state_ == GameState::Result || state_ == GameState::NextRound) ? last_round_result_ : nlohmann::json(nullptr)},
    };

    if (state_ == GameState::Investigation && current_detective_ != -1) {
        sync["current_detective_id"] = current_detective_;
        int attempt = 1;
        auto it = attempts_used_.find(current_detective_);
        if (it != attempts_used_.end()) attempt = it->second + 1;
        sync["current_attempt"] = attempt;
    }

    // Re-sends the same private answer key the criminal already got right
    // after crime_score_revealed — crime_eval_ is still sitting in memory,
    // no recomputation needed. Detectives never get this (see
    // run_ai_judging's identical guard).
    if (round_in_progress && player_id == criminal_id_ && crime_score_revealed_) {
        nlohmann::json answer_key_json = nlohmann::json::array();
        for (const auto& a : crime_eval_.answer_key) {
            answer_key_json.push_back({{"aspect", a.aspect}, {"answer", a.answer}});
        }
        sync["answer_key"] = answer_key_json;
        sync["score_breakdown"] = build_score_breakdown_json();
    }

    return sync;
}

nlohmann::json GameRoom::build_room_update() const
{
    nlohmann::json players_json = nlohmann::json::array();
    for (const auto& p : players_) {
        players_json.push_back({{"id", p.id},
                                 {"name", p.name},
                                 {"connected", p.connected},
                                 {"score", p.score},
                                 {"has_been_criminal", p.has_been_criminal},
                                 {"is_host", p.id == host_id_}});
    }

    return {{"type", "room_update"},
            {"state", to_string(state_)},
            {"round", round_number_},
            {"players", players_json}};
}

void GameRoom::broadcast_room_update()
{
    sender_.broadcast(build_room_update());
}

void GameRoom::send_room_snapshot(int player_id)
{
    sender_.send(player_id, build_room_update());
    send_board_info(player_id);
}

bool GameRoom::is_empty() const
{
    return std::none_of(players_.begin(), players_.end(), [](const Player& p) { return p.connected; });
}

}
