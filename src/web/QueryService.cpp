#include "web/QueryService.h"
#include "utils/OximetryDevice.h"
#include "cpapdash/parser/SleepIndex.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <map>
#include <optional>
#include "services/InsightsEngine.h"
#include <sstream>
#include <algorithm>
#include <ctime>
#include <iostream>

namespace hms_cpap {

namespace {

// Route parameters reach us as either "2026-04-17" or "20260417". The date
// dialect helpers all want the punctuated form, so settle on it here rather
// than letting each caller guess. Anything that is not eight digits after
// stripping punctuation is handed back untouched, so a malformed date fails
// in the database as a bad date rather than silently matching a real night.
std::string normalizeSleepDay(const std::string& date) {
    std::string digits;
    for (unsigned char c : date)
        if (std::isdigit(c)) digits.push_back(static_cast<char>(c));
    if (digits.size() != 8) return date;
    return digits.substr(0, 4) + "-" + digits.substr(4, 2) + "-" + digits.substr(6, 2);
}

// A numeric column, keeping the difference between "absent" and zero. The index
// needs that difference: a night with no leak channel drops the leak component
// and renormalises, where a leak of 0 earns full credit for it. The database
// layers hand numbers back as strings on some engines and as numbers on others,
// so both are accepted.
std::optional<double> jopt(const Json::Value& obj, const char* key) {
    auto v = obj.get(key, Json::nullValue);
    if (v.isNull()) return std::nullopt;
    if (v.isDouble() || v.isInt()) return v.asDouble();
    if (v.isString()) {
        const std::string s = v.asString();
        if (s.empty()) return std::nullopt;
        try { return std::stod(s); } catch (...) {}
    }
    return std::nullopt;
}

// SDD-019: the index for one row of cpap_daily_summary.
//
// Computed on read and never stored. The weights are tunable, and a stored
// index would be wrong the moment one of them moved, leaving two answers in the
// same database with one of them silently stale.
//
// Usage comes from duration_minutes, NOT from patient_hours. The two agree on
// almost every row, because the session-derived writer fills both from the same
// SUM(duration_seconds), but not on all of them: of the 230 rows on the hub,
// two hold a patient_hours near 1050 against nights of 80 and 89 minutes, which
// is a counter rather than a day. duration_minutes is consistent across all of
// them and is already what getStatistics trusts for the compliance percentage.
std::optional<int> rowIndex(const Json::Value& row) {
    const auto minutes = jopt(row, "duration_minutes");
    return cpapdash::parser::nightlyIndex(
        minutes ? std::optional<double>(*minutes / 60.0) : std::nullopt,
        jopt(row, "ahi"),
        jopt(row, "leak_95"));
}

// Attach the index to a row in place. A night that cannot be scored gets null
// rather than 0, which is a real distinction: 0 is the worst night there is,
// null is a night we know nothing about.
void annotateIndex(Json::Value& row) {
    const auto index = rowIndex(row);
    if (index) {
        row["sleep_index"] = *index;
        row["sleep_index_band"] = cpapdash::parser::bandKey(cpapdash::parser::bandFor(*index));
    } else {
        row["sleep_index"] = Json::nullValue;
        row["sleep_index_band"] = Json::nullValue;
    }
}

} // namespace

QueryService::QueryService(std::shared_ptr<IDatabase> db, const std::string& device_id)
    : db_(db), device_id_(device_id), dt_(db->dbType()) {}

Json::Value QueryService::getDashboard() {
    // --- Latest night (from daily summary — whole night, not single session) ---
    std::string q_latest =
        "SELECT record_date as sleep_day,"
        " " + sql::round("duration_minutes / 60.0", 2, dt_) + " as usage_hours,"
        " " + sql::round("ahi", 2, dt_) + " as ahi,"
        " " + sql::round("COALESCE(leak_50, 0)", 1, dt_) + " as leak_avg,"
        // SDD-019 inputs. duration_minutes and leak_95 are carried raw rather
        // than reusing usage_hours/leak_avg above: leak_avg is the median and
        // COALESCEs a missing channel to 0, which would earn the night full
        // leak credit for a measurement it never made.
        " duration_minutes,"
        " leak_95,"
        " COALESCE(mode, 0) as therapy_mode"
        " FROM cpap_daily_summary"
        " WHERE device_id = " + sql::param(1, dt_) +
        " ORDER BY record_date DESC LIMIT 1";

    // --- The index over the trailing week (SDD-019) ---
    std::string q_index_week =
        "SELECT duration_minutes, ahi, leak_95"
        " FROM cpap_daily_summary"
        " WHERE device_id = " + sql::param(1, dt_) +
        " ORDER BY record_date DESC LIMIT 7";

    // --- AHI trend (30 days) ---
    std::string q_ahi =
        "SELECT record_date as date, ahi as value"
        " FROM cpap_daily_summary"
        " WHERE device_id = " + sql::param(1, dt_) +
        " AND record_date >= " + sql::currentDateMinus(30, dt_) +
        " ORDER BY record_date";

    // --- Usage trend (30 days) ---
    std::string q_usage =
        "SELECT record_date as date, " + sql::round("duration_minutes / 60.0", 2, dt_) + " as value"
        " FROM cpap_daily_summary"
        " WHERE device_id = " + sql::param(1, dt_) +
        " AND record_date >= " + sql::currentDateMinus(30, dt_) +
        " ORDER BY record_date";

    // --- Compliance (30 days) ---
    std::string q_compliance =
        "SELECT " + sql::round(
            "SUM(CASE WHEN duration_minutes >= 240 THEN 1 ELSE 0 END) * 100.0 / NULLIF(COUNT(*), 0)",
            1, dt_) + " as compliance_pct"
        " FROM cpap_daily_summary"
        " WHERE device_id = " + sql::param(1, dt_) +
        " AND record_date >= " + sql::currentDateMinus(30, dt_);

    std::vector<std::string> p1 = {device_id_};

    auto latest    = db_->executeQuery(q_latest, p1);
    auto ahi_trend = db_->executeQuery(q_ahi, p1);
    auto usage_trend = db_->executeQuery(q_usage, p1);
    auto compliance  = db_->executeQuery(q_compliance, p1);
    auto index_week  = db_->executeQuery(q_index_week, p1);

    // --- Build result ---
    Json::Value result;
    Json::Value ln;
    if (latest.size() > 0) {
        ln["date"]        = latest[0].get("sleep_day", Json::nullValue);
        ln["ahi"]         = latest[0].get("ahi", "0");
        ln["usage_hours"] = latest[0].get("usage_hours", "0");
        ln["leak_avg"]    = latest[0].get("leak_avg", "0");
        ln["therapy_mode"] = latest[0].get("therapy_mode", "0");

        // SDD-019. Read off the raw columns in latest[0], written onto ln.
        if (const auto index = rowIndex(latest[0])) {
            ln["sleep_index"] = *index;
            ln["sleep_index_band"] =
                cpapdash::parser::bandKey(cpapdash::parser::bandFor(*index));
        } else {
            ln["sleep_index"] = Json::nullValue;
            ln["sleep_index_band"] = Json::nullValue;
        }
    }

    // The headline number: the index averaged over the trailing week. Nights
    // that cannot be scored still consume one of the seven, so a week with two
    // blank nights averages the other five rather than reaching further back.
    {
        std::vector<std::optional<int>> week;
        week.reserve(index_week.size());
        for (const auto& row : index_week) week.push_back(rowIndex(row));

        if (const auto avg = cpapdash::parser::trailingAverage(week, 7)) {
            result["sleep_index_7night"] = std::round(*avg * 10.0) / 10.0;
            result["sleep_index_7night_band"] = cpapdash::parser::bandKey(
                cpapdash::parser::bandFor(static_cast<int>(std::lround(*avg))));
        } else {
            result["sleep_index_7night"] = Json::nullValue;
            result["sleep_index_7night_band"] = Json::nullValue;
        }
    }
    if (compliance.size() > 0 && !compliance[0]["compliance_pct"].isNull()) {
        ln["compliance_pct"] = compliance[0]["compliance_pct"];
    } else {
        ln["compliance_pct"] = "0";
    }
    result["latest_night"] = ln;

    result["ahi_trend"]   = ahi_trend;
    result["usage_trend"] = usage_trend;
    return result;
}

Json::Value QueryService::getSessions(int limit, int offset) {
    // Group by sleep day (date shifted -12h) so multiple mask-on/off
    // events in the same night appear as one row. Returns the most recent
    // nights first, paginated by limit/offset (no date window) so the UI
    // can "load more" back through the full history.
    std::string cpap_arm =
        "SELECT " + sql::sleepDay("MIN(s.session_start)", dt_) + " as sleep_day,"
        " MIN(s.session_start) as session_start,"
        " MAX(s.session_end) as session_end,"
        " SUM(s.duration_seconds) as duration_seconds,"
        " " + sql::round("SUM(s.duration_seconds) / 3600.0", 2, dt_) + " as duration_hours,"
        " " + sql::round("CASE WHEN SUM(s.duration_seconds) > 0"
        "   THEN SUM(COALESCE(m.obstructive_apneas, 0) + COALESCE(m.central_apneas, 0)"
        "   + COALESCE(m.hypopneas, 0) + COALESCE(m.clear_airway_apneas, 0))"
        "   / (SUM(s.duration_seconds) / 3600.0)"
        "   ELSE 0 END", 2, dt_) + " as ahi,"
        " SUM(COALESCE(m.total_events, 0)) as total_events,"
        " SUM(COALESCE(m.obstructive_apneas, 0)) as obstructive_apneas,"
        " SUM(COALESCE(m.central_apneas, 0)) as central_apneas,"
        " SUM(COALESCE(m.hypopneas, 0)) as hypopneas,"
        " SUM(COALESCE(m.reras, 0)) as reras,"
        " " + sql::round("AVG(NULLIF(m.avg_spo2, 0))", 1, dt_) + " as avg_spo2,"
        " " + sql::round("AVG(NULLIF(m.avg_heart_rate, 0))", 0, dt_) + " as avg_heart_rate,"
        " SUM(CASE WHEN s.session_end IS NULL THEN 1 ELSE 0 END) as has_live,"
        " 0 as oximetry_only"
        " FROM cpap_sessions s"
        " LEFT JOIN cpap_session_metrics m ON m.session_id = s.id"
        " WHERE s.device_id = " + sql::param(1, dt_) +
        " GROUP BY " + sql::sleepDay("s.session_start", dt_);

    // A night that exists only as an O2 recording (CSV upload, or a BLE pull
    // for a night the machine never synced) gets its own row. Without this
    // arm the recording has no UI surface at all: the list is what links to
    // the detail page (ticket 67, "won't take July dates").
    std::string oxi_arm =
        "SELECT " + sql::sleepDay("MIN(o.start_time)", dt_) + " as sleep_day,"
        " MIN(o.start_time) as session_start,"
        " MAX(o.end_time) as session_end,"
        " SUM(o.duration_seconds) as duration_seconds,"
        " " + sql::round("SUM(o.duration_seconds) / 3600.0", 2, dt_) + " as duration_hours,"
        " NULL as ahi,"
        " NULL as total_events,"
        " NULL as obstructive_apneas,"
        " NULL as central_apneas,"
        " NULL as hypopneas,"
        " NULL as reras,"
        " " + sql::round("AVG(NULLIF(o.avg_spo2, 0))", 1, dt_) + " as avg_spo2,"
        " " + sql::round("AVG(NULLIF(o.avg_hr, 0))", 0, dt_) + " as avg_heart_rate,"
        " 0 as has_live,"
        " 1 as oximetry_only"
        " FROM oximetry_sessions o"
        " WHERE " + sql::sleepDay("o.start_time", dt_) + " NOT IN ("
        "SELECT DISTINCT " + sql::sleepDay("s2.session_start", dt_) +
        " FROM cpap_sessions s2 WHERE s2.device_id = " + sql::param(2, dt_) + ")"
        " GROUP BY " + sql::sleepDay("o.start_time", dt_);

    // UNION ALL rather than a join: the arms are disjoint by construction, and
    // pagination has to apply to the combined set or the frontend's
    // offset-by-rows-loaded "load more" walks past nights.
    std::string q = cpap_arm + " UNION ALL " + oxi_arm +
        " ORDER BY sleep_day DESC"
        " LIMIT " + std::to_string(limit) +
        " OFFSET " + std::to_string(offset);

    Json::Value rows = db_->executeQuery(q, {device_id_, device_id_});

    // SDD-008: attach the night's transfer state so the frontend does not
    // re-derive it a third time (it already reimplements the live test in
    // TypeScript). Enriched here in C++ rather than joined in SQL on purpose:
    // the join key needs a string transform, and this query is built for three
    // dialects, so one loop beats three subtly different expressions.
    //
    // sleep_day here and the ledger's str_day are both date(session_start,
    // '-12 hours'), so they identify the same night; only the punctuation
    // differs.
    std::map<std::string, NightState> by_day;
    for (const auto& f : db_->listSyncFolders()) {
        if (!f.str_day.empty()) by_day[f.str_day] = nightState(f);
    }
    for (auto& row : rows) {
        std::string key = row.isMember("sleep_day") && !row["sleep_day"].isNull()
                              ? row["sleep_day"].asString() : std::string();
        key.erase(std::remove_if(key.begin(), key.end(),
                                 [](unsigned char c) { return !std::isdigit(c); }),
                  key.end());
        if (key.size() > 8) key.resize(8);

        auto it = by_day.find(key);
        // No ledger row means no transfer was tracked for this night (a local
        // directory or a night predating the ledger). Fall back to the existing
        // has_live signal rather than inventing a state.
        if (it == by_day.end()) {
            const bool live = row.isMember("has_live") &&
                              !row["has_live"].isNull() &&
                              row["has_live"].asString() != "0";
            row["night_state"] = live ? "live" : "complete";
            row["partial"] = false;
            continue;
        }
        row["night_state"] = nightStateString(it->second);
        row["partial"] = (it->second == NightState::Partial);
    }

    return rows;
}

Json::Value QueryService::getSessionDetail(const std::string& date) {
    // Get sessions for a given sleep day
    std::string q_sessions =
        "SELECT s.id, s.session_start, s.session_end, s.duration_seconds,"
        " " + sql::round("s.duration_seconds / 3600.0", 2, dt_) + " as duration_hours,"
        " " + sql::round("m.ahi", 2, dt_) + " as ahi, m.total_events, m.obstructive_apneas, m.central_apneas,"
        " m.hypopneas, m.reras, m.clear_airway_apneas,"
        " m.avg_event_duration, m.max_event_duration, m.time_in_apnea_percent,"
        " m.avg_spo2, m.min_spo2, m.spo2_drops, m.odi,"
        " m.avg_heart_rate, m.min_heart_rate, m.max_heart_rate,"
        " COALESCE(d.mode, 0) as therapy_mode"
        " FROM cpap_sessions s"
        " LEFT JOIN cpap_session_metrics m ON m.session_id = s.id"
        " LEFT JOIN cpap_daily_summary d ON d.device_id = s.device_id"
        " AND d.record_date = " + sql::sleepDay("s.session_start", dt_) +
        " WHERE s.device_id = " + sql::param(1, dt_) +
        " AND " + sql::sleepDay("s.session_start", dt_) + " = " + sql::castDate(2, dt_) +
        " ORDER BY s.session_start";

    auto sessions = db_->executeQuery(q_sessions, {device_id_, date});

    // For each session, fetch its events
    Json::Value result(Json::arrayValue);
    for (const auto& sj : sessions) {
        Json::Value row = sj;
        std::string session_id = sj.get("id", "").asString();
        if (!session_id.empty()) {
            std::string q_events =
                "SELECT event_type, event_timestamp, duration_seconds, details"
                " FROM cpap_events WHERE session_id = " + sql::param(1, dt_) +
                " ORDER BY event_timestamp";
            row["events"] = db_->executeQuery(q_events, {session_id});
        } else {
            row["events"] = Json::Value(Json::arrayValue);
        }
        result.append(row);
    }

    // SDD-008: carry the same night_state the sessions list shows, so the
    // detail page's LIVE banner cannot disagree with the list. The ledger's
    // str_day and the requested date name the same night; only punctuation
    // differs.
    std::string key = date;
    key.erase(std::remove_if(key.begin(), key.end(),
                             [](unsigned char c) { return !std::isdigit(c); }),
              key.end());
    if (key.size() > 8) key.resize(8);

    const char* state = nullptr;
    for (const auto& f : db_->listSyncFolders()) {
        if (!f.str_day.empty() && f.str_day == key) {
            state = nightStateString(nightState(f));
            break;
        }
    }
    if (!state) {
        // No ledger row (local source, or a night predating the ledger): fall
        // back to the open-session test, matching getSessions.
        bool open = false;
        for (const auto& row : result) {
            if (!row.isMember("session_end") || row["session_end"].isNull() ||
                row["session_end"].asString().empty()) {
                open = true;
                break;
            }
        }
        state = open ? "live" : "complete";
    }
    for (auto& row : result) row["night_state"] = state;

    return result;
}

Json::Value QueryService::getDailySummary(const std::string& start, const std::string& end) {
    std::string q =
        "SELECT record_date, duration_minutes, ahi, hi, ai, oai, cai, uai, rin,"
        " leak_50, leak_95, leak_max,"
        " mask_press_50, mask_press_95, mask_press_max,"
        " spo2_50, spo2_95,"
        " resp_rate_50, tid_vol_50, min_vent_50,"
        " mode, epr_level, pressure_setting"
        " FROM cpap_daily_summary"
        " WHERE device_id = " + sql::param(1, dt_) +
        " AND record_date >= " + sql::castDate(2, dt_) +
        " AND record_date <= " + sql::castDate(3, dt_) +
        " ORDER BY record_date";

    auto rows = db_->executeQuery(q, {device_id_, start, end});
    for (auto& row : rows) annotateIndex(row);  // SDD-019
    return rows;
}

Json::Value QueryService::getTrend(const std::string& metric, int days) {
    std::string columns;
    if (metric == "ahi")           columns = "record_date, ahi, hi, ai, oai, cai";
    else if (metric == "pressure") columns = "record_date, mask_press_50, mask_press_95, mask_press_max";
    else if (metric == "leak")     columns = "record_date, leak_50, leak_95, leak_max";
    else if (metric == "spo2")     columns = "record_date, spo2_50, spo2_95";
    else if (metric == "hr")       columns = "record_date, resp_rate_50";
    else if (metric == "duration") columns = "record_date, duration_minutes";
    else if (metric == "events")  columns = "record_date, oai, cai, hi, rin";
    else if (metric == "respiratory") columns = "record_date, resp_rate_50, tid_vol_50, min_vent_50";
    else if (metric == "csr")    columns = "record_date, csr";
    else if (metric == "epr")    columns = "record_date, epr_level";
    else {
        Json::Value err;
        err["error"] = "Unknown metric. Use: ahi, pressure, leak, spo2, hr, duration";
        return err;
    }

    std::string q = "SELECT " + columns + " FROM cpap_daily_summary"
        " WHERE device_id = " + sql::param(1, dt_) +
        " AND record_date >= " + sql::currentDateMinus(days, dt_) +
        " ORDER BY record_date";

    return db_->executeQuery(q, {device_id_});
}

Json::Value QueryService::getStatistics(const std::string& start, const std::string& end) {
    std::string q =
        "SELECT COUNT(*) as total_nights,"
        " " + sql::round("AVG(ahi)", 2, dt_) + " as avg_ahi,"
        " " + sql::round("MIN(ahi)", 2, dt_) + " as min_ahi,"
        " " + sql::round("MAX(ahi)", 2, dt_) + " as max_ahi,"
        " " + sql::round(sql::stddev("ahi", dt_), 2, dt_) + " as stddev_ahi,"
        " " + sql::round("AVG(duration_minutes)", 1, dt_) + " as avg_duration_min,"
        " " + sql::round("AVG(leak_95)", 1, dt_) + " as avg_leak_95,"
        " " + sql::round("AVG(mask_press_95)", 1, dt_) + " as avg_pressure_95,"
        " " + sql::round("AVG(spo2_50)", 1, dt_) + " as avg_spo2,"
        " " + sql::round(
            "SUM(CASE WHEN duration_minutes >= 240 THEN 1 ELSE 0 END) * 100.0 / NULLIF(COUNT(*), 0)",
            1, dt_) + " as compliance_pct,"
        " SUM(duration_minutes) as total_therapy_minutes"
        " FROM cpap_daily_summary"
        " WHERE device_id = " + sql::param(1, dt_) +
        " AND record_date >= " + sql::castDate(2, dt_) +
        " AND record_date <= " + sql::castDate(3, dt_);

    return db_->executeQuery(q, {device_id_, start, end});
}

Json::Value QueryService::getSummaries(const std::string& period, int limit) {
    std::string q =
        "SELECT id, period, range_start, range_end, nights_count,"
        " avg_ahi, avg_usage_hours, compliance_pct, summary_text, created_at"
        " FROM cpap_summaries"
        " WHERE device_id = " + sql::param(1, dt_);

    std::vector<std::string> params = {device_id_};
    if (!period.empty()) {
        q += " AND period = " + sql::param(2, dt_);
        params.push_back(period);
    }
    q += " ORDER BY created_at DESC LIMIT " + std::to_string(limit);

    return db_->executeQuery(q, params);
}

static double jdouble(const Json::Value& obj, const char* key) {
    auto v = obj.get(key, Json::nullValue);
    if (v.isNull()) return 0;
    if (v.isDouble() || v.isInt()) return v.asDouble();
    if (v.isString()) { try { return std::stod(v.asString()); } catch (...) {} }
    return 0;
}

Json::Value QueryService::getInsights(int days) {
    std::string q =
        "SELECT record_date, duration_minutes, ahi, hi, ai, oai, cai, uai, rin,"
        " COALESCE(csr, 0) as csr,"
        " mask_press_50, mask_press_95, mask_press_max,"
        " leak_50, leak_95, leak_max,"
        " spo2_50, spo2_95,"
        " resp_rate_50, tid_vol_50, min_vent_50,"
        " COALESCE(mode, 0) as mode, epr_level, pressure_setting"
        " FROM cpap_daily_summary"
        " WHERE device_id = " + sql::param(1, dt_) +
        " AND record_date >= " + sql::currentDateMinus(days, dt_) +
        " ORDER BY record_date";

    auto rows = db_->executeQuery(q, {device_id_});

    std::vector<STRDailyRecord> records;
    for (const auto& r : rows) {
        STRDailyRecord rec;
        rec.device_id = device_id_;
        std::string date_str = r.get("record_date", "").asString();
        if (date_str.size() >= 10) {
            std::tm tm{};
            tm.tm_year = std::stoi(date_str.substr(0, 4)) - 1900;
            tm.tm_mon  = std::stoi(date_str.substr(5, 2)) - 1;
            tm.tm_mday = std::stoi(date_str.substr(8, 2));
            tm.tm_hour = 12;
            rec.record_date = std::chrono::system_clock::from_time_t(std::mktime(&tm));
        }
        rec.duration_minutes = jdouble(r, "duration_minutes");
        rec.ahi = jdouble(r, "ahi");
        rec.hi = jdouble(r, "hi");
        rec.ai = jdouble(r, "ai");
        rec.oai = jdouble(r, "oai");
        rec.cai = jdouble(r, "cai");
        rec.uai = jdouble(r, "uai");
        rec.rin = jdouble(r, "rin");
        rec.csr = jdouble(r, "csr");
        rec.mask_press_50 = jdouble(r, "mask_press_50");
        rec.mask_press_95 = jdouble(r, "mask_press_95");
        rec.mask_press_max = jdouble(r, "mask_press_max");
        rec.leak_50 = jdouble(r, "leak_50");
        rec.leak_95 = jdouble(r, "leak_95");
        rec.leak_max = jdouble(r, "leak_max");
        rec.spo2_50 = jdouble(r, "spo2_50");
        rec.spo2_95 = jdouble(r, "spo2_95");
        rec.resp_rate_50 = jdouble(r, "resp_rate_50");
        rec.tid_vol_50 = jdouble(r, "tid_vol_50");
        rec.min_vent_50 = jdouble(r, "min_vent_50");
        rec.mode = static_cast<int>(jdouble(r, "mode"));
        rec.epr_level = jdouble(r, "epr_level");
        rec.pressure_setting = jdouble(r, "pressure_setting");
        records.push_back(rec);
    }

    auto insights = InsightsEngine::analyze(records);
    return InsightsEngine::toJson(insights);
}

Json::Value QueryService::getSessionSignals(const std::string& date) {
    // Join breathing_summary + calculated_metrics for per-minute signal data
    std::string q =
        "SELECT b.timestamp,"
        " b.avg_flow_rate, b.max_flow_rate, b.min_flow_rate,"
        " b.avg_pressure, b.max_pressure, b.min_pressure,"
        " c.respiratory_rate, c.tidal_volume, c.minute_ventilation,"
        " c.ie_ratio, c.flow_limitation, c.leak_rate,"
        " c.mask_pressure, c.epr_pressure, c.snore_index, c.target_ventilation"
        " FROM cpap_sessions s"
        " JOIN cpap_breathing_summary b ON b.session_id = s.id"
        " LEFT JOIN cpap_calculated_metrics c ON c.session_id = s.id AND c.timestamp = b.timestamp"
        " WHERE s.device_id = " + sql::param(1, dt_) +
        " AND " + sql::sleepDay("s.session_start", dt_) + " = " + sql::castDate(2, dt_) +
        " ORDER BY b.timestamp";

    auto rows = db_->executeQuery(q, {device_id_, date});

    // Convert row-oriented to column-oriented for Chart.js
    Json::Value result;
    Json::Value timestamps(Json::arrayValue);
    Json::Value flow_avg(Json::arrayValue), flow_max(Json::arrayValue), flow_min(Json::arrayValue);
    Json::Value press_avg(Json::arrayValue), press_max(Json::arrayValue), press_min(Json::arrayValue);
    Json::Value rr(Json::arrayValue), tv(Json::arrayValue), mv(Json::arrayValue);
    Json::Value ie(Json::arrayValue), fl(Json::arrayValue), leak(Json::arrayValue);
    Json::Value mask_press(Json::arrayValue), epr_press(Json::arrayValue), snore(Json::arrayValue), tgt_vent(Json::arrayValue);

    for (const auto& r : rows) {
        timestamps.append(r.get("timestamp", Json::nullValue));
        flow_avg.append(r.get("avg_flow_rate", Json::nullValue));
        flow_max.append(r.get("max_flow_rate", Json::nullValue));
        flow_min.append(r.get("min_flow_rate", Json::nullValue));
        press_avg.append(r.get("avg_pressure", Json::nullValue));
        press_max.append(r.get("max_pressure", Json::nullValue));
        press_min.append(r.get("min_pressure", Json::nullValue));
        rr.append(r.get("respiratory_rate", Json::nullValue));
        tv.append(r.get("tidal_volume", Json::nullValue));
        mv.append(r.get("minute_ventilation", Json::nullValue));
        ie.append(r.get("ie_ratio", Json::nullValue));
        fl.append(r.get("flow_limitation", Json::nullValue));
        leak.append(r.get("leak_rate", Json::nullValue));
        mask_press.append(r.get("mask_pressure", Json::nullValue));
        epr_press.append(r.get("epr_pressure", Json::nullValue));
        snore.append(r.get("snore_index", Json::nullValue));
        tgt_vent.append(r.get("target_ventilation", Json::nullValue));
    }

    result["timestamps"] = timestamps;
    result["flow_avg"] = flow_avg;
    result["flow_max"] = flow_max;
    result["flow_min"] = flow_min;
    result["pressure_avg"] = press_avg;
    result["pressure_max"] = press_max;
    result["pressure_min"] = press_min;
    result["respiratory_rate"] = rr;
    result["tidal_volume"] = tv;
    result["minute_ventilation"] = mv;
    result["ie_ratio"] = ie;
    result["flow_limitation"] = fl;
    result["leak_rate"] = leak;
    result["mask_pressure"] = mask_press;
    result["epr_pressure"] = epr_press;
    result["snore_index"] = snore;
    result["target_ventilation"] = tgt_vent;
    return result;
}

Json::Value QueryService::getSessionVitals(const std::string& date, int interval) {
    if (interval < 1) interval = 30;

    std::string bucket_expr;
    switch (dt_) {
        case DbType::POSTGRESQL:
            bucket_expr =
                "date_trunc('minute', v.timestamp) + "
                "(FLOOR(EXTRACT(SECOND FROM v.timestamp) / " + std::to_string(interval) +
                ") * " + std::to_string(interval) + ") * INTERVAL '1 second'";
            break;
        case DbType::MYSQL:
            bucket_expr =
                "DATE_FORMAT(v.timestamp, '%Y-%m-%d %H:%i:00') + INTERVAL "
                "(FLOOR(SECOND(v.timestamp) / " + std::to_string(interval) +
                ") * " + std::to_string(interval) + ") SECOND";
            break;
        case DbType::SQLITE:
            bucket_expr =
                "strftime('%Y-%m-%d %H:%M:', v.timestamp) || "
                "printf('%02d', (CAST(strftime('%S', v.timestamp) AS INTEGER) / " +
                std::to_string(interval) + ") * " + std::to_string(interval) + ")";
            break;
    }

    std::string q =
        "SELECT " + bucket_expr + " AS bucket,"
        " " + sql::round("AVG(v.spo2)", 1, dt_) + " as spo2,"
        " MIN(v.spo2) as spo2_min,"
        " " + sql::round("AVG(v.heart_rate)", 0, dt_) + " as heart_rate,"
        " MIN(v.heart_rate) as hr_min, MAX(v.heart_rate) as hr_max"
        " FROM cpap_sessions s"
        " JOIN cpap_vitals v ON v.session_id = s.id"
        " WHERE s.device_id = " + sql::param(1, dt_) +
        " AND " + sql::sleepDay("s.session_start", dt_) + " = " + sql::castDate(2, dt_) +
        " AND v.spo2 > 0"
        " GROUP BY bucket ORDER BY bucket";

    auto rows = db_->executeQuery(q, {device_id_, date});

    // Column-oriented
    Json::Value result;
    Json::Value timestamps(Json::arrayValue);
    Json::Value spo2(Json::arrayValue), spo2_min(Json::arrayValue);
    Json::Value hr(Json::arrayValue), hr_min(Json::arrayValue), hr_max(Json::arrayValue);

    for (const auto& r : rows) {
        timestamps.append(r.get("bucket", Json::nullValue));
        spo2.append(r.get("spo2", Json::nullValue));
        spo2_min.append(r.get("spo2_min", Json::nullValue));
        hr.append(r.get("heart_rate", Json::nullValue));
        hr_min.append(r.get("hr_min", Json::nullValue));
        hr_max.append(r.get("hr_max", Json::nullValue));
    }

    result["timestamps"] = timestamps;
    result["spo2"] = spo2;
    result["spo2_min"] = spo2_min;
    result["heart_rate"] = hr;
    result["hr_min"] = hr_min;
    result["hr_max"] = hr_max;
    return result;
}

Json::Value QueryService::getSessionEvents(const std::string& date) {
    std::string q =
        "SELECT e.event_type, e.event_timestamp, e.duration_seconds, e.details"
        " FROM cpap_sessions s"
        " JOIN cpap_events e ON e.session_id = s.id"
        " WHERE s.device_id = " + sql::param(1, dt_) +
        " AND " + sql::sleepDay("s.session_start", dt_) + " = " + sql::castDate(2, dt_) +
        " ORDER BY e.event_timestamp";

    return db_->executeQuery(q, {device_id_, date});
}

Json::Value QueryService::getEvents(const std::string& start, const std::string& end,
                                    const std::vector<std::string>& types,
                                    int min_duration, int limit, int offset) {
    // SDD-009: the cross-night event search behind /api/events. Clauses are
    // appended in the same order their parameters are pushed, which is what
    // keeps the '?' dialects aligned with the parameter vector.
    std::vector<std::string> params;
    int idx = 1;

    std::string q =
        "SELECT e.event_type, e.event_timestamp, e.duration_seconds, e.details,"
        " " + sql::sleepDay("s.session_start", dt_) + " as sleep_day"
        " FROM cpap_events e"
        " JOIN cpap_sessions s ON s.id = e.session_id"
        " WHERE s.device_id = " + sql::param(idx++, dt_);
    params.push_back(device_id_);

    if (!start.empty()) {
        q += " AND " + sql::sleepDay("s.session_start", dt_) +
             " >= " + sql::castDate(idx++, dt_);
        params.push_back(start);
    }
    if (!end.empty()) {
        q += " AND " + sql::sleepDay("s.session_start", dt_) +
             " <= " + sql::castDate(idx++, dt_);
        params.push_back(end);
    }
    if (!types.empty()) {
        q += " AND e.event_type IN (";
        for (size_t i = 0; i < types.size(); ++i) {
            if (i) q += ", ";
            q += sql::param(idx++, dt_);
            params.push_back(types[i]);
        }
        q += ")";
    }
    if (min_duration > 0) {
        q += " AND e.duration_seconds >= " + std::to_string(min_duration);
    }

    q += " ORDER BY e.event_timestamp DESC"
         " LIMIT " + std::to_string(limit) +
         " OFFSET " + std::to_string(offset);

    return db_->executeQuery(q, params);
}

Json::Value QueryService::getSessionBreaths(const std::string& date) {
    std::string q =
        "SELECT b.onset, b.tidal_volume, b.inspiratory_time, b.expiratory_time, b.flow_limitation"
        " FROM cpap_sessions s"
        " JOIN cpap_breaths b ON b.session_id = s.id"
        " WHERE s.device_id = " + sql::param(1, dt_) +
        " AND " + sql::sleepDay("s.session_start", dt_) + " = " + sql::castDate(2, dt_) +
        " ORDER BY b.onset";

    return db_->executeQuery(q, {device_id_, date});
}

Json::Value QueryService::getSessionOximetry(const std::string& date, int interval) {
    if (interval < 1) interval = 4;

    // A recording belongs to exactly one night, and it is the same night the
    // CPAP side would file it under: date(start - 12h). Everything else here
    // already uses that rule (sql::sleepDay), so the oximetry chart lines up
    // with the therapy chart for free.
    //
    // This used to match "this day OR the next day" against cpap_session_date
    // AND against four filename LIKE patterns, on the theory that the ring
    // labels a night with the morning's date. Nothing then narrowed it back
    // down, so EVERY session was returned for two consecutive nights: one CSV
    // import showed the identical trace on both, which is what an owner
    // reported. The night is derivable from start_time, so derive it instead
    // of guessing from a filename.
    std::string sleep_day = normalizeSleepDay(date);

    std::string q =
        "SELECT s.timestamp" + std::string(dt_ == DbType::POSTGRESQL ? "::text" : "") + " AS ts,"
        " s.spo2, s.heart_rate, s.motion"
        " FROM oximetry_sessions os"
        " JOIN oximetry_samples s ON s.oximetry_session_id = os.id"
        " WHERE os.device_id = " + sql::param(1, dt_) +
        " AND " + sql::sleepDay("os.start_time", dt_) + " = " + sql::castDate(2, dt_) +
        " AND s." + std::string(dt_ == DbType::SQLITE ? "valid = 1" : "valid = true") +
        " ORDER BY s.timestamp";

    auto rows = db_->executeQuery(q, {kOximetryDeviceId, sleep_day});

    Json::Value result;
    Json::Value timestamps(Json::arrayValue);
    Json::Value spo2(Json::arrayValue);
    Json::Value heart_rate(Json::arrayValue);
    Json::Value motion(Json::arrayValue);

    for (const auto& r : rows) {
        timestamps.append(r.get("ts", Json::nullValue));
        spo2.append(r.get("spo2", Json::nullValue));
        heart_rate.append(r.get("heart_rate", Json::nullValue));
        motion.append(r.get("motion", Json::nullValue));
    }

    result["timestamps"] = timestamps;
    result["spo2"] = spo2;
    result["heart_rate"] = heart_rate;
    result["motion"] = motion;
    return result;
}

} // namespace hms_cpap
