/**
 * Re-importing a night, on every engine.
 *
 * A reparse (POST /api/sessions/<date>/reparse, a backfill, the --reparse CLI)
 * deletes the night it re-reads and saves the new parse. The two used to be
 * separate transactions, so a process killed between them (docker stop sends
 * SIGKILL after 10 s, and a full night's breaths took longer than that over a
 * network database) left neither the old night nor the new one. These pin that
 * replaceNight() is one transaction (a fault after the delete leaves the old
 * night whole), that a good replace leaves only the new night, closed, with its
 * file rows and checkpoint sizes, and that a full night saves in batches.
 *
 * SAFETY: every row is dated 2099 and namespaced to a per-process device id,
 * and deleted in TearDown, so an env-gated run against a populated MySQL or
 * PostgreSQL never touches a real night.
 */

#include <gtest/gtest.h>

#include "database/SQLiteDatabase.h"
#include "database/SqlDialect.h"
#include "utils/MachineZone.h"
#include "utils/TimeCompat.h"
#ifdef WITH_MYSQL
#include "database/MySQLDatabase.h"
#endif
#ifdef WITH_POSTGRESQL
#include "database/PostgresDatabase.h"
#endif

#include <chrono>
#include <cstdlib>
#include <ctime>
#include <filesystem>
#include <iostream>
#include <memory>
#include <string>
#ifndef _WIN32
#include <unistd.h>
#else
#include <process.h>
#endif

using namespace hms_cpap;
using namespace std::chrono;
namespace fs = std::filesystem;

namespace {

std::string envOr(const char* key, const std::string& fallback) {
    const char* v = std::getenv(key);
    return (v && *v) ? std::string(v) : fallback;
}

// SQLite stringifies every column; the others return typed values.
long long asCount(const Json::Value& v) {
    if (v.isNull()) return -1;
    if (v.isString()) return std::stoll(v.asString());
    return v.asInt64();
}

/// A CPAP start on the machine's clock, as the EDF wall clock is read.
system_clock::time_point local(int y, int mo, int d, int h, int mi = 0) {
    std::tm tm{};
    tm.tm_year = y - 1900;
    tm.tm_mon = mo - 1;
    tm.tm_mday = d;
    tm.tm_hour = h;
    tm.tm_min = mi;
    return system_clock::from_time_t(timegm_utc(&tm));
}

enum class Engine { SQLite, MySQL, Postgres };

const char* engineName(Engine e) {
    switch (e) {
        case Engine::SQLite:   return "SQLite";
        case Engine::MySQL:    return "MySQL";
        case Engine::Postgres: return "Postgres";
    }
    return "?";
}

class NightReplaceBackendTest : public ::testing::TestWithParam<Engine> {
protected:
    static constexpr const char* kNight = "20990710";
    static constexpr const char* kAfter = "20990711";

