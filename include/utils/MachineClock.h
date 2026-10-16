#pragma once
//
// SDD-054 D3: the machine's clock, learned from its own card.
//
// The DATALOG folder names, and so "tonight" and "noon", come from the
// machine's clock, which need not match the host's: travel, a clock never set,
// a wrong DST. Two things are learned from a fresh listing of a night folder:
//
//   drift  An AirSense writes FAT stamps in UTC and file NAMES in its local
//          time. When a file's stamp moved since the previous listing of the
//          folder, the machine wrote it within the last burst, so
//          stamp - host time is how far the machine's UTC is off the host's,
//          rounded to 15 minutes.
//   zone   For each file, name time - stamp is the zone minus how long after
//          the session's start the file was last written, so the largest value
//          (the CSL, closed at the start) is the zone. Rounded to 15 minutes
//          and accepted only within 120 s of a step. Only a fresh listing
//          teaches it: an old folder carries the zone it was written in.
//
// machine_now = host_now + drift + zone. Until a zone is learned the HOST's
// zone stands in (Albin, 2026-10-15: a home install's own zone is the best
// first guess).
//
// A local folder learns the same way when its files keep the card's stamps
// (a card reader, a copy that preserves mtime); when they do not, no stamp
// moves in step with the machine and the host zone stays.
//
// Stored in cpap_machine_clock, one row per device, created at runtime like
// cpap_night_quiet.
//
#include "clients/EzShareClient.h"
#include "database/IDatabase.h"
#include "database/SqlDialect.h"
#include "utils/TimeCompat.h"

#include <cstdlib>
#include <ctime>
#include <map>
#include <optional>
#include <string>
#include <vector>

