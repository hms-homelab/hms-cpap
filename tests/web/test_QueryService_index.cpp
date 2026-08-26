/**
 * SDD-019: the sleep index on the read paths, across every engine we ship.
 *
 * The index itself is defined and tested in hms-cpapdash-parser. What is tested
 * here is everything hms-cpap adds around it, which is where the mistakes live:
 *
 *   - which column feeds the usage component (duration_minutes, not
 *     patient_hours, which on the hub holds a counter near 1050 on two of its
 *     230 rows against nights of 80 and 89 minutes);
 *   - that an absent input drops out rather than scoring zero, which depends on
 *     telling NULL apart from 0 through three different result encodings;
 *   - that the two queries it touches are accepted by all three dialects.
 *
 * That last one is the reason this file is parameterised rather than written
 * against SQLite alone. QueryService hand-writes SQL for three engines, the
 * dashboard's index query adds a LIMIT and two bare columns, and a dialect
 * rejecting either would break the dashboard for exactly the users we cannot
 * see. SQLite always runs. MySQL and PostgreSQL run when a server is reachable
 * and skip cleanly otherwise, so nobody's local suite goes red for lack of one.
 *
 * SAFETY: every row is namespaced to a synthetic per-process device_id and
 * deleted in TearDown. Nothing here drops or truncates anything, so pointing
 * these at a populated database cannot disturb real therapy data.
 */

#include <gtest/gtest.h>

#include "database/SQLiteDatabase.h"
#include "database/SqlDialect.h"
#include "web/QueryService.h"
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

// SQLite stringifies every non-null column while the others return typed
// values, so any assertion on a number has to accept both shapes. This is the
// same reason QueryService::jopt exists at all.
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

class QueryServiceIndexTest : public ::testing::TestWithParam<Engine> {
protected:
    void SetUp() override {
        device_ = "idx_test_" + std::to_string(::getpid());

        switch (GetParam()) {
            case Engine::SQLite:   setUpSQLite();   break;
            case Engine::MySQL:    setUpMySQL();    break;
            case Engine::Postgres: setUpPostgres(); break;
        }
        if (!db_) return;  // a GTEST_SKIP above
        qs_ = std::make_unique<QueryService>(db_, device_);
    }

    void setUpSQLite() {
        static int counter = 0;
        path_ = (fs::temp_directory_path() /
                 ("hms_cpap_idx_" + std::to_string(::getpid()) + "_" +
                  std::to_string(counter++) + ".db")).string();
        removeSQLiteFiles();
        auto lite = std::make_shared<SQLiteDatabase>(path_);
        ASSERT_TRUE(lite->connect());
        db_ = std::move(lite);
    }

    void setUpMySQL() {
#ifndef WITH_MYSQL
        GTEST_SKIP() << "built without MySQL (-DBUILD_WITH_MYSQL=OFF)";
#else
        const std::string host = envOr("MYSQL_TEST_HOST", "");
        if (host.empty()) {
            GTEST_SKIP() << "MYSQL_TEST_HOST unset, skipping the index on MySQL. "
                            "Set MYSQL_TEST_HOST/PORT/DB/USER/PASSWORD to run it.";
        }
        auto my = std::make_shared<MySQLDatabase>(
            host,
            std::stoi(envOr("MYSQL_TEST_PORT", "3306")),
            envOr("MYSQL_TEST_USER", ""),
            envOr("MYSQL_TEST_PASSWORD", ""),
            envOr("MYSQL_TEST_DB", "hms_cpap_test"));
        if (!my->connect()) {
            GTEST_SKIP() << "No usable MySQL at " << host << ", skipping the index there.";
        }
        db_ = std::move(my);
#endif
    }

    void setUpPostgres() {
#ifndef WITH_POSTGRESQL
        GTEST_SKIP() << "built without PostgreSQL (-DBUILD_WITH_POSTGRESQL=OFF)";
#else
        const std::string host = envOr("PGHOST", "");
        if (host.empty()) {
            GTEST_SKIP() << "PGHOST unset, skipping the index on PostgreSQL. "
                            "Set PGHOST/PGPORT/PGUSER/PGPASSWORD/PGDATABASE to run it.";
        }
        const std::string conn =
            "host=" + host +
            " port=" + envOr("PGPORT", "5432") +
            " user=" + envOr("PGUSER", "maestro") +
            " password=" + envOr("PGPASSWORD", "") +
            " dbname=" + envOr("PGDATABASE", "cpap_monitoring") +
            " connect_timeout=3";
        auto pg = std::make_shared<PostgresDatabase>(conn);
        if (!pg->connect()) {
            GTEST_SKIP() << "No usable PostgreSQL at " << host << ", skipping the index there.";
        }
        db_ = std::move(pg);
#endif
    }

