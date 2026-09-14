#pragma once

#include <functional>
#include <string>
#include <vector>

#include "Crime.hpp"
#include "GameData.hpp"

namespace mom {

// One dimension of feedback on a single guess, e.g. {"무기", "일치"}.
// `verdict` is one of "일치" (match), "유사" (partial), "불일치" (no match) —
// a plain string rather than an enum so a future AI-backed judge can phrase
// it more richly without changing the wire shape.
struct AspectFeedback {
    std::string aspect;
    std::string verdict;
};

struct GuessFeedback {
    bool correct = false;
    std::vector<AspectFeedback> aspects;
};

// Judges one detective's guess against the crime. Detectives now go one at
// a time (see docs/DESIGN.md revision: turn-based investigation instead of
// batched simultaneous guesses), so this trades the original "one AI call
// per attempt round" batching for "one call per guess" — a deliberate
// tradeoff for immediate per-turn feedback.
// `map` is whichever MapDef the room currently has selected — see
// CrimeEvaluator.hpp for why it's passed per call instead of fixed at
// construction.
class IGuessJudge {
public:
    virtual ~IGuessJudge() = default;
    virtual void judge(
        const Crime& crime,
        const MapDef& map,
        const std::string& guess_text,
        std::function<void(GuessFeedback)> on_done) = 0;
};

// Placeholder heuristic, not real language understanding: splits the crime
// text roughly in half to stand in for "method" vs "concealment" aspects,
// and checks the weapon name directly. Exists to exercise the turn engine
// end to end before an AI-backed judge (Phase 2) replaces it.
class MockGuessJudge : public IGuessJudge {
public:
    void judge(
        const Crime& crime,
        const MapDef& map,
        const std::string& guess_text,
        std::function<void(GuessFeedback)> on_done) override;
};

}