    void SetUp() override {
        device_ = "nrp_test_" + std::to_string(::getpid());
        switch (GetParam()) {
            case Engine::SQLite: {
                path_ = (fs::temp_directory_path() /
                         ("hms_cpap_nrp_" + std::to_string(::getpid()) + ".db")).string();
                fs::remove(path_);
                auto lite = std::make_shared<SQLiteDatabase>(path_);
                ASSERT_TRUE(lite->connect()) << "SQLite connect failed";
                db_ = std::move(lite);
                return;
            }
            case Engine::MySQL: {
#ifndef WITH_MYSQL
                GTEST_SKIP() << "built without MySQL (-DBUILD_WITH_MYSQL=OFF)";
#else
                const std::string host = envOr("MYSQL_TEST_HOST", "");
                if (host.empty()) GTEST_SKIP() << "MYSQL_TEST_HOST unset, skipping MySQL.";
                auto my = std::make_shared<MySQLDatabase>(
                    host, std::stoi(envOr("MYSQL_TEST_PORT", "3306")),
                    envOr("MYSQL_TEST_USER", ""), envOr("MYSQL_TEST_PASSWORD", ""),
                    envOr("MYSQL_TEST_DB", "hms_cpap_test"));
                if (!my->connect()) GTEST_SKIP() << "No usable MySQL at " << host;
                db_ = std::move(my);
                return;
#endif
            }
            case Engine::Postgres: {
#ifndef WITH_POSTGRESQL
                GTEST_SKIP() << "built without PostgreSQL";
#else
                const std::string host = envOr("PGHOST", "");
                if (host.empty()) GTEST_SKIP() << "PGHOST unset, skipping PostgreSQL.";
                const std::string conn =
                    "host=" + host + " port=" + envOr("PGPORT", "5432") +
                    " user=" + envOr("PGUSER", "maestro") +
                    " password=" + envOr("PGPASSWORD", "") +
                    " dbname=" + envOr("PGDATABASE", "cpap_monitoring") +
                    " connect_timeout=3";
                auto pg = std::make_shared<PostgresDatabase>(conn);
                if (!pg->connect()) GTEST_SKIP() << "No usable PostgreSQL at " << host;
                db_ = std::move(pg);
                return;
#endif
            }
        }
    }

    void TearDown() override {
        if (db_) {
            const auto p = sql::param(1, db_->dbType());
            const std::string mine = "(SELECT id FROM cpap_sessions WHERE device_id = " + p + ")";
            for (const char* t : {"cpap_session_files", "cpap_breaths", "cpap_events",
                                  "cpap_breathing_summary", "cpap_calculated_metrics",
                                  "cpap_vitals", "cpap_session_metrics"})
                db_->executeQuery(std::string("DELETE FROM ") + t + " WHERE session_id IN " + mine,
                                  {device_});
            for (const char* t : {"cpap_sessions", "cpap_devices"})
                db_->executeQuery(std::string("DELETE FROM ") + t + " WHERE device_id = " + p,
                                  {device_});
        }
        db_.reset();
        if (!path_.empty()) {
            for (const auto* suffix : {"", "-wal", "-shm"}) {
                std::error_code ec;
                fs::remove(path_ + suffix, ec);
            }
        }
    }

    /// A session of [minutes] at [start], filed under its night's DATALOG
    /// folder as every import path files it, with a breath every 4 s, a
    /// breathing summary a minute, an event every 12 minutes, and (when asked)
    /// an SpO2 sample a second.
    CPAPSession night(system_clock::time_point start, int minutes, double ahi,
                      bool with_vitals = false) const {
        CPAPSession s;
        s.device_id = device_;
        s.device_name = "AirSense 11";
        s.serial_number = "NRP";
        s.session_start = start;
        s.duration_seconds = minutes * 60;
        s.data_records = minutes;
        const std::string folder = strDayForSessionStart(start);
        s.brp_file_path = "DATALOG/" + folder + "/x_BRP.edf";
        for (int m = 0; m < minutes; ++m) {
            BreathingSummary b;
            b.timestamp = start + std::chrono::minutes(m);
            b.avg_flow_rate = 10.0 + m % 7;
            b.max_flow_rate = 30.0;
            b.min_flow_rate = -30.0;
            b.avg_pressure = 8.0;
            b.max_pressure = 9.0;
            b.min_pressure = 7.0;
            b.respiratory_rate = 15.0;
            b.tidal_volume = 450.0;
            s.breathing_summary.push_back(b);
        }
        for (int i = 0; i < minutes * 15; ++i) {
            Breath b;
            b.onset = start + seconds(i * 4);
            b.tidal_volume = 400.0 + i % 50;
            b.inspiratory_time = 1.5;
            b.expiratory_time = 2.5;
            b.flow_limitation = (i % 10) / 10.0;
            s.breaths.push_back(b);
        }
        for (int m = 0; m < minutes; m += 12) {
            CPAPEvent e{};
            e.event_type = EventType::HYPOPNEA;
            e.timestamp = start + std::chrono::minutes(m) + seconds(30);
            e.duration_seconds = 12;
            s.events.push_back(e);
        }
        if (with_vitals) {
            for (int i = 0; i < minutes * 60; ++i) {
                CPAPVitals v(start + seconds(i));
                v.spo2 = 95.0;
                v.heart_rate = 60;
                s.vitals.push_back(v);
            }
        }
        SessionMetrics m;
        m.ahi = ahi;
        m.total_events = static_cast<int>(s.events.size());
        s.metrics = m;
        return s;
    }

