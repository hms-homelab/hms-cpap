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
            // SDD-026 cases save a session too; its metrics cascade.
            db_->executeQuery("DELETE FROM cpap_sessions WHERE device_id = " +
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

    // ── SDD-026 helpers: the same night from both writers ─────────────────
    //
    // Both writers key the night by a LOCAL date: the STR writer formats
    // record_date with localtime, the session writer takes date(session_start
    // - 12 hours) on the timestamp it stored, also in local time. So the two
    // dates are derived from ONE instant: the session starts ten hours after
    // the anchor, and the STR's record_date is that start minus twelve hours,
    // which is exactly the shift the session writer applies. Anything else
    // depends on the machine's timezone: an anchor-based record_date matched
    // at UTC-4 and split the night into two rows on the UTC runner.
    static system_clock::time_point strDay() {
        return system_clock::time_point{} + seconds(1756000000);
    }
    static system_clock::time_point ourSessionStart() { return strDay() + hours(10); }
    static system_clock::time_point strRecordDate()   { return ourSessionStart() - hours(12); }

    /// The STR's view of the night: its own duration, index and leak.
    void saveStrNight(double duration_minutes, double ahi, double leak_95) {
        STRDailyRecord r;
        r.device_id = device_;
        r.record_date = strRecordDate();
        r.duration_minutes = duration_minutes;
        r.patient_hours = 1050.0;
        r.ahi = ahi;
        r.hi = ahi / 2;
        r.ai = ahi / 2;
        r.oai = ahi / 4;
        r.leak_95 = leak_95;
        ASSERT_TRUE(db_->saveSTRDailyRecords({r}));
    }

    /// Our view of the same night: one mask-on session with its own
    /// duration and event counts, folded into the daily summary the way the
    /// collector does after every save. See strRecordDate() for why its start
    /// and the STR's date are tied to each other.
    void saveOurNight(int duration_seconds, double ahi, int obstructive, int hypopneas) {
        CPAPSession s;
        s.device_id = device_;
        s.device_name = "AirSense 11";
        s.serial_number = "SDD026";
        s.session_start = ourSessionStart();
        s.duration_seconds = duration_seconds;
        s.data_records = 10;
        SessionMetrics m;
        m.ahi = ahi;
        m.obstructive_apneas = obstructive;
        m.hypopneas = hypopneas;
        m.total_events = obstructive + hypopneas;
        m.leak_p95 = 99.0;   // a session mean that must NOT replace the STR's
        s.metrics = m;
        ASSERT_TRUE(db_->saveSession(s));
        ASSERT_TRUE(db_->aggregateDailySummaryFromSessions(device_));
    }

    Json::Value readNight() {
        return db_->executeQuery(
            "SELECT duration_minutes, ahi, leak_95, index_source,"
            " ahi_str, duration_minutes_str"
            " FROM cpap_daily_summary WHERE device_id = " + sql::param(1, db_->dbType()),
            {device_});
    }
    static std::string asText(const Json::Value& v) {
        return v.isNull() ? std::string("<null>") : v.asString();
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

// ── SDD-026: our numbers win, the STR stays official ─────────────────────
//
// The night as the STR reports it: 184 minutes, AHI 4.2 (floored to one
// decimal by the file format), leak p95 20. The night as our sessions add
// up: 13 index events over a 193-minute recording span. The rule Albin set on
// 2026-09-08, "match the cloud, divide vs STR duration when it has one": the
// night's hours are the STR's 184 minutes, our 13 events divide by them, so
// the headline reads 13 / 3.067 h = 4.24, the same figure cpapdash.com shows.
// The _str columns keep the STR's own 4.2, and the STR's leak, a kind A
// field, must survive our session mean.
//
// saveOurNight stores m.ahi = 13 events / 3.217 h = 4.041 over 193 minutes,
// which the writer turns back into the count before dividing again.

TEST_P(DailyHoursBackendTest, OurEventsOverTheStrHoursWhenTheStrCameFirst) {
    saveStrNight(184.0, 4.2, 20.0);
    saveOurNight(193 * 60, 13.0 / (193.0 / 60.0), 10, 3);

    const auto rows = readNight();
    ASSERT_EQ(rows.size(), 1u) << engineName(GetParam()) << ": one row per night";
    const auto& r = rows[0];
    EXPECT_NEAR(asNumber(r["ahi"]), 4.24, 0.01)
        << engineName(GetParam()) << ": 13 of our events over the STR's 184 minutes";
    EXPECT_NEAR(asNumber(r["duration_minutes"]), 184.0, 0.1)
        << engineName(GetParam()) << ": the hours are the STR's Duration when the night has one";
    EXPECT_NEAR(asNumber(r["ahi_str"]), 4.2, 0.01) << engineName(GetParam()) << ": the STR's AHI is kept beside it";
    EXPECT_NEAR(asNumber(r["duration_minutes_str"]), 184.0, 0.1) << engineName(GetParam());
    EXPECT_NEAR(asNumber(r["leak_95"]), 20.0, 0.01)
        << engineName(GetParam()) << ": leak is the machine's own and a session mean must not replace it";
    EXPECT_EQ(asText(r["index_source"]), "computed") << engineName(GetParam());
}

TEST_P(DailyHoursBackendTest, OurEventsOverTheStrHoursWhenTheStrCameLast) {
    saveOurNight(193 * 60, 13.0 / (193.0 / 60.0), 10, 3);
    {
        // No STR yet: our span is all there is.
        const auto rows = readNight();
        ASSERT_EQ(rows.size(), 1u) << engineName(GetParam());
        EXPECT_NEAR(asNumber(rows[0]["ahi"]), 4.04, 0.01) << engineName(GetParam());
        EXPECT_NEAR(asNumber(rows[0]["duration_minutes"]), 193.0, 0.1) << engineName(GetParam());
    }
    saveStrNight(184.0, 4.2, 20.0);
    // The collector re-derives after every STR read (processSessionSummary),
    // which is what moves the night onto the STR's hours.
    ASSERT_TRUE(db_->aggregateDailySummaryFromSessions(device_));

    const auto rows = readNight();
    ASSERT_EQ(rows.size(), 1u) << engineName(GetParam());
    const auto& r = rows[0];
    EXPECT_NEAR(asNumber(r["ahi"]), 4.24, 0.01)
        << engineName(GetParam()) << ": once the STR is in, our events divide by its hours";
    EXPECT_NEAR(asNumber(r["duration_minutes"]), 184.0, 0.1) << engineName(GetParam());
    EXPECT_NEAR(asNumber(r["ahi_str"]), 4.2, 0.01) << engineName(GetParam());
    EXPECT_NEAR(asNumber(r["duration_minutes_str"]), 184.0, 0.1) << engineName(GetParam());
    EXPECT_NEAR(asNumber(r["leak_95"]), 20.0, 0.01) << engineName(GetParam());
    EXPECT_EQ(asText(r["index_source"]), "computed") << engineName(GetParam());
}

TEST_P(DailyHoursBackendTest, WithoutAnStrALaterSessionSaveUpdatesTheSessionMeans) {
    // A session is saved after every checkpoint file, so the leak of the
    // first ten minutes must not be frozen into the night by the first save.
    saveOurNight(60 * 60, 4.0, 2, 1);        // first parse, leak p95 99
    {
        auto rows = db_->executeQuery(
            "UPDATE cpap_session_metrics SET leak_p95 = 7.5 WHERE session_id IN"
            " (SELECT id FROM cpap_sessions WHERE device_id = " + sql::param(1, db_->dbType()) + ")",
            {device_});
    }
    ASSERT_TRUE(db_->aggregateDailySummaryFromSessions(device_));   // later parse, leak p95 7.5

    const auto rows = readNight();
    ASSERT_EQ(rows.size(), 1u) << engineName(GetParam());
    EXPECT_NEAR(asNumber(rows[0]["leak_95"]), 7.5, 0.01)
        << engineName(GetParam()) << ": the first parse's session mean was frozen into the night";
}

TEST_P(DailyHoursBackendTest, AnStrOnlyNightKeepsTheStrFiguresAndSaysSo) {
    // History the card no longer holds: the STR is all there is, and the
    // trends must not go blank for it.
    saveStrNight(184.0, 4.2, 20.0);

    const auto rows = readNight();
    ASSERT_EQ(rows.size(), 1u) << engineName(GetParam());
    const auto& r = rows[0];
    EXPECT_NEAR(asNumber(r["ahi"]), 4.2, 0.01) << engineName(GetParam());
    EXPECT_NEAR(asNumber(r["duration_minutes"]), 184.0, 0.1) << engineName(GetParam());
    EXPECT_NEAR(asNumber(r["ahi_str"]), 4.2, 0.01) << engineName(GetParam());
    EXPECT_EQ(asText(r["index_source"]), "str") << engineName(GetParam());
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

// ── SDD-024: index_kind must survive the database ───────────────────────────
//
// This is the test whose ABSENCE let SDD-023 look finished. That change branches
// on SessionMetrics::index_kind at the MQTT publisher and its unit tests passed,
// but the Sefam path calls the publisher with metrics READ BACK FROM THE
// DATABASE -- and there was no column, so the value was always the struct
// default (AHI) and an apnea-only number went out under the name "AHI".
//
// A test that never crosses the database proves nothing about a path that does.

TEST_P(DailyHoursBackendTest, IndexKindSurvivesTheRoundTrip) {
    const auto start = std::chrono::system_clock::now() - std::chrono::hours(8);

    CPAPSession s;
    s.device_id = device_;
    s.session_start = start;
    s.session_end = start + std::chrono::hours(7);
    s.duration_seconds = 7 * 3600;
    s.status = CPAPSession::Status::COMPLETED;

    SessionMetrics m;
    m.ahi = 4.2;
    m.obstructive_apneas = 20;
    m.total_events = 20;
    // The Sefam case: apneas scored, hypopneas not marked at all.
    m.index_kind = SessionMetrics::IndexKind::Ungraded;
    s.metrics = m;

    ASSERT_TRUE(db_->saveSession(s)) << engineName(GetParam());

    auto back = db_->getNightlyMetrics(device_, start);
    ASSERT_TRUE(back.has_value()) << engineName(GetParam());
    EXPECT_EQ(back->index_kind, SessionMetrics::IndexKind::Ungraded)
        << engineName(GetParam())
        << ": an apnea-only index came back claiming to be an AHI. Every "
           "consumer downstream -- MQTT, the dashboard, the reports, the LLM "
           "summary -- believes this field.";
}

// The upgrade path. Every row already on disk was written before the column
// existed, and every one of them is ResMed, so the absence must read as `ahi`
// rather than as unknown or as ungraded.
TEST_P(DailyHoursBackendTest, AResMedNightStillReadsAsAnAhi) {
    const auto start = std::chrono::system_clock::now() - std::chrono::hours(20);

    CPAPSession s;
    s.device_id = device_;
    s.session_start = start;
    s.session_end = start + std::chrono::hours(6);
    s.duration_seconds = 6 * 3600;
    s.status = CPAPSession::Status::COMPLETED;

    SessionMetrics m;
    m.ahi = 3.1;
    m.hypopneas = 9;
    m.total_events = 9;
    // Left at the default, exactly as a ResMed session arrives.
    s.metrics = m;

    ASSERT_TRUE(db_->saveSession(s)) << engineName(GetParam());

    auto back = db_->getNightlyMetrics(device_, start);
    ASSERT_TRUE(back.has_value()) << engineName(GetParam());
    EXPECT_EQ(back->index_kind, SessionMetrics::IndexKind::AHI)
        << engineName(GetParam()) << ": a ResMed night must stay gradable";
}

INSTANTIATE_TEST_SUITE_P(
    Engines, DailyHoursBackendTest,
    ::testing::Values(Engine::SQLite, Engine::MySQL, Engine::Postgres),
    [](const ::testing::TestParamInfo<Engine>& info) {
        return std::string(engineName(info.param));
    });
