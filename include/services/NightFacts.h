#pragma once
//
// SDD-072: the AI is given facts that add up, and says only those.
//
// Three places used to tell a model about a night, each built by hand: the
// daily summary text, the weekly and monthly text, and the chat agent's tools
// with their own SQL. They drifted (issue #78: a night of 3 hypopneas and 3
// unclassified apneas reached the model as "Total events: 6 (obstructive=0,
// central=0, hypopnea=3, RERA=0)" and came back as six hypopneas).
//
// D1: one NightFacts per night, built in code from the night's stored metrics
// (getNightlyMetrics / getMetricsForDateRange, one query per engine), its own
// STR row and its own oximetry. Every AI path renders from it:
//   renderNight()  the daily summary text
//   renderRange()  the weekly and monthly text
//   toJson()       the chat agent's tool results
// The events behind the index are named by type and their parts equal the
// night's total_events. An Ungraded index (SDD-024) carries no severity words
// and no per-type zeros. A figure the night does not have is absent, never 0.
//
// D2: the rules ride inside the rendered facts (rules()), so they also reach a
// user's own LLM_PROMPT_FILE and the agent's tool results.
//
#include "database/IDatabase.h"
#include "parsers/CpapdashBridge.h"

#include <nlohmann/json.hpp>

#include <optional>
#include <string>
#include <vector>

namespace hms_cpap {

/// The night's own O2 Ring figures (IDatabase::OxiSummary, when found).
struct NightOximetryFacts {
    double avg_spo2 = 0, min_spo2 = 0, spo2_baseline = 0;
    double odi_3pct = 0, time_below_90 = 0, time_below_88 = 0;
    double avg_hr = 0;
    int min_hr = 0, max_hr = 0;
};

/// The night's own STR row: the machine's official figures for the day.
struct NightStrFacts {
    double ahi = 0;
    int mask_on_off_pairs = 0;
    double leak_95 = 0;
    double mask_press_95 = 0;
    std::optional<double> asv_epap, asv_min_ps, asv_max_ps;
    std::optional<double> tgt_ipap_50, tgt_epap_50, tgt_vent_50;
};

struct NightFacts {
    std::string sleep_day;   ///< YYYY-MM-DD, empty when the caller did not name it

    /// SDD-024: false for a machine that detects no hypopneas. Its index is not
    /// an AHI and its events have no per-type breakdown.
    bool graded = true;
    double index = 0;

    // The events behind the index, by type (graded nights only).
    int obstructive = 0, central = 0, clear_airway = 0, unclassified = 0, hypopnea = 0;
    // The events outside it: RERAs, and every other recorded type
    // (Cheyne-Stokes, flow limitation, large leak, snore, unknown annotations).
    int reras = 0, other = 0;
    int total = 0;   ///< total_events: inIndex() + reras + other on a graded night

    int inIndex() const { return obstructive + central + clear_airway + unclassified + hypopnea; }
    int outsideIndex() const { return reras + other; }

    std::optional<double> usage_hours, usage_percent;
    std::optional<double> avg_event_duration, max_event_duration;
    std::optional<double> therapy_pressure;   ///< delivered by the machine (issue #56)
    std::optional<double> pressure_avg, pressure_min, pressure_max, pressure_p95;
    std::optional<double> leak_avg, leak_max, leak_p95, leak_p50;
    std::optional<double> mask_pressure, epr_pressure, snore, target_ventilation;
    std::optional<double> respiratory_rate, tidal_volume, minute_ventilation, flow_limitation;
    std::optional<std::string> therapy_mode;   ///< named through the family (SDD-030)

    std::optional<NightStrFacts> str;
    std::optional<NightOximetryFacts> oximetry;

    /// The one construction every path uses.
    static NightFacts from(const SessionMetrics& m,
                           const STRDailyRecord* str = nullptr,
                           const std::optional<NightOximetryFacts>& oximetry = std::nullopt);
};

/// A range's O2 Ring figures (IDatabase::OxiRangeSummary, when found).
struct RangeOximetryFacts {
    int nights = 0;
    double avg_spo2 = 0, min_spo2 = 0, avg_odi = 0, avg_below_90 = 0, avg_hr = 0;
};

namespace night_facts {

/// D2: how to report each event type. Issue #78.
extern const char* const kEventLabelRule;
/// D2, SDD-024: what an Ungraded night's index and events may be called.
extern const char* const kUngradedRule;
/// D2/D4: every number stated is one given; a missing figure is said missing.
extern const char* const kNoGapRule;

/// The rules for [nights]: the event rule when any night is graded, the
/// SDD-024 rule when any is not, and the no-gap rule always.
std::vector<std::string> rules(const std::vector<NightFacts>& nights);

/// The daily summary's facts block (the {metrics} of the prompt).
std::string renderNight(const NightFacts& night);

/// The weekly / monthly facts block. [label] is "Weekly" or "Monthly".
std::string renderRange(const std::vector<NightFacts>& nights, const std::string& label,
                        const std::optional<RangeOximetryFacts>& oximetry = std::nullopt);

/// The agent's tool result for one night. Numbers carry the two places the
/// text renderings show, so both say the same figure.
nlohmann::json toJson(const NightFacts& night);

// -- From the database (through IDatabase, so on every engine) ---------------

/// The night's O2 Ring summary, or none. [sleep_day] is YYYY-MM-DD.
std::optional<NightOximetryFacts> oximetryForNight(IDatabase& db, const std::string& sleep_day);

/// The range's O2 Ring summary, or none. Days are YYYY-MM-DD.
std::optional<RangeOximetryFacts> oximetryForRange(IDatabase& db, const std::string& start_day,
                                                   const std::string& end_day);

/// Every closed night with sleep_day in [start_day, end_day] (YYYY-MM-DD),
/// oldest first, each with its own oximetry. No STR row: the STR is read off
/// the card by the summary paths; the agent reads the daily rows itself.
std::vector<NightFacts> loadRange(IDatabase& db, const std::string& device_id,
                                  const std::string& start_day, const std::string& end_day);

/// One night by its sleep day, or none.
std::optional<NightFacts> loadNight(IDatabase& db, const std::string& device_id,
                                    const std::string& sleep_day);

/// YYYY-MM-DD of [now] - [days] days on the local clock (the sleep-day rule:
/// days=0 is the night that started before now - 12 h).
std::string sleepDayDaysAgo(int days);

/// True for a well-formed YYYY-MM-DD.
bool isIsoDay(const std::string& day);

}  // namespace night_facts
}  // namespace hms_cpap
