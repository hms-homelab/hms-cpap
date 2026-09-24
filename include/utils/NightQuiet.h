#pragma once
//
// SDD-046: a night is over after an hour with no growth, on every live path.
//
// A session still closes on the burst that finds its files unchanged (its
// session_end), which on a 65 s cycle can be a pause of a minute in the middle
// of the night. The NIGHT is over only when nothing in its folder has grown for
// kNightQuiet: a mask back on within the hour is the same night, which is also
// how ResMed draws a closed night. That moment, not the session close, is when
// the night is announced: the STR is read once, the outcome is published and
// SleepHQ gets the night, whether or not the STR carries it yet.
//
// One small table, cpap_night_quiet, per (device, date folder): the last time a
// burst saw the night grow, the newest session start in it, when it was
// announced, and the STR signature it was announced with. Times are epoch
// seconds, so the one table reads the same on SQLite, MySQL and PostgreSQL.
// Built on executeQuery and the sql:: helpers like SessionEnd.h, so it is one
// implementation for all three engines and the local path, which keeps no
// folder ledger, gets the same rule as a card.
//
#include "database/IDatabase.h"
#include "database/SqlDialect.h"
#include "parsers/CpapdashBridge.h"

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <map>
#include <string>
#include <vector>

namespace hms_cpap {

/// SDD-046 D1 (Albin, 2026-09-24): an hour, ResMed's own idea of a closed night.
inline constexpr std::chrono::minutes kNightQuiet{60};

/// One night's row in cpap_night_quiet.
struct QuietNight {
    std::string date_folder;
    long long   last_growth  = 0;   ///< epoch s of the last burst that saw it grow
    long long   newest_start = 0;   ///< epoch s of the newest session start in it
    long long   announced    = 0;   ///< epoch s it was announced; 0 = not yet
    std::string str_sig;            ///< its STR record when announced, "" = none
};

namespace night_quiet {

inline long long epochOf(std::chrono::system_clock::time_point t) {
    return std::chrono::duration_cast<std::chrono::seconds>(t.time_since_epoch()).count();
}

inline long long asLong(const Json::Value& v) {
    if (v.isNull()) return 0;
    if (v.isString()) return std::atoll(v.asCString());
    return v.asInt64();
}

/// Idempotent, and cheap enough to run before each read or write: the table
/// is tiny and the statement is a no-op once it exists.
inline void ensureTable(IDatabase& db) {
    db.executeQuery(
        "CREATE TABLE IF NOT EXISTS cpap_night_quiet ("
        " device_id VARCHAR(128) NOT NULL,"
        " date_folder VARCHAR(8) NOT NULL,"
        " last_growth BIGINT NOT NULL DEFAULT 0,"
        " newest_start BIGINT NOT NULL DEFAULT 0,"
        " announced BIGINT NOT NULL DEFAULT 0,"
        " str_sig VARCHAR(255) NOT NULL DEFAULT '',"
        " PRIMARY KEY (device_id, date_folder))",
        {});
}

inline std::vector<QuietNight> rowsOf(const Json::Value& rows) {
    std::vector<QuietNight> out;
    for (const auto& r : rows) {
        QuietNight n;
        n.date_folder  = r["date_folder"].asString();
        n.last_growth  = asLong(r["last_growth"]);
        n.newest_start = asLong(r["newest_start"]);
        n.announced    = asLong(r["announced"]);
        n.str_sig      = r["str_sig"].isNull() ? "" : r["str_sig"].asString();
        out.push_back(std::move(n));
    }
    return out;
}

}  // namespace night_quiet

/// A burst saw [date_folder] grow: a new session stored, or a session's files
/// changed. The hour starts again, and a night already announced is announced
/// again once it settles, because what it grew into is a night nobody has seen.
inline bool noteNightGrowth(IDatabase& db, const std::string& device_id,
                            const std::string& date_folder,
                            std::chrono::system_clock::time_point session_start,
                            std::chrono::system_clock::time_point now) {
    night_quiet::ensureTable(db);
    const DbType dt = db.dbType();
    const std::string cols =
        "INSERT INTO cpap_night_quiet (device_id, date_folder, last_growth, newest_start) "
        "VALUES (" + sql::param(1, dt) + ", " + sql::param(2, dt) + ", " + sql::param(3, dt) +
        ", " + sql::param(4, dt) + ") ";
    const std::string upsert = dt == DbType::MYSQL
        ? "ON DUPLICATE KEY UPDATE last_growth = VALUES(last_growth), "
          "newest_start = GREATEST(newest_start, VALUES(newest_start)), announced = 0"
        : "ON CONFLICT (device_id, date_folder) DO UPDATE SET "
          "last_growth = excluded.last_growth, "
          "newest_start = CASE WHEN excluded.newest_start > cpap_night_quiet.newest_start "
          "THEN excluded.newest_start ELSE cpap_night_quiet.newest_start END, "
          "announced = 0";
    return db.executeQueryChecked(cols + upsert,
                                  {device_id, date_folder,
                                   std::to_string(night_quiet::epochOf(now)),
                                   std::to_string(night_quiet::epochOf(session_start))})
        .ok;
}

/// A burst saw [date_folder] without growth. Only a night with no row yet is
/// written, and its hour starts now. A night the collector had already closed
/// before this rule existed ([already_closed]) is recorded as announced: it was
/// announced at its close, and must not be announced a second time an hour
/// after the upgrade.
inline bool noteNightSeen(IDatabase& db, const std::string& device_id,
                          const std::string& date_folder,
                          std::chrono::system_clock::time_point session_start,
                          std::chrono::system_clock::time_point now, bool already_closed) {
    night_quiet::ensureTable(db);
    const DbType dt = db.dbType();
    const std::string head = dt == DbType::MYSQL ? "INSERT IGNORE INTO " : "INSERT INTO ";
    const std::string tail =
        dt == DbType::MYSQL ? "" : " ON CONFLICT (device_id, date_folder) DO NOTHING";
    const std::string now_s = std::to_string(night_quiet::epochOf(now));
    return db.executeQueryChecked(
                 head + "cpap_night_quiet (device_id, date_folder, last_growth, newest_start, "
                        "announced) VALUES (" + sql::param(1, dt) + ", " + sql::param(2, dt) +
                     ", " + sql::param(3, dt) + ", " + sql::param(4, dt) + ", " +
                     sql::param(5, dt) + ")" + tail,
                 {device_id, date_folder, now_s,
                  std::to_string(night_quiet::epochOf(session_start)),
                  already_closed ? now_s : "0"})
        .ok;
}

/// The nights not announced yet whose last growth is at least kNightQuiet old
/// at [now], newest first.
inline std::vector<QuietNight> quietUnannouncedNights(IDatabase& db, const std::string& device_id,
                                                      std::chrono::system_clock::time_point now) {
    night_quiet::ensureTable(db);
    const DbType dt = db.dbType();
    const auto cutoff = now - kNightQuiet;
    return night_quiet::rowsOf(db.executeQuery(
        "SELECT date_folder, last_growth, newest_start, announced, str_sig "
        "FROM cpap_night_quiet WHERE device_id = " + sql::param(1, dt) +
            " AND announced = 0 AND last_growth <= " + sql::param(2, dt) +
            " ORDER BY date_folder DESC",
        {device_id, std::to_string(night_quiet::epochOf(cutoff))}));
}

/// The newest night already announced, if any.
inline std::vector<QuietNight> newestAnnouncedNight(IDatabase& db, const std::string& device_id) {
    night_quiet::ensureTable(db);
    const DbType dt = db.dbType();
    return night_quiet::rowsOf(db.executeQuery(
        "SELECT date_folder, last_growth, newest_start, announced, str_sig "
        "FROM cpap_night_quiet WHERE device_id = " + sql::param(1, dt) +
            " AND announced > 0 ORDER BY date_folder DESC LIMIT 1",
        {device_id}));
}

/// Has each night's hour passed, by date folder? Only folders with a row are
/// in the map: a folder with none predates SDD-046 and was closed under the
/// old rule, so readers take a missing entry as passed (nightHourPassed).
inline std::map<std::string, bool> nightHourStates(IDatabase& db, const std::string& device_id) {
    night_quiet::ensureTable(db);
    const DbType dt = db.dbType();
    std::map<std::string, bool> out;
    for (const auto& n : night_quiet::rowsOf(db.executeQuery(
             "SELECT date_folder, last_growth, newest_start, announced, str_sig "
             "FROM cpap_night_quiet WHERE device_id = " + sql::param(1, dt),
             {device_id}))) {
        out[n.date_folder] = n.announced > 0;
    }
    return out;
}

inline bool nightHourPassed(const std::map<std::string, bool>& states,
                            const std::string& date_folder) {
    const auto it = states.find(date_folder);
    return it == states.end() || it->second;
}

/// Record the announcement, with the STR signature it was made on.
inline bool markNightAnnounced(IDatabase& db, const std::string& device_id,
                               const std::string& date_folder,
                               std::chrono::system_clock::time_point now,
                               const std::string& str_sig) {
    night_quiet::ensureTable(db);
    const DbType dt = db.dbType();
    return db.executeQueryChecked(
                 "UPDATE cpap_night_quiet SET announced = " + sql::param(1, dt) +
                     ", str_sig = " + sql::param(2, dt) + " WHERE device_id = " +
                     sql::param(3, dt) + " AND date_folder = " + sql::param(4, dt),
                 {std::to_string(night_quiet::epochOf(now)), str_sig, device_id, date_folder})
        .ok;
}

/// SDD-046 D4: the figures of an STR day record that a later STR can move. Two
/// equal signatures are the same night as far as anything we publish is
/// concerned; "" means the STR has no record for the day.
inline std::string strSignature(const STRDailyRecord& r) {
    char buf[256];
    std::snprintf(buf, sizeof(buf),
                  "%.1f|%.2f|%.2f|%.2f|%.2f|%.2f|%.2f|%.2f|%.2f|%.1f|%.2f|%.2f|%.2f|%.2f|%.2f|%d",
                  r.duration_minutes, r.patient_hours, r.ahi, r.hi, r.ai, r.oai, r.cai, r.uai,
                  r.rin, r.csr, r.mask_press_50, r.mask_press_95, r.leak_50, r.leak_95,
                  r.resp_rate_50, r.mask_events);
    return buf;
}

}  // namespace hms_cpap
