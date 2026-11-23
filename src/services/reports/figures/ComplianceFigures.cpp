#include "services/reports/figures/ComplianceFigures.h"

#include <cmath>
#include <map>

namespace hms_cpap::compliance {

using clinician::dateOf;
using clinician::dayNumber;
using clinician::Night;

namespace {

/// Minutes per day number inside [s, e]; a date that repeats keeps its
/// largest figure, so a duplicate row can never hide a night.
std::map<long, double> minutesByDay(const std::vector<Night>& nights, long s, long e) {
    std::map<long, double> out;
    for (const auto& n : nights) {
        const auto d = dayNumber(n.date);
        if (!d || *d < s || *d > e) continue;
        auto& m = out[*d];
        if (n.minutes > m) m = n.minutes;
    }
    return out;
}

bool meetsHours(double minutes, double hours) {
    // 4.0 h is 240 min exactly; the epsilon covers an hours value whose
    // sixtyfold is not exact in binary (0.1 h), so a night at the line counts.
    return minutes > 0 && minutes + 1e-6 >= hours * 60.0;
}

}  // namespace

bool validRule(double hours, int share) {
    return std::isfinite(hours) && hours >= kMinHours && hours <= kMaxHours &&
           share >= kMinShare && share <= kMaxShare;
}

Judgement judge(const std::vector<Night>& nights, const std::string& start,
                const std::string& end, const Rule& rule) {
    Judgement j;
    const auto s = dayNumber(start);
    const auto e = dayNumber(end);
    if (!s || !e || *e < *s) return j;
    j.nights = static_cast<int>(*e - *s + 1);
    for (const auto& [day, minutes] : minutesByDay(nights, *s, *e)) {
        if (minutes <= 0) continue;
        ++j.nights_used;
        if (meetsHours(minutes, rule.hours)) ++j.nights_met;
    }
    // Decided in whole nights so the boundary is exact: 21 of 30 at 70 % is
    // 2100 >= 2100, met; the percentage printed is the same division.
    j.nights_needed = (rule.share * j.nights + 99) / 100;
    j.pct    = 100.0 * j.nights_met / j.nights;
    j.meets  = j.nights_met * 100 >= rule.share * j.nights;
    j.margin = j.nights_met - j.nights_needed;
    return j;
}

Json::Value verdictOf(const Judgement& j, const Rule& rule) {
    Json::Value v(Json::objectValue);
    v["meets"]       = j.meets;
    v["hours"]       = rule.hours;
    v["share"]       = rule.share;
    v["nights"]      = j.nights;
    v["nights_used"] = j.nights_used;
    v["nights_met"]  = j.nights_met;
    v["pct"]         = std::round(j.pct * 100.0) / 100.0;
    return v;
}

std::vector<std::pair<std::string, double>> calendarMinutes(const std::vector<Night>& nights,
                                                            const std::string& start,
                                                            const std::string& end) {
    std::vector<std::pair<std::string, double>> out;
    const auto s = dayNumber(start);
    const auto e = dayNumber(end);
    if (!s || !e || *e < *s) return out;
    const auto by = minutesByDay(nights, *s, *e);
    out.reserve(static_cast<std::size_t>(*e - *s + 1));
    for (long d = *s; d <= *e; ++d) {
        const auto it = by.find(d);
        out.emplace_back(dateOf(d), it == by.end() ? 0.0 : it->second);
    }
    return out;
}

std::vector<std::pair<std::string, double>> rollingShare(const std::vector<Night>& nights,
                                                         const std::string& start,
                                                         const std::string& end, double hours,
                                                         int window) {
    std::vector<std::pair<std::string, double>> out;
    const auto s = dayNumber(start);
    const auto e = dayNumber(end);
    if (!s || !e || window <= 0 || *e - *s + 1 < window) return out;
    const auto by = minutesByDay(nights, *s, *e);
    for (long last = *s + window - 1; last <= *e; ++last) {
        int met = 0;
        for (long d = last - window + 1; d <= last; ++d) {
            const auto it = by.find(d);
            if (it != by.end() && meetsHours(it->second, hours)) ++met;
        }
        out.emplace_back(dateOf(last), 100.0 * met / window);
    }
    return out;
}

int weekdayOf(long day) {
    // 1970-01-01 was a Thursday: three days past that week's Monday.
    const long w = (day + 3) % 7;
    return static_cast<int>(w < 0 ? w + 7 : w);
}

}  // namespace hms_cpap::compliance
