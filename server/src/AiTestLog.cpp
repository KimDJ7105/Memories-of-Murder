#include "AiTestLog.hpp"

#include <chrono>
#include <cstdlib>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <sstream>

namespace mom {

namespace {

bool ai_test_mode_enabled()
{
    const char* v = std::getenv("AI_TEST_MODE");
    if (!v) return false;
    const std::string s = v;
    return s == "1" || s == "true" || s == "TRUE" || s == "True";
}

std::tm local_now()
{
    const std::time_t t = std::chrono::system_clock::to_time_t(std::chrono::system_clock::now());
    std::tm tm{};
    localtime_s(&tm, &t);
    return tm;
}

}

void log_ai_test_event(nlohmann::json record)
{
    if (!ai_test_mode_enabled()) return;

    const std::tm tm = local_now();

    if (!record.contains("timestamp")) {
        std::ostringstream ts;
        ts << std::put_time(&tm, "%Y-%m-%dT%H:%M:%S");
        record["timestamp"] = ts.str();
    }

    std::ostringstream date;
    date << std::put_time(&tm, "%Y-%m-%d");

    const std::filesystem::path dir = "logs/ai_test";
    std::error_code ec;
    std::filesystem::create_directories(dir, ec);
    if (ec) return;

    std::ofstream out(dir / (date.str() + ".jsonl"), std::ios::app);
    if (!out) return;
    // replace, not the default throwing handler: this can carry a fallback
    // CrimeEvaluation's evaluation text, which can itself carry a raw OS
    // error message (see Session::send for why that's not guaranteed to
    // be valid UTF-8) — a debug/testing log write must never be able to
    // crash the server.
    out << record.dump(-1, ' ', false, nlohmann::json::error_handler_t::replace) << "\n";
}

}
