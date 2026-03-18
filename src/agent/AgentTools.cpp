#include "agent/AgentTools.h"
#include <iostream>
#include <sstream>
#include <stdexcept>

namespace hms_cpap {

AgentTools::AgentTools(const std::string& device_id)
    : device_id_(device_id) {
    buildDefinitions();
}

const std::vector<hms::ToolDefinition>& AgentTools::definitions() const {
    return definitions_;
}

bool AgentTools::hasTool(const std::string& name) const {
    for (const auto& d : definitions_) {
        if (d.name == name) return true;
    }
    return false;
}

std::string AgentTools::execute(const std::string& name,
                                 const nlohmann::json& arguments,
                                 pqxx::connection& conn) {
    try {
        if (name == "get_recent_sessions")  return getRecentSessions(arguments, conn);
        if (name == "get_session_details")  return getSessionDetails(arguments, conn);
        if (name == "get_daily_summary")    return getDailySummary(arguments, conn);
        if (name == "get_trend_data")       return getTrendData(arguments, conn);
        if (name == "compare_periods")      return comparePeriods(arguments, conn);
        if (name == "get_vitals")           return getVitals(arguments, conn);
        if (name == "get_statistics")       return getStatistics(arguments, conn);
        return R"({"error": "Unknown tool: )" + name + R"("})";
    } catch (const std::exception& e) {
        return std::string(R"({"error": ")") + e.what() + R"("})";
    }
}

void AgentTools::buildDefinitions() {
    definitions_ = {
        {
            "get_recent_sessions",
            "Get recent CPAP therapy sessions with metrics (AHI, leak, pressure, duration).",
            {
                {"type", "object"},
                {"properties", {
                    {"days", {{"type", "integer"}, {"description", "Number of days to look back (default 7)"}}},
                    {"limit", {{"type", "integer"}, {"description", "Maximum sessions to return (default 10)"}}}
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

std::string AgentTools::getRecentSessions(const nlohmann::json& args, pqxx::connection& conn) {
    int days = args.value("days", 7);
    int limit = args.value("limit", 10);

    pqxx::work txn(conn);
    auto result = txn.exec_params(R"(
        SELECT s.session_start, s.session_end, s.duration_seconds,
               ROUND(s.duration_seconds / 3600.0, 2) as duration_hours,
               m.ahi, m.total_events, m.obstructive_apneas, m.central_apneas,
               m.hypopneas, m.reras,
               m.avg_spo2, m.min_spo2,
               m.avg_heart_rate, m.min_heart_rate, m.max_heart_rate
        FROM cpap_sessions s
        LEFT JOIN cpap_session_metrics m ON m.session_id = s.id
        WHERE s.device_id = $1
          AND s.session_start >= NOW() - make_interval(days := $2)
        ORDER BY s.session_start DESC
        LIMIT $3
    )", device_id_, days, limit);
    txn.commit();

    return resultToJson(result);
}

std::string AgentTools::getSessionDetails(const nlohmann::json& args, pqxx::connection& conn) {
    std::string date = args.at("date").get<std::string>();

    pqxx::work txn(conn);

    // Get session + metrics for the given date (noon-to-noon sleep day)
    auto sessions = txn.exec_params(R"(
        SELECT s.id, s.session_start, s.session_end, s.duration_seconds,
               ROUND(s.duration_seconds / 3600.0, 2) as duration_hours,
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

    // Get events for those sessions
    nlohmann::json result = nlohmann::json::array();
    for (const auto& row : sessions) {
        nlohmann::json session_json;
        for (int i = 0; i < row.size(); ++i) {
            if (row[i].is_null()) {
                session_json[sessions.column_name(i)] = nullptr;
            } else {
                session_json[sessions.column_name(i)] = row[i].c_str();
            }
        }

        int session_id = row["id"].as<int>();
        auto events = txn.exec_params(R"(
            SELECT event_type, event_timestamp, duration_seconds, details
            FROM cpap_events
            WHERE session_id = $1
            ORDER BY event_timestamp
        )", session_id);

        nlohmann::json events_json = nlohmann::json::array();
        for (const auto& e : events) {
            nlohmann::json ej;
            for (int i = 0; i < e.size(); ++i) {
                ej[events.column_name(i)] = e[i].is_null() ? nlohmann::json(nullptr) : nlohmann::json(e[i].c_str());
            }
            events_json.push_back(ej);
        }
        session_json["events"] = events_json;
        result.push_back(session_json);
    }

    txn.commit();
    return result.dump();
}

std::string AgentTools::getDailySummary(const nlohmann::json& args, pqxx::connection& conn) {
    std::string start_date = args.at("start_date").get<std::string>();
    std::string end_date = args.at("end_date").get<std::string>();

    pqxx::work txn(conn);
    auto result = txn.exec_params(R"(
        SELECT record_date, duration_minutes, ahi, hi, ai, oai, cai, uai, rin,
               leak_50, leak_95, leak_max,
               mask_press_50, mask_press_95, mask_press_max,
               spo2_50, spo2_95,
               resp_rate_50, tid_vol_50, min_vent_50,
               mode, epr_level, pressure_setting
        FROM cpap_daily_summary
        WHERE device_id = $1
          AND record_date >= $2::date
          AND record_date <= $3::date
        ORDER BY record_date
    )", device_id_, start_date, end_date);
    txn.commit();

    return resultToJson(result);
}

std::string AgentTools::getTrendData(const nlohmann::json& args, pqxx::connection& conn) {
    std::string metric = args.at("metric").get<std::string>();
    int days = args.value("days", 30);

    // Map metric name to column(s) in cpap_daily_summary
    std::string columns;
    if (metric == "ahi")       columns = "record_date, ahi, hi, ai, oai, cai";
    else if (metric == "pressure")  columns = "record_date, mask_press_50, mask_press_95, mask_press_max, pressure_setting";
    else if (metric == "leak")      columns = "record_date, leak_50, leak_95, leak_max";
    else if (metric == "spo2")      columns = "record_date, spo2_50, spo2_95";
    else if (metric == "hr")        columns = "record_date, resp_rate_50";
    else if (metric == "duration")  columns = "record_date, duration_minutes";
    else return R"({"error": "Unknown metric. Use: ahi, pressure, leak, spo2, hr, duration"})";

    std::string query = "SELECT " + columns + " FROM cpap_daily_summary"
        " WHERE device_id = $1 AND record_date >= NOW() - make_interval(days := $2)"
        " ORDER BY record_date";

    pqxx::work txn(conn);
    auto result = txn.exec_params(query, device_id_, days);
    txn.commit();

    return resultToJson(result);
}

std::string AgentTools::comparePeriods(const nlohmann::json& args, pqxx::connection& conn) {
    std::string p1_start = args.at("period1_start").get<std::string>();
    std::string p1_end = args.at("period1_end").get<std::string>();
    std::string p2_start = args.at("period2_start").get<std::string>();
    std::string p2_end = args.at("period2_end").get<std::string>();

    pqxx::work txn(conn);

    auto aggregate = [&](const std::string& start, const std::string& end) {
        return txn.exec_params(R"(
            SELECT COUNT(*) as nights,
                   ROUND(AVG(ahi)::numeric, 2) as avg_ahi,
                   ROUND(AVG(duration_minutes)::numeric, 1) as avg_duration_min,
                   ROUND(AVG(leak_95)::numeric, 1) as avg_leak_95,
                   ROUND(AVG(mask_press_95)::numeric, 1) as avg_pressure_95,
                   ROUND(AVG(spo2_50)::numeric, 1) as avg_spo2,
                   ROUND(AVG(resp_rate_50)::numeric, 1) as avg_resp_rate,
                   ROUND(SUM(CASE WHEN duration_minutes >= 240 THEN 1 ELSE 0 END) * 100.0 /
                         NULLIF(COUNT(*), 0), 1) as compliance_pct
            FROM cpap_daily_summary
            WHERE device_id = $1
              AND record_date >= $2::date
              AND record_date <= $3::date
        )", device_id_, start, end);
    };

    auto r1 = aggregate(p1_start, p1_end);
    auto r2 = aggregate(p2_start, p2_end);
    txn.commit();

    nlohmann::json result;
    auto rowToJson = [](const pqxx::result& r, const std::string& label) -> nlohmann::json {
        nlohmann::json j;
        j["period"] = label;
        if (!r.empty()) {
            for (int i = 0; i < r[0].size(); ++i) {
                j[r.column_name(i)] = r[0][i].is_null() ? nlohmann::json(nullptr) : nlohmann::json(r[0][i].c_str());
            }
        }
        return j;
    };

    result["period1"] = rowToJson(r1, p1_start + " to " + p1_end);
    result["period2"] = rowToJson(r2, p2_start + " to " + p2_end);

    return result.dump();
}

std::string AgentTools::getVitals(const nlohmann::json& args, pqxx::connection& conn) {
    std::string date = args.at("date").get<std::string>();

    pqxx::work txn(conn);
    auto result = txn.exec_params(R"(
        SELECT date_trunc('minute', v.timestamp) as minute,
               ROUND(AVG(v.spo2)::numeric, 1) as avg_spo2,
               MIN(v.spo2) as min_spo2,
               ROUND(AVG(v.heart_rate)::numeric, 1) as avg_hr,
               MIN(v.heart_rate) as min_hr,
               MAX(v.heart_rate) as max_hr
        FROM cpap_vitals v
        JOIN cpap_sessions s ON v.session_id = s.id
        WHERE s.device_id = $1
          AND DATE(s.session_start - INTERVAL '12 hours') = $2::date
        GROUP BY date_trunc('minute', v.timestamp)
        ORDER BY minute
    )", device_id_, date);
    txn.commit();

    return resultToJson(result);
}

std::string AgentTools::getStatistics(const nlohmann::json& args, pqxx::connection& conn) {
    std::string start_date = args.at("start_date").get<std::string>();
    std::string end_date = args.at("end_date").get<std::string>();

    pqxx::work txn(conn);
    auto result = txn.exec_params(R"(
        SELECT COUNT(*) as total_nights,
               ROUND(AVG(ahi)::numeric, 2) as avg_ahi,
               ROUND(MIN(ahi)::numeric, 2) as min_ahi,
               ROUND(MAX(ahi)::numeric, 2) as max_ahi,
               ROUND(STDDEV(ahi)::numeric, 2) as stddev_ahi,
               ROUND(AVG(duration_minutes)::numeric, 1) as avg_duration_min,
               ROUND(MIN(duration_minutes)::numeric, 1) as min_duration_min,
               ROUND(MAX(duration_minutes)::numeric, 1) as max_duration_min,
               ROUND(AVG(leak_95)::numeric, 1) as avg_leak_95,
               ROUND(AVG(mask_press_95)::numeric, 1) as avg_pressure_95,
               ROUND(AVG(spo2_50)::numeric, 1) as avg_spo2,
               ROUND(AVG(resp_rate_50)::numeric, 1) as avg_resp_rate,
               ROUND(SUM(CASE WHEN duration_minutes >= 240 THEN 1 ELSE 0 END) * 100.0 /
                     NULLIF(COUNT(*), 0), 1) as compliance_pct,
               SUM(duration_minutes) as total_therapy_minutes
        FROM cpap_daily_summary
        WHERE device_id = $1
          AND record_date >= $2::date
          AND record_date <= $3::date
    )", device_id_, start_date, end_date);
    txn.commit();

    return resultToJson(result);
}

// ── Helpers ─────────────────────────────────────────────────────────────────

std::string AgentTools::resultToJson(const pqxx::result& result) {
    nlohmann::json arr = nlohmann::json::array();
    for (const auto& row : result) {
        nlohmann::json obj;
        for (int i = 0; i < row.size(); ++i) {
            if (row[i].is_null()) {
                obj[result.column_name(i)] = nullptr;
            } else {
                obj[result.column_name(i)] = row[i].c_str();
            }
        }
        arr.push_back(obj);
    }
    return arr.dump();
}

} // namespace hms_cpap
