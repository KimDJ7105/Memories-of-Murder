#pragma once

#include <map>
#include <memory>

#include <boost/asio/io_context.hpp>

#include "CrimeEvaluator.hpp"
#include "GameData.hpp"
#include "GuessJudge.hpp"
#include "RoomManager.hpp"
#include "Session.hpp"

namespace mom {

// Ties the network layer to game logic. GameServer itself is a thin
// router: it knows which room code and player id each Session belongs to
// (nothing else), and hands every actual message off to RoomManager, which
// owns the GameRoom instances (docs/DESIGN.md Phase 3).
//
// Which AI backend each room's GameRoom gets is chosen once here via
// environment variables, so switching models (or falling back to the
// deterministic Mock implementations for fast scripted tests that
// shouldn't depend on Ollama being up) never requires touching GameRoom
// or RoomManager:
//   MOM_AI_BACKEND   "ollama" (default) or "mock"
//   MOM_OLLAMA_MODEL model tag, default "qwen2.5:7b" (exaone3.5:7.8b was
//                    tried first but showed a real weakness: it sometimes
//                    judged a meaningless guess like "몰라" as matching
//                    the weapon anyway, even with a stricter prompt and
//                    lower temperature — qwen2.5:7b didn't have this problem)
//   MOM_OLLAMA_HOST  default "localhost"
//   MOM_OLLAMA_PORT  default "11434"
class GameServer {
public:
    explicit GameServer(boost::asio::io_context& ioc);

    void on_message(const std::shared_ptr<Session>& session, const std::string& text);
    void on_disconnect(const std::shared_ptr<Session>& session);

private:
    std::unique_ptr<ICrimeEvaluator> make_evaluator(boost::asio::io_context& ioc) const;
    std::unique_ptr<IGuessJudge> make_judge(boost::asio::io_context& ioc) const;

    void handle_create_room(const std::shared_ptr<Session>& session, const nlohmann::json& msg);
    void handle_join_room(const std::shared_ptr<Session>& session, const nlohmann::json& msg);

    // Declared before room_manager_ so it's constructed first and outlives
    // every GameRoom, which holds a reference to it.
    GameData game_data_;
    RoomManager room_manager_;
};

}
