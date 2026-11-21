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
//   3. the text is generated greedily (temperature 0), the decode every
//      measurement of narrator-v1 used (2026-11-21, Albin: greedy). A greedy
//      retry would write the same text, so there is none: a rejected text
//      gives the deterministic template, so the user gets a plain correct
//      summary rather than a fluent wrong one, and the log says why.
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
/// have and each date that is not the block's own, as written, and a usage
/// judgement that contradicts the block's target (usageWordingProblem).
/// Empty: it passes.
std::vector<std::string> violations(const std::string& text, const std::string& block);

/// The block's "Usage: X hours (Y% of Th target)" against what [text] says
/// of it: X >= T must not be "short of" / "below" / "missed" the target, and
/// X < T must not be "target met" / "reached" / "achieved". "" when it agrees
/// (or the block has no target line); else the phrase and why.
std::string usageWordingProblem(const std::string& text, const std::string& block);

/// The deterministic summary for [night], every figure from its fact block.
std::string templateSummary(const NightFacts& night);

struct Narration {
    bool written = false;     ///< false: nothing to store (no night, or the model did not run)
    /// D-701-3: nothing written because the night's local summary was
    /// written from this same fact block (narrateAndStore).
    bool skipped = false;
    std::string text;
    std::string how;          ///< "model" or "template"
    std::string why;          ///< the reason for a template, or for nothing written
    NightFacts night;
};

/// The night the model is given: [m] with the night's own STR row [str]
/// (none when the card has no record for it), the way the daily summary is
/// built. The STR names the mode through the machine's family (SDD-030), so an
/// AutoSet reads APAP and an AirCurve VAuto. With no STR a stored mode of 0 is
/// no mode: every engine stores "none" as 0 and no ResMed parser fills it, so
/// 0 would otherwise reach the model as "CPAP" for every ResMed night.
NightFacts modelNight(SessionMetrics m, const STRDailyRecord* str,
                      const std::optional<NightOximetryFacts>& oximetry);

/// Greedy decode: the one try's temperature.
constexpr double kTemperature = 0.0;

/// One night's summary from [model]. Reads nothing but [night].
Narration narrate(const NightFacts& night, ILocalModel& model);

/// The night [sleep_day] (YYYY-MM-DD) of [device_id], narrated and stored
/// as a daily summary with source "local". [str] is the night's STR row, when
/// the card has one (modelNight). Nothing is stored when the night is not in
/// the database or the model is not ready.
///
/// D-701-3: the row keeps the fact block it was written from
/// (cpap_summaries.facts). A night whose fact block is the one its local
/// summary was written from is skipped before the model runs (skipped); a
/// night whose facts changed replaces its earlier local summary, so a night
/// has one.
Narration narrateAndStore(IDatabase& db, const std::string& device_id, const std::string& sleep_day,
                          ILocalModel& model, const STRDailyRecord* str = nullptr);

/// One of a night's local summary rows: its id, and the fact block it was
/// written from ("" for a row stored before cpap_summaries.facts existed).
struct LocalSummaryRow {
    std::string id;
    std::string facts;
};
/// The night's local daily summaries, newest first.
std::vector<LocalSummaryRow> localSummaryRows(IDatabase& db, const std::string& device_id,
                                              const std::string& sleep_day);
/// The fact block the night's newest local summary was written from; "" when
/// it has none (or one written before the column existed).
std::string storedFactBlock(IDatabase& db, const std::string& device_id, const std::string& sleep_day);

}  // namespace hms_cpap::model
