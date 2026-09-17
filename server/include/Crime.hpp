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

// One rubric category's contribution to CrimeEvaluation::score (see
// docs/DESIGN.md section 7's evaluation criteria table). `max` travels
// with each item so the client can render something like "18/20" without
// hardcoding the rubric's point allocations on its own side.
struct ScoreBreakdownItem {
    std::string category;
    int score = 0;
    int max = 0;
};

// One aspect of the "official" reading of the crime — the same five
// aspects a detective's guess is judged against (장소/무기/살해 방법/은닉
// 장소/은닉 방법), but stating what the crime actually was rather than
// judging a guess against it. Sent to the criminal only (see GameRoom's
// `crime_answer_key` message) so they can understand *why* a detective's
// guess was scored the way it was — playtesting found that even the
// criminal often couldn't tell why their own crime was judged "유사"
// instead of "일치" on some aspect without seeing this.
struct AspectAnswer {
    std::string aspect;
    std::string answer;
};

// Structured output of an ICrimeEvaluator. `key_facts` is a best-effort
// extraction used by IGuessJudge to compare a detective's guess against the
// crime; it is a derived aid, not a substitute for `Crime::text`.
//
// `score` is always the sum of `breakdown`'s items, never a separately
// AI-supplied number — see OllamaCrimeEvaluator's parse_evaluation for why:
// trusting a model's own arithmetic invites a total that doesn't match the
// parts it just listed, which would look broken to a player comparing them.
struct CrimeEvaluation {
    int score = 0;
    std::string evaluation;
    std::vector<std::string> key_facts;
    std::vector<ScoreBreakdownItem> breakdown;
    // Always 5 entries in the fixed order 장소/무기/살해 방법/은닉 장소/은닉
    // 방법 once populated by an evaluator.
    std::vector<AspectAnswer> answer_key;
};

}
