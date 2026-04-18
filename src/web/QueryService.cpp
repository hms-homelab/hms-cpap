#include "web/QueryService.h"
#include <sstream>
#include <algorithm>
#include <ctime>

namespace hms_cpap {

QueryService::QueryService(std::shared_ptr<IDatabase> db, const std::string& device_id)
    : db_(db), device_id_(device_id), dt_(db->dbType()) {}

Json::Value QueryService::getDashboard() {
    // --- Latest night (from daily summary — whole night, not single session) ---
    std::string q_latest =
        "SELECT record_date as sleep_day,"
        " " + sql::round("duration_minutes / 60.0", 2, dt_) + " as usage_hours,"
        " " + sql::round("ahi", 2, dt_) + " as ahi,"
        " " + sql::round("COALESCE(leak_50, 0)", 1, dt_) + " as leak_avg,"
        " COALESCE(mode, 0) as therapy_mode"
        " FROM cpap_daily_summary"
        " WHERE device_id = " + sql::param(1, dt_) +
        " ORDER BY record_date DESC LIMIT 1";

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
        ln["therapy_mode"] = latest[0].get("therapy_mode", "0");
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
    // Group by sleep day (date shifted -12h) so multiple mask-on/off
    // events in the same night appear as one row
    std::string q =
        "SELECT " + sql::sleepDay("MIN(s.session_start)", dt_) + " as sleep_day,"
        " MIN(s.session_start) as session_start,"
        " MAX(s.session_end) as session_end,"
        " SUM(s.duration_seconds) as duration_seconds,"
        " " + sql::round("SUM(s.duration_seconds) / 3600.0", 2, dt_) + " as duration_hours,"
        " " + sql::round("CASE WHEN SUM(s.duration_seconds) > 0"
        "   THEN SUM(COALESCE(m.total_events, 0)) / (SUM(s.duration_seconds) / 3600.0)"
        "   ELSE 0 END", 2, dt_) + " as ahi,"
        " SUM(COALESCE(m.total_events, 0)) as total_events,"
        " SUM(COALESCE(m.obstructive_apneas, 0)) as obstructive_apneas,"
        " SUM(COALESCE(m.central_apneas, 0)) as central_apneas,"
        " SUM(COALESCE(m.hypopneas, 0)) as hypopneas,"
        " SUM(COALESCE(m.reras, 0)) as reras,"
        " " + sql::round("AVG(NULLIF(m.avg_spo2, 0))", 1, dt_) + " as avg_spo2,"
        " " + sql::round("AVG(NULLIF(m.avg_heart_rate, 0))", 0, dt_) + " as avg_heart_rate,"
        " SUM(CASE WHEN s.session_end IS NULL THEN 1 ELSE 0 END) as has_live"
        " FROM cpap_sessions s"
        " LEFT JOIN cpap_session_metrics m ON m.session_id = s.id"
        " WHERE s.device_id = " + sql::param(1, dt_) +
        " AND s.session_start >= " + sql::daysAgo(days, dt_) +
        " GROUP BY " + sql::sleepDay("s.session_start", dt_) +
        " ORDER BY sleep_day DESC"
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
        "SELECT e.event_type, e.event_timestamp, e.duration_seconds"
        " FROM cpap_sessions s"
        " JOIN cpap_events e ON e.session_id = s.id"
        " WHERE s.device_id = " + sql::param(1, dt_) +
        " AND " + sql::sleepDay("s.session_start", dt_) + " = " + sql::castDate(2, dt_) +
        " ORDER BY e.event_timestamp";

    return db_->executeQuery(q, {device_id_, date});
}

Json::Value QueryService::getSessionOximetry(const std::string& date, int interval) {
    if (interval < 1) interval = 4;

    // sleep_day is the evening date (e.g. 2026-04-17) but the ring records
    // with the next morning date (20260418). Match both the sleep_day and
    // the next day's YYYYMMDD in cpap_session_date and filename prefix.
    // Strip dashes: "2026-04-17" → "20260417"
    std::string date_nodash = date;
    date_nodash.erase(std::remove(date_nodash.begin(), date_nodash.end(), '-'), date_nodash.end());

    // Next day: parse YYYYMMDD, add 1 day
    std::string next_day;
    {
        std::tm tm{};
        tm.tm_year = std::stoi(date_nodash.substr(0, 4)) - 1900;
        tm.tm_mon = std::stoi(date_nodash.substr(4, 2)) - 1;
        tm.tm_mday = std::stoi(date_nodash.substr(6, 2)) + 1;
        mktime(&tm);
        char buf[9];
        strftime(buf, sizeof(buf), "%Y%m%d", &tm);
        next_day = buf;
    }

    std::string q =
        "SELECT s.timestamp" + std::string(dt_ == DbType::POSTGRESQL ? "::text" : "") + " AS ts,"
        " s.spo2, s.heart_rate, s.motion"
        " FROM oximetry_sessions os"
        " JOIN oximetry_samples s ON s.oximetry_session_id = os.id"
        " WHERE os.device_id = " + sql::param(1, dt_) +
        " AND (os.cpap_session_date IN (" + sql::param(2, dt_) + ", " + sql::param(3, dt_) + ")" +
        "  OR os.filename LIKE " + sql::param(2, dt_) + " || '%'" +
        "  OR os.filename LIKE " + sql::param(3, dt_) + " || '%')" +
        " AND s." + std::string(dt_ == DbType::SQLITE ? "valid = 1" : "valid = true") +
        " ORDER BY s.timestamp";

    auto rows = db_->executeQuery(q, {"o2ring", date_nodash, next_day});

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