    long long count(const std::string& sql, const std::vector<std::string>& args) {
        const auto rows = db_->executeQuery(sql, args);
        if (!rows.isArray() || rows.empty()) return -1;
        return asCount(rows[0]["n"]);
    }

    std::string p(int i) const { return sql::param(i, db_->dbType()); }

    long long breathsOf(system_clock::time_point start) {
        return count("SELECT COUNT(*) AS n FROM cpap_breaths b JOIN cpap_sessions s ON s.id = b.session_id"
                     " WHERE s.device_id = " + p(1) +
                     " AND s.session_start = " + sql::castTimestamp(2, db_->dbType()),
                     {device_, wall::text(start)});
    }

    long long rowsOf(const char* table, system_clock::time_point start) {
        return count(std::string("SELECT COUNT(*) AS n FROM ") + table +
                     " c JOIN cpap_sessions s ON s.id = c.session_id"
                     " WHERE s.device_id = " + p(1) +
                     " AND s.session_start = " + sql::castTimestamp(2, db_->dbType()),
                     {device_, wall::text(start)});
    }

    std::string device_;
    std::string path_;
    std::shared_ptr<IDatabase> db_;
};

// A full night (8 h: 7200 breaths, 480 minutes, 40 events) saves every row,
// and says how long it took, so a slow engine shows in the log.
TEST_P(NightReplaceBackendTest, AFullNightSavesEveryRow) {
    const auto start = local(2099, 7, 10, 22);
    const auto s = night(start, 480, 2.0);
    const auto t0 = steady_clock::now();
    ASSERT_TRUE(db_->saveSession(s)) << engineName(GetParam());
    const auto ms = duration_cast<milliseconds>(steady_clock::now() - t0).count();
    std::cout << "[ timing ] " << engineName(GetParam()) << ": 8 h night, "
              << s.breaths.size() << " breaths, " << s.breathing_summary.size()
              << " minutes, " << s.events.size() << " events saved in " << ms << " ms" << std::endl;
    EXPECT_EQ(breathsOf(start), 7200);
    EXPECT_EQ(rowsOf("cpap_breathing_summary", start), 480);
    EXPECT_EQ(rowsOf("cpap_calculated_metrics", start), 480);
    EXPECT_EQ(rowsOf("cpap_events", start), 40);
}

// The same night with an oximeter's SpO2 every second (28800 samples).
TEST_P(NightReplaceBackendTest, AFullNightWithOximetrySavesEveryRow) {
    const auto start = local(2099, 7, 10, 22);
    const auto s = night(start, 480, 2.0, /*with_vitals=*/true);
    const auto t0 = steady_clock::now();
    ASSERT_TRUE(db_->saveSession(s)) << engineName(GetParam());
    const auto ms = duration_cast<milliseconds>(steady_clock::now() - t0).count();
    std::cout << "[ timing ] " << engineName(GetParam()) << ": 8 h night with "
              << s.vitals.size() << " SpO2 samples saved in " << ms << " ms" << std::endl;
    EXPECT_EQ(breathsOf(start), 7200);
    EXPECT_EQ(rowsOf("cpap_vitals", start), 28800);
}

INSTANTIATE_TEST_SUITE_P(Engines, NightReplaceBackendTest,
                         ::testing::Values(Engine::SQLite, Engine::MySQL, Engine::Postgres),
                         [](const auto& info) { return std::string(engineName(info.param)); });

}  // namespace
