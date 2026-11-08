//
// test_UnmeasuredMinuteBackends.cpp: a minute the parser marks unmeasured is
// stored as NULL, never as 0, on every engine.
//
// BreathingSummary carries two flags, pressure_measured and flow_measured. A
// minute with one of them false holds 0 placeholders in the three fields the
// flag covers, not readings: a BMC / React Health Luna minute with the leak at
// the machine's cap, or too few seconds on the card, has no flow, and a Luna
// whose model has no measured pressure scale (or a Philips flow-only waveform)
// has no pressure. Stored as 0 those placeholders draw a flat zero on the
// session charts and pull every average down; stored as NULL the chart leaves
// a gap and SQL averages skip them. Three engines write cpap_breathing_summary,
// each with its own insert, so three engines are tested.
//
// SQLite always runs. MySQL runs when MYSQL_TEST_HOST is set, PostgreSQL when
// PGHOST is; both skip cleanly otherwise.
//
// SAFETY: every row is dated 2099 and namespaced to a per-process device id,
// and deleted in TearDown, so pointing MYSQL_TEST_*/PG* at a populated database
// cannot disturb real data.
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

#include <chrono>
#include <cstdlib>
#include <ctime>
#include <filesystem>
#include <memory>
#include <string>
#ifndef _WIN32
#include <unistd.h>
#else
#include <process.h>
#endif

using namespace hms_cpap;
using std::chrono::system_clock;
using std::chrono::minutes;
namespace fs = std::filesystem;

