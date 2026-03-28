#include "web/QueryService.h"
#include <sstream>

namespace hms_cpap {

QueryService::QueryService(std::shared_ptr<DatabaseService> db, const std::string& device_id)
    : db_(db), device_id_(device_id) {}

Json::Value QueryService::resultToJson(const pqxx::result& result) {
    Json::Value arr(Json::arrayValue);
    for (const auto& row : result) {
        Json::Value obj;
        for (pqxx::row_size_type i = 0; i < row.size(); ++i) {
            if (row[i].is_null())
                obj[std::string(result.column_name(i))] = Json::nullValue;
            else
                obj[std::string(result.column_name(i))] = row[i].c_str();
        }
        arr.append(obj);
    }
    return arr;
}

Json::Value QueryService::getDashboard() {
    auto conn = db_->rawConnection();
    pqxx::work txn(*conn);

    auto latest = txn.exec_params(R"(
        SELECT DATE(s.session_start - INTERVAL '12 hours') as sleep_day,
               ROUND(s.duration_seconds / 3600.0, 2) as usage_hours,
               m.ahi,
               COALESCE(m.avg_leak_rate, 0) as leak_avg
        FROM cpap_sessions s
        LEFT JOIN cpap_session_metrics m ON m.session_id = s.id
        WHERE s.device_id = $1 AND s.session_end IS NOT NULL
        ORDER BY s.session_start DESC LIMIT 1
    )", device_id_);

    auto ahi_trend = txn.exec_params(R"(
        SELECT record_date as date, ahi as value
        FROM cpap_daily_summary
        WHERE device_id = $1 AND record_date >= CURRENT_DATE - 30
        ORDER BY record_date
    )", device_id_);

    auto usage_trend = txn.exec_params(R"(
        SELECT record_date as date, ROUND((duration_minutes / 60.0)::numeric, 2) as value
        FROM cpap_daily_summary
        WHERE device_id = $1 AND record_date >= CURRENT_DATE - 30
        ORDER BY record_date
    )", device_id_);

    auto compliance = txn.exec_params(R"(
        SELECT ROUND(SUM(CASE WHEN duration_minutes >= 240 THEN 1 ELSE 0 END) * 100.0 /
                     NULLIF(COUNT(*), 0), 1) as compliance_pct
        FROM cpap_daily_summary
        WHERE device_id = $1 AND record_date >= CURRENT_DATE - 30
    )", device_id_);

    txn.commit();

    Json::Value result;
    Json::Value ln;
    if (!latest.empty()) {
        ln["date"] = latest[0]["sleep_day"].c_str();
        ln["ahi"] = latest[0]["ahi"].as<double>(0);
        ln["usage_hours"] = latest[0]["usage_hours"].as<double>(0);
        ln["leak_avg"] = latest[0]["leak_avg"].as<double>(0);
    }
    ln["compliance_pct"] = (!compliance.empty() && !compliance[0][0].is_null())
        ? compliance[0][0].as<double>(0) : 0.0;
    result["latest_night"] = ln;

    Json::Value ahi_arr(Json::arrayValue);
    for (const auto& row : ahi_trend) {
        Json::Value p;
        p["date"] = row["date"].c_str();
        p["value"] = row["value"].as<double>(0);
        ahi_arr.append(p);
    }
    result["ahi_trend"] = ahi_arr;

    Json::Value usage_arr(Json::arrayValue);
    for (const auto& row : usage_trend) {
        Json::Value p;
        p["date"] = row["date"].c_str();
        p["value"] = row["value"].as<double>(0);
        usage_arr.append(p);
    }
    result["usage_trend"] = usage_arr;

    return result;
}

Json::Value QueryService::getSessions(int days, int limit) {
    auto conn = db_->rawConnection();
    pqxx::work txn(*conn);
    auto result = txn.exec_params(R"(
        SELECT s.session_start, s.session_end, s.duration_seconds,
               ROUND((s.duration_seconds / 3600.0)::numeric, 2) as duration_hours,
               m.ahi, m.total_events, m.obstructive_apneas, m.central_apneas,
               m.hypopneas, m.reras,
               m.avg_spo2, m.min_spo2,
               m.avg_heart_rate, m.min_heart_rate, m.max_heart_rate
        FROM cpap_sessions s
        LEFT JOIN cpap_session_metrics m ON m.session_id = s.id
        WHERE s.device_id = $1
          AND s.session_start >= NOW() - make_interval(days => $2)
          AND s.session_end IS NOT NULL
        ORDER BY s.session_start DESC
        LIMIT $3
    )", device_id_, days, limit);
    txn.commit();
    return resultToJson(result);
}

Json::Value QueryService::getSessionDetail(const std::string& date) {
    auto conn = db_->rawConnection();
    pqxx::work txn(*conn);

    auto sessions = txn.exec_params(R"(
        SELECT s.id, s.session_start, s.session_end, s.duration_seconds,
               ROUND((s.duration_seconds / 3600.0)::numeric, 2) as duration_hours,
               m.ahi, m.total_events, m.obstructive_apneas, m.central_apneas,
               m.hypopneas, m.reras, m.clear_airway_apneas,
               m.avg_event_duration, m.max_event_duration, m.time_in_apnea_percent,
               m.avg_spo2, m.min_spo2,
               m.avg_heart_rate, m.min_heart_rate, m.max_heart_rate
        FROM cpap_sessions s
        LEFT JOIN cpap_session_metrics m ON m.session_id = s.id
        WHERE s.device_id = $1
          AND DATE(s.session_start - INTERVAL '12 hours') = $2::date
        ORDER BY s.session_start
    )", device_id_, date);

    Json::Value result(Json::arrayValue);
    for (const auto& row : sessions) {
        Json::Value sj;
        for (pqxx::row_size_type i = 0; i < row.size(); ++i) {
            std::string col(sessions.column_name(i));
            sj[col] = row[i].is_null() ? Json::nullValue : Json::Value(row[i].c_str());
        }

        int session_id = row["id"].as<int>();
        auto events = txn.exec_params(R"(
            SELECT event_type, event_timestamp, duration_seconds, details
            FROM cpap_events WHERE session_id = $1 ORDER BY event_timestamp
        )", session_id);

        Json::Value ev_arr(Json::arrayValue);
        for (const auto& e : events) {
            Json::Value ej;
            for (pqxx::row_size_type i = 0; i < e.size(); ++i) {
                std::string col(events.column_name(i));
                ej[col] = e[i].is_null() ? Json::nullValue : Json::Value(e[i].c_str());
            }
            ev_arr.append(ej);
        }
        sj["events"] = ev_arr;
        result.append(sj);
    }
    txn.commit();
    return result;
}

Json::Value QueryService::getDailySummary(const std::string& start, const std::string& end) {
    auto conn = db_->rawConnection();
    pqxx::work txn(*conn);
    auto result = txn.exec_params(R"(
        SELECT record_date, duration_minutes, ahi, hi, ai, oai, cai, uai, rin,
               leak_50, leak_95, leak_max,
               mask_press_50, mask_press_95, mask_press_max,
               spo2_50, spo2_95,
               resp_rate_50, tid_vol_50, min_vent_50,
               mode, epr_level, pressure_setting
        FROM cpap_daily_summary
        WHERE device_id = $1
          AND record_date >= $2::date AND record_date <= $3::date
        ORDER BY record_date
    )", device_id_, start, end);
    txn.commit();
    return resultToJson(result);
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

    std::string query = "SELECT " + columns + " FROM cpap_daily_summary"
        " WHERE device_id = $1 AND record_date >= CURRENT_DATE - " + std::to_string(days) +
        " ORDER BY record_date";

    auto conn = db_->rawConnection();
    pqxx::work txn(*conn);
    auto result = txn.exec_params(query, device_id_);
    txn.commit();
    return resultToJson(result);
}

Json::Value QueryService::getStatistics(const std::string& start, const std::string& end) {
    auto conn = db_->rawConnection();
    pqxx::work txn(*conn);
    auto result = txn.exec_params(R"(
        SELECT COUNT(*) as total_nights,
               ROUND(AVG(ahi)::numeric, 2) as avg_ahi,
               ROUND(MIN(ahi)::numeric, 2) as min_ahi,
               ROUND(MAX(ahi)::numeric, 2) as max_ahi,
               ROUND(STDDEV(ahi)::numeric, 2) as stddev_ahi,
               ROUND(AVG(duration_minutes)::numeric, 1) as avg_duration_min,
               ROUND(AVG(leak_95)::numeric, 1) as avg_leak_95,
               ROUND(AVG(mask_press_95)::numeric, 1) as avg_pressure_95,
               ROUND(AVG(spo2_50)::numeric, 1) as avg_spo2,
               ROUND(SUM(CASE WHEN duration_minutes >= 240 THEN 1 ELSE 0 END) * 100.0 /
                     NULLIF(COUNT(*), 0), 1) as compliance_pct,
               SUM(duration_minutes) as total_therapy_minutes
        FROM cpap_daily_summary
        WHERE device_id = $1
          AND record_date >= $2::date AND record_date <= $3::date
    )", device_id_, start, end);
    txn.commit();
    return resultToJson(result);
}

Json::Value QueryService::getSummaries(const std::string& period, int limit) {
    auto conn = db_->rawConnection();
    pqxx::work txn(*conn);

    std::string query = R"(
        SELECT id, period, range_start, range_end, nights_count,
               avg_ahi, avg_usage_hours, compliance_pct, summary_text, created_at
        FROM cpap_summaries
        WHERE device_id = $1
    )";
    if (!period.empty()) query += " AND period = '" + period + "'";
    query += " ORDER BY created_at DESC LIMIT " + std::to_string(limit);

    auto result = txn.exec_params(query, device_id_);
    txn.commit();
    return resultToJson(result);
}

} // namespace hms_cpap
