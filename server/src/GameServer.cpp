#include "GameServer.hpp"

#include <cstdlib>
#include <iostream>

#include "OllamaClient.hpp"
#include "OllamaCrimeEvaluator.hpp"
#include "OllamaGuessJudge.hpp"

namespace mom {

namespace {

std::string env_or(const char* name, std::string fallback)
{
    const char* v = std::getenv(name);
    return v ? std::string(v) : fallback;
}

struct AiConfig {
    std::string backend;
    std::string model;
    std::string host;
    std::string port;
};

AiConfig resolve_ai_config()
{
    return {env_or("MOM_AI_BACKEND", "ollama"),
            env_or("MOM_OLLAMA_MODEL", "qwen2.5:7b"),
            env_or("MOM_OLLAMA_HOST", "localhost"),
            env_or("MOM_OLLAMA_PORT", "11434")};
}

// Ollama loads model weights into VRAM lazily, on its first request for
// that model — which can take far longer than a normal inference call.
// Firing one throwaway request at startup (before any player can trigger
// a real one) moves that cold-start cost out of actual gameplay instead
// of it randomly landing on whichever crime/guess happens to be first.
void warm_up_ollama(boost::asio::io_context& ioc, const AiConfig& cfg)
{
    auto client = std::make_shared<OllamaClient>(ioc, cfg.host, cfg.port);
    client->chat_json(
        cfg.model,
        "당신은 서버 시작 시의 준비 상태 확인 호출을 받는 중입니다.",
        "다음 JSON으로만 응답하세요: {\"ok\": true}",
        [](bool ok, std::string content) {
            if (ok) {
                std::cout << "[game_server] Ollama warm-up complete." << std::endl;
            } else {
                std::cerr << "[game_server] Ollama warm-up failed (" << content
                          << ") - the first real AI call may be slow." << std::endl;
            }
        });
}

}

GameServer::GameServer(boost::asio::io_context& ioc)
    : game_data_(GameData::load_default())
    , room_manager_(ioc, game_data_,
                    [this](boost::asio::io_context& ioc) { return make_evaluator(ioc); },
                    [this](boost::asio::io_context& ioc) { return make_judge(ioc); })
{
    const AiConfig cfg = resolve_ai_config();
    if (cfg.backend != "mock") warm_up_ollama(ioc, cfg);
}

std::unique_ptr<ICrimeEvaluator> GameServer::make_evaluator(boost::asio::io_context& ioc) const
{
    const AiConfig cfg = resolve_ai_config();
    if (cfg.backend == "mock") return std::make_unique<MockCrimeEvaluator>();
    return std::make_unique<OllamaCrimeEvaluator>(ioc, game_data_, cfg.model, cfg.host, cfg.port);
}

std::unique_ptr<IGuessJudge> GameServer::make_judge(boost::asio::io_context& ioc) const
{
    const AiConfig cfg = resolve_ai_config();
    if (cfg.backend == "mock") return std::make_unique<MockGuessJudge>();
    return std::make_unique<OllamaGuessJudge>(ioc, game_data_, cfg.model, cfg.host, cfg.port);
}

void GameServer::on_message(const std::shared_ptr<Session>& session, const std::string& text)
{
    nlohmann::json msg;
    try {
        msg = nlohmann::json::parse(text);
    } catch (const nlohmann::json::parse_error&) {
        session->send({{"type", "error"}, {"message", "잘못된 메시지 형식입니다."}});
        return;
    }

    if (!session->room_code()) {
        const std::string type = msg.value("type", "");
        if (type == "create_room") {
            handle_create_room(session, msg);
        } else if (type == "join_room") {
            handle_join_room(session, msg);
        } else if (type == "rejoin") {
            handle_rejoin(session, msg);
        } else {
            session->send({{"type", "error"}, {"message", "create_room, join_room 또는 rejoin 메시지가 필요합니다."}});
        }
        return;
    }

    room_manager_.handle_message(*session->room_code(), *session->player_id(), msg);
}

void GameServer::handle_create_room(const std::shared_ptr<Session>& session, const nlohmann::json& msg)
{
    const std::string name = msg.value("name", "");
    const JoinResult result = room_manager_.create_room(session, name);

    session->set_room_code(result.room_code);
    session->set_player_id(result.player_id);
    // The token goes out once, here, to this player alone — see
    // docs/RECONNECT_DESIGN.md. It never appears in any broadcast.
    session->send({{"type", "room_created"},
                    {"room_code", result.room_code},
                    {"player_id", result.player_id},
                    {"token", result.token}});
    // create_room already broadcast a room_update inside GameRoom, but
    // that happened before this session was registered above, so this
    // player (the only one in the room so far) missed it.
    room_manager_.send_room_snapshot(result.room_code, result.player_id);
}

void GameServer::handle_join_room(const std::shared_ptr<Session>& session, const nlohmann::json& msg)
{
    const std::string room_code = msg.value("room_code", "");
    const std::string name = msg.value("name", "");

    const std::optional<JoinResult> result = room_manager_.join_room(room_code, session, name);
    if (!result) {
        session->send({{"type", "error"}, {"message", "방을 찾을 수 없거나 입장할 수 없습니다 (게임 진행 중이거나 인원이 가득 찼습니다)."}});
        return;
    }

    session->set_room_code(result->room_code);
    session->set_player_id(result->player_id);
    session->send({{"type", "joined"},
                    {"room_code", result->room_code},
                    {"player_id", result->player_id},
                    {"token", result->token}});
    // Same reasoning as handle_create_room: this player's own session
    // wasn't registered yet when GameRoom broadcast its own room_update.
    room_manager_.send_room_snapshot(result->room_code, result->player_id);
}

void GameServer::handle_rejoin(const std::shared_ptr<Session>& session, const nlohmann::json& msg)
{
    const std::string room_code = msg.value("room_code", "");
    const int player_id = msg.value("player_id", -1);
    const std::string token = msg.value("token", "");

    if (!room_manager_.rejoin_room(room_code, player_id, token, session)) {
        session->send({{"type", "error"}, {"message", "재접속할 수 없습니다. 방이 사라졌거나 정보가 일치하지 않습니다."}});
        return;
    }

    session->set_room_code(room_code);
    session->set_player_id(player_id);
    session->send({{"type", "rejoined"}, {"room_code", room_code}, {"player_id", player_id}});
    // room_update/board_info first (roster, scores, map, weapons), then
    // the round-specific context a plain snapshot can't express.
    room_manager_.send_room_snapshot(room_code, player_id);
    room_manager_.send_game_state_sync(room_code, player_id);
}

void GameServer::on_disconnect(const std::shared_ptr<Session>& session)
{
    if (session->room_code() && session->player_id()) {
        room_manager_.handle_disconnect(*session->room_code(), *session->player_id());
    }
}

}
