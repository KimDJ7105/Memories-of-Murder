#include "GameRoom.hpp"

#include <algorithm>

namespace mom {

namespace {

constexpr int kMinPlayers = 3;
constexpr int kMaxPlayers = 6;
constexpr int kMaxAttempts = 3;
constexpr double kAttemptMultipliers[kMaxAttempts] = {1.0, 0.9, 0.8};
constexpr int kCorrectBonus = 10;

const std::vector<std::string> kLocations = {"주방", "서재", "차고", "정원", "지하실", "다락방"};
const std::vector<std::string> kWeapons = {"칼", "둔기", "밧줄", "독약", "총", "촛대"};

}

GameRoom::GameRoom(MessageSender& sender,
                    std::unique_ptr<ICrimeEvaluator> evaluator,
                    std::unique_ptr<IGuessJudge> judge)
    : sender_(sender)
    , evaluator_(std::move(evaluator))
    , judge_(std::move(judge))
    , rng_(std::random_device{}())
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
        crime_eval_ = CrimeEvaluation{};
        solved_at_attempt_.clear();
        finish_round();
        return;
    }

    if (state_ == GameState::Investigation) {
        pending_detectives_.erase(player_id);
        current_guesses_.erase(player_id);

        if (pending_detectives_.empty()) {
            finish_round();
            return;
        }

        bool all_in = true;
        for (int id : pending_detectives_) {
            if (!current_guesses_.count(id)) { all_in = false; break; }
        }
        if (all_in) run_guess_judging();
        return;
    }

    broadcast_room_update();
}

void GameRoom::handle_message(int player_id, const nlohmann::json& msg)
{
    const std::string type = msg.value("type", "");

    if (type == "start_game") {
        handle_start_game(player_id);
    } else if (type == "submit_crime") {
        handle_submit_crime(player_id, msg);
    } else if (type == "submit_guess") {
        handle_submit_guess(player_id, msg);
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
    start_round();
}

void GameRoom::start_round()
{
    state_ = GameState::RoleAssignment;

    criminal_id_ = criminal_queue_.front();
    criminal_queue_.erase(criminal_queue_.begin());
    if (Player* criminal = find_player(criminal_id_)) criminal->has_been_criminal = true;

    round_number_++;

    std::uniform_int_distribution<size_t> loc_dist(0, kLocations.size() - 1);
    std::uniform_int_distribution<size_t> weapon_dist(0, kWeapons.size() - 1);
    location_ = kLocations[loc_dist(rng_)];
    weapon_ = kWeapons[weapon_dist(rng_)];

    crime_text_.clear();
    crime_eval_ = CrimeEvaluation{};
    attempt_ = 0;
    pending_detectives_.clear();
    current_guesses_.clear();
    solved_at_attempt_.clear();

    state_ = GameState::CrimeWriting;

    sender_.broadcast({{"type", "round_start"},
               {"round", round_number_},
               {"location", location_},
               {"weapon", weapon_}});

    for (const auto& p : players_) {
        sender_.send(p.id, {{"type", "your_role"},
                             {"role", to_string(p.id == criminal_id_ ? Role::Criminal : Role::Detective)}});
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
    if (text.empty()) {
        send_error(player_id, "범행 내용을 입력하세요.");
        return;
    }

    crime_text_ = text;
    state_ = GameState::AIJudging;
    broadcast_room_update();
    run_ai_judging();
}

void GameRoom::run_ai_judging()
{
    Crime crime{location_, weapon_, crime_text_};
    evaluator_->evaluate(crime, [this](CrimeEvaluation eval) {
        crime_eval_ = std::move(eval);
        start_investigation_attempt();
    });
}

void GameRoom::start_investigation_attempt()
{
    attempt_++;

    if (attempt_ == 1) {
        pending_detectives_.clear();
        for (const auto& p : players_) {
            if (p.id != criminal_id_ && p.connected) pending_detectives_.insert(p.id);
        }
    }

    if (pending_detectives_.empty()) {
        finish_round();
        return;
    }

    current_guesses_.clear();
    state_ = GameState::Investigation;

    nlohmann::json pending = nlohmann::json::array();
    for (int id : pending_detectives_) pending.push_back(id);

    sender_.broadcast({{"type", "investigation_start"}, {"attempt", attempt_}, {"pending_detectives", pending}});
    broadcast_room_update();
}

void GameRoom::handle_submit_guess(int player_id, const nlohmann::json& msg)
{
    if (state_ != GameState::Investigation) {
        send_error(player_id, "지금은 추리를 제출할 수 없습니다.");
        return;
    }
    if (!pending_detectives_.count(player_id)) {
        send_error(player_id, "추리를 제출할 수 없는 상태입니다.");
        return;
    }
    if (current_guesses_.count(player_id)) {
        send_error(player_id, "이번 시도에는 이미 제출했습니다.");
        return;
    }
    const std::string text = msg.value("text", "");
    if (text.empty()) {
        send_error(player_id, "추리 내용을 입력하세요.");
        return;
    }

    current_guesses_[player_id] = text;

    for (int id : pending_detectives_) {
        if (!current_guesses_.count(id)) return;
    }
    run_guess_judging();
}

void GameRoom::run_guess_judging()
{
    Crime crime{location_, weapon_, crime_text_};
    judge_->judge(crime, current_guesses_, [this](std::map<int, bool> results) {
        for (const auto& [id, correct] : results) {
            if (correct) {
                solved_at_attempt_[id] = attempt_;
                pending_detectives_.erase(id);
            }
        }

        nlohmann::json results_json = nlohmann::json::object();
        for (const auto& [id, correct] : results) results_json[std::to_string(id)] = correct;
        nlohmann::json still_pending = nlohmann::json::array();
        for (int id : pending_detectives_) still_pending.push_back(id);

        sender_.broadcast({{"type", "guess_result"},
                   {"attempt", attempt_},
                   {"results", results_json},
                   {"still_pending", still_pending}});

        current_guesses_.clear();

        if (pending_detectives_.empty() || attempt_ >= kMaxAttempts) {
            finish_round();
        } else {
            start_investigation_attempt();
        }
    });
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

void GameRoom::broadcast_room_update()
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

    sender_.broadcast({{"type", "room_update"},
               {"state", to_string(state_)},
               {"round", round_number_},
               {"players", players_json}});
}

}