namespace {

std::string envOr(const char* key, const std::string& fallback) {
    const char* v = std::getenv(key);
    return (v && *v) ? std::string(v) : fallback;
}

system_clock::time_point local(int y, int mo, int d, int h, int mi = 0) {
    std::tm tm{};
    tm.tm_year = y - 1900;
    tm.tm_mon = mo - 1;
    tm.tm_mday = d;
    tm.tm_hour = h;
    tm.tm_min = mi;
    tm.tm_isdst = -1;
    return system_clock::from_time_t(std::mktime(&tm));
}

double num(const Json::Value& v) {
    return v.isString() ? std::stod(v.asString()) : v.asDouble();
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

class UnmeasuredMinuteBackendTest : public ::testing::TestWithParam<Engine> {
protected:
    void SetUp() override {
        device_ = "unmeasured_test_" + std::to_string(::getpid());
        switch (GetParam()) {
            case Engine::SQLite: {
                path_ = (fs::temp_directory_path() /
                         ("hms_cpap_unmeasured_" + std::to_string(::getpid()) + ".db")).string();
                fs::remove(path_);
                auto lite = std::make_shared<SQLiteDatabase>(path_);
                ASSERT_TRUE(lite->connect()) << "SQLite connect failed";
                db_ = std::move(lite);
                return;
            }
            case Engine::MySQL: {
#ifndef WITH_MYSQL
                GTEST_SKIP() << "built without MySQL";
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
            db_->executeQuery("DELETE FROM cpap_breathing_summary WHERE session_id IN " + mine, {device_});
            db_->executeQuery("DELETE FROM cpap_calculated_metrics WHERE session_id IN " + mine, {device_});
            db_->executeQuery("DELETE FROM cpap_session_files WHERE session_id IN " + mine, {device_});
            db_->executeQuery("DELETE FROM cpap_sessions WHERE device_id = " + p, {device_});
        }
        db_.reset();
        if (!path_.empty()) {
            for (const auto* suffix : {"", "-wal", "-shm"}) {
                std::error_code ec;
                fs::remove(path_ + suffix, ec);
            }
        }
    }

    static BreathingSummary minute(system_clock::time_point t, double flow, double press) {
        BreathingSummary b(t);
        b.avg_flow_rate = flow;
        b.max_flow_rate = flow + 20.0;
        b.min_flow_rate = flow - 20.0;
        b.avg_pressure = press;
        b.max_pressure = press + 1.5;
        b.min_pressure = press - 1.5;
        return b;
    }

    /// Three minutes: both measured, pressure unmeasured, flow unmeasured.
    CPAPSession makeSession() {
        const auto start = local(2099, 5, 20, 23, 0);
        CPAPSession s;
        s.device_id = device_;
        s.device_name = "Luna G3";
        s.serial_number = "UNMEASURED";
        s.session_start = start;
        s.duration_seconds = 180;
        s.data_records = 3;

        auto both = minute(start, 1.5, 7.0);
        both.tidal_volume = 420.0;

        auto no_pressure = minute(start + minutes(1), 2.5, 0.0);
        no_pressure.pressure_measured = false;
        no_pressure.avg_pressure = no_pressure.max_pressure = no_pressure.min_pressure = 0.0;
        no_pressure.tidal_volume = 410.0;

        auto no_flow = minute(start + minutes(2), 0.0, 6.0);
        no_flow.flow_measured = false;
        no_flow.avg_flow_rate = no_flow.max_flow_rate = no_flow.min_flow_rate = 0.0;
        no_flow.leak_rate = 100.0;   // the leak at the cap is the reason, and it is kept

        s.breathing_summary = {both, no_pressure, no_flow};
        s.has_summary = true;
        return s;
    }

    Json::Value storedMinutes() {
        const auto p = sql::param(1, db_->dbType());
        return db_->executeQuery(
            "SELECT b.avg_flow_rate, b.max_flow_rate, b.min_flow_rate,"
            " b.avg_pressure, b.max_pressure, b.min_pressure, c.leak_rate"
            " FROM cpap_breathing_summary b"
            " JOIN cpap_sessions s ON s.id = b.session_id"
            " LEFT JOIN cpap_calculated_metrics c"
            "   ON c.session_id = b.session_id AND c.timestamp = b.timestamp"
            " WHERE s.device_id = " + p + " ORDER BY b.timestamp", {device_});
    }

    std::shared_ptr<IDatabase> db_;
    std::string device_;
    std::string path_;
};

const char* kFlow[] = {"avg_flow_rate", "max_flow_rate", "min_flow_rate"};
const char* kPressure[] = {"avg_pressure", "max_pressure", "min_pressure"};

}  // namespace

TEST_P(UnmeasuredMinuteBackendTest, AMeasuredMinuteKeepsItsReadings) {
    ASSERT_TRUE(db_->saveSession(makeSession())) << engineName(GetParam());
    const auto rows = storedMinutes();
    ASSERT_EQ(rows.size(), 3u) << engineName(GetParam());

    const auto& r = rows[0];
    for (const char* c : kFlow) ASSERT_FALSE(r[c].isNull()) << engineName(GetParam()) << " " << c;
    for (const char* c : kPressure) ASSERT_FALSE(r[c].isNull()) << engineName(GetParam()) << " " << c;
    EXPECT_NEAR(num(r["avg_flow_rate"]), 1.5, 1e-6) << engineName(GetParam());
    EXPECT_NEAR(num(r["max_flow_rate"]), 21.5, 1e-6) << engineName(GetParam());
    EXPECT_NEAR(num(r["min_flow_rate"]), -18.5, 1e-6) << engineName(GetParam());
    EXPECT_NEAR(num(r["avg_pressure"]), 7.0, 1e-6) << engineName(GetParam());
    EXPECT_NEAR(num(r["max_pressure"]), 8.5, 1e-6) << engineName(GetParam());
    EXPECT_NEAR(num(r["min_pressure"]), 5.5, 1e-6) << engineName(GetParam());
}

TEST_P(UnmeasuredMinuteBackendTest, AnUnmeasuredPressureIsNullNotZero) {
    ASSERT_TRUE(db_->saveSession(makeSession())) << engineName(GetParam());
    const auto rows = storedMinutes();
    ASSERT_EQ(rows.size(), 3u) << engineName(GetParam());

    const auto& r = rows[1];
    for (const char* c : kPressure)
        EXPECT_TRUE(r[c].isNull()) << engineName(GetParam()) << ": " << c << " is " << r[c];
    // The flow of the same minute is a reading and stays one.
    for (const char* c : kFlow) ASSERT_FALSE(r[c].isNull()) << engineName(GetParam()) << " " << c;
    EXPECT_NEAR(num(r["avg_flow_rate"]), 2.5, 1e-6) << engineName(GetParam());
}

TEST_P(UnmeasuredMinuteBackendTest, AnUnmeasuredFlowIsNullNotZero) {
    ASSERT_TRUE(db_->saveSession(makeSession())) << engineName(GetParam());
    const auto rows = storedMinutes();
    ASSERT_EQ(rows.size(), 3u) << engineName(GetParam());

    const auto& r = rows[2];
    for (const char* c : kFlow)
        EXPECT_TRUE(r[c].isNull()) << engineName(GetParam()) << ": " << c << " is " << r[c];
    for (const char* c : kPressure) ASSERT_FALSE(r[c].isNull()) << engineName(GetParam()) << " " << c;
    EXPECT_NEAR(num(r["avg_pressure"]), 6.0, 1e-6) << engineName(GetParam());
    // The minute itself is kept: its leak is what says why it has no flow.
    ASSERT_FALSE(r["leak_rate"].isNull()) << engineName(GetParam());
    EXPECT_NEAR(num(r["leak_rate"]), 100.0, 1e-6) << engineName(GetParam());
}

// The night's figures are SQL averages over these columns, and AVG skips a
// NULL: the unmeasured minutes do not pull them toward 0.
TEST_P(UnmeasuredMinuteBackendTest, AveragesSkipTheUnmeasuredMinutes) {
    ASSERT_TRUE(db_->saveSession(makeSession())) << engineName(GetParam());
    const auto p = sql::param(1, db_->dbType());
    const auto rows = db_->executeQuery(
        "SELECT AVG(b.avg_pressure) AS p, AVG(b.avg_flow_rate) AS f"
        " FROM cpap_breathing_summary b JOIN cpap_sessions s ON s.id = b.session_id"
        " WHERE s.device_id = " + p, {device_});
    ASSERT_EQ(rows.size(), 1u) << engineName(GetParam());
    EXPECT_NEAR(num(rows[0]["p"]), 6.5, 1e-6) << engineName(GetParam());   // (7 + 6) / 2
    EXPECT_NEAR(num(rows[0]["f"]), 2.0, 1e-6) << engineName(GetParam());   // (1.5 + 2.5) / 2
}

INSTANTIATE_TEST_SUITE_P(
    AllEngines, UnmeasuredMinuteBackendTest,
    ::testing::Values(Engine::SQLite, Engine::MySQL, Engine::Postgres),
    [](const ::testing::TestParamInfo<Engine>& info) { return std::string(engineName(info.param)); });
