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
            env_or("MOM_OLLAMA_MODEL", "exaone3.5:7.8b"),
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
    , room_(ioc, game_data_, *this, make_evaluator(ioc), make_judge(ioc))
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

    if (!session->player_id()) {
        if (msg.value("type", "") != "join") {
            session->send({{"type", "error"}, {"message", "join 메시지가 필요합니다."}});
            return;
        }
        const std::string name = msg.value("name", "");
        const int id = room_.handle_join(name);
        if (id == -1) {
            session->send({{"type", "error"}, {"message", "입장할 수 없습니다 (게임 진행 중이거나 인원이 가득 찼습니다)."}});
            session->close();
            return;
        }
        session->set_player_id(id);
        sessions_[id] = session;
        session->send({{"type", "joined"}, {"player_id", id}});
        // handle_join already broadcast a room_update, but that happened
        // before this session was registered above, so this player missed
        // it — send them a snapshot directly now that they're registered.
        room_.send_room_snapshot(id);
        return;
    }

    room_.handle_message(*session->player_id(), msg);
}

void GameServer::on_disconnect(const std::shared_ptr<Session>& session)
{
    if (auto id = session->player_id()) {
        sessions_.erase(*id);
        room_.handle_disconnect(*id);
    }
}

void GameServer::send(int player_id, const nlohmann::json& msg)
{
    auto it = sessions_.find(player_id);
    if (it != sessions_.end()) it->second->send(msg);
}

void GameServer::broadcast(const nlohmann::json& msg)
{
    for (auto& [id, session] : sessions_) session->send(msg);
}

}
