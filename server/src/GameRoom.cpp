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
constexpr int kTurnSeconds = 10;

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
    , turn_timer_(ioc)
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
    players_.push_back(p);

    if (host_id_ == -1) host_id_ = p.id;

    broadcast_room_update();
    return p.id;
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

    const bool round_in_progress =
        state_ == GameState::CrimeWriting || state_ == GameState::AIJudging ||
        state_ == GameState::Investigation;

    if (player_id == criminal_id_ && round_in_progress) {
        sender_.broadcast({{"type", "error"}, {"message", "범인의 연결이 끊겨 라운드를 종료합니다."}});
        if (awaiting_advance_) { turn_timer_.cancel(); awaiting_advance_ = false; }
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
            if (awaiting_advance_) { turn_timer_.cancel(); awaiting_advance_ = false; }
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
    } else if (type == "submit_crime") {
        handle_submit_crime(player_id, msg);
    } else if (type == "submit_guess") {
        handle_submit_guess(player_id, msg);
    } else if (type == "next_turn") {
        handle_next_turn(player_id);
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
    pending_order_.clear();
    attempts_used_.clear();
    solved_at_attempt_.clear();
    current_detective_ = -1;
    guess_in_flight_ = false;
    awaiting_advance_ = false;

    state_ = GameState::Lobby;
    broadcast_room_update();
}

void GameRoom::broadcast_board_info()
{
    // Static for the whole game: the map (rooms + adjacency) and the
    // weapon list never change per round, so this is sent once rather
    // than repeated in every round_start.
    nlohmann::json rooms = nlohmann::json::array();
    for (const auto& r : data_.map.rooms) {
        rooms.push_back({{"id", r.id}, {"name", r.name}});
    }
    nlohmann::json edges = nlohmann::json::array();
    for (const auto& [a, b] : data_.map.edges) edges.push_back({a, b});
    nlohmann::json weapons = nlohmann::json::array();
    for (const auto& w : data_.weapons) weapons.push_back({{"id", w.id}, {"name", w.name}});

    sender_.broadcast({{"type", "board_info"},
               {"map", {{"id", data_.map.id}, {"name", data_.map.name}, {"image", data_.map.image ? nlohmann::json(*data_.map.image) : nlohmann::json(nullptr)}}},
               {"rooms", rooms},
               {"edges", edges},
               {"weapons", weapons}});
}

void GameRoom::start_round()
{
    state_ = GameState::RoleAssignment;

    criminal_id_ = criminal_queue_.front();
    criminal_queue_.erase(criminal_queue_.begin());
    if (Player* criminal = find_player(criminal_id_)) criminal->has_been_criminal = true;

    round_number_++;

    crime_location_id_.clear();
    crime_location_name_.clear();
    crime_weapon_.clear();
    crime_text_.clear();
    crime_eval_ = CrimeEvaluation{};
    pending_order_.clear();
    attempts_used_.clear();
    solved_at_attempt_.clear();
    current_detective_ = -1;
    guess_in_flight_ = false;
    awaiting_advance_ = false;

    state_ = GameState::CrimeWriting;

    sender_.broadcast({{"type", "round_start"}, {"round", round_number_}});

    for (const auto& p : players_) {
        if (p.id == criminal_id_) {
            sender_.send(p.id, {{"type", "your_role"}, {"role", "criminal"}});
        } else {
            sender_.send(p.id, {{"type", "your_role"}, {"role", "detective"}});
        }
    }

    broadcast_room_update();
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
    const std::string weapon = msg.value("weapon", "");
    if (text.empty()) {
        send_error(player_id, "범행 내용을 입력하세요.");
        return;
    }
    if (!data_.is_valid_weapon(weapon)) {
        send_error(player_id, "유효한 흉기를 선택하세요.");
        return;
    }
    // The weapon picker is a separate UI control from the free-text
    // narrative, so nothing else forces them to agree — without this
    // check a criminal could pick "총" but write a narrative about
    // bludgeoning someone with a candlestick, and the judge would then
    // reasonably score guesses against what the *text* actually
    // describes, silently disagreeing with the picked weapon. Requiring
    // the picked weapon's name to appear in the text keeps both sources
    // of truth consistent.
    if (text.find(weapon) == std::string::npos) {
        send_error(player_id, "범행 내용에 선택한 흉기(" + weapon + ")를 포함해 주세요.");
        return;
    }
    // Location isn't a separate field — the criminal only writes free
    // text (plus picks a weapon), and the location is extracted from that
    // text by matching it against the map's known room names.
    const RoomDef* room = data_.extract_room_mention(text);
    if (!room) {
        send_error(player_id, "범행 내용에 지도에 있는 구체적인 장소를 포함해 주세요.");
        return;
    }

    crime_text_ = text;
    crime_weapon_ = weapon;
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
    evaluator_->evaluate(crime, [this, crime, round, criminal_id](CrimeEvaluation eval) {
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
        // The score alone (not the reasoning or key_facts, which could
        // hint at the answer) is revealed to everyone as soon as it's
        // known, rather than waiting for round_result — both the
        // criminal and the detectives use it to gauge the stakes before
        // investigation starts.
        sender_.broadcast({{"type", "crime_score_revealed"}, {"score", crime_eval_.score}});
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
               {"attempt", attempt_number}});
    broadcast_room_update();
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
    sender_.send(player_id, {{"type", "guess_pending"}});
    const int generation = turn_generation_;

    Crime crime{crime_location_name_, crime_weapon_, crime_text_};
    judge_->judge(crime, text, [this, player_id, text, crime, generation](GuessFeedback fb) {
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

        awaiting_advance_ = true;
        schedule_turn_advance();
    });
}

void GameRoom::schedule_turn_advance()
{
    turn_timer_.expires_after(std::chrono::seconds(kTurnSeconds));
    turn_timer_.async_wait([this](const boost::system::error_code& ec) {
        if (ec) return;
        if (!awaiting_advance_) return;
        awaiting_advance_ = false;
        begin_next_turn();
    });
}

void GameRoom::handle_next_turn(int player_id)
{
    if (state_ != GameState::Investigation || !awaiting_advance_ || player_id != current_detective_) {
        send_error(player_id, "지금은 다음 차례로 넘길 수 없습니다.");
        return;
    }
    turn_timer_.cancel();
    awaiting_advance_ = false;
    begin_next_turn();
}

void GameRoom::finish_round()
{
    state_ = GameState::Result;

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

    sender_.broadcast({{"type", "round_result"},
               {"round", round_number_},
               {"criminal_id", criminal_id_},
               {"crime_text", crime_text_},
               {"weapon", crime_weapon_},
               {"location", crime_location_name_},
               {"location_id", crime_location_id_},
               {"crime_score", crime_eval_.score},
               {"evaluation", crime_eval_.evaluation},
               {"key_facts", crime_eval_.key_facts},
               {"scores_gained", gained_json},
               {"total_scores", total_scores}});

    state_ = GameState::NextRound;

    if (criminal_queue_.empty()) {
        state_ = GameState::GameOver;
        int winner_id = -1;
        int best = -1;
        for (const auto& p : players_) {
            if (p.score > best) { best = p.score; winner_id = p.id; }
        }
        sender_.broadcast({{"type", "game_over"}, {"total_scores", total_scores}, {"winner_id", winner_id}});
        broadcast_room_update();
    } else {
        start_round();
    }
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
}

}
