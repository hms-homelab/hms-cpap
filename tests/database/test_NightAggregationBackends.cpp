// SDD-033: a night is its minutes, not its sessions.
//
// A night of two sessions of unequal length used to average the sessions'
// averages, so a three-minute mask-fit weighed as much as a five-hour night
// (an AirCurve 11 card, hms-cpap #33: published IPAP 9.395 where the card's
// minutes give 9.313). Its events were the MAX of each type across sessions,
// which counted only the busiest one. And a percentile does not combine at
// all, so on a night of more than one session the STR's own day wins (D1).
//
// Every case runs on each engine the service supports. MySQL and PostgreSQL
// skip unless their env is set; SQLite always runs.
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

#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <memory>
#include <string>
#include <unistd.h>
#include <vector>

using namespace hms_cpap;
using std::chrono::minutes;
using std::chrono::seconds;
using std::chrono::system_clock;
namespace fs = std::filesystem;

namespace {

std::string envOr(const char* key, const std::string& fallback) {
    const char* v = std::getenv(key);
    return (v && *v) ? std::string(v) : fallback;
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

class NightAggregationBackendTest : public ::testing::TestWithParam<Engine> {
protected:
    void SetUp() override {
        device_ = "night_agg_" + std::to_string(::getpid());
        switch (GetParam()) {
            case Engine::SQLite: {
                path_ = (fs::temp_directory_path() /
                         ("hms_cpap_night_agg_" + std::to_string(::getpid()) + ".db")).string();
                fs::remove(path_);
                auto lite = std::make_shared<SQLiteDatabase>(path_);
                ASSERT_TRUE(lite->connect());
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
            for (const char* table : {"cpap_sessions", "cpap_daily_summary"}) {
                db_->executeQuery(std::string("DELETE FROM ") + table + " WHERE device_id = " +
                                      sql::param(1, db_->dbType()),
                                  {device_});
            }
        }
        db_.reset();
        if (!path_.empty()) {
            for (const auto* suffix : {"", "-wal", "-shm"}) {
                std::error_code ec;
                fs::remove(path_ + suffix, ec);
            }
        }
    }

    // 23:00 on the local clock, like test_BilevelPressureBackends: both
    // sessions of the night and the STR's record_date then sit on the same
    // sleep day (start - 12 h) whatever the runner's timezone. A mid-morning
    // anchor would put the second session on the next night.
    static system_clock::time_point nightStart() {
        std::tm tm{};
        tm.tm_year = 2099 - 1900;
        tm.tm_mon = 6;
        tm.tm_mday = 1;
        tm.tm_hour = 23;
        tm.tm_isdst = -1;
        return system_clock::from_time_t(std::mktime(&tm));
    }
    static system_clock::time_point strRecordDate() {
        return nightStart() - std::chrono::hours(12);
    }

    /// One session of the night: [mins] minutes, each minute carrying [leak]
    /// and [ipap], plus its own event counts and leak percentile.
    void saveSession(system_clock::time_point start, int mins, double leak, double ipap,
                     int obstructive, int hypopneas, double leak_p95, double spo2 = 0) {
        CPAPSession s;
        s.device_id = device_;
        s.device_name = "AirCurve 11 VAuto";
        s.serial_number = "SDD033";
        s.session_start = start;
        s.duration_seconds = mins * 60;
        s.data_records = mins;
        for (int i = 0; i < mins; ++i) {
            BreathingSummary b;
            b.timestamp = start + minutes(i);
            b.leak_rate = leak;
            b.therapy_pressure = ipap;
            b.respiratory_rate = 14.0;
            s.breathing_summary.push_back(b);
        }
        SessionMetrics m;
        m.obstructive_apneas = obstructive;
        m.hypopneas = hypopneas;
        m.total_events = obstructive + hypopneas;
        m.ahi = (obstructive + hypopneas) * 3600.0 / (mins * 60.0);
        m.leak_p95 = leak_p95;
        if (spo2 > 0) m.avg_spo2 = spo2;
        s.metrics = m;
        ASSERT_TRUE(db_->saveSession(s)) << engineName(GetParam());
        // saveSession always writes session_end NULL; the collector closes a
        // session with markSessionCompleted, and the range query reads only
        // closed ones.
        db_->markSessionCompleted(device_, start);
    }

    /// The machine's own day, including the percentiles it computed over every
    /// sample of the night.
    void saveStrDay(double leak_95, double leak_50 = 0, double spo2_50 = 0) {
        STRDailyRecord r;
        r.device_id = device_;
        r.record_date = strRecordDate();
        r.duration_minutes = 264;
        r.ahi = 1.3;
        r.leak_95 = leak_95;
        r.leak_50 = leak_50;
        r.spo2_50 = spo2_50;
        ASSERT_TRUE(db_->saveSTRDailyRecords({r})) << engineName(GetParam());
    }

    Json::Value dailyRow() {
        return db_->executeQuery(
            "SELECT leak_95, leak_50, spo2_50, leak_95_str, spo2_50_str, mask_events"
            " FROM cpap_daily_summary WHERE device_id = " + sql::param(1, db_->dbType()),
            {device_});
    }
    static double num(const Json::Value& v) {
        return v.isNull() ? -1.0 : (v.isString() ? std::atof(v.asCString()) : v.asDouble());
    }

    std::string path_;
    std::string device_;
    std::shared_ptr<IDatabase> db_;
};

}  // namespace

// The night of 2026-09-12 on the reporter's card, in shape: 161 minutes at one
// pressure and 103 at another. The minutes decide, not the session count.
TEST_P(NightAggregationBackendTest, ANightsAveragesAreOverItsMinutes) {
    saveSession(nightStart(), 161, /*leak=*/10.0, /*ipap=*/9.023, 0, 1, 8.4);
    saveSession(nightStart() + minutes(300), 103, /*leak=*/20.0, /*ipap=*/9.768, 2, 3, 16.8);

    const auto m = db_->getNightlyMetrics(device_, nightStart());
    ASSERT_TRUE(m.has_value()) << engineName(GetParam());

    const double weighted_ipap = (9.023 * 161 + 9.768 * 103) / 264.0;   // 9.313
    const double weighted_leak = (10.0 * 161 + 20.0 * 103) / 264.0;     // 13.90
    ASSERT_TRUE(m->avg_therapy_pressure.has_value()) << engineName(GetParam());
    EXPECT_NEAR(*m->avg_therapy_pressure, weighted_ipap, 1e-3)
        << engineName(GetParam()) << ": the plain mean of the sessions would be 9.3955";
    ASSERT_TRUE(m->avg_leak_rate.has_value()) << engineName(GetParam());
    EXPECT_NEAR(*m->avg_leak_rate, weighted_leak, 1e-3) << engineName(GetParam());
}

// Each session's EVE is its own since SDD-014, so the night's events are their
// sum. MAX gave 5 where the card holds 6, and an AHI of 1.14 against the daily
// summary's 1.36.
TEST_P(NightAggregationBackendTest, ANightsEventsAreTheSumOfItsSessions) {
    saveSession(nightStart(), 161, 10.0, 9.0, /*OA=*/0, /*H=*/1, 8.4);
    saveSession(nightStart() + minutes(300), 103, 20.0, 9.7, /*OA=*/2, /*H=*/3, 16.8);

    const auto m = db_->getNightlyMetrics(device_, nightStart());
    ASSERT_TRUE(m.has_value()) << engineName(GetParam());
    EXPECT_EQ(m->total_events, 6) << engineName(GetParam());
    EXPECT_EQ(m->obstructive_apneas, 2) << engineName(GetParam());
    EXPECT_EQ(m->hypopneas, 4) << engineName(GetParam());
    EXPECT_NEAR(m->ahi, 6 * 3600.0 / (264 * 60.0), 1e-3)
        << engineName(GetParam()) << ": MAX per type would give 5 events";

    // The range query reads the same night the same way.
    const auto range = db_->getMetricsForDateRange(device_, 36500);
    ASSERT_FALSE(range.empty()) << engineName(GetParam());
    EXPECT_EQ(range.back().total_events, 6) << engineName(GetParam());
}

// D1: a percentile does not combine, so a night of more than one session takes
// the STR's; a single-session night keeps the parser's own, from the samples.
TEST_P(NightAggregationBackendTest, AMultiSessionNightTakesTheStrsPercentile) {
    saveSession(nightStart(), 161, 10.0, 9.0, 0, 1, /*leak_p95=*/8.4);
    saveSession(nightStart() + minutes(300), 103, 20.0, 9.7, 2, 3, /*leak_p95=*/16.8);
    saveStrDay(/*leak_95=*/15.6, /*leak_50=*/6.0, /*spo2_50=*/95.0);
    ASSERT_TRUE(db_->aggregateDailySummaryFromSessions(device_)) << engineName(GetParam());

    const auto m = db_->getNightlyMetrics(device_, nightStart());
    ASSERT_TRUE(m.has_value()) << engineName(GetParam());
    ASSERT_TRUE(m->leak_p95.has_value()) << engineName(GetParam());
    EXPECT_NEAR(*m->leak_p95, 15.6, 1e-6)
        << engineName(GetParam()) << ": the MAX of the sessions would be 16.8";

    auto rows = dailyRow();
    ASSERT_TRUE(rows.isArray() && !rows.empty()) << engineName(GetParam());
    EXPECT_NEAR(num(rows[0u]["leak_95"]), 15.6, 1e-6) << engineName(GetParam());
    EXPECT_NEAR(num(rows[0u]["leak_95_str"]), 15.6, 1e-6)
        << engineName(GetParam()) << ": the STR's copy is kept in its own column";
    EXPECT_NEAR(num(rows[0u]["spo2_50"]), 95.0, 1e-6)
        << engineName(GetParam()) << ": no session measured SpO2, so the STR's stands";
    EXPECT_EQ(static_cast<int>(num(rows[0u]["mask_events"])), 6) << engineName(GetParam());
}

TEST_P(NightAggregationBackendTest, ASingleSessionNightKeepsItsOwnPercentile) {
    saveSession(nightStart(), 264, 12.0, 9.3, 2, 4, /*leak_p95=*/8.4);
    saveStrDay(/*leak_95=*/15.6);
    ASSERT_TRUE(db_->aggregateDailySummaryFromSessions(device_)) << engineName(GetParam());

    const auto m = db_->getNightlyMetrics(device_, nightStart());
    ASSERT_TRUE(m.has_value()) << engineName(GetParam());
    ASSERT_TRUE(m->leak_p95.has_value()) << engineName(GetParam());
    EXPECT_NEAR(*m->leak_p95, 8.4, 1e-6)
        << engineName(GetParam()) << ": one session's percentile IS the night's";

    auto rows = dailyRow();
    ASSERT_TRUE(rows.isArray() && !rows.empty()) << engineName(GetParam());
    EXPECT_NEAR(num(rows[0u]["leak_95"]), 8.4, 1e-6) << engineName(GetParam());
}

// A card with no STR day (Löwenstein, Sefam, or a night the STR has not
// reached): the night keeps our estimate rather than losing the figure.
TEST_P(NightAggregationBackendTest, WithNoStrDayTheNightKeepsOurs) {
    saveSession(nightStart(), 161, 10.0, 9.0, 0, 1, /*leak_p95=*/8.4);
    saveSession(nightStart() + minutes(300), 103, 20.0, 9.7, 2, 3, /*leak_p95=*/16.8);
    ASSERT_TRUE(db_->aggregateDailySummaryFromSessions(device_)) << engineName(GetParam());

    const auto m = db_->getNightlyMetrics(device_, nightStart());
    ASSERT_TRUE(m.has_value()) << engineName(GetParam());
    ASSERT_TRUE(m->leak_p95.has_value()) << engineName(GetParam());
    EXPECT_NEAR(*m->leak_p95, 16.8, 1e-6) << engineName(GetParam());

    auto rows = dailyRow();
    ASSERT_TRUE(rows.isArray() && !rows.empty()) << engineName(GetParam());
    EXPECT_TRUE(rows[0u]["leak_95_str"].isNull()) << engineName(GetParam());
}

// The STR's sentinels are not values: no oximeter reads SpO2 0, and a day
// still recording reads negative. Neither may beat a session's own number.
TEST_P(NightAggregationBackendTest, TheStrsSentinelsAreNotCopied) {
    saveSession(nightStart(), 161, 10.0, 9.0, 0, 1, 8.4, /*spo2=*/94.0);
    saveSession(nightStart() + minutes(300), 103, 20.0, 9.7, 2, 3, 16.8, /*spo2=*/96.0);
    saveStrDay(/*leak_95=*/-0.02, /*leak_50=*/-0.02, /*spo2_50=*/0.0);
    ASSERT_TRUE(db_->aggregateDailySummaryFromSessions(device_)) << engineName(GetParam());

    auto rows = dailyRow();
    ASSERT_TRUE(rows.isArray() && !rows.empty()) << engineName(GetParam());
    EXPECT_TRUE(rows[0u]["leak_95_str"].isNull()) << engineName(GetParam());
    EXPECT_TRUE(rows[0u]["spo2_50_str"].isNull()) << engineName(GetParam());
    EXPECT_NEAR(num(rows[0u]["leak_95"]), 12.6, 1e-6)
        << engineName(GetParam()) << ": ours, since the STR day holds no reading";
    // Two sessions' SpO2, weighted by their minutes.
    EXPECT_NEAR(num(rows[0u]["spo2_50"]), (94.0 * 161 + 96.0 * 103) / 264.0, 0.05)
        << engineName(GetParam());
}

INSTANTIATE_TEST_SUITE_P(Engines, NightAggregationBackendTest,
                         ::testing::Values(Engine::SQLite, Engine::MySQL, Engine::Postgres),
                         [](const ::testing::TestParamInfo<Engine>& i) {
                             return engineName(i.param);
                         });
