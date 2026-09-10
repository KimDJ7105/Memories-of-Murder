#include "CrimeEvaluator.hpp"

#include <algorithm>

namespace mom {

void MockCrimeEvaluator::evaluate(const Crime& crime, std::function<void(CrimeEvaluation)> on_done)
{
    // Placeholder scoring only: rewards a longer, more detailed confession
    // and mentioning the assigned location/weapon. No real judgement of
    // plausibility happens here — that arrives with the Ollama-backed
    // evaluator in Phase 2.
    int score = 40 + static_cast<int>(crime.text.size() / 4);
    if (crime.text.find(crime.location) != std::string::npos) score += 10;
    if (crime.text.find(crime.weapon) != std::string::npos) score += 10;
    score = std::clamp(score, 0, 100);

    CrimeEvaluation eval;
    eval.score = score;
    eval.evaluation = "Mock 평가: 세부 묘사와 장소/무기 언급을 기준으로 산출된 임시 점수입니다.";
    eval.key_facts = {"장소: " + crime.location, "무기: " + crime.weapon};

    on_done(std::move(eval));
}

}
