/**
 * cpap_daily_summary.patient_hours means ONE thing, on every engine.
 *
 * It did not. Two writers filled it with two different quantities: the
 * session-derived path wrote SUM(duration_seconds)/3600, the day's usage, while
 * the STR path wrote ResMed's `PatientHours` signal, which the machine's own
 * STR.edf shows to be a LIFETIME COUNTER in hours whose per-day delta is the
 * day's usage. On the hub's 230 rows the two disagreed twice, holding ~1050
 * against nights of 80 and 89 minutes.
 *
 * So: patient_hours is the day's usage from both writers, and the counter lives
 * in machine_hours. These tests pin both halves, plus the migration that repairs
 * rows written the old way, on all three engines, because a schema change that
 * lands on one backend and not the others is this codebase's most expensive
 * recurring bug.
 *
 * SAFETY: every row is namespaced to a per-process device id and deleted in
 * TearDown. Nothing here drops or truncates.
 */

#include <gtest/gtest.h>

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

using namespace hms_cpap;
using namespace std::chrono;
namespace fs = std::filesystem;

namespace {

std::string envOr(const char* key, const std::string& fallback) {
    const char* v = std::getenv(key);
    return (v && *v) ? std::string(v) : fallback;
}

// SQLite stringifies every column; the others return typed values.
double asNumber(const Json::Value& v) {
    if (v.isNull()) return -1;
    if (v.isString()) return std::stod(v.asString());
    return v.asDouble();
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

class DailyHoursBackendTest : public ::testing::TestWithParam<Engine> {
protected:
    void SetUp() override {
        device_ = "hours_test_" + std::to_string(::getpid());
        makeDb(db_);
    }

    /// Builds and connects a database of the parameterised engine into [out].
    ///
    /// An out-parameter rather than a return value because GTEST_SKIP expands to
    /// a bare `return`, which only compiles in a void function. The migration
    /// tests call this a second time to get a fresh connection over the same
    /// store, since reconnecting is what actually re-runs the migration.
    void makeDb(std::shared_ptr<IDatabase>& out) {
        out.reset();
        switch (GetParam()) {
            case Engine::SQLite: {
                if (path_.empty()) {
                    static int counter = 0;
                    path_ = (fs::temp_directory_path() /
                             ("hms_cpap_hours_" + std::to_string(::getpid()) + "_" +
                              std::to_string(counter++) + ".db")).string();
                }
                auto lite = std::make_shared<SQLiteDatabase>(path_);
                ASSERT_TRUE(lite->connect()) << "SQLite connect failed";
                out = std::move(lite);
                return;
            }
            case Engine::MySQL: {
#ifndef WITH_MYSQL
                GTEST_SKIP() << "built without MySQL (-DBUILD_WITH_MYSQL=OFF)";
#else
                const std::string host = envOr("MYSQL_TEST_HOST", "");
                if (host.empty()) {
                    GTEST_SKIP() << "MYSQL_TEST_HOST unset, skipping MySQL.";
                }
                auto my = std::make_shared<MySQLDatabase>(
                    host, std::stoi(envOr("MYSQL_TEST_PORT", "3306")),
                    envOr("MYSQL_TEST_USER", ""), envOr("MYSQL_TEST_PASSWORD", ""),
                    envOr("MYSQL_TEST_DB", "hms_cpap_test"));
                if (!my->connect()) GTEST_SKIP() << "No usable MySQL at " << host;
                out = std::move(my);
                return;
#endif
            }
            case Engine::Postgres: {
#ifndef WITH_POSTGRESQL
                GTEST_SKIP() << "built without PostgreSQL";
#else
                const std::string host = envOr("PGHOST", "");
                if (host.empty()) {
                    GTEST_SKIP() << "PGHOST unset, skipping PostgreSQL.";
                }
                const std::string conn =
                    "host=" + host + " port=" + envOr("PGPORT", "5432") +
                    " user=" + envOr("PGUSER", "maestro") +
                    " password=" + envOr("PGPASSWORD", "") +
                    " dbname=" + envOr("PGDATABASE", "cpap_monitoring") +
                    " connect_timeout=3";
                auto pg = std::make_shared<PostgresDatabase>(conn);
                if (!pg->connect()) GTEST_SKIP() << "No usable PostgreSQL at " << host;
                out = std::move(pg);
                return;
#endif
            }
        }
    }

    void TearDown() override {
        if (db_) {
            db_->executeQuery("DELETE FROM cpap_daily_summary WHERE device_id = " +
                                  sql::param(1, db_->dbType()),
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

    /// Save one STR day. `counter` is the raw ResMed PatientHours signal, i.e.
    /// what the machine reports as its lifetime total.
    void saveStrDay(double duration_minutes, double counter) {
        STRDailyRecord r;
        r.device_id = device_;
        r.record_date = system_clock::time_point{} + seconds(1756000000);
        r.duration_minutes = duration_minutes;
        r.patient_hours = counter;
        ASSERT_TRUE(db_->saveSTRDailyRecords({r}));
    }

    Json::Value readRow() {
        return db_->executeQuery(
            "SELECT duration_minutes, patient_hours, machine_hours"
            " FROM cpap_daily_summary WHERE device_id = " + sql::param(1, db_->dbType()),
            {device_});
    }

    std::string path_;
    std::string device_;
    std::shared_ptr<IDatabase> db_;
};

}  // namespace

TEST_P(DailyHoursBackendTest, TheStrWriterSplitsTheDayFromTheCounter) {
    // 7h12m of therapy on a machine that has now run 1050 hours in total.
    saveStrDay(432.0, 1050.0);

    const auto rows = readRow();
    ASSERT_EQ(rows.size(), 1u) << engineName(GetParam());

    EXPECT_NEAR(asNumber(rows[0]["patient_hours"]), 7.2, 0.001)
        << engineName(GetParam())
        << ": patient_hours must be THIS DAY's usage, not the lifetime counter";
    EXPECT_NEAR(asNumber(rows[0]["machine_hours"]), 1050.0, 0.001)
        << engineName(GetParam()) << ": the lifetime counter must be kept, not discarded";
}

TEST_P(DailyHoursBackendTest, PatientHoursAgreesWithDurationMinutes) {
    // The invariant that makes the column usable at all, and the one the index
    // in SDD-019 depends on.
    saveStrDay(89.0, 1049.0);

    const auto rows = readRow();
    ASSERT_EQ(rows.size(), 1u);
    const double minutes = asNumber(rows[0]["duration_minutes"]);
    const double hours = asNumber(rows[0]["patient_hours"]);
    EXPECT_NEAR(hours, minutes / 60.0, 0.001)
        << engineName(GetParam()) << ": " << minutes << " minutes reported as " << hours << " h";
    EXPECT_LT(hours, 24.0) << engineName(GetParam()) << ": a day cannot hold more than 24 hours";
}

TEST_P(DailyHoursBackendTest, AZeroUsageDayIsZeroHoursNotACounter) {
    saveStrDay(0.0, 1050.0);

    const auto rows = readRow();
    ASSERT_EQ(rows.size(), 1u);
    EXPECT_NEAR(asNumber(rows[0]["patient_hours"]), 0.0, 0.001) << engineName(GetParam());
    EXPECT_NEAR(asNumber(rows[0]["machine_hours"]), 1050.0, 0.001) << engineName(GetParam());
}

TEST_P(DailyHoursBackendTest, TheMigrationRepairsARowWrittenTheOldWay) {
    saveStrDay(80.0, 1050.0);

    // Put the row back into the shape the old writer left: the counter sitting
    // in patient_hours, and machine_hours empty.
    const std::string ph = sql::param(1, db_->dbType());
    db_->executeQuery("UPDATE cpap_daily_summary"
                      "   SET patient_hours = 1050, machine_hours = NULL"
                      " WHERE device_id = " + ph,
                      {device_});
    ASSERT_NEAR(asNumber(readRow()[0]["patient_hours"]), 1050.0, 0.001)
        << "the legacy shape was not set up";

    // Connecting again re-runs the schema migration, which is the real code
    // path an upgrading install takes.
    std::shared_ptr<IDatabase> second;
    makeDb(second);
    ASSERT_TRUE(second != nullptr);
    const auto rows = second->executeQuery(
        "SELECT duration_minutes, patient_hours, machine_hours"
        " FROM cpap_daily_summary WHERE device_id = " + sql::param(1, second->dbType()),
        {device_});
    ASSERT_EQ(rows.size(), 1u) << engineName(GetParam());

    EXPECT_NEAR(asNumber(rows[0]["patient_hours"]), 80.0 / 60.0, 0.001)
        << engineName(GetParam()) << ": the repair did not rewrite patient_hours";
    EXPECT_NEAR(asNumber(rows[0]["machine_hours"]), 1050.0, 0.001)
        << engineName(GetParam()) << ": the repair threw the counter away instead of moving it";
}

TEST_P(DailyHoursBackendTest, TheMigrationLeavesACorrectRowAlone) {
    // The guard is patient_hours > 24. A healthy row must survive untouched, and
    // must survive it twice, because the migration runs on every single start.
    saveStrDay(432.0, 1050.0);

    for (int pass = 0; pass < 2; ++pass) {
        std::shared_ptr<IDatabase> again;
        makeDb(again);
        ASSERT_TRUE(again != nullptr);
        const auto rows = again->executeQuery(
            "SELECT patient_hours, machine_hours FROM cpap_daily_summary"
            " WHERE device_id = " + sql::param(1, again->dbType()),
            {device_});
        ASSERT_EQ(rows.size(), 1u);
        EXPECT_NEAR(asNumber(rows[0]["patient_hours"]), 7.2, 0.001)
            << engineName(GetParam()) << ": pass " << pass << " moved a correct value";
        EXPECT_NEAR(asNumber(rows[0]["machine_hours"]), 1050.0, 0.001)
            << engineName(GetParam()) << ": pass " << pass << " clobbered the counter";
    }
}

INSTANTIATE_TEST_SUITE_P(
    Engines, DailyHoursBackendTest,
    ::testing::Values(Engine::SQLite, Engine::MySQL, Engine::Postgres),
    [](const ::testing::TestParamInfo<Engine>& info) {
        return std::string(engineName(info.param));
    });
