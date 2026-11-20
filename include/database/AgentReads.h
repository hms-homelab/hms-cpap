#pragma once
//
// SDD-072 D4: what the chat agent reads besides a night's facts, through
// IDatabase, so it runs on SQLite, MySQL and PostgreSQL alike. The tools hold
// no SQL and no connection of their own; the SQL is here, written once with
// the dialect helpers.
//
#include "database/IDatabase.h"

#include <optional>
#include <stdexcept>
#include <string>
#include <vector>

namespace hms_cpap {
namespace agent_reads {

/// One cpap_daily_summary row: the machine's own day (the STR) where it has
/// one, the sessions' aggregate otherwise. Absent columns stay absent.
struct DayRow {
    std::string record_date;   ///< YYYY-MM-DD
    bool graded = true;        ///< SDD-024 index_kind
    std::optional<double> duration_minutes;
    std::optional<double> ahi, hi, ai, oai, cai, uai, rin;
    std::optional<double> leak_50, leak_95, leak_max;
    std::optional<double> mask_press_50, mask_press_95, mask_press_max;
    std::optional<double> therapy_press_50, therapy_press_95, therapy_press_max;
    std::optional<double> spo2_50, spo2_95;
    std::optional<double> resp_rate_50, tid_vol_50, min_vent_50;
    std::optional<int> mode;
    std::optional<double> epr_level, pressure_setting;
};

/// A read that did not run: the engine's message.
struct ReadError : std::runtime_error {
    using std::runtime_error::runtime_error;
};

/// The device's daily rows with record_date in [start_day, end_day], oldest
/// first. Throws ReadError when the query did not run.
std::vector<DayRow> dailyRows(IDatabase& db, const std::string& device_id,
                              const std::string& start_day, const std::string& end_day);

/// One minute of a night's cpap_vitals.
struct VitalsMinute {
    std::string minute;   ///< "YYYY-MM-DD HH:MM:00"
    std::optional<double> avg_spo2, min_spo2, avg_hr, min_hr, max_hr;
};

/// The night's vitals, one row per minute. Throws ReadError.
std::vector<VitalsMinute> vitalsByMinute(IDatabase& db, const std::string& device_id,
                                         const std::string& sleep_day);

/// One stored event of a night.
struct NightEvent {
    std::string type;
    std::string timestamp;
    std::optional<double> duration_seconds;
    std::string details;
};

/// The night's events in time order. Throws ReadError.
std::vector<NightEvent> nightEvents(IDatabase& db, const std::string& device_id,
                                    const std::string& sleep_day);

}  // namespace agent_reads
}  // namespace hms_cpap
