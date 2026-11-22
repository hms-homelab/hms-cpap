#pragma once
//
// Issue #74 (SDD-082 section 8): the configured LLM's summary of a
// SUMMARY-ONLY night, one with no session files but a daily record (myAir's,
// or the card's STR row).
//
// The night is NightFacts' summary-only night (night_facts::loadDailyOnlyNight),
// rendered by the shared renderer into the {metrics} of the user's own prompt,
// so the prompt file and its rules work as for any night. The reply is held to
// the SDD-072 D3 grounding check as every LLM summary is. The row keeps the
// fact block it was written from (cpap_summaries.facts), so a night announced
// again with the same figures is not sent to the LLM again (the 7.0.1 rule the
// local model follows); a night whose figures changed gets a new summary and
// its earlier one goes.
//
// Nothing here runs on a clock: it runs when asked (Regenerate) or when a
// summary-only night lands (night_landed, through the summary queue).
//
#include "database/IDatabase.h"
#include "services/AiGrounding.h"
#include "services/NightFacts.h"

#include <condition_variable>
#include <deque>
#include <functional>
#include <mutex>
#include <optional>
#include <string>
#include <thread>

namespace hms_cpap::daily_only {

struct Result {
    bool written = false;
    bool skipped = false;     ///< written before from these same facts
    std::string text;
    std::string why;          ///< why nothing was written, or why it was skipped
    std::string facts;        ///< the fact block the LLM was given
};

/// The source an LLM summary row carries (IDatabase::saveSummary's default).
extern const char* const kLlmSource;   // "llm"

/// The LLM summary of the summary-only night [sleep_day] (YYYY-MM-DD).
/// [prompt_template] is the configured prompt ({metrics} is replaced by the
/// fact block); [generate] is one LLM call. Nothing is sent when the night is
/// not summary-only (it has sessions, or no daily record). [force]: SDD-082
/// D-082-6, a Regenerate writes a new text even from the same facts; the
/// event path (force false) skips a night summarized from these facts.
Result summarize(IDatabase& db, const std::string& device_id, const std::string& sleep_day,
                 const std::string& prompt_template, const grounding::Generate& generate,
                 bool force = false);

/// D-082-7: the night's summary-only LLM summaries go once its full night's
/// summary is written (the session close's own). Returns how many went.
int dropSummaryOnly(IDatabase& db, const std::string& device_id, const std::string& sleep_day);

/// The fact block the night's newest LLM summary was written from ("" when none).
std::string storedFacts(IDatabase& db, const std::string& device_id, const std::string& sleep_day);

/// Takes landed nights (night_landed's listener only enqueues) and summarizes
/// them one at a time on its own thread through [work].
class Queue {
public:
    explicit Queue(std::function<void(const std::string& sleep_day)> work) : work_(std::move(work)) {}
    ~Queue() { stop(); }
    void start();
    void stop();
    /// [date_folder] is YYYYMMDD or YYYY-MM-DD. Returns at once.
    void enqueue(const std::string& date_folder);

private:
    void run();
    std::function<void(const std::string&)> work_;
    std::mutex mu_;
    std::condition_variable cv_;
    std::deque<std::string> days_;
    bool stopping_ = false;
    std::thread thread_;
};

}  // namespace hms_cpap::daily_only
