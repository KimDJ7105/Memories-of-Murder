#include "OllamaCrimeEvaluator.hpp"

#include <algorithm>
#include <cmath>
#include <iterator>
#include <sstream>

#include <nlohmann/json.hpp>

#include "OllamaClient.hpp"

namespace mom {

namespace {

// The rubric from docs/DESIGN.md section 7. Both the prompt and the parser
// are built from this single list, so the categories, their point caps, and
// their Korean labels can never drift out of sync with each other.
struct CategorySpec {
    const char* name;
    int max;
};
constexpr CategorySpec kCategories[] = {
    {"장소/환경 일치성", 25},
    {"이동 및 실행 가능성", 20},
    {"무기 사용의 개연성", 20},
    {"범행 과정의 개연성", 20},
    {"증거/무기 은닉의 개연성", 15},
};

// The three aspects that aren't already deterministically known (장소/무기
// come straight from Crime::location/weapon, extracted by exact substring
// match — see GameData::extract_room_mention/extract_weapon_mention).
// These need the model to actually read and summarize the free text.
constexpr const char* kFreeformAspects[] = {"살해 방법", "은닉 장소", "은닉 방법"};

std::string build_system_prompt(const GameData& data, const MapDef& map)
{
    std::ostringstream rooms;
    for (const auto& r : map.rooms) rooms << "- " << r.name << "\n";

    std::ostringstream edges;
    for (const auto& [a, b] : map.edges) {
        const RoomDef* ra = data.find_room(map, a);
        const RoomDef* rb = data.find_room(map, b);
        edges << "- " << (ra ? ra->name : a) << " <-> " << (rb ? rb->name : b) << "\n";
    }

    std::ostringstream weapons;
    for (const auto& w : map.weapons) weapons << "- " << w.name << "\n";

    // Surveilled rooms are always-on risk zones (see docs/PROTOCOL.md
    // "감시 구역") — a static map property, never a time-of-day or patrol
    // schedule the criminal's free text could just claim was empty. Folded
    // into the existing "범행 과정의 개연성" category rather than adding a
    // sixth rubric item, so the 100-point total never needs rebalancing.
    std::ostringstream surveillance;
    for (const auto& r : map.rooms) {
        if (r.surveilled) surveillance << "- " << r.name << "\n";
    }
    const bool has_surveillance = map.surveillance_label.has_value() && !surveillance.str().empty();

    std::ostringstream criteria;
    for (const auto& c : kCategories) {
        criteria << "- " << c.name << " (" << c.max << "점)";
        if (has_surveillance && std::string(c.name) == "범행 과정의 개연성") {
            criteria << ": 감시 구역을 지나가거나 그 안에서 범행을 저질렀다면, 그 위험을 "
                         "어떻게 피하거나 대비했는지도 이 항목에서 함께 판단하세요. 서술에 "
                         "그런 위험 인지·대응이 전혀 없다면 감점하세요.";
        }
        criteria << "\n";
    }

    std::ostringstream breakdown_schema;
    for (size_t i = 0; i < std::size(kCategories); ++i) {
        breakdown_schema << "{\"category\":\"" << kCategories[i].name << "\",\"score\":0에서 "
                         << kCategories[i].max << " 사이의 정수}";
        if (i + 1 < std::size(kCategories)) breakdown_schema << ", ";
    }

    std::ostringstream aspect_summary_schema;
    for (size_t i = 0; i < std::size(kFreeformAspects); ++i) {
        aspect_summary_schema << "\"" << kFreeformAspects[i] << "\":\"...\"";
        if (i + 1 < std::size(kFreeformAspects)) aspect_summary_schema << ", ";
    }

    std::ostringstream prompt;
    prompt <<
        "당신은 추리 게임 '살인의 추억'의 범행 평가관입니다.\n\n"
        "지도에 있는 방 목록:\n" << rooms.str() <<
        "\n서로 붙어 있어 이동 가능한 방 쌍:\n" << edges.str() <<
        "\n사용 가능한 흉기 목록:\n" << weapons.str();
    if (has_surveillance) {
        prompt << "\n항상 " << *map.surveillance_label << "(으)로 감시되는 위험 구역:\n" << surveillance.str();
    }
    prompt <<
        "\n범인이 자유 서술형으로 작성한 범행을 다음 다섯 항목으로 나누어 평가하세요:\n" << criteria.str() <<
        "\n각 항목마다 그 항목의 만점을 넘지 않는 정수 점수를 매기세요. 항목별 점수의 합이 "
        "최종 점수가 되므로, 전체적으로 몇 점을 주고 싶은지를 먼저 정한 뒤 그걸 다섯 항목에 "
        "억지로 나눠 맞추지 말고, 각 항목을 그 항목 자체의 기준으로 독립적으로 채점하세요.\n\n"
        "또한 범인 본인이 나중에 참고할 수 있도록, 서술에서 실제로 어떤 내용이었는지를 다음 세 "
        "항목 각각 한 문장으로 요약하세요 (탐정의 추리를 채점할 때 쓰는 것과 같은 항목입니다). "
        "서술에 명시되지 않은 내용은 추측해서 채우지 말고 \"서술에 명시되지 않음\"이라고 쓰세요: "
        "살해 방법, 은닉 장소(무엇을 어디에 숨겼는지), 은닉 방법(어떻게 숨겼는지).\n\n"
        "반드시 다음 JSON 형식으로만 응답하고 다른 텍스트는 포함하지 마세요:\n"
        "{\"breakdown\": [" << breakdown_schema.str() << "], "
        "\"evaluation\": \"한두 문장의 평가 이유\", "
        "\"key_facts\": [\"범행의 핵심 사실을 나열한 문자열\", \"...\"], "
        "\"aspect_summary\": {" << aspect_summary_schema.str() << "}}\n"
        "key_facts는 범인의 창의적인 세부 묘사(예: 흉기를 숨긴 구체적인 방법)를 뭉뚱그려 "
        "요약하지 말고, 사실 관계를 그대로 보존해서 나열하세요.";
    return prompt.str();
}

std::string build_user_prompt(const Crime& crime)
{
    return "장소(범인이 서술한 곳): " + crime.location +
           "\n무기: " + crime.weapon +
           "\n\n범행 서술:\n" + crime.text;
}

// Half credit in every category, so a total AI failure still produces an
// internally consistent (if uninformative) breakdown rather than an empty
// one the client would have to special-case.
std::vector<ScoreBreakdownItem> half_credit_breakdown()
{
    std::vector<ScoreBreakdownItem> breakdown;
    for (const auto& c : kCategories) {
        breakdown.push_back({c.name, static_cast<int>(std::lround(c.max / 2.0)), c.max});
    }
    return breakdown;
}

int sum_breakdown(const std::vector<ScoreBreakdownItem>& breakdown)
{
    int total = 0;
    for (const auto& item : breakdown) total += item.score;
    return total;
}

CrimeEvaluation fallback_evaluation(const std::string& reason)
{
    CrimeEvaluation eval;
    eval.breakdown = half_credit_breakdown();
    eval.score = sum_breakdown(eval.breakdown);
    eval.evaluation = "AI 평가에 실패하여 항목별 절반 점수가 적용되었습니다. (" + reason + ")";
    for (const char* aspect : kFreeformAspects) eval.answer_key.push_back({aspect, "(AI 평가 실패로 확인 불가)"});
    return eval;
}

// Field-by-field validation so one malformed field doesn't discard the
// other well-formed ones, and so a missing/misshapen response never
// throws back into the caller.
//
// `score` is deliberately never read from the model's JSON: it's always
// the sum of `breakdown`, computed here rather than trusted from the
// model, so the total shown to a player always matches the parts also
// shown to them. Each breakdown category is matched by its exact Korean
// label against `kCategories` (the same list the prompt was built from);
// a category the model omitted or renamed defaults to 0 for that category
// rather than being guessed at, the same "don't assume credit that wasn't
// earned" rule the guess judge already applies to its own aspects.
CrimeEvaluation parse_evaluation(bool ok, const std::string& content)
{
    if (!ok) return fallback_evaluation(content);

    nlohmann::json j;
    try {
        j = nlohmann::json::parse(content);
    } catch (const std::exception& e) {
        return fallback_evaluation(std::string("malformed JSON: ") + e.what());
    }
    if (!j.is_object()) return fallback_evaluation("response was not a JSON object");

    CrimeEvaluation eval;

    for (const auto& c : kCategories) {
        int score = 0;
        if (j.contains("breakdown") && j.at("breakdown").is_array()) {
            for (const auto& item : j.at("breakdown")) {
                if (!item.is_object()) continue;
                const std::string category = (item.contains("category") && item.at("category").is_string())
                                                  ? item.at("category").get<std::string>()
                                                  : "";
                if (category != c.name) continue;
                if (item.contains("score") && item.at("score").is_number_integer()) {
                    score = std::clamp(item.at("score").get<int>(), 0, c.max);
                }
                break;
            }
        }
        eval.breakdown.push_back({c.name, score, c.max});
    }
    eval.score = sum_breakdown(eval.breakdown);

    eval.evaluation = (j.contains("evaluation") && j.at("evaluation").is_string())
                           ? j.at("evaluation").get<std::string>()
                           : "(평가 내용 없음)";

    if (j.contains("key_facts") && j.at("key_facts").is_array()) {
        for (const auto& fact : j.at("key_facts")) {
            if (fact.is_string()) eval.key_facts.push_back(fact.get<std::string>());
        }
    }

    const bool has_summary = j.contains("aspect_summary") && j.at("aspect_summary").is_object();
    for (const char* aspect : kFreeformAspects) {
        std::string answer = "(정보 없음)";
        if (has_summary) {
            const auto& summary = j.at("aspect_summary");
            if (summary.contains(aspect) && summary.at(aspect).is_string()) {
                answer = summary.at(aspect).get<std::string>();
            }
        }
        eval.answer_key.push_back({aspect, answer});
    }

    return eval;
}

}

OllamaCrimeEvaluator::OllamaCrimeEvaluator(boost::asio::io_context& ioc,
                                           const GameData& data,
                                           std::string model,
                                           std::string host,
                                           std::string port)
    : ioc_(ioc)
    , data_(data)
    , model_(std::move(model))
    , host_(std::move(host))
    , port_(std::move(port))
{
}

void OllamaCrimeEvaluator::evaluate(const Crime& crime, const MapDef& map, std::function<void(CrimeEvaluation)> on_done)
{
    auto client = std::make_shared<OllamaClient>(ioc_, host_, port_);
    client->chat_json(model_, build_system_prompt(data_, map), build_user_prompt(crime),
        [on_done = std::move(on_done), location = crime.location, weapon = crime.weapon]
        (bool ok, std::string content) mutable {
            CrimeEvaluation eval = parse_evaluation(ok, content);
            // 장소/무기 are never asked of the model — they're already known
            // exactly from the deterministic extraction in GameRoom, so
            // prepend them here rather than risk the model contradicting a
            // fact the server already settled.
            eval.answer_key.insert(eval.answer_key.begin(), {{"장소", location}, {"무기", weapon}});
            on_done(std::move(eval));
        });
}

}
