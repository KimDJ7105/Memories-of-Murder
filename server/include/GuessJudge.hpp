#pragma once

#include <functional>
#include <map>
#include <string>
#include "Crime.hpp"

namespace mom {

// Judges a batch of detective guesses against one crime in a single call,
// matching docs/DESIGN.md section 11 (one AI call per attempt round, not
// one per detective). Keyed by player id -> guess text in, player id ->
// correct(bool) out.
class IGuessJudge {
public:
    virtual ~IGuessJudge() = default;
    virtual void judge(
        const Crime& crime,
        const std::map<int, std::string>& guesses,
        std::function<void(std::map<int, bool>)> on_done) = 0;
};

// Placeholder heuristic: tokenizes the crime text and a guess, and counts a
// guess correct once it covers enough of the crime's own tokens. Crude on
// purpose — it exists to let the round flow be tested end to end before an
// AI-backed judge (Phase 2) replaces it.
class MockGuessJudge : public IGuessJudge {
public:
    void judge(
        const Crime& crime,
        const std::map<int, std::string>& guesses,
        std::function<void(std::map<int, bool>)> on_done) override;
};

}
