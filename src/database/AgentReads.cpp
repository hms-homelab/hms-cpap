#include "database/AgentReads.h"

#include "database/SqlDialect.h"

#include <cstdlib>

namespace hms_cpap {
namespace agent_reads {

namespace {

std::optional<double> optDouble(const Json::Value& row, const char* key) {
    const auto& v = row[key];
    if (v.isNull()) return std::nullopt;
    const std::string s = v.asString();
    if (s.empty()) return std::nullopt;
    char* end = nullptr;
    const double d = std::strtod(s.c_str(), &end);
    if (end == s.c_str()) return std::nullopt;
    return d;
}

std::optional<int> optInt(const Json::Value& row, const char* key) {
    const auto d = optDouble(row, key);
    if (!d) return std::nullopt;
    return static_cast<int>(*d);
}

std::string text(const Json::Value& row, const char* key) {
    const auto& v = row[key];
    return v.isNull() ? std::string() : v.asString();
}

Json::Value run(IDatabase& db, const std::string& sql, const std::vector<std::string>& params) {
    auto out = db.executeQueryChecked(sql, params);
    if (!out.ok) throw ReadError(out.error.empty() ? "query failed" : out.error);
    return out.rows;
}

}  // namespace

std::vector<DayRow> dailyRows(IDatabase& db, const std::string& device_id,
                              const std::string& start_day, const std::string& end_day) {
    const DbType t = db.dbType();
    const std::string sql =
        "SELECT record_date, duration_minutes, ahi, hi, ai, oai, cai, uai, rin,"
        " leak_50, leak_95, leak_max, mask_press_50, mask_press_95, mask_press_max,"
        " therapy_press_50, therapy_press_95, therapy_press_max, spo2_50, spo2_95,"
        " resp_rate_50, tid_vol_50, min_vent_50, mode, epr_level, pressure_setting,"
        " COALESCE(index_kind, 'ahi') AS index_kind"
        " FROM cpap_daily_summary"
        " WHERE device_id = " + sql::param(1, t) +
        " AND record_date >= " + sql::castDate(2, t) +
        " AND record_date <= " + sql::castDate(3, t) +
        " ORDER BY record_date";
    std::vector<DayRow> rows;
    for (const auto& r : run(db, sql, {device_id, start_day, end_day})) {
        DayRow d;
        d.record_date = text(r, "record_date").substr(0, 10);
        d.graded = text(r, "index_kind") != "ungraded";
        d.duration_minutes = optDouble(r, "duration_minutes");
        d.ahi = optDouble(r, "ahi");
        d.hi = optDouble(r, "hi");
        d.ai = optDouble(r, "ai");
        d.oai = optDouble(r, "oai");
        d.cai = optDouble(r, "cai");
        d.uai = optDouble(r, "uai");
        d.rin = optDouble(r, "rin");
        d.leak_50 = optDouble(r, "leak_50");
        d.leak_95 = optDouble(r, "leak_95");
        d.leak_max = optDouble(r, "leak_max");
        d.mask_press_50 = optDouble(r, "mask_press_50");
        d.mask_press_95 = optDouble(r, "mask_press_95");
        d.mask_press_max = optDouble(r, "mask_press_max");
        d.therapy_press_50 = optDouble(r, "therapy_press_50");
        d.therapy_press_95 = optDouble(r, "therapy_press_95");
        d.therapy_press_max = optDouble(r, "therapy_press_max");
        d.spo2_50 = optDouble(r, "spo2_50");
        d.spo2_95 = optDouble(r, "spo2_95");
        d.resp_rate_50 = optDouble(r, "resp_rate_50");
        d.tid_vol_50 = optDouble(r, "tid_vol_50");
        d.min_vent_50 = optDouble(r, "min_vent_50");
        d.mode = optInt(r, "mode");
        d.epr_level = optDouble(r, "epr_level");
        d.pressure_setting = optDouble(r, "pressure_setting");
        rows.push_back(std::move(d));
    }
    return rows;
}

std::vector<VitalsMinute> vitalsByMinute(IDatabase& db, const std::string& device_id,
                                         const std::string& sleep_day) {
    const DbType t = db.dbType();
    const std::string minute = sql::minuteText("v.timestamp", t);
    const std::string sql =
        "SELECT " + minute + " AS minute,"
        " " + sql::round("AVG(v.spo2)", 1, t) + " AS avg_spo2,"
        " MIN(v.spo2) AS min_spo2,"
        " " + sql::round("AVG(v.heart_rate)", 1, t) + " AS avg_hr,"
        " MIN(v.heart_rate) AS min_hr,"
        " MAX(v.heart_rate) AS max_hr"
        " FROM cpap_vitals v"
        " JOIN cpap_sessions s ON v.session_id = s.id"
        " WHERE s.device_id = " + sql::param(1, t) +
        " AND " + sql::sleepDay("s.session_start", t) + " = " + sql::castDate(2, t) +
        " GROUP BY " + minute +
        " ORDER BY " + minute;
    std::vector<VitalsMinute> out;
    for (const auto& r : run(db, sql, {device_id, sleep_day})) {
        VitalsMinute v;
        v.minute = text(r, "minute");
        v.avg_spo2 = optDouble(r, "avg_spo2");
        v.min_spo2 = optDouble(r, "min_spo2");
        v.avg_hr = optDouble(r, "avg_hr");
        v.min_hr = optDouble(r, "min_hr");
        v.max_hr = optDouble(r, "max_hr");
        out.push_back(std::move(v));
    }
    return out;
}

std::vector<NightEvent> nightEvents(IDatabase& db, const std::string& device_id,
                                    const std::string& sleep_day) {
    const DbType t = db.dbType();
    const std::string sql =
        "SELECT e.event_type, " + sql::tsText("e.event_timestamp", t) + " AS event_timestamp,"
        " e.duration_seconds, e.details"
        " FROM cpap_events e"
        " JOIN cpap_sessions s ON e.session_id = s.id"
        " WHERE s.device_id = " + sql::param(1, t) +
        " AND " + sql::sleepDay("s.session_start", t) + " = " + sql::castDate(2, t) +
        " ORDER BY e.event_timestamp";
    std::vector<NightEvent> out;
    for (const auto& r : run(db, sql, {device_id, sleep_day})) {
        NightEvent e;
        e.type = text(r, "event_type");
        e.timestamp = text(r, "event_timestamp");
        e.duration_seconds = optDouble(r, "duration_seconds");
        e.details = text(r, "details");
        out.push_back(std::move(e));
    }
    return out;
}

}  // namespace agent_reads
}  // namespace hms_cpap
