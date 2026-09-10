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

}

void MockGuessJudge::judge(
    const Crime& crime,
    const std::map<int, std::string>& guesses,
    std::function<void(std::map<int, bool>)> on_done)
{
    // Placeholder heuristic, not real language understanding: a guess
    // counts as correct only if it names both the location and the weapon
    // *and* covers at least half of the crime text's own tokens. This is
    // meant to reject the "칼을 냉장고에 숨겼다" partial-match case from
    // docs/DESIGN.md section 10, not to be linguistically precise — the
    // real judgement arrives with the AI-backed judge in Phase 2.
    const std::vector<std::string> crime_tokens = tokenize(crime.text);

    std::map<int, bool> results;
    for (const auto& [player_id, guess_text] : guesses) {
        const bool has_location = contains(guess_text, crime.location);
        const bool has_weapon = contains(guess_text, crime.weapon);

        size_t matched = 0;
        for (const auto& token : crime_tokens) {
            if (contains(guess_text, token)) matched++;
        }
        const double overlap_ratio =
            crime_tokens.empty() ? 0.0 : static_cast<double>(matched) / crime_tokens.size();

        results[player_id] = has_location && has_weapon && overlap_ratio >= 0.5;
    }

    on_done(std::move(results));
}

}
