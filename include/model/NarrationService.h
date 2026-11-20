#pragma once
//
// SDD-013: query -> facts -> generate -> validate, for one night.
//
// Retrieval is deterministic: C++ reads the night through IDatabase into the
// shared NightFacts (SDD-072 D1, the structure every AI path renders from) and
// renders it as the fact block. The model is never asked what to fetch. Its
// prompt is the fact block, and its only job is the language around it.
//
// The generation contract:
//   1. every number in the text comes from the fact block;
//   2. the text is checked: a number the block does not have (the SDD-072 D3
//      grounding rule, which allows a figure said with fewer places) or a date
//      other than the night's own rejects it;
//   3. a rejected text is generated once more at a lower temperature, and a
//      second rejection gives the deterministic template, so the user gets a
//      plain correct summary rather than a fluent wrong one, and the log says
//      why.
//
#include "database/IDatabase.h"
#include "model/LocalModel.h"
#include "services/NightFacts.h"

#include <optional>
#include <string>
#include <vector>

namespace hms_cpap::model {

/// The summaries this path writes carry this source (IDatabase::saveSummary).
extern const char* const kLocalSummarySource;   // "local"

/// The fact block for [night]: its day, then the shared renderer's lines.
/// Stable and ordered, because prompt drift silently degrades a fine-tuned
/// model.
std::string factBlock(const NightFacts& night);

/// What is wrong with [text] against [block]: each number the block does not
/// have and each date that is not the block's own, as written. Empty: it
/// passes.
std::vector<std::string> violations(const std::string& text, const std::string& block);

/// The deterministic summary for [night], every figure from its fact block.
std::string templateSummary(const NightFacts& night);

struct Narration {
    bool written = false;     ///< false: nothing to store (no night, or the model did not run)
    std::string text;
    std::string how;          ///< "model", "model_retry" or "template"
    std::string why;          ///< the reason for a template, or for nothing written
    NightFacts night;
};

/// The temperatures of the first try and the one retry.
constexpr double kFirstTemperature = 0.7;
constexpr double kRetryTemperature = 0.2;

/// One night's summary from [model]. Reads nothing but [night].
Narration narrate(const NightFacts& night, ILocalModel& model);

/// The night [sleep_day] (YYYY-MM-DD) of [device_id], narrated and stored
/// as a daily summary with source "local". Nothing is stored when the night
/// is not in the database or the model is not ready.
Narration narrateAndStore(IDatabase& db, const std::string& device_id, const std::string& sleep_day,
                          ILocalModel& model);

}  // namespace hms_cpap::model
