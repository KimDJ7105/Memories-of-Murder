#include "GuessJudge.hpp"

namespace mom {

namespace {

bool contains(const std::string& haystack, const std::string& needle)
{
    return !needle.empty() && haystack.find(needle) != std::string::npos;
}

std::vector<std::string> tokenize(const std::string& text)
{
    static const std::string delimiters = " \t\n,.!?'\"";
    std::vector<std::string> tokens;
    std::string current;
    for (char c : text) {
        if (delimiters.find(c) != std::string::npos) {
            if (current.size() >= 2) tokens.push_back(current);
            current.clear();
        } else {
            current += c;
        }
    }
    if (current.size() >= 2) tokens.push_back(current);
    return tokens;
}

double overlap_ratio(const std::vector<std::string>& crime_tokens, const std::string& guess_text)
{
    if (crime_tokens.empty()) return 0.0;
    size_t matched = 0;
    for (const auto& token : crime_tokens) {
        if (contains(guess_text, token)) matched++;
    }
    return static_cast<double>(matched) / crime_tokens.size();
}

std::string verdict_from_ratio(double ratio)
{
    if (ratio >= 0.5) return "일치";
    if (ratio >= 0.25) return "유사";
    return "불일치";
}

}

void MockGuessJudge::judge(
    const Crime& crime,
    const std::string& guess_text,
    std::function<void(GuessFeedback)> on_done)
{
    // Placeholder heuristic, not real language understanding. The crime
    // text is split roughly in half to stand in for "method" (first half)
    // vs "concealment" (second half) since Phase 1 has no real structured
    // extraction yet — that arrives with the AI-backed judge in Phase 2.
    const std::string& text = crime.text;
    const size_t mid = text.size() / 2;
    const std::string method_half = text.substr(0, mid);
    const std::string concealment_half = text.substr(mid);

    const bool location_match = contains(guess_text, crime.location);
    const bool weapon_match = contains(guess_text, crime.weapon);
    const double method_ratio = overlap_ratio(tokenize(method_half), guess_text);
    const double concealment_ratio = overlap_ratio(tokenize(concealment_half), guess_text);
    const double overall_ratio = overlap_ratio(tokenize(text), guess_text);

    GuessFeedback fb;
    fb.aspects = {
        {"장소", location_match ? "일치" : "불일치"},
        {"무기", weapon_match ? "일치" : "불일치"},
        {"살해 방법", verdict_from_ratio(method_ratio)},
        {"은닉 방법", verdict_from_ratio(concealment_ratio)},
    };
    // Overall correctness now also requires naming the location, since the
    // criminal picks it freely instead of it being announced up front —
    // see docs/DESIGN.md section 10's rejection of partial matches like
    // "칼을 냉장고에 숨겼다".
    fb.correct = location_match && weapon_match && overall_ratio >= 0.5;

    on_done(std::move(fb));
}

}
