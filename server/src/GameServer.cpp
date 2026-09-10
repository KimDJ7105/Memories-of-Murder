#include "GameServer.hpp"
#include "CrimeEvaluator.hpp"
#include "GuessJudge.hpp"

namespace mom {

GameServer::GameServer()
    : room_(*this, std::make_unique<MockCrimeEvaluator>(), std::make_unique<MockGuessJudge>())
{
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
