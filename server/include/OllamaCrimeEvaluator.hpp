#pragma once

#include <string>

#include <boost/asio/io_context.hpp>

#include "CrimeEvaluator.hpp"
#include "GameData.hpp"

namespace mom {

// Real AI-backed evaluator: sends the crime to a local Ollama model over
// HTTP (OllamaClient) instead of MockCrimeEvaluator's keyword heuristic.
// The room's currently-selected map (passed in per call — a room can
// change its map via select_map after this evaluator already exists) and
// the shared weapon list (GameData) are baked into the system prompt so
// the model can judge things like "does this weapon exist" and "is this
// movement plausible given room adjacency" (docs/DESIGN.md section 7)
// rather than evaluating the text in a vacuum.
//
// Output is defensively normalized field-by-field in the .cpp: a model
// that gets one field wrong (or returns no valid JSON at all, or times
// out) still yields a usable CrimeEvaluation rather than propagating an
// exception into GameRoom.
class OllamaCrimeEvaluator : public ICrimeEvaluator {
public:
    OllamaCrimeEvaluator(boost::asio::io_context& ioc,
                         const GameData& data,
                         std::string model,
                         std::string host,
                         std::string port);

    void evaluate(const Crime& crime, const MapDef& map, std::function<void(CrimeEvaluation)> on_done) override;

private:
    boost::asio::io_context& ioc_;
    const GameData& data_;
    std::string model_;
    std::string host_;
    std::string port_;
};

}
