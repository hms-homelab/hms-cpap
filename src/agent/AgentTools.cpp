#include "agent/AgentTools.h"

#include "database/AgentReads.h"
#include "services/BilevelSensors.h"
#include "services/NightFacts.h"

#include <algorithm>
#include <cmath>
#include <stdexcept>

namespace hms_cpap {

namespace {

using json = nlohmann::json;

double roundTo(double v, int places) {
    const double f = std::pow(10.0, places);
    return std::round(v * f) / f;
}

std::string dayArg(const json& args, const char* key) {
    if (!args.contains(key) || !args.at(key).is_string())
        throw std::invalid_argument(std::string("missing ") + key + " (YYYY-MM-DD)");
    const std::string d = args.at(key).get<std::string>();
    if (!night_facts::isIsoDay(d))
        throw std::invalid_argument(std::string(key) + " must be YYYY-MM-DD, got " + d);
    return d;
}

int intArg(const json& args, const char* key, int fallback, int lo, int hi) {
    int v = fallback;
    if (args.contains(key) && args.at(key).is_number()) v = args.at(key).get<int>();
    return (std::max)(lo, (std::min)(hi, v));
}

json rulesFor(const std::vector<NightFacts>& nights) {
    return night_facts::rules(nights);
}

json rulesForDays(const std::vector<agent_reads::DayRow>& rows) {
    std::vector<NightFacts> marks;
    for (const auto& r : rows) {
        NightFacts f;
        f.graded = r.graded;
        marks.push_back(f);
    }
    return rulesFor(marks);
}

void put(json& j, const char* key, const std::optional<double>& v, int places = 2) {
    if (v) j[key] = roundTo(*v, places);
}

constexpr const char* kUngradedAhi = "not available: this machine does not detect hypopneas";

/// A daily row as the agent gets it. SDD-024: an Ungraded day carries no
/// index and no per-type zeros.
json dayJson(const agent_reads::DayRow& r) {
    json j;
    j["date"] = r.record_date;
    put(j, "duration_minutes", r.duration_minutes, 1);
    if (r.graded) {
        put(j, "ahi", r.ahi);
        put(j, "hypopnea_index", r.hi);
        put(j, "apnea_index", r.ai);
        put(j, "obstructive_apnea_index", r.oai);
        put(j, "central_apnea_index", r.cai);
        put(j, "unclassified_apnea_index", r.uai);
        put(j, "rera_index", r.rin);
    } else {
        j["ahi"] = kUngradedAhi;
    }
    put(j, "leak_median_l_min", r.leak_50);
    put(j, "leak_p95_l_min", r.leak_95);
    put(j, "leak_max_l_min", r.leak_max);
    put(j, "mask_pressure_median_cmh2o", r.mask_press_50);
    put(j, "mask_pressure_p95_cmh2o", r.mask_press_95);
    put(j, "mask_pressure_max_cmh2o", r.mask_press_max);
    put(j, "therapy_pressure_median_cmh2o", r.therapy_press_50);
    put(j, "therapy_pressure_p95_cmh2o", r.therapy_press_95);
    put(j, "therapy_pressure_max_cmh2o", r.therapy_press_max);
    // SDD-019: a card without an oximeter writes 0 or -1; neither is a reading.
    if (r.spo2_50 && strSpo2Present(*r.spo2_50)) put(j, "spo2_median_pct", r.spo2_50);
    if (r.spo2_95 && strSpo2Present(*r.spo2_95)) put(j, "spo2_p95_pct", r.spo2_95);
    put(j, "respiratory_rate_median", r.resp_rate_50);
    put(j, "tidal_volume_median", r.tid_vol_50);
    put(j, "minute_ventilation_median", r.min_vent_50);
    if (r.mode) j["mode_code"] = *r.mode;
    put(j, "epr_level", r.epr_level);
    put(j, "pressure_setting_cmh2o", r.pressure_setting);
    return j;
}

/// Mean of the present values, rounded, or null.
template <typename Pick>
json meanOf(const std::vector<agent_reads::DayRow>& rows, Pick pick, int places) {
    double sum = 0;
    int n = 0;
    for (const auto& r : rows)
        if (const auto v = pick(r)) {
            sum += *v;
            ++n;
        }
    return n ? json(roundTo(sum / n, places)) : json(nullptr);
}

/// The figures compare_periods and get_statistics share.
json aggregate(const std::vector<agent_reads::DayRow>& rows, bool with_spread) {
    json j;
    const int nights = static_cast<int>(rows.size());
    j["nights"] = nights;

    std::vector<double> ahi;
    for (const auto& r : rows)
        if (r.graded && r.ahi) ahi.push_back(*r.ahi);
    if (!ahi.empty()) {
        double sum = 0;
        for (double v : ahi) sum += v;
        const double mean = sum / ahi.size();
        j["avg_ahi"] = roundTo(mean, 2);
        if (static_cast<int>(ahi.size()) < nights) j["nights_with_ahi"] = ahi.size();
        if (with_spread) {
            j["min_ahi"] = roundTo(*std::min_element(ahi.begin(), ahi.end()), 2);
            j["max_ahi"] = roundTo(*std::max_element(ahi.begin(), ahi.end()), 2);
            if (ahi.size() > 1) {
                double ss = 0;
                for (double v : ahi) ss += (v - mean) * (v - mean);
                j["stddev_ahi"] = roundTo(std::sqrt(ss / (ahi.size() - 1)), 2);
            }
        }
    } else {
        j["avg_ahi"] = nights ? json("not available: no night in this period has an AHI")
                              : json(nullptr);
    }

    j["avg_duration_min"] = meanOf(rows, [](const auto& r) { return r.duration_minutes; }, 1);
    if (with_spread) {
        std::optional<double> lo, hi;
        double total = 0;
        bool any = false;
        for (const auto& r : rows)
            if (r.duration_minutes) {
                lo = lo ? (std::min)(*lo, *r.duration_minutes) : *r.duration_minutes;
                hi = hi ? (std::max)(*hi, *r.duration_minutes) : *r.duration_minutes;
                total += *r.duration_minutes;
                any = true;
            }
        if (lo) j["min_duration_min"] = roundTo(*lo, 1);
        if (hi) j["max_duration_min"] = roundTo(*hi, 1);
        if (any) j["total_therapy_minutes"] = roundTo(total, 1);
    }
    j["avg_leak_95"] = meanOf(rows, [](const auto& r) { return r.leak_95; }, 1);
    // Issue #56: the machine's pressure, the mask's only where a night has none.
    j["avg_pressure_95"] = meanOf(rows, [](const auto& r) {
        return r.therapy_press_95 ? r.therapy_press_95 : r.mask_press_95;
    }, 1);
    j["avg_spo2"] = meanOf(rows, [](const auto& r) -> std::optional<double> {
        if (r.spo2_50 && strSpo2Present(*r.spo2_50)) return r.spo2_50;
        return std::nullopt;
    }, 1);
    j["avg_resp_rate"] = meanOf(rows, [](const auto& r) { return r.resp_rate_50; }, 1);
    if (nights) {
        int compliant = 0;
        for (const auto& r : rows)
            if (r.duration_minutes && *r.duration_minutes >= 240) ++compliant;
        j["compliance_pct"] = roundTo(compliant * 100.0 / nights, 1);
    } else {
        j["compliance_pct"] = nullptr;
    }
    return j;
}

}  // namespace

AgentTools::AgentTools(const std::string& device_id)
    : device_id_(device_id) {
    buildDefinitions();
}

const std::vector<hms::ToolDefinition>& AgentTools::definitions() const {
    return definitions_;
}

std::string AgentTools::execute(const std::string& name, const nlohmann::json& arguments,
                                IDatabase& db) {
    try {
        const json& args = arguments.is_object() ? arguments : json::object();
        if (name == "get_recent_sessions")  return getRecentSessions(args, db).dump();
        if (name == "get_session_details")  return getSessionDetails(args, db).dump();
        if (name == "get_daily_summary")    return getDailySummary(args, db).dump();
        if (name == "get_trend_data")       return getTrendData(args, db).dump();
        if (name == "compare_periods")      return comparePeriods(args, db).dump();
        if (name == "get_vitals")           return getVitals(args, db).dump();
        if (name == "get_statistics")       return getStatistics(args, db).dump();
        return json{{"error", "Unknown tool: " + name}}.dump();
    } catch (const std::exception& e) {
        return json{{"error", e.what()}}.dump();
    }
}

void AgentTools::buildDefinitions() {
    definitions_ = {
        {
            "get_recent_sessions",
            "Get recent CPAP therapy nights with their metrics (AHI and the events behind it "
            "by type, leak, pressure, usage), newest first. Says when the list was capped.",
            {
                {"type", "object"},
                {"properties", {
                    {"days", {{"type", "integer"}, {"description", "Number of days to look back (default 7)"}}},
                    {"limit", {{"type", "integer"}, {"description", "Maximum nights to return (default 10, at most 31)"}}}
                }},
                {"required", nlohmann::json::array()}
            }
        },
        {
            "get_session_details",
            "Get detailed data for a specific night's therapy session including events and metrics.",
            {
                {"type", "object"},
                {"properties", {
                    {"date", {{"type", "string"}, {"description", "Date in YYYY-MM-DD format"}}}
                }},
                {"required", {"date"}}
            }
        },
        {
            "get_daily_summary",
            "Get daily therapy summaries from the ResMed STR file (official device stats).",
            {
                {"type", "object"},
                {"properties", {
                    {"start_date", {{"type", "string"}, {"description", "Start date YYYY-MM-DD"}}},
                    {"end_date", {{"type", "string"}, {"description", "End date YYYY-MM-DD"}}}
                }},
                {"required", {"start_date", "end_date"}}
            }
        },
        {
            "get_trend_data",
            "Get trend data for a specific metric over time (for identifying patterns).",
            {
                {"type", "object"},
                {"properties", {
                    {"metric", {{"type", "string"}, {"description", "Metric name: ahi, pressure, leak, spo2, hr, duration"}, {"enum", {"ahi", "pressure", "leak", "spo2", "hr", "duration"}}}},
                    {"days", {{"type", "integer"}, {"description", "Number of days to look back (default 30)"}}}
                }},
                {"required", {"metric"}}
            }
        },
        {
            "compare_periods",
            "Compare therapy metrics between two date ranges (e.g., this week vs last week).",
            {
                {"type", "object"},
                {"properties", {
                    {"period1_start", {{"type", "string"}, {"description", "Period 1 start date YYYY-MM-DD"}}},
                    {"period1_end", {{"type", "string"}, {"description", "Period 1 end date YYYY-MM-DD"}}},
                    {"period2_start", {{"type", "string"}, {"description", "Period 2 start date YYYY-MM-DD"}}},
                    {"period2_end", {{"type", "string"}, {"description", "Period 2 end date YYYY-MM-DD"}}}
                }},
                {"required", {"period1_start", "period1_end", "period2_start", "period2_end"}}
            }
        },
        {
            "get_vitals",
            "Get SpO2 and heart rate data for a specific night (1-minute aggregated).",
            {
                {"type", "object"},
                {"properties", {
                    {"date", {{"type", "string"}, {"description", "Date in YYYY-MM-DD format"}}}
                }},
                {"required", {"date"}}
            }
        },
        {
            "get_statistics",
            "Get aggregate statistics and compliance rate over a date range.",
            {
                {"type", "object"},
                {"properties", {
                    {"start_date", {{"type", "string"}, {"description", "Start date YYYY-MM-DD"}}},
                    {"end_date", {{"type", "string"}, {"description", "End date YYYY-MM-DD"}}}
                }},
                {"required", {"start_date", "end_date"}}
            }
        }
    };
}

// ── Tool implementations ────────────────────────────────────────────────────

json AgentTools::getRecentSessions(const json& args, IDatabase& db) {
    const int days = intArg(args, "days", 7, 1, 3660);
    const int limit = intArg(args, "limit", 10, 1, kMaxNights);

    // "The last N days": from N days ago to the night that ended last.
    auto nights = night_facts::loadRange(db, device_id_, night_facts::sleepDayDaysAgo(days),
                                         night_facts::sleepDayDaysAgo(0));
    std::reverse(nights.begin(), nights.end());   // newest first
    const int available = static_cast<int>(nights.size());
    if (available > limit) nights.resize(limit);

    json out;
    out["nights"] = json::array();
    for (const auto& n : nights) out["nights"].push_back(night_facts::toJson(n));
    out["returned"] = nights.size();
    out["available"] = available;
    // D4: a list that was cut says so, so the model does not read it as all.
    out["capped"] = available > limit;
    if (available > limit)
        out["capped_note"] = "only the newest " + std::to_string(limit) + " of " +
                             std::to_string(available) + " nights are listed";
    out["rules"] = rulesFor(nights);
    return out;
}

json AgentTools::getSessionDetails(const json& args, IDatabase& db) {
    const std::string date = dayArg(args, "date");
    const auto night = night_facts::loadNight(db, device_id_, date);
    if (!night) {
        return json{{"date", date},
                    {"found", false},
                    {"note", "no therapy night is stored for this date"},
                    {"rules", rulesFor({})}};
    }
    json out = night_facts::toJson(*night);
    out["found"] = true;

    const auto events = agent_reads::nightEvents(db, device_id_, date);
    json list = json::array();
    for (size_t i = 0; i < events.size() && i < static_cast<size_t>(kMaxEvents); ++i) {
        json e;
        e["type"] = events[i].type;
        e["time"] = events[i].timestamp;
        put(e, "duration_s", events[i].duration_seconds, 1);
        if (!events[i].details.empty()) e["details"] = events[i].details;
        list.push_back(std::move(e));
    }
    out["event_list"] = std::move(list);
    out["event_list_returned"] = (std::min)(events.size(), static_cast<size_t>(kMaxEvents));
    out["event_list_stored"] = events.size();
    out["event_list_capped"] = events.size() > static_cast<size_t>(kMaxEvents);
    out["rules"] = rulesFor({*night});
    return out;
}

json AgentTools::getDailySummary(const json& args, IDatabase& db) {
    const auto rows = agent_reads::dailyRows(db, device_id_, dayArg(args, "start_date"),
                                             dayArg(args, "end_date"));
    json out;
    out["days"] = json::array();
    for (const auto& r : rows) out["days"].push_back(dayJson(r));
    out["returned"] = rows.size();
    out["rules"] = rulesForDays(rows);
    return out;
}

json AgentTools::getTrendData(const json& args, IDatabase& db) {
    if (!args.contains("metric") || !args.at("metric").is_string())
        throw std::invalid_argument("missing metric");
    const std::string metric = args.at("metric").get<std::string>();
    static const std::vector<std::string> kMetrics = {"ahi", "pressure", "leak", "spo2", "hr", "duration"};
    if (std::find(kMetrics.begin(), kMetrics.end(), metric) == kMetrics.end())
        return json{{"error", "Unknown metric. Use: ahi, pressure, leak, spo2, hr, duration"}};
    const int days = intArg(args, "days", 30, 1, 3660);

    const auto rows =
        agent_reads::dailyRows(db, device_id_, night_facts::sleepDayDaysAgo(days),
                               night_facts::sleepDayDaysAgo(0));
    json points = json::array();
    for (const auto& r : rows) {
        json p;
        p["date"] = r.record_date;
        if (metric == "ahi") {
            if (r.graded) {
                put(p, "ahi", r.ahi);
                put(p, "hypopnea_index", r.hi);
                put(p, "apnea_index", r.ai);
                put(p, "obstructive_apnea_index", r.oai);
                put(p, "central_apnea_index", r.cai);
                put(p, "unclassified_apnea_index", r.uai);
            } else {
                p["ahi"] = kUngradedAhi;
            }
        } else if (metric == "pressure") {
            put(p, "therapy_pressure_median_cmh2o", r.therapy_press_50);
            put(p, "therapy_pressure_p95_cmh2o", r.therapy_press_95);
            put(p, "therapy_pressure_max_cmh2o", r.therapy_press_max);
            put(p, "mask_pressure_median_cmh2o", r.mask_press_50);
            put(p, "mask_pressure_p95_cmh2o", r.mask_press_95);
            put(p, "mask_pressure_max_cmh2o", r.mask_press_max);
            put(p, "pressure_setting_cmh2o", r.pressure_setting);
        } else if (metric == "leak") {
            put(p, "leak_median_l_min", r.leak_50);
            put(p, "leak_p95_l_min", r.leak_95);
            put(p, "leak_max_l_min", r.leak_max);
        } else if (metric == "spo2") {
            if (r.spo2_50 && strSpo2Present(*r.spo2_50)) put(p, "spo2_median_pct", r.spo2_50);
            if (r.spo2_95 && strSpo2Present(*r.spo2_95)) put(p, "spo2_p95_pct", r.spo2_95);
        } else if (metric == "hr") {
            // The daily row holds no heart rate; this was always its
            // respiratory rate, and is now labelled as such.
            put(p, "respiratory_rate_median", r.resp_rate_50);
        } else {
            put(p, "duration_minutes", r.duration_minutes, 1);
        }
        points.push_back(std::move(p));
    }
    json out;
    out["metric"] = metric;
    out["points"] = std::move(points);
    if (metric == "hr")
        out["note"] = "the daily summary holds no heart rate; respiratory rate is given. "
                      "Use get_vitals for a night's heart rate.";
    out["rules"] = rulesForDays(rows);
    return out;
}

json AgentTools::comparePeriods(const json& args, IDatabase& db) {
    const std::string p1s = dayArg(args, "period1_start"), p1e = dayArg(args, "period1_end");
    const std::string p2s = dayArg(args, "period2_start"), p2e = dayArg(args, "period2_end");
    const auto r1 = agent_reads::dailyRows(db, device_id_, p1s, p1e);
    const auto r2 = agent_reads::dailyRows(db, device_id_, p2s, p2e);

    json out;
    out["period1"] = aggregate(r1, false);
    out["period1"]["period"] = p1s + " to " + p1e;
    out["period2"] = aggregate(r2, false);
    out["period2"]["period"] = p2s + " to " + p2e;
    auto all = r1;
    all.insert(all.end(), r2.begin(), r2.end());
    out["rules"] = rulesForDays(all);
    return out;
}

json AgentTools::getVitals(const json& args, IDatabase& db) {
    const std::string date = dayArg(args, "date");
    json minutes = json::array();
    for (const auto& v : agent_reads::vitalsByMinute(db, device_id_, date)) {
        json m;
        m["minute"] = v.minute;
        put(m, "avg_spo2", v.avg_spo2, 1);
        put(m, "min_spo2", v.min_spo2, 1);
        put(m, "avg_hr", v.avg_hr, 1);
        put(m, "min_hr", v.min_hr, 0);
        put(m, "max_hr", v.max_hr, 0);
        minutes.push_back(std::move(m));
    }
    json out;
    out["date"] = date;
    out["minutes"] = std::move(minutes);
    out["returned"] = out["minutes"].size();
    out["rules"] = rulesFor({});
    return out;
}

json AgentTools::getStatistics(const json& args, IDatabase& db) {
    const std::string s = dayArg(args, "start_date"), e = dayArg(args, "end_date");
    const auto rows = agent_reads::dailyRows(db, device_id_, s, e);
    json out = aggregate(rows, true);
    out["total_nights"] = out["nights"];
    out.erase("nights");
    out["period"] = s + " to " + e;
    out["rules"] = rulesForDays(rows);
    return out;
}

} // namespace hms_cpap
