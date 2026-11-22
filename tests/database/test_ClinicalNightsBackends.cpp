/**
 * SDD-082: the clinician and compliance reports' numbers, on every engine.
 *
 * QueryService::getClinicalNights builds the per-night figures CpapDash's
 * ingest computes at query time (flow limitation 95th, the minimum pressure,
 * EPAP, minutes over 24 L/min, the machine's Large Leak minutes) from the
 * night's own minutes and events, and reads the STR settings B10 adds to
 * cpap_daily_summary. The SQL is hand-built for three dialects, so it is run
 * on each: SQLite always, MySQL when MYSQL_TEST_HOST is set, PostgreSQL when
 * PGHOST is set.
 *
 * SAFETY: rows are namespaced to a per-process device id, dated 2099, and
 * deleted in TearDown.
 */

#include <gtest/gtest.h>

#include "database/SQLiteDatabase.h"
#include "database/SqlDialect.h"
#include "utils/TimeCompat.h"
#include "web/QueryService.h"
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
using namespace std::chrono;
namespace fs = std::filesystem;

namespace {

std::string envOr(const char* key, const std::string& fallback) {
    const char* v = std::getenv(key);
    return (v && *v) ? std::string(v) : fallback;
}

system_clock::time_point label(int y, int mo, int d, int h, int mi = 0) {
    std::tm tm{};
    tm.tm_year = y - 1900;
    tm.tm_mon = mo - 1;
    tm.tm_mday = d;
    tm.tm_hour = h;
    tm.tm_min = mi;
    return system_clock::from_time_t(timegm_utc(&tm));   // a label (SDD-074 D4)
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

double num(const Json::Value& row, const char* key) {
    const auto& v = row[key];
    if (v.isString()) return std::stod(v.asString());
    return v.asDouble();
}

class ClinicalNightsBackendTest : public ::testing::TestWithParam<Engine> {
protected:
    void SetUp() override {
        device_ = "clinical_test_" + std::to_string(::getpid());
        switch (GetParam()) {
            case Engine::SQLite: {
                path_ = (fs::temp_directory_path() /
                         ("hms_cpap_clinical_" + std::to_string(::getpid()) + ".db")).string();
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
            const auto p = sql::param(1, db_->dbType());
            db_->executeQuery("DELETE FROM cpap_sessions WHERE device_id = " + p, {device_});
            db_->executeQuery("DELETE FROM cpap_daily_summary WHERE device_id = " + p, {device_});
            db_->executeQuery("DELETE FROM cpap_reports WHERE device_id = " + p, {device_});
        }
        db_.reset();
        if (!path_.empty()) {
            for (const auto* suffix : {"", "-wal", "-shm"}) {
                std::error_code ec;
                fs::remove(path_ + suffix, ec);
            }
        }
    }

    /// The night of 2099-07-01: 21 minutes from 23:00. Flow limitation 0.00
    /// to 0.20, leak 0 to 40 L/min in steps of 2 (eight minutes over 24),
    /// pressure 6 to 10 with one minute off (0) and one glitch (40), EPAP 5
    /// to 7, and one Large Leak flag of 90 s.
    CPAPSession night() {
        CPAPSession s;
        s.device_id = device_;
        s.device_name = "AirSense 10 AutoSet";
        s.serial_number = "SDD082";
        s.session_start = label(2099, 7, 1, 23);
        s.duration_seconds = 21 * 60;
        s.data_records = 21;
        for (int i = 0; i < 21; ++i) {
            BreathingSummary b;
            b.timestamp = *s.session_start + minutes(i);
            b.respiratory_rate = 14.0;
            b.flow_limitation = i / 100.0;
            b.leak_rate = 2.0 * i;
            b.therapy_pressure = i == 3 ? 0.0 : (i == 4 ? 40.0 : 6.0 + i / 5.0);
            b.epr_pressure = 5.0 + (i % 3);
            s.breathing_summary.push_back(b);
        }
        s.events.emplace_back(EventType::LARGE_LEAK, *s.session_start + minutes(5), 90.0);
        s.events.emplace_back(EventType::HYPOPNEA, *s.session_start + minutes(7), 12.0);
        SessionMetrics m;
        m.ahi = 2.0;
        s.metrics = m;
        return s;
    }

    STRDailyRecord strDay(int day, double minutes_used, STRDailyRecord::Family family) {
        STRDailyRecord r;
        r.device_id = device_;
        r.record_date = label(2099, 7, day, 12);
        r.duration_minutes = minutes_used;
        r.ahi = 2.0;
        r.mode = 1;
        r.epr_level = 3;
        r.pressure_setting = 10;
        r.family = family;
        if (family == STRDailyRecord::Family::AutoSet) {
            r.min_pressure = 4.0;
            r.max_pressure = 20.0;
        }
        return r;
    }

    std::string path_;
    std::string device_;
    std::shared_ptr<IDatabase> db_;
};

}  // namespace

TEST_P(ClinicalNightsBackendTest, TheNightCarriesTheFiguresTheStrDoesNot) {
    ASSERT_TRUE(db_->saveSession(night())) << engineName(GetParam());
    ASSERT_TRUE(db_->saveSTRDailyRecords(
        {strDay(1, 420, STRDailyRecord::Family::AutoSet),
         strDay(3, 300, STRDailyRecord::Family::Unknown)}))
        << engineName(GetParam());

    QueryService qs(db_, device_);
    const auto rows = qs.getClinicalNights("2099-07-01", "2099-07-05");
    ASSERT_EQ(rows.size(), 2u) << engineName(GetParam());
    const auto& a = rows[0u];
    EXPECT_EQ(a["record_date"].asString(), "2099-07-01") << engineName(GetParam());

    // percentile_cont(0.95) of 0.00..0.20 is 0.19 exactly.
    EXPECT_NEAR(num(a, "flow_lim_95"), 0.19, 1e-9) << engineName(GetParam());
    EXPECT_NEAR(num(a, "press_min"), 6.0, 1e-9)
        << engineName(GetParam()) << ": the 0 and the 40 are not pressures";
    EXPECT_NEAR(num(a, "epap_min"), 5.0, 1e-9) << engineName(GetParam());
    EXPECT_NEAR(num(a, "epap_max"), 7.0, 1e-9) << engineName(GetParam());
    EXPECT_NEAR(num(a, "epap_avg"), 6.0, 0.01) << engineName(GetParam());
    EXPECT_NEAR(num(a, "leak_over_24_minutes"), 8.0, 1e-9)
        << engineName(GetParam()) << ": 26..40 L/min, eight minutes";
    EXPECT_NEAR(num(a, "large_leak_minutes"), 1.5, 1e-9) << engineName(GetParam());

    // B10: the STR's settings, back as the report reads them.
    EXPECT_EQ(static_cast<int>(num(a, "family")), 2) << engineName(GetParam());
    EXPECT_NEAR(num(a, "apap_min_pressure"), 4.0, 1e-9) << engineName(GetParam());
    EXPECT_NEAR(num(a, "apap_max_pressure"), 20.0, 1e-9) << engineName(GetParam());
    EXPECT_TRUE(a["bl_min_epap"].isNull()) << engineName(GetParam());

    // A night with no minutes and no flags: absent, never 0.
    const auto& b = rows[1u];
    EXPECT_EQ(b["record_date"].asString(), "2099-07-03") << engineName(GetParam());
    EXPECT_TRUE(b["flow_lim_95"].isNull()) << engineName(GetParam());
    EXPECT_TRUE(b["leak_over_24_minutes"].isNull()) << engineName(GetParam());
    EXPECT_TRUE(b["large_leak_minutes"].isNull()) << engineName(GetParam());
    EXPECT_EQ(static_cast<int>(num(b, "family")), 0) << engineName(GetParam());
    EXPECT_TRUE(b["apap_min_pressure"].isNull()) << engineName(GetParam());

    const auto onRecord = qs.getNightsOnRecord();
    EXPECT_EQ(onRecord["first_night"].asString(), "2099-07-01") << engineName(GetParam());
    EXPECT_EQ(onRecord["last_night"].asString(), "2099-07-03") << engineName(GetParam());
}

TEST_P(ClinicalNightsBackendTest, AnStrReadAgainWritesTheSettingsOverTheOldRow) {
    // A row written before B10 has family 0; the next STR read fills it.
    ASSERT_TRUE(db_->saveSTRDailyRecords({strDay(2, 400, STRDailyRecord::Family::Unknown)}));
    ASSERT_TRUE(db_->saveSTRDailyRecords({strDay(2, 400, STRDailyRecord::Family::AutoSet)}));
    QueryService qs(db_, device_);
    const auto rows = qs.getClinicalNights("2099-07-02", "2099-07-02");
    ASSERT_EQ(rows.size(), 1u) << engineName(GetParam());
    EXPECT_EQ(static_cast<int>(num(rows[0u], "family")), 2) << engineName(GetParam());
    EXPECT_NEAR(num(rows[0u], "apap_max_pressure"), 20.0, 1e-9) << engineName(GetParam());
}

TEST_P(ClinicalNightsBackendTest, AReportRowCarriesItsRule) {
    const DbType dt = db_->dbType();
    const int id = db_->insertReturningId(
        "INSERT INTO cpap_reports (device_id, range_start, range_end, filename, filepath, status, rule)"
        " VALUES (" + sql::param(1, dt) + "," + sql::param(2, dt) + "," + sql::param(3, dt) + "," +
        sql::param(4, dt) + "," + sql::param(5, dt) + ",'pending'," + sql::param(6, dt) + ")",
        {device_, "2099-07-01", "2099-07-30", "cpap_compliance_report_2099-07-01_to_2099-07-30.pdf",
         "/tmp/x.pdf", "{\"hours\":4.5,\"share\":80}"});
    ASSERT_GT(id, 0) << engineName(GetParam());
    const auto rows = db_->executeQuery(
        "SELECT rule FROM cpap_reports WHERE id = " + sql::param(1, dt), {std::to_string(id)});
    ASSERT_EQ(rows.size(), 1u) << engineName(GetParam());
    EXPECT_EQ(rows[0u]["rule"].asString(), "{\"hours\":4.5,\"share\":80}") << engineName(GetParam());
}

INSTANTIATE_TEST_SUITE_P(AllEngines, ClinicalNightsBackendTest,
                         ::testing::Values(Engine::SQLite, Engine::MySQL, Engine::Postgres),
                         [](const ::testing::TestParamInfo<Engine>& info) {
                             return std::string(engineName(info.param));
                         });
