#pragma once
// SDD-082: the clinician report's arithmetic, pure.
//
// Every figure here is computed from the per-night rows QueryService answers
// (getClinicalNights) plus the calendar. Nothing here asks anything or draws
// anything, so every rule is unit tested on its own. The same arithmetic as
// CpapDash's clinician report, figure for figure (SDD-082 section 3.2).
//
// Dates are "YYYY-MM-DD" strings throughout; a day is a civil day, counted
// with days-from-civil arithmetic, never through a clock or a time zone.

#include <json/json.h>

#include <optional>
#include <string>
#include <vector>

namespace hms_cpap::clinician {

// ---- Calendar ----------------------------------------------------------------

/// Days since 1970-01-01 for "YYYY-MM-DD" (also accepts a longer string that
/// starts with one), or nullopt.
std::optional<long> dayNumber(const std::string& ymd);
/// "YYYY-MM-DD" for a day number.
std::string dateOf(long day);
/// The same day `months` calendar months earlier (or later, if negative),
/// clamped to the last day of the target month. "2026-03-31" - 1 = "2026-02-28".
std::string addMonths(const std::string& ymd, int months);

// ---- One night ------------------------------------------------------------------

/// A daily row as the report reads it. A figure the row does not carry
/// (absent or null) is nullopt: absent is not zero.
struct Night {
    std::string date;               // "YYYY-MM-DD"
    double minutes{0};              // duration_minutes, ours once sessions exist (SDD-026)
    std::optional<double> ahi, oai, hi, uai, cai, rin;
    std::optional<double> csr_minutes;
    std::optional<double> leak_50, leak_95, leak_max;
    std::optional<double> press_50, press_95, press_max, press_min;
    std::optional<double> epap_min, epap_avg, epap_max;
    std::optional<double> flow_lim_95;
    std::optional<double> large_leak_minutes;     // the machine's own flags
    std::optional<double> leak_over_24_minutes;   // minutes over 24 L/min
    int family{0};                  // 0 unknown 1 CPAP 2 AutoSet 3 bi-level 4 ASV
    std::optional<int> mode;
    std::optional<double> epr_level, pressure_setting;
    std::optional<double> apap_min, apap_max;
    std::optional<double> bl_min_epap, bl_max_ipap, bl_ps, bl_ipap, bl_epap;
    bool ungraded{false};           // SDD-024: the index is not an AHI
};

/// One daily row, or nullopt when it has no usable date.
std::optional<Night> nightFrom(const Json::Value& row);
/// Every row of a daily answer, sorted by date, one per date (the last one
/// wins if a date repeats).
std::vector<Night> nightsFrom(const Json::Value& rows);

// ---- One window -----------------------------------------------------------------

/// A span of civil days, both ends included.
struct Window {
    std::string label;   // "Last night", "Last 7 days", a month name...
    std::string start;
    std::string end;
};

/// The figure set for one window. A figure no night in the window carries is
/// nullopt and prints as a dash.
struct WindowFigures {
    int total_days{0};
    int used_days{0};
    int days_4h{0};          // used days >= the threshold
    int days_under_4h{0};    // used days < the threshold (and > 0)
    double pct_total_4h{0};  // of ALL days in the window
    double pct_used_4h{0};   // of the used days
    double avg_hours{0};     // over used days
    double median_hours{0};  // over used days
    // Indices are weighted by each night's hours: a 7 h night counts seven
    // times a 1 h night, which is what events / hours over the window means.
    std::optional<double> ahi, ahi_median, oai, hi, uai, cai, rin;
    std::optional<double> flow_lim_95;
    std::optional<double> csr_pct;           // csr minutes / therapy minutes
    std::optional<double> leak_median, leak_95;
    // % of therapy time with a large leak: the machine's flags on nights that
    // carry them, the 24 L/min minutes on the others.
    std::optional<double> large_leak_pct;
    bool large_leak_uses_flags{false};       // any night used the machine's flags
    bool large_leak_uses_line{false};        // any night used the 24 L/min line
    std::optional<double> press_median, press_95, press_max, press_min;
    std::optional<double> epap_avg, epap_min, epap_max;
};

/// The figures of `w` over `nights` (any order; only those inside count).
/// `threshold_minutes` is the line the `days_4h` / `pct_*_4h` figures count
/// against: the clinician report passes 240 (its "4 h"); the compliance
/// report passes its own rule's hours.
WindowFigures figuresFor(const std::vector<Night>& nights, const Window& w,
                         double threshold_minutes = 240.0);

/// Last night, 7 days, 30 days, 6 months, 1 year, counted back from `end`,
/// then the user's range [start, end]. "Last night" is the newest night on or
/// before `end` that has use (a 4 minute stub counts). A fixed window that
/// begins before `first_night` is dropped, and so is the user's range when it
/// is exactly one of the fixed ones.
std::vector<Window> standardWindows(const std::vector<Night>& nights, const std::string& start,
                                    const std::string& end, const std::string& first_night);

/// The monthly page: the 12 calendar months ending with `end`'s month, newest
/// first, each clipped to `end`; a month wholly before `first_night` is
/// dropped. Labels "YYYY-MM".
std::vector<Window> monthlyWindows(const std::string& end, const std::string& first_night);

// ---- Settings periods ----------------------------------------------------------

/// One stretch of nights with the same settings.
struct SettingsPeriod {
    std::string first, last;
    int nights{0};
    std::string mode_label;    // "APAP", "CPAP", "VAuto"... or "" when unknown
    std::string pressure;      // "4.0 - 20.0 cmH2O", "10.0 cmH2O", ...
    std::string relief;        // "EPR 3", "EPR off", ""
    std::optional<double> ahi, flow_lim_95, leak_95;
    double avg_hours{0};
};

/// The therapy name, only when family and mode agree; anything else is "".
std::string therapyLabel(int family, std::optional<int> mode);

/// Consecutive nights (by date) grouped by identical settings, oldest first.
std::vector<SettingsPeriod> settingsPeriods(const std::vector<Night>& nights);

// ---- CMS adherence ----------------------------------------------------------------

struct CmsAdherence {
    bool known{false};          // there is a first night to count from
    std::string first_night;    // day 1: the first night on record
    std::string day_90;         // day 90
    bool complete{false};       // day 90 is on or before `today`
    std::string best_start, best_end;   // the best 30-day window inside the 90
    int best_nights_4h{0};              // nights >= 4 h in it
    double best_pct{0};                 // of 30
    bool met{false};                    // >= 21 of 30 (70%)
};

/// "4 h or more on 70% of nights in any 30 consecutive days within the first
/// 90 days." `nights` must cover [first_night, first_night + 89]; a day with
/// no row is a 0 h day. `today` bounds the windows that can exist yet.
CmsAdherence cmsAdherence(const std::vector<Night>& nights, const std::string& first_night,
                          const std::string& today);

// ---- One night's page -----------------------------------------------------------

/// The night with the highest AHI in [start, end], or nullopt. A tie goes
/// to the later night.
std::optional<Night> worstNight(const std::vector<Night>& nights, const std::string& start,
                                const std::string& end);
/// The newest night on or before `end` (any usage counts).
std::optional<Night> lastNight(const std::vector<Night>& nights, const std::string& end);

/// An event as getSessionEvents answers it, its time in epoch seconds.
struct Event {
    std::string type;
    long t{0};
    double duration{0};
};

/// "2026-09-07 03:38:17+02" (or with ":00" minutes in the offset, a 'T',
/// fractional seconds, or no offset at all) to epoch seconds, or nullopt. A
/// stamp with no offset is read as it stands: the machine's clock.
std::optional<long> epochOf(const std::string& timestamptz);

std::vector<Event> eventsFrom(const Json::Value& rows);

/// The event-flag rows of the night page, top to bottom, and which stored
/// event types each one draws. Types not listed are left out of the strip.
struct FlagRow {
    std::string label;                  // "OA", "CA", "H", ...
    std::vector<std::string> types;     // stored event_type values
};
const std::vector<FlagRow>& flagRows();

/// Counts per flag row, in flagRows() order.
std::vector<int> flagCounts(const std::vector<Event>& events);

/// The running AHI through the night: at each event, the apnea and hypopnea
/// events so far divided by the hours since `t0`. Points (epoch, value).
std::vector<std::pair<long, double>> runningAhi(const std::vector<Event>& events, long t0);

/// Minutes at each pressure, in 0.5 cmH2O bins, from a per-minute series
/// (zeros, a machine that is off, left out). Pairs (bin lower bound, minutes).
std::vector<std::pair<double, double>> timeAtPressure(const std::vector<double>& per_minute);

/// The 95th percentile the way PostgreSQL's percentile_cont(0.95) computes
/// it: linear between the two ranks around (n - 1) x 0.95. nullopt when empty.
std::optional<double> percentileCont(std::vector<double> values, double p);

}  // namespace hms_cpap::clinician
