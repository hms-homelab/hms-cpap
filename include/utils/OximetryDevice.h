#pragma once

#include "utils/TimeCompat.h"

#include <chrono>
#include <ctime>
#include <string>

namespace hms_cpap {

/**
 * The device_id every oximetry row is stored under.
 *
 * Oximetry is namespaced by device in the schema (oximetry_sessions.device_id
 * is NOT NULL), but there is only ever one logical ring: the BLE path, the file
 * import and the CSV upload all write the same value, and every reader filters
 * on it. Before this constant that agreement was nine unrelated string literals
 * in eight files, and a mismatch between any writer and any reader would have
 * produced an empty chart with nothing logged — the same silent-empty failure
 * as issue #18.
 *
 * NOT interchangeable with two other "o2ring" strings in this codebase:
 *   - O2RingBleClient's KNOWN_NAME_SUBSTRINGS, which matches what a ring
 *     ADVERTISES over BLE ("o2ring", "checkme", "viatom", ...). That is device
 *     discovery, it is deliberately a list of vendor names, and it has nothing
 *     to do with how a session is keyed once stored.
 *   - The "o2ring" object in config.json (AppConfig). That is a serialization
 *     contract with files already on disk; renaming this constant must never
 *     rename that key.
 */
inline constexpr const char* kOximetryDeviceId = "o2ring";

/**
 * The clock a live O2 Ring sample is stamped on.
 *
 * Every ring intake (.vld, O2Ring-S file, CSV, mule/bridge or BLE download)
 * stores the ring's own printed wall clock, which the parsers read AS IF it were
 * UTC (timegm) and the writers render back with gmtime. A live poll carries no
 * ring time, so the nearest equivalent is this host's local wall clock, put into
 * that same shape. Stamping with the database's NOW() instead followed the
 * database server's time zone (UTC in most Docker setups) on PostgreSQL and
 * MySQL, which put the live samples hours away from the CPAP data and from the
 * ring's own file for the same night.
 */
inline std::chrono::system_clock::time_point liveOximetryWallClock(
    std::chrono::system_clock::time_point now) {
    const std::time_t t = std::chrono::system_clock::to_time_t(now);
    std::tm local{};
    localtime_r(&t, &local);
    return std::chrono::system_clock::from_time_t(timegm_utc(&local));
}

/// live_<YYYYMMDD>.vld: the synthetic session a day's live samples collect in,
/// dated on the same wall clock as the samples themselves.
inline std::string liveOximetryFilename(std::chrono::system_clock::time_point wall_clock) {
    const std::time_t t = std::chrono::system_clock::to_time_t(wall_clock);
    std::tm tm{};
    gmtime_r(&t, &tm);
    char buf[9];
    std::strftime(buf, sizeof(buf), "%Y%m%d", &tm);
    return std::string("live_") + buf + ".vld";
}

inline bool isLiveOximetryFilename(const std::string& filename) {
    return filename.rfind("live_", 0) == 0;
}

/// The sleep day ("YYYY-MM-DD", the date 12 hours before) of a ring wall-clock
/// time_point, the same key sql::sleepDay computes from a stored start_time.
inline std::string oximetrySleepDay(std::chrono::system_clock::time_point wall_clock) {
    const std::time_t t =
        std::chrono::system_clock::to_time_t(wall_clock - std::chrono::hours(12));
    std::tm tm{};
    gmtime_r(&t, &tm);
    char buf[11];
    std::strftime(buf, sizeof(buf), "%Y-%m-%d", &tm);
    return buf;
}

}  // namespace hms_cpap
