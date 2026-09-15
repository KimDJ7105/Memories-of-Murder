#include "CrimeEvaluator.hpp"

#include <algorithm>
#include <cmath>

namespace mom {

namespace {

struct CategorySpec {
    const char* name;
    int max;
};
// Mirrors OllamaCrimeEvaluator's kCategories (docs/DESIGN.md section 7)
// purely so the client's breakdown UI has real per-category numbers to
// render when testing against the mock backend, without special-casing it.
constexpr CategorySpec kCategories[] = {
    {"장소/환경 일치성", 25},
    {"이동 및 실행 가능성", 20},
    {"무기 사용의 개연성", 20},
    {"범행 과정의 개연성", 20},
    {"증거/무기 은닉의 개연성", 15},
};

}

void MockCrimeEvaluator::evaluate(const Crime& crime, const MapDef&, std::function<void(CrimeEvaluation)> on_done)
{
    // Placeholder scoring only: rewards a longer, more detailed confession
    // and mentioning the assigned location/weapon. No real judgement of
    // plausibility happens here — that arrives with the Ollama-backed
    // evaluator. The overall score is split across the same five rubric
    // categories proportionally to their weight, and the final score is
    // the sum of that breakdown (same invariant OllamaCrimeEvaluator
    // follows) rather than the standalone `overall` value below.
    int overall = 40 + static_cast<int>(crime.text.size() / 4);
    if (crime.text.find(crime.location) != std::string::npos) overall += 10;
    if (crime.text.find(crime.weapon) != std::string::npos) overall += 10;
    overall = std::clamp(overall, 0, 100);

    CrimeEvaluation eval;
    for (const auto& c : kCategories) {
        const int score = std::clamp(static_cast<int>(std::lround(c.max * overall / 100.0)), 0, c.max);
        eval.breakdown.push_back({c.name, score, c.max});
        eval.score += score;
    }

    eval.evaluation = "Mock 평가: 세부 묘사와 장소/무기 언급을 기준으로 산출된 임시 점수입니다.";
    eval.key_facts = {"장소: " + crime.location, "무기: " + crime.weapon};

    on_done(std::move(eval));
}

}
