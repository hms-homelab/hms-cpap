#pragma once
//
// SDD-072: the engines the AI-facts tests run on, and the nights they seed.
// SQLite always runs; MySQL and PostgreSQL run when their env is set
// (MYSQL_TEST_HOST / PGHOST, as test_NightAggregationBackends), and skip
// otherwise.
//
#include <gtest/gtest.h>

#include "database/IDatabase.h"
#include "database/SQLiteDatabase.h"
#include "database/SqlDialect.h"
#ifdef WITH_MYSQL
#include "database/MySQLDatabase.h"
#endif
#ifdef WITH_POSTGRESQL
#include "database/PostgresDatabase.h"
#endif
#include "utils/OximetryDevice.h"

#include <chrono>
#include <cstdlib>
#include <ctime>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

namespace ai_facts_test {

using namespace hms_cpap;
using cpapdash::parser::EventType;
using cpapdash::parser::SleepEvent;
using cpapdash::parser::VitalSample;
using std::chrono::minutes;
using std::chrono::system_clock;

enum class Engine { SQLite, MySQL, Postgres };

inline const char* engineName(Engine e) {
    switch (e) {
        case Engine::SQLite:   return "SQLite";
        case Engine::MySQL:    return "MySQL";
        case Engine::Postgres: return "Postgres";
    }
    return "?";
}

inline std::string engineParamName(const ::testing::TestParamInfo<Engine>& info) {
    return engineName(info.param);
}

inline std::string envOr(const char* key, const std::string& fallback) {
    const char* v = std::getenv(key);
    return (v && *v) ? std::string(v) : fallback;
}

/// Opens [e]. Null with [skip] set when the engine is not available here.
inline std::shared_ptr<IDatabase> openEngine(Engine e, const std::string& sqlite_path,
                                             std::string& skip) {
    switch (e) {
        case Engine::SQLite: {
            std::filesystem::remove(sqlite_path);
            auto lite = std::make_shared<SQLiteDatabase>(sqlite_path);
            if (!lite->connect()) {
                skip = "SQLite would not open";
                return nullptr;
            }
            return lite;
        }
        case Engine::MySQL: {
#ifndef WITH_MYSQL
            skip = "built without MySQL (-DBUILD_WITH_MYSQL=OFF)";
            return nullptr;
#else
            const std::string host = envOr("MYSQL_TEST_HOST", "");
            if (host.empty()) {
                skip = "MYSQL_TEST_HOST unset, skipping MySQL.";
                return nullptr;
            }
            auto my = std::make_shared<MySQLDatabase>(
                host, std::stoi(envOr("MYSQL_TEST_PORT", "3306")), envOr("MYSQL_TEST_USER", ""),
                envOr("MYSQL_TEST_PASSWORD", ""), envOr("MYSQL_TEST_DB", "hms_cpap_test"));
            if (!my->connect()) {
                skip = "No usable MySQL at " + host;
                return nullptr;
            }
            return my;
#endif
        }
        case Engine::Postgres: {
#ifndef WITH_POSTGRESQL
            skip = "built without PostgreSQL";
            return nullptr;
#else
            const std::string host = envOr("PGHOST", "");
            if (host.empty()) {
                skip = "PGHOST unset, skipping PostgreSQL.";
                return nullptr;
            }
            const std::string conn = "host=" + host + " port=" + envOr("PGPORT", "5432") +
                                     " user=" + envOr("PGUSER", "maestro") +
                                     " password=" + envOr("PGPASSWORD", "") +
                                     " dbname=" + envOr("PGDATABASE", "cpap_monitoring") +
                                     " connect_timeout=3";
            auto pg = std::make_shared<PostgresDatabase>(conn);
            if (!pg->connect()) {
                skip = "No usable PostgreSQL at " + host;
                return nullptr;
            }
            return pg;
#endif
        }
    }
    return nullptr;
}

/// [hour]:[min] on the local clock of y-mo-d.
inline system_clock::time_point localAt(int y, int mo, int d, int hour, int min = 0) {
    std::tm tm{};
    tm.tm_year = y - 1900;
    tm.tm_mon = mo - 1;
    tm.tm_mday = d;
    tm.tm_hour = hour;
    tm.tm_min = min;
    tm.tm_isdst = -1;
    return system_clock::from_time_t(std::mktime(&tm));
}

/// A night's counts, as the parser stores them on the session.
struct NightSpec {
    int mins = 360;
    int obstructive = 0, central = 0, clear_airway = 0, unclassified = 0, hypopneas = 0;
    int reras = 0, other = 0;
    bool ungraded = false;
    double leak = 6.0;   ///< every minute's leak
};

/// One closed session of [n], starting at [start], with its events and vitals.
inline void saveNight(IDatabase& db, const std::string& device, system_clock::time_point start,
                      const NightSpec& n, const std::vector<SleepEvent>& events = {},
                      const std::vector<VitalSample>& vitals = {}) {
    CPAPSession s;
    s.device_id = device;
    s.device_name = "AirCurve 11 ASV";
    s.serial_number = "SDD072";
    s.session_start = start;
    s.duration_seconds = n.mins * 60;
    s.data_records = n.mins;
    for (int i = 0; i < n.mins; ++i) {
        BreathingSummary b;
        b.timestamp = start + minutes(i);
        b.leak_rate = n.leak;
        b.respiratory_rate = 14.0;
        s.breathing_summary.push_back(b);
    }
    s.events = events;
    s.vitals = vitals;
    SessionMetrics m;
    m.obstructive_apneas = n.obstructive;
    m.central_apneas = n.central;
    m.clear_airway_apneas = n.clear_airway;
    m.unclassified_apneas = n.unclassified;
    m.hypopneas = n.hypopneas;
    m.reras = n.reras;
    m.total_events = n.obstructive + n.central + n.clear_airway + n.unclassified +
                     n.hypopneas + n.reras + n.other;
    m.ahi = (n.obstructive + n.central + n.clear_airway + n.unclassified + n.hypopneas) *
            60.0 / n.mins;
    m.index_kind = n.ungraded ? SessionMetrics::IndexKind::Ungraded
                              : SessionMetrics::IndexKind::AHI;
    s.metrics = m;
    ASSERT_TRUE(db.saveSession(s));
    // saveSession writes session_end NULL; the range query reads closed ones.
    db.markSessionCompleted(device, start);
}

/// Removes everything [device] wrote, and the O2 Ring rows named [oxi_files].
inline void cleanup(IDatabase& db, const std::string& device,
                    const std::vector<std::string>& oxi_files = {}) {
    const auto t = db.dbType();
    for (const char* table : {"cpap_sessions", "cpap_daily_summary"})
        db.executeQuery(std::string("DELETE FROM ") + table + " WHERE device_id = " +
                            sql::param(1, t),
                        {device});
    for (const auto& f : oxi_files) {
        db.executeQuery("DELETE FROM oximetry_samples WHERE session_id IN (SELECT id FROM "
                        "oximetry_sessions WHERE filename = " + sql::param(1, t) + ")",
                        {f});
        db.executeQuery("DELETE FROM oximetry_sessions WHERE filename = " + sql::param(1, t),
                        {f});
    }
}

inline void removeSqlite(const std::string& path) {
    for (const auto* suffix : {"", "-wal", "-shm"}) {
        std::error_code ec;
        std::filesystem::remove(path + suffix, ec);
    }
}

}  // namespace ai_facts_test