namespace hms_cpap {

struct MachineClock {
    long long drift_s{0};
    long long zone_s{0};
    bool      zone_known{false};
};

namespace machine_clock {

constexpr long long kStep = 15 * 60;
constexpr long long kZoneTolerance = 120;

inline long long roundToStep(long long v) {
    const long long half = kStep / 2;
    return (v >= 0 ? (v + half) / kStep : -((-v + half) / kStep)) * kStep;
}

/// A listing entry's stamp read in a fixed UTC frame; 0 when unusable.
inline long long stampOf(const EzShareFileEntry& e) {
    if (e.year < 1980 || e.month < 1 || e.month > 12 || e.day < 1 || e.day > 31) return 0;
    std::tm tm{};
    tm.tm_year = e.year - 1900;
    tm.tm_mon  = e.month - 1;
    tm.tm_mday = e.day;
    tm.tm_hour = e.hour;
    tm.tm_min  = e.minute;
    tm.tm_sec  = e.second;
    return static_cast<long long>(timegm_utc(&tm));
}

/// The YYYYMMDD_HHMMSS a session file is named for, read in a fixed UTC frame;
/// 0 when the name has none.
inline long long nameTimeOf(const std::string& name) {
    if (name.size() < 16 || name[8] != '_') return 0;
    for (int i = 0; i < 15; ++i)
        if (i != 8 && (name[i] < '0' || name[i] > '9')) return 0;
    std::tm tm{};
    tm.tm_year = std::atoi(name.substr(0, 4).c_str()) - 1900;
    tm.tm_mon  = std::atoi(name.substr(4, 2).c_str()) - 1;
    tm.tm_mday = std::atoi(name.substr(6, 2).c_str());
    tm.tm_hour = std::atoi(name.substr(9, 2).c_str());
    tm.tm_min  = std::atoi(name.substr(11, 2).c_str());
    tm.tm_sec  = std::atoi(name.substr(13, 2).c_str());
    return static_cast<long long>(timegm_utc(&tm));
}

}  // namespace machine_clock

/// The host's own offset from UTC at [now], in seconds.
inline long long hostZoneOffset(long long now) {
    const std::time_t t = static_cast<std::time_t>(now);
    std::tm local{};
    localtime_r(&t, &local);
    return static_cast<long long>(timegm_utc(&local)) - now;
}

/// machine_now - host_now: the drift plus the learned zone, or the host's zone.
inline long long machineOffset(const MachineClock& c, long long now) {
    return c.drift_s + (c.zone_known ? c.zone_s : hostZoneOffset(now));
}

/// The stamps of one listing, by file name, for the next listing to compare.
inline std::map<std::string, long long> stampsOf(const std::vector<EzShareFileEntry>& files) {
    std::map<std::string, long long> out;
    for (const auto& f : files)
        if (!f.is_dir) out[f.name] = machine_clock::stampOf(f);
    return out;
}

/// Learn from a listing of a night folder taken at [host_now], given the
/// previous listing's stamps. Returns the updated clock, or nullopt when the
/// listing is not fresh (no stamp moved) and so teaches nothing.
inline std::optional<MachineClock> learnMachineClock(
    const MachineClock& prev, const std::map<std::string, long long>& prev_stamps,
    const std::vector<EzShareFileEntry>& files, long long host_now) {

    long long newest_moved = 0;
    for (const auto& f : files) {
        if (f.is_dir) continue;
        const long long s = machine_clock::stampOf(f);
        const auto it = prev_stamps.find(f.name);
        if (s == 0 || it == prev_stamps.end() || it->second == s) continue;
        if (s > newest_moved) newest_moved = s;
    }
    if (newest_moved == 0) return std::nullopt;

    MachineClock next = prev;
    next.drift_s = machine_clock::roundToStep(newest_moved - host_now);

    bool any = false;
    long long best = 0;
    for (const auto& f : files) {
        if (f.is_dir) continue;
        const long long n = machine_clock::nameTimeOf(f.name);
        const long long s = machine_clock::stampOf(f);
        if (n == 0 || s == 0) continue;
        const long long v = n - s;
        if (!any || v > best) best = v;
        any = true;
    }
    if (any) {
        const long long z = machine_clock::roundToStep(best);
        if (std::llabs(best - z) <= machine_clock::kZoneTolerance) {
            next.zone_s = z;
            next.zone_known = true;
        }
    }
    return next;
}

// ── storage ──────────────────────────────────────────────────────────────────

namespace machine_clock {

inline void ensureTable(IDatabase& db) {
    db.executeQuery(
        "CREATE TABLE IF NOT EXISTS cpap_machine_clock ("
        " device_id VARCHAR(128) NOT NULL PRIMARY KEY,"
        " drift_s BIGINT NOT NULL DEFAULT 0,"
        " zone_s BIGINT NOT NULL DEFAULT 0,"
        " zone_known INTEGER NOT NULL DEFAULT 0,"
        " updated_at BIGINT NOT NULL DEFAULT 0)",
        {});
}

inline long long asLong(const Json::Value& v) {
    if (v.isNull()) return 0;
    if (v.isString()) return std::atoll(v.asCString());
    return v.asInt64();
}

}  // namespace machine_clock

inline MachineClock loadMachineClock(IDatabase& db, const std::string& device_id) {
    machine_clock::ensureTable(db);
    MachineClock c;
    const auto rows = db.executeQuery(
        "SELECT drift_s, zone_s, zone_known FROM cpap_machine_clock WHERE device_id = " +
            sql::param(1, db.dbType()),
        {device_id});
    if (rows.isArray() && !rows.empty()) {
        c.drift_s    = machine_clock::asLong(rows[0]["drift_s"]);
        c.zone_s     = machine_clock::asLong(rows[0]["zone_s"]);
        c.zone_known = machine_clock::asLong(rows[0]["zone_known"]) != 0;
    }
    return c;
}

inline bool saveMachineClock(IDatabase& db, const std::string& device_id,
                             const MachineClock& c, long long now) {
    machine_clock::ensureTable(db);
    const DbType dt = db.dbType();
    const std::string ins =
        "INSERT INTO cpap_machine_clock (device_id, drift_s, zone_s, zone_known, updated_at) "
        "VALUES (" + sql::param(1, dt) + ", " + sql::param(2, dt) + ", " + sql::param(3, dt) +
        ", " + sql::param(4, dt) + ", " + sql::param(5, dt) + ") ";
    const std::string upsert = dt == DbType::MYSQL
        ? "ON DUPLICATE KEY UPDATE drift_s = VALUES(drift_s), zone_s = VALUES(zone_s), "
          "zone_known = VALUES(zone_known), updated_at = VALUES(updated_at)"
        : "ON CONFLICT (device_id) DO UPDATE SET drift_s = excluded.drift_s, "
          "zone_s = excluded.zone_s, zone_known = excluded.zone_known, "
          "updated_at = excluded.updated_at";
    return db.executeQueryChecked(ins + upsert,
                                  {device_id, std::to_string(c.drift_s), std::to_string(c.zone_s),
                                   c.zone_known ? "1" : "0", std::to_string(now)})
        .ok;
}

}  // namespace hms_cpap
