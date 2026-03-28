#include "web/QueryService.h"
#include <sstream>

namespace hms_cpap {

QueryService::QueryService(std::shared_ptr<IDatabase> db, const std::string& device_id)
    : db_(db), device_id_(device_id), dt_(db->dbType()) {}

Json::Value QueryService::getDashboard() {
    // --- Latest night ---
    std::string q_latest =
        "SELECT " + sql::sleepDay("s.session_start", dt_) + " as sleep_day,"
        " " + sql::round("s.duration_seconds / 3600.0", 2, dt_) + " as usage_hours,"
        " " + sql::round("m.ahi", 2, dt_) + " as ahi,"
        " " + sql::round("COALESCE(m.avg_leak_rate, 0)", 1, dt_) + " as leak_avg"
        " FROM cpap_sessions s"
        " LEFT JOIN cpap_session_metrics m ON m.session_id = s.id"
        " WHERE s.device_id = " + sql::param(1, dt_) + " AND s.session_end IS NOT NULL"
        " ORDER BY s.session_start DESC LIMIT 1";

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

    // --- Build result ---
    Json::Value result;
    Json::Value ln;
    if (latest.size() > 0) {
        ln["date"]        = latest[0].get("sleep_day", Json::nullValue);
        ln["ahi"]         = latest[0].get("ahi", "0");
        ln["usage_hours"] = latest[0].get("usage_hours", "0");
        ln["leak_avg"]    = latest[0].get("leak_avg", "0");
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

Json::Value QueryService::getSessions(int days, int limit) {
    std::string q =
        "SELECT s.session_start, s.session_end, s.duration_seconds,"
        " " + sql::round("s.duration_seconds / 3600.0", 2, dt_) + " as duration_hours,"
        " " + sql::round("m.ahi", 2, dt_) + " as ahi, m.total_events, m.obstructive_apneas, m.central_apneas,"
        " m.hypopneas, m.reras,"
        " m.avg_spo2, m.min_spo2,"
        " m.avg_heart_rate, m.min_heart_rate, m.max_heart_rate"
        " FROM cpap_sessions s"
        " LEFT JOIN cpap_session_metrics m ON m.session_id = s.id"
        " WHERE s.device_id = " + sql::param(1, dt_) +
        " AND s.session_start >= " + sql::daysAgo(days, dt_) +
        " AND s.session_end IS NOT NULL"
        " ORDER BY s.session_start DESC"
        " LIMIT " + std::to_string(limit);

    return db_->executeQuery(q, {device_id_});
}

Json::Value QueryService::getSessionDetail(const std::string& date) {
    // Get sessions for a given sleep day
    std::string q_sessions =
        "SELECT s.id, s.session_start, s.session_end, s.duration_seconds,"
        " " + sql::round("s.duration_seconds / 3600.0", 2, dt_) + " as duration_hours,"
        " " + sql::round("m.ahi", 2, dt_) + " as ahi, m.total_events, m.obstructive_apneas, m.central_apneas,"
        " m.hypopneas, m.reras, m.clear_airway_apneas,"
        " m.avg_event_duration, m.max_event_duration, m.time_in_apnea_percent,"
        " m.avg_spo2, m.min_spo2,"
        " m.avg_heart_rate, m.min_heart_rate, m.max_heart_rate"
        " FROM cpap_sessions s"
        " LEFT JOIN cpap_session_metrics m ON m.session_id = s.id"
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

    return db_->executeQuery(q, {device_id_, start, end});
}

Json::Value QueryService::getTrend(const std::string& metric, int days) {
    std::string columns;
    if (metric == "ahi")           columns = "record_date, ahi, hi, ai, oai, cai";
    else if (metric == "pressure") columns = "record_date, mask_press_50, mask_press_95, mask_press_max";
    else if (metric == "leak")     columns = "record_date, leak_50, leak_95, leak_max";
    else if (metric == "spo2")     columns = "record_date, spo2_50, spo2_95";
    else if (metric == "hr")       columns = "record_date, resp_rate_50";
    else if (metric == "duration") columns = "record_date, duration_minutes";
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

} // namespace hms_cpap
