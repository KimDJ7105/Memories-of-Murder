#include "OllamaGuessJudge.hpp"

#include <set>
#include <sstream>

#include <nlohmann/json.hpp>

#include "OllamaClient.hpp"

namespace mom {

namespace {

const std::set<std::string> kValidVerdicts = {"일치", "유사", "불일치"};

std::string build_system_prompt(const GameData& data)
{
    std::ostringstream rooms;
    for (const auto& r : data.map.rooms) rooms << "- " << r.name << "\n";
    std::ostringstream weapons;
    for (const auto& w : data.weapons) weapons << "- " << w.name << "\n";

    std::ostringstream prompt;
    prompt <<
        "당신은 추리 게임 '살인의 추억'의 탐정 추리 판정관입니다.\n\n"
        "지도에 있는 방 목록:\n" << rooms.str() <<
        "\n사용 가능한 흉기 목록:\n" << weapons.str() <<
        "\n실제 범행 내용과 탐정의 추리를 비교해서, 아래 다섯 항목 각각을 "
        "\"일치\", \"유사\", \"불일치\" 중 하나로 판정하세요: 장소, 무기, 살해 방법, 은닉 장소, 은닉 방법.\n"
        "\"은닉 장소\"는 흉기/증거를 *어디에* 숨겼는지(예: 냉장고 안, 화단 흙 속), \"은닉 방법\"은 "
        "*어떻게* 숨겼는지(예: 연어 배 속에 넣어서, 잘게 잘라서)를 뜻하며 서로 다른 항목입니다.\n"
        "각 항목을 \"일치\"로 판정하려면 탐정의 추리 문장 안에 그 항목에 해당하는 내용이 "
        "실제로 적혀 있어야 합니다. 추리 문장에 아예 등장하지 않는 내용을 있다고 가정하고 "
        "\"일치\"나 \"유사\"를 주면 안 됩니다.\n"
        "\"유사\"는 그 항목에 대해 구체적으로 답했지만 실제와 세부가 다른 경우에만 사용하세요 "
        "(예: 흉기 종류는 맞았지만 다른 항목이 다름, 방향은 맞는데 디테일이 어긋남). "
        "탐정의 추리가 그 항목에 대해 아무 내용도 제시하지 않았거나(예: \"몰라\", \"음...\", \"잉?\" 같은 "
        "무의미한 답변이거나 범행과 전혀 무관한 내용) 완전히 틀렸다면 애매하게 봐주지 말고 반드시 "
        "\"불일치\"로 판정하세요. 확신이 없다고 해서 \"유사\"나 \"일치\"를 기본값으로 쓰지 마세요.\n\n"
        "예시: 탐정의 추리가 \"몰라\"처럼 아무 정보가 없는 한 마디뿐이라면, 장소·무기·살해 방법· "
        "은닉 장소·은닉 방법 다섯 항목 모두 \"불일치\", correct는 false여야 합니다. 이때 실제 범행에 "
        "어떤 흉기가 나왔든, \"몰라\"라는 문장 자체에는 그 흉기 이름이 없으므로 \"무기\"도 반드시 "
        "\"불일치\"입니다.\n\n"
        "전체 정답 여부(correct)는 탐정이 장소·무기·핵심 행위(살해 방법·은닉 장소·은닉 방법)를 "
        "충분히 맞혔을 때만 true로 판단하세요. 일부 단어가 겹치더라도 실제로는 다른 사건을 "
        "설명하고 있다면(예: 흉기는 맞았지만 살해 방법과 은닉 방법이 전혀 다름) correct는 false입니다.\n\n"
        "반드시 다음 JSON 형식으로만 응답하세요:\n"
        "{\"correct\": true 또는 false, \"aspects\": ["
        "{\"aspect\":\"장소\",\"verdict\":\"...\"}, {\"aspect\":\"무기\",\"verdict\":\"...\"}, "
        "{\"aspect\":\"살해 방법\",\"verdict\":\"...\"}, {\"aspect\":\"은닉 장소\",\"verdict\":\"...\"}, "
        "{\"aspect\":\"은닉 방법\",\"verdict\":\"...\"}]}";
    return prompt.str();
}

std::string build_user_prompt(const Crime& crime, const std::string& guess_text)
{
    return "실제 범행: " + crime.text +
           "\n(장소: " + crime.location + ", 무기: " + crime.weapon + ")" +
           "\n\n탐정의 추리: " + guess_text;
}

GuessFeedback failure_feedback(const std::string& reason)
{
    GuessFeedback fb;
    fb.correct = false;
    fb.aspects = {{"오류", "AI 판정 실패: " + reason}};
    return fb;
}

// Field-by-field validation, same rationale as OllamaCrimeEvaluator: never
// throw back into the caller, and clamp any verdict string the model
// invents outside the three we support down to a safe default.
GuessFeedback parse_feedback(bool ok, const std::string& content)
{
    if (!ok) return failure_feedback(content);

    nlohmann::json j;
    try {
        j = nlohmann::json::parse(content);
    } catch (const std::exception& e) {
        return failure_feedback(std::string("malformed JSON: ") + e.what());
    }
    if (!j.is_object()) return failure_feedback("response was not a JSON object");

    GuessFeedback fb;
    fb.correct = (j.contains("correct") && j.at("correct").is_boolean())
                     ? j.at("correct").get<bool>()
                     : false;

    if (j.contains("aspects") && j.at("aspects").is_array()) {
        for (const auto& a : j.at("aspects")) {
            if (!a.is_object()) continue;
            std::string aspect = (a.contains("aspect") && a.at("aspect").is_string())
                                      ? a.at("aspect").get<std::string>()
                                      : "?";
            std::string verdict = (a.contains("verdict") && a.at("verdict").is_string())
                                       ? a.at("verdict").get<std::string>()
                                       : "불일치";
            if (!kValidVerdicts.count(verdict)) verdict = "불일치";
            fb.aspects.push_back({std::move(aspect), std::move(verdict)});
        }
    }

    if (fb.aspects.empty()) {
        fb.aspects.push_back({"판정", fb.correct ? "일치" : "불일치"});
    }

    return fb;
}

}

OllamaGuessJudge::OllamaGuessJudge(boost::asio::io_context& ioc,
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

void OllamaGuessJudge::judge(const Crime& crime,
                             const std::string& guess_text,
                             std::function<void(GuessFeedback)> on_done)
{
    auto client = std::make_shared<OllamaClient>(ioc_, host_, port_);
    client->chat_json(model_, build_system_prompt(data_), build_user_prompt(crime, guess_text),
        [on_done = std::move(on_done)](bool ok, std::string content) mutable {
            on_done(parse_feedback(ok, content));
        });
}

}