    void TearDown() override {
        if (db_) {
            // Only ever this process's own synthetic device. The placeholder
            // has to come from the dialect helper: PostgreSQL wants $1 where
            // the other two want ?, and a literal here would silently delete
            // nothing on one engine and leave rows behind.
            db_->executeQuery("DELETE FROM cpap_daily_summary WHERE device_id = " +
                                  sql::param(1, db_->dbType()),
                              {device_});
        }
        qs_.reset();
        db_.reset();
        removeSQLiteFiles();
    }

    void removeSQLiteFiles() {
        if (path_.empty()) return;
        for (const auto* suffix : {"", "-wal", "-shm"}) {
            std::error_code ec;
            fs::remove(path_ + suffix, ec);
        }
    }

    /// Noon, days_ago back from today. Relative rather than fixed, because the
    /// dashboard's own window is relative and a hard-coded date rots out of it.
    static system_clock::time_point noonDaysAgo(int days_ago) {
        const long secs = duration_cast<seconds>(system_clock::now().time_since_epoch()).count();
        const long midnight = (secs / 86400) * 86400;
        return system_clock::time_point{} +
               seconds(midnight - static_cast<long>(days_ago) * 86400 + 12 * 3600);
    }

    /// One day in cpap_daily_summary. leak_95 of -1 means "this machine
    /// reported no leak", which the writers store as NULL.
    void addDay(int days_ago, double ahi, double duration_minutes, double leak_95) {
        STRDailyRecord r;
        r.device_id = device_;
        r.record_date = noonDaysAgo(days_ago);
        r.ahi = ahi;
        r.duration_minutes = duration_minutes;
        r.leak_95 = leak_95 < 0 ? 0 : leak_95;
        ASSERT_TRUE(db_->saveSTRDailyRecords({r}));
        if (leak_95 < 0) {
            // saveSTRDailyRecords takes a plain double and cannot express
            // "absent", so the NULL the real writers store for a machine with
            // no leak channel has to be put back by hand.
            db_->executeQuery("UPDATE cpap_daily_summary SET leak_95 = NULL"
                              " WHERE device_id = " + sql::param(1, db_->dbType()),
                              {device_});
        }
    }

    Json::Value allDays() {
        return qs_->getDailySummary("2000-01-01", "2099-12-31");
    }

    std::string path_;
    std::string device_;
    std::shared_ptr<IDatabase> db_;
    std::unique_ptr<QueryService> qs_;
};

// ─────────────────────────────────────────────────────────────────────────────
// The queries have to be accepted by every dialect before anything else matters
// ─────────────────────────────────────────────────────────────────────────────

TEST_P(QueryServiceIndexTest, BothIndexQueriesRunOnAnEmptyDatabase) {
    // The dashboard's new trailing-week query has a LIMIT and two bare columns
    // that the other dialects have to accept too. An empty database still
    // parses and plans them, which is the half of the risk that has nothing to
    // do with the data.
    EXPECT_NO_THROW({
        const auto dash = qs_->getDashboard();
        EXPECT_TRUE(dash.isObject()) << engineName(GetParam());
        EXPECT_TRUE(dash.isMember("sleep_index_7night"))
            << engineName(GetParam()) << ": the dashboard lost the index";
        EXPECT_TRUE(dash["sleep_index_7night"].isNull())
            << engineName(GetParam()) << ": an empty database has no index to report";
        (void)allDays();
    }) << engineName(GetParam());
}

// ─────────────────────────────────────────────────────────────────────────────
// The index itself
// ─────────────────────────────────────────────────────────────────────────────

TEST_P(QueryServiceIndexTest, APerfectNightScoresAHundred) {
    addDay(1, /*ahi=*/2.0, /*minutes=*/480, /*leak_95=*/10.0);

    const auto rows = allDays();
    ASSERT_EQ(rows.size(), 1u) << engineName(GetParam());
    EXPECT_EQ(asNumber(rows[0]["sleep_index"]), 100) << engineName(GetParam());
    EXPECT_EQ(rows[0]["sleep_index_band"].asString(), "excellent") << engineName(GetParam());
}

