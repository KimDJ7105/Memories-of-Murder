#pragma once

#include <functional>

#include "Crime.hpp"
#include "GameData.hpp"

namespace mom {

// Abstracts "how good is this crime". The Mock scores locally with no
// external calls; the Ollama-backed implementation calls a local model.
// The callback shape (rather than a direct return) is deliberate: a
// synchronous mock can call it immediately, and a future async
// implementation (Phase 4's AI job queue) can call it later without
// forcing callers to change.
//
// `map` is whichever MapDef the room currently has selected — passed per
// call rather than fixed at construction, since a room's map can change
// (via select_map) after its evaluator is already constructed.
class ICrimeEvaluator {
public:
    virtual ~ICrimeEvaluator() = default;
    virtual void evaluate(const Crime& crime, const MapDef& map, std::function<void(CrimeEvaluation)> on_done) = 0;
};

// Deterministic-ish placeholder: scores from text length and rewards
// mentioning the assigned location/weapon. Good enough to exercise the
// full round flow without an AI backend.
class MockCrimeEvaluator : public ICrimeEvaluator {
public:
    void evaluate(const Crime& crime, const MapDef& map, std::function<void(CrimeEvaluation)> on_done) override;
};

}
