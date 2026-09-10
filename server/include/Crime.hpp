#pragma once

#include <string>
#include <vector>

namespace mom {

// The criminal's original free-text confession. This is the Truth Source
// for the round (see docs/DESIGN.md section 8) — evaluators and judges may
// derive structured summaries from it, but never replace it.
struct Crime {
    std::string location;
    std::string weapon;
    std::string text;
};

// Structured output of an ICrimeEvaluator. `key_facts` is a best-effort
// extraction used by IGuessJudge to compare a detective's guess against the
// crime; it is a derived aid, not a substitute for `Crime::text`.
struct CrimeEvaluation {
    int score = 0;
    std::string evaluation;
    std::vector<std::string> key_facts;
};

}