TEST_P(QueryServiceIndexTest, EveryComponentHalfCreditScoresFifty) {
    // 3.5h of 7, AHI halfway from 5 to 30, leak halfway from 24 to 40.
    addDay(1, /*ahi=*/17.5, /*minutes=*/210, /*leak_95=*/32.0);

    const auto rows = allDays();
    ASSERT_EQ(rows.size(), 1u);
    EXPECT_EQ(asNumber(rows[0]["sleep_index"]), 50) << engineName(GetParam());
    EXPECT_EQ(rows[0]["sleep_index_band"].asString(), "fair") << engineName(GetParam());
}

TEST_P(QueryServiceIndexTest, AMissingLeakRenormalisesRatherThanScoringZero) {
    // Same night as above with no leak channel at all. Dropping the component
    // and renormalising over the remaining 80 leaves the index unchanged at 50.
    // Scoring the absent leak as zero would give 40, and clamping it to full
    // credit would give 60, so this single number separates all three.
    addDay(1, /*ahi=*/17.5, /*minutes=*/210, /*leak_95=*/-1);

    const auto rows = allDays();
    ASSERT_EQ(rows.size(), 1u);
    EXPECT_EQ(asNumber(rows[0]["sleep_index"]), 50)
        << engineName(GetParam())
        << ": a machine that reports no leak was not renormalised. 40 means the "
           "absent leak scored zero, 60 means it was read as a perfect seal.";
}

TEST_P(QueryServiceIndexTest, TheWorstNightScoresZeroRatherThanNothing) {
    // Zero is a real answer and must not be confused with "no index".
    addDay(1, /*ahi=*/60.0, /*minutes=*/0, /*leak_95=*/80.0);

    const auto rows = allDays();
    ASSERT_EQ(rows.size(), 1u);
    ASSERT_FALSE(rows[0]["sleep_index"].isNull()) << engineName(GetParam());
    EXPECT_EQ(asNumber(rows[0]["sleep_index"]), 0) << engineName(GetParam());
    EXPECT_EQ(rows[0]["sleep_index_band"].asString(), "needs_attention");
}

TEST_P(QueryServiceIndexTest, TheDashboardScoresTheLatestNightAndTheWeek) {
    // Newest night is perfect; the six before it are the half-credit night.
    addDay(1, 2.0, 480, 10.0);
    for (int i = 2; i <= 7; ++i) addDay(i, 17.5, 210, 32.0);

    const auto dash = qs_->getDashboard();
    ASSERT_TRUE(dash.isMember("latest_night"));
    EXPECT_EQ(asNumber(dash["latest_night"]["sleep_index"]), 100)
        << engineName(GetParam()) << ": the dashboard scored the wrong night";
    EXPECT_EQ(dash["latest_night"]["sleep_index_band"].asString(), "excellent");

    // (100 + 6*50) / 7 = 57.1
    ASSERT_FALSE(dash["sleep_index_7night"].isNull()) << engineName(GetParam());
    EXPECT_NEAR(asNumber(dash["sleep_index_7night"]), 57.1, 0.05) << engineName(GetParam());
    EXPECT_EQ(dash["sleep_index_7night_band"].asString(), "fair");
}

TEST_P(QueryServiceIndexTest, TheWeeklyAverageStopsAtSevenNights) {
    // Eight perfect nights and one terrible one nine days back. The ninth is
    // outside the window and must not pull the average down.
    for (int i = 1; i <= 8; ++i) addDay(i, 2.0, 480, 10.0);
    addDay(9, 60.0, 0, 80.0);

    const auto dash = qs_->getDashboard();
    EXPECT_NEAR(asNumber(dash["sleep_index_7night"]), 100.0, 0.05)
        << engineName(GetParam()) << ": the trailing week reached past seven nights";
}

INSTANTIATE_TEST_SUITE_P(
    Engines, QueryServiceIndexTest,
    ::testing::Values(Engine::SQLite, Engine::MySQL, Engine::Postgres),
    [](const ::testing::TestParamInfo<Engine>& info) {
        return std::string(engineName(info.param));
    });

}  // namespace
