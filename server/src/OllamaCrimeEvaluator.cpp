#include "OllamaCrimeEvaluator.hpp"

#include <algorithm>
#include <sstream>

#include <nlohmann/json.hpp>

#include "OllamaClient.hpp"

namespace mom {

namespace {

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
    for (const auto& w : data.weapons) weapons << "- " << w.name << "\n";

    std::ostringstream prompt;
    prompt <<
        "당신은 추리 게임 '살인의 추억'의 범행 평가관입니다.\n\n"
        "지도에 있는 방 목록:\n" << rooms.str() <<
        "\n서로 붙어 있어 이동 가능한 방 쌍:\n" << edges.str() <<
        "\n사용 가능한 흉기 목록:\n" << weapons.str() <<
        "\n범인이 자유 서술형으로 작성한 범행을 다음 기준으로 평가하세요 (총점 100점):\n"
        "- 장소/환경 일치성 (25점): 언급된 장소가 위 지도의 방과 실제로 일치하는가\n"
        "- 이동 및 실행 가능성 (20점): 다른 방으로 이동했다면 위 인접 관계상 실제로 가능한 경로인가\n"
        "- 무기 사용의 개연성 (20점)\n"
        "- 범행 과정의 개연성 (20점)\n"
        "- 증거/무기 은닉의 개연성 (15점)\n\n"
        "반드시 다음 JSON 형식으로만 응답하고 다른 텍스트는 포함하지 마세요:\n"
        "{\"score\": 0에서 100 사이의 정수, \"evaluation\": \"한두 문장의 평가 이유\", "
        "\"key_facts\": [\"범행의 핵심 사실을 나열한 문자열\", \"...\"]}\n"
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

CrimeEvaluation fallback_evaluation(const std::string& reason)
{
    CrimeEvaluation eval;
    eval.score = 50;
    eval.evaluation = "AI 평가에 실패하여 기본 점수(50)가 적용되었습니다. (" + reason + ")";
    return eval;
}

// Field-by-field validation so one malformed field doesn't discard the
// other well-formed ones, and so a missing/misshapen response never
// throws back into the caller.
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

    eval.score = (j.contains("score") && j.at("score").is_number_integer())
                     ? j.at("score").get<int>()
                     : 50;
    eval.score = std::clamp(eval.score, 0, 100);

    eval.evaluation = (j.contains("evaluation") && j.at("evaluation").is_string())
                           ? j.at("evaluation").get<std::string>()
                           : "(평가 내용 없음)";

    if (j.contains("key_facts") && j.at("key_facts").is_array()) {
        for (const auto& fact : j.at("key_facts")) {
            if (fact.is_string()) eval.key_facts.push_back(fact.get<std::string>());
        }
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
        [on_done = std::move(on_done)](bool ok, std::string content) mutable {
            on_done(parse_evaluation(ok, content));
        });
}

}
