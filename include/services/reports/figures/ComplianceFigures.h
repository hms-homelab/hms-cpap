#pragma once
// SDD-082: the compliance report's arithmetic, pure (CpapDash's, unchanged).
//
// One rule, at least H hours on at least P percent of the nights in a range,
// and one verdict. Every calendar night of the range counts: a night with no
// row is a night with no use, and a night with no use is a miss. Nothing here
// asks anything or draws anything.

#include "services/reports/figures/ClinicianFigures.h"

#include <json/json.h>

#include <string>
#include <utility>
#include <vector>

namespace hms_cpap::compliance {

/// The rule a compliance report is judged against.
struct Rule {
    double hours{4.0};
    int    share{70};
};

inline constexpr double kMinHours = 0.5;
inline constexpr double kMaxHours = 24.0;
inline constexpr int    kMinShare = 1;
inline constexpr int    kMaxShare = 100;
inline constexpr int    kDefaultShare = 70;

/// Whether a rule is one a report may be asked for.
bool validRule(double hours, int share);

/// The verdict over [start, end] under a rule.
struct Judgement {
    int    nights{0};          // calendar nights in the range
    int    nights_used{0};     // nights with any use
    int    nights_met{0};      // nights at or over the rule's hours
    int    nights_needed{0};   // the fewest nights_met that meet the share
    double pct{0};             // nights_met * 100 / nights
    bool   meets{false};       // nights_met * 100 >= share * nights (exact)
    int    margin{0};          // nights_met - nights_needed: to spare, or short
};

/// Judge `nights` (any order, those outside the range ignored) over every
/// calendar night of [start, end]. A backwards or malformed range is a
/// Judgement with no nights, which does not meet.
Judgement judge(const std::vector<clinician::Night>& nights, const std::string& start,
                const std::string& end, const Rule& rule);

/// The verdict as the ready message carries it:
/// {meets, hours, share, nights, nights_used, nights_met, pct}.
Json::Value verdictOf(const Judgement& j, const Rule& rule);

/// Every calendar night of [start, end] in order with its minutes of use, 0
/// for a night with no row. Empty for a bad range.
std::vector<std::pair<std::string, double>> calendarMinutes(
    const std::vector<clinician::Night>& nights, const std::string& start, const std::string& end);

/// For every window of `window` consecutive nights inside the range, the
/// share (percent) of its nights at or over `hours`, keyed by the window's
/// last night. Empty when the range is shorter than the window.
std::vector<std::pair<std::string, double>> rollingShare(
    const std::vector<clinician::Night>& nights, const std::string& start, const std::string& end,
    double hours, int window = 30);

/// 0 for Monday through 6 for Sunday, for a day number (clinician::dayNumber).
int weekdayOf(long day);

}  // namespace hms_cpap::compliance
