#pragma once

#include <map>
#include <memory>

#include <boost/asio/io_context.hpp>

#include "CrimeEvaluator.hpp"
#include "GameData.hpp"
#include "GameRoom.hpp"
#include "GuessJudge.hpp"
#include "MessageSender.hpp"
#include "Session.hpp"

namespace mom {

// Ties the network layer to game logic. Phase 1 owns exactly one GameRoom;
// a RoomManager can later hand GameServer a room per connection/lobby code
// without changing how Session or GameRoom work (docs/DESIGN.md Phase 3).
//
// Which AI backend GameRoom gets is chosen once here via environment
// variables, so switching models (or falling back to the deterministic
// Mock implementations for fast scripted tests that shouldn't depend on
// Ollama being up) never requires touching GameRoom itself:
//   MOM_AI_BACKEND   "ollama" (default) or "mock"
//   MOM_OLLAMA_MODEL model tag, default "qwen2.5:7b" (exaone3.5:7.8b was
//                    tried first but showed a real weakness: it sometimes
//                    judged a meaningless guess like "몰라" as matching
//                    the weapon anyway, even with a stricter prompt and
//                    lower temperature — qwen2.5:7b didn't have this problem)
//   MOM_OLLAMA_HOST  default "localhost"
//   MOM_OLLAMA_PORT  default "11434"
class GameServer : public MessageSender {
public:
    explicit GameServer(boost::asio::io_context& ioc);

    void on_message(const std::shared_ptr<Session>& session, const std::string& text);
    void on_disconnect(const std::shared_ptr<Session>& session);

    void send(int player_id, const nlohmann::json& msg) override;
    void broadcast(const nlohmann::json& msg) override;

private:
    std::unique_ptr<ICrimeEvaluator> make_evaluator(boost::asio::io_context& ioc) const;
    std::unique_ptr<IGuessJudge> make_judge(boost::asio::io_context& ioc) const;

    std::map<int, std::shared_ptr<Session>> sessions_;
    // Declared before room_ so it's constructed first and outlives the
    // GameRoom that holds a reference to it.
    GameData game_data_;
    GameRoom room_;
};

}
