#pragma once

#include <string>

#include <boost/asio/io_context.hpp>

#include "GameData.hpp"
#include "GuessJudge.hpp"

namespace mom {

// Real AI-backed judge: asks a local Ollama model whether a detective's
// free-text guess actually describes the same crime, instead of
// MockGuessJudge's token-overlap heuristic. This is the piece
// docs/DESIGN.md section 10 cares about most — rejecting a guess like
// "칼을 냉장고에 숨겼다" that shares surface words with the real crime but
// misses the actual method — since an LLM can judge semantic equivalence
// rather than substring overlap.
class OllamaGuessJudge : public IGuessJudge {
public:
    OllamaGuessJudge(boost::asio::io_context& ioc,
                     const GameData& data,
                     std::string model,
                     std::string host,
                     std::string port);

    void judge(const Crime& crime,
               const MapDef& map,
               const std::string& guess_text,
               std::function<void(GuessFeedback)> on_done) override;

private:
    boost::asio::io_context& ioc_;
    const GameData& data_;
    std::string model_;
    std::string host_;
    std::string port_;
};

}
