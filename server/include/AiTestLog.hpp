#pragma once

#include <nlohmann/json.hpp>

namespace mom {

// Appends `record` as one line to logs/ai_test/YYYY-MM-DD.jsonl when the
// AI_TEST_MODE environment variable is set (docs/DESIGN.md section 13) —
// for comparing models, spotting bad scores, and reviewing judging calls
// after the fact instead of watching them live. A "timestamp" field is
// added automatically if not already present.
//
// No-op — no env lookup cost beyond a single getenv, no file I/O at all —
// when AI_TEST_MODE isn't set, which is the default and the common case
// during normal play.
void log_ai_test_event(nlohmann::json record);

}
