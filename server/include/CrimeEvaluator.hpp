#pragma once

#include <functional>
#include "Crime.hpp"

namespace mom {

// Abstracts "how good is this crime". The Phase 1 mock scores locally with
// no external calls; Phase 2 swaps in an Ollama-backed implementation
// behind this same interface. The callback shape (rather than a direct
// return) is deliberate: a synchronous mock can call it immediately, and a
// future async implementation (Phase 4's AI job queue) can call it later
// without forcing callers to change.
class ICrimeEvaluator {
public:
    virtual ~ICrimeEvaluator() = default;
    virtual void evaluate(const Crime& crime, std::function<void(CrimeEvaluation)> on_done) = 0;
};

// Deterministic-ish placeholder: scores from text length and rewards
// mentioning the assigned location/weapon. Good enough to exercise the
// full round flow without an AI backend; replaced wholesale in Phase 2.
class MockCrimeEvaluator : public ICrimeEvaluator {
public:
    void evaluate(const Crime& crime, std::function<void(CrimeEvaluation)> on_done) override;
};

}
