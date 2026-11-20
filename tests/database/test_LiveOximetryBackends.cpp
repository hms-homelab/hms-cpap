//
// test_LiveOximetryBackends.cpp: the bridge's live O2 Ring stream, on every engine.
//
// In bridge mode each poll appends one sample to a synthetic session,
// live_<YYYYMMDD>.vld, until the ring's own file for the night lands. Three
// things are pinned here, on SQLite always and on MySQL and PostgreSQL when a
// server is configured (MYSQL_TEST_*, PG*):
//
//   1. A live sample is stored on the clock the caller hands in (the ring's
//      wall clock, as every other ring intake stores it), never the database's
//      NOW(). On PostgreSQL and MySQL NOW() follows the database server's time
//      zone, UTC in most containers, which put the live samples hours away
//      from the CPAP data and from the ring's own file for the same night.
//   2. Saving the ring's own file for a night removes that night's live
//      session, on that event, and leaves other nights' live sessions alone.
//   3. A live session that is still beside a real file (written before this
//      fix, or a second wear the same sleep day) is skipped by every sample
//      reader: the night chart, the sessions list and the SleepHQ export. A
//      live-only night still reads as before.
//
// SAFETY: these tests never DROP or TRUNCATE. They write under the real
// kOximetryDeviceId, because that is what the chart and the export read, so
// every row is dated 2099 (far from any real night, and under libstdc++'s 2262
// time_point limit) and removed again in TearDown by those dates.
//
#include <gtest/gtest.h>

#include "database/IDatabase.h"
#include "database/SQLiteDatabase.h"
#include "database/SqlDialect.h"
#include "services/SleepHqExportService.h"
#include "utils/OximetryDevice.h"
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
using cpapdash::parser::OximetrySample;
using cpapdash::parser::OximetrySession;
namespace fs = std::filesystem;

namespace {

std::string envOr(const char* key, const std::string& fallback) {
    const char* v = std::getenv(key);
    return (v && *v) ? std::string(v) : fallback;
}

// SQLite stringifies every column; the others return typed values.
long long asNum(const Json::Value& v) {
    if (v.isNull()) return -1;
    if (v.isString()) return std::stoll(v.asString());
    return v.asInt64();
}

/// A ring wall-clock time: its printed time read AS IF it were UTC (timegm),
/// the shape every oximetry parser produces.
std::chrono::system_clock::time_point ring(int y, int mo, int d, int h, int mi) {
    std::tm tm{};
    tm.tm_year = y - 1900;
    tm.tm_mon  = mo - 1;
    tm.tm_mday = d;
    tm.tm_hour = h;
    tm.tm_min  = mi;
    return std::chrono::system_clock::from_time_t(timegm_utc(&tm));
}

// The ring's own file for a night: n samples, 4 s apart, all readable.
OximetrySession ringFile(const std::string& filename,
                         std::chrono::system_clock::time_point start, int n) {
    OximetrySession s;
    s.filename = filename;
    s.start_time = start;
    s.sample_interval = 4.0;
    for (int i = 0; i < n; ++i) {
        OximetrySample smp{};
        smp.timestamp    = start + std::chrono::seconds(i * 4);
        smp.spo2         = 96;
        smp.heart_rate   = 60;
        smp.invalid_flag = 0;
        s.samples.push_back(smp);
    }
    s.end_time = s.samples.back().timestamp;
    s.duration_seconds = (n - 1) * 4;
    s.metrics.avg_spo2 = 96.0;
    s.metrics.min_spo2 = 96.0;
    s.metrics.valid_samples = n;
    s.metrics.total_samples = n;
    return s;
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

class LiveOximetryBackendTest : public ::testing::TestWithParam<Engine> {
protected:
    void SetUp() override {
        file_ = "liveo2_" + std::to_string(::getpid()) + ".vld";
        switch (GetParam()) {
            case Engine::SQLite: {
                path_ = (fs::temp_directory_path() /
                         ("hms_cpap_liveo2_" + std::to_string(::getpid()) + ".db")).string();
                fs::remove(path_);
                auto lite = std::make_shared<SQLiteDatabase>(path_);
                ASSERT_TRUE(lite->connect()) << "SQLite connect failed";
                db_ = std::move(lite);
                break;
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
                break;
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
                break;
#endif
            }
        }
        cleanup();   // a previous crashed run must not leave a night behind
    }

    void TearDown() override {
        auto& svc = SleepHqExportService::getInstance();
        svc.initialize(nullptr, nullptr);   // never leave the singleton holding this db
        if (db_) cleanup();
        db_.reset();
        if (!path_.empty()) {
            for (const auto* suffix : {"", "-wal", "-shm"}) {
                std::error_code ec;
                fs::remove(path_ + suffix, ec);
            }
        }
    }

    // Only the 2099 live sessions and this process's own ring file (under any
    // device: one test files it under a second device).
    void cleanup() {
        const auto p1 = sql::param(1, db_->dbType());
        const auto p2 = sql::param(2, db_->dbType());
        const std::string which =
            "(device_id = " + p1 + " AND filename LIKE 'live_2099%') OR filename = " + p2;
        db_->executeQuery("DELETE FROM oximetry_samples WHERE oximetry_session_id IN "
                          "(SELECT id FROM oximetry_sessions WHERE " + which + ")",
                          {kOximetryDeviceId, file_});
        db_->executeQuery("DELETE FROM oximetry_sessions WHERE " + which,
                          {kOximetryDeviceId, file_});
    }

    bool live(std::chrono::system_clock::time_point at, int spo2 = 91) {
        return db_->saveLiveOximetrySample(kOximetryDeviceId, at, spo2, 70, 0);
    }

    long long liveSamplesIn(const std::string& filename) {
        const auto p1 = sql::param(1, db_->dbType());
        const auto p2 = sql::param(2, db_->dbType());
        auto r = db_->executeQuery(
            "SELECT COUNT(*) AS n FROM oximetry_samples s"
            " JOIN oximetry_sessions os ON os.id = s.oximetry_session_id"
            " WHERE os.device_id = " + p1 + " AND os.filename = " + p2,
            {kOximetryDeviceId, filename});
        return (r.isArray() && r.size() == 1u) ? asNum(r[0]["n"]) : -1;
    }

    // SleepHQ export's read of a night ("YYYYMMDD").
    OximetrySession exported(const std::string& folder) {
        auto& svc = SleepHqExportService::getInstance();
        svc.initialize(nullptr, db_.get());
        auto out = svc.oximetrySessionFor(folder);
        svc.initialize(nullptr, nullptr);
        return out;
    }

    // The night page's chart read of a night ("YYYY-MM-DD").
    Json::Value charted(const std::string& night) {
        QueryService qs(db_, "liveo2-cpap-" + std::to_string(::getpid()));
        return qs.getSessionOximetry(night, 1);
    }

    const char* eng() const { return engineName(GetParam()); }

    std::shared_ptr<IDatabase> db_;
    std::string path_;
    std::string file_;
};

// ── 1. The clock ─────────────────────────────────────────────────────────────

TEST_P(LiveOximetryBackendTest, ALiveSampleIsStoredOnTheClockItWasGiven) {
    // 23:15 and 23:15:04 on the ring's wall clock. NOW() could not produce a
    // 2099 timestamp, so this only passes when the database stores what it
    // was handed.
    const auto at = ring(2099, 3, 1, 23, 15);
    ASSERT_TRUE(live(at)) << eng();
    ASSERT_TRUE(live(at + std::chrono::seconds(4))) << eng();

    const auto dt = db_->dbType();
    auto sess = db_->executeQuery(
        "SELECT " + sql::tsText("start_time", dt) + " AS st, " +
            sql::tsText("end_time", dt) + " AS et, duration_seconds, total_samples"
            " FROM oximetry_sessions WHERE device_id = " + sql::param(1, dt) +
            " AND filename = " + sql::param(2, dt),
        {kOximetryDeviceId, "live_20990301.vld"});
    ASSERT_TRUE(sess.isArray() && sess.size() == 1u)
        << eng() << ": the live session is named for the wall-clock date";
    EXPECT_EQ(sess[0]["st"].asString(), "2099-03-01 23:15:00") << eng();
    EXPECT_EQ(sess[0]["et"].asString(), "2099-03-01 23:15:04") << eng();
    EXPECT_EQ(asNum(sess[0]["duration_seconds"]), 4) << eng();
    EXPECT_EQ(asNum(sess[0]["total_samples"]), 2) << eng();

    auto samples = db_->executeQuery(
        "SELECT " + sql::tsText("s.timestamp", dt) + " AS ts, s.source"
            " FROM oximetry_samples s JOIN oximetry_sessions os"
            " ON os.id = s.oximetry_session_id"
            " WHERE os.device_id = " + sql::param(1, dt) +
            " AND os.filename = " + sql::param(2, dt) + " ORDER BY s.timestamp",
        {kOximetryDeviceId, "live_20990301.vld"});
    ASSERT_TRUE(samples.isArray() && samples.size() == 2u) << eng();
    EXPECT_EQ(samples[0]["ts"].asString(), "2099-03-01 23:15:00") << eng();
    EXPECT_EQ(samples[1]["ts"].asString(), "2099-03-01 23:15:04") << eng();
    EXPECT_EQ(samples[0]["source"].asString(), "live") << eng();
}

// ── 2. The real file supersedes the live session, on that event ─────────────

TEST_P(LiveOximetryBackendTest, SavingTheRingFileRemovesThatNightsLiveSessions) {
    // The night of 2099-03-01 polled live across midnight: two calendar-dated
    // live sessions, one sleep day.
    ASSERT_TRUE(live(ring(2099, 3, 1, 23, 15))) << eng();
    ASSERT_TRUE(live(ring(2099, 3, 2, 0, 30))) << eng();
    // Another night's live stream, which the file must not touch.
    ASSERT_TRUE(live(ring(2099, 3, 5, 23, 0))) << eng();

    ASSERT_TRUE(db_->oximetrySessionExists(kOximetryDeviceId, "live_20990301.vld")) << eng();
    ASSERT_TRUE(db_->oximetrySessionExists(kOximetryDeviceId, "live_20990302.vld")) << eng();

    // The ring's own file for the night of the 1st lands.
    ASSERT_TRUE(db_->saveOximetrySession(kOximetryDeviceId,
                                         ringFile(file_, ring(2099, 3, 1, 22, 50), 5)))
        << eng();

    EXPECT_FALSE(db_->oximetrySessionExists(kOximetryDeviceId, "live_20990301.vld"))
        << eng() << ": the night's live session outlived its ring file";
    EXPECT_FALSE(db_->oximetrySessionExists(kOximetryDeviceId, "live_20990302.vld"))
        << eng() << ": the after-midnight half of the night's live stream was kept";
    EXPECT_EQ(liveSamplesIn("live_20990301.vld"), 0) << eng() << ": orphaned live samples";
    EXPECT_EQ(liveSamplesIn("live_20990302.vld"), 0) << eng() << ": orphaned live samples";

    EXPECT_TRUE(db_->oximetrySessionExists(kOximetryDeviceId, "live_20990305.vld"))
        << eng() << ": another night's live session was removed";
    EXPECT_EQ(liveSamplesIn("live_20990305.vld"), 1) << eng();
    EXPECT_EQ(liveSamplesIn(file_), 5) << eng() << ": the ring file itself must be whole";
}

TEST_P(LiveOximetryBackendTest, AnotherDevicesRingFileDoesNotSupersede) {
    ASSERT_TRUE(live(ring(2099, 3, 1, 23, 15))) << eng();
    const std::string other = "liveo2_other_" + std::to_string(::getpid());
    ASSERT_TRUE(db_->saveOximetrySession(other, ringFile(file_, ring(2099, 3, 1, 22, 50), 3)))
        << eng();
    EXPECT_TRUE(db_->oximetrySessionExists(kOximetryDeviceId, "live_20990301.vld")) << eng();
}

// ── 3. Readers never use a live session beside a real file ──────────────────

TEST_P(LiveOximetryBackendTest, ALiveOnlyNightStillChartsAndExports) {
    // No bridge file yet: the live stream is all there is, and it is shown.
    ASSERT_TRUE(live(ring(2099, 3, 1, 23, 15))) << eng();
    ASSERT_TRUE(live(ring(2099, 3, 1, 23, 16))) << eng();

    EXPECT_EQ(exported("20990301").samples.size(), 2u) << eng();
    EXPECT_EQ(charted("2099-03-01")["spo2"].size(), 2u) << eng();
}

TEST_P(LiveOximetryBackendTest, ALiveSessionBesideTheRingFileIsNeitherExportedNorCharted) {
    // The file first, then live samples for the same sleep day: a second wear
    // that morning, or rows written before saving a file removed them.
    ASSERT_TRUE(db_->saveOximetrySession(kOximetryDeviceId,
                                         ringFile(file_, ring(2099, 3, 1, 22, 50), 5)))
        << eng();
    ASSERT_TRUE(live(ring(2099, 3, 2, 7, 0), 85)) << eng();
    ASSERT_TRUE(live(ring(2099, 3, 2, 7, 1), 85)) << eng();
    ASSERT_TRUE(db_->oximetrySessionExists(kOximetryDeviceId, "live_20990302.vld")) << eng();

    const auto out = exported("20990301");
    ASSERT_EQ(out.samples.size(), 5u)
        << eng() << ": SleepHQ would get the live samples beside the ring file";
    for (const auto& s : out.samples) EXPECT_EQ(s.spo2, 96) << eng();

    const auto chart = charted("2099-03-01");
    ASSERT_EQ(chart["spo2"].size(), 5u) << eng() << ": the night chart drew the live samples too";
    for (const auto& v : chart["spo2"]) EXPECT_EQ(asNum(v), 96) << eng();
}

TEST_P(LiveOximetryBackendTest, TheSessionsListCountsTheRingFileOnce) {
    ASSERT_TRUE(db_->saveOximetrySession(kOximetryDeviceId,
                                         ringFile(file_, ring(2099, 3, 1, 22, 50), 5)))
        << eng();
    // A live stand-in spanning an hour for the same sleep day.
    ASSERT_TRUE(live(ring(2099, 3, 2, 6, 0))) << eng();
    ASSERT_TRUE(live(ring(2099, 3, 2, 7, 0))) << eng();

    QueryService qs(db_, "liveo2-cpap-" + std::to_string(::getpid()));
    const auto rows = qs.getSessions(10, 0);
    ASSERT_TRUE(rows.isArray()) << eng();
    bool found = false;
    for (const auto& r : rows) {
        if (r["sleep_day"].asString().rfind("2099-03-01", 0) != 0) continue;
        found = true;
        EXPECT_EQ(asNum(r["duration_seconds"]), 16)
            << eng() << ": the live stand-in's hour was added to the night";
    }
    EXPECT_TRUE(found) << eng() << ": the oximetry-only night is missing from the list";
}

INSTANTIATE_TEST_SUITE_P(
    Engines, LiveOximetryBackendTest,
    ::testing::Values(Engine::SQLite, Engine::MySQL, Engine::Postgres),
    [](const ::testing::TestParamInfo<Engine>& info) {
        return std::string(engineName(info.param));
    });

// ── The host clock a live sample is stamped with ───────────────────────────
//
// liveOximetryWallClock must give the host's LOCAL wall clock in the ring-file
// shape (read back with gmtime), whatever the zone. Pinned under a zone that is
// not UTC, because on a UTC runner local and UTC agree and nothing is tested.

#ifndef _WIN32
TEST(LiveOximetryWallClock, IsTheHostsLocalWallClockInTheRingFileShape) {
    const char* old = std::getenv("TZ");
    const std::string saved = old ? old : "";
    setenv("TZ", "EST5EDT", 1);
    tzset();

    // 2099-03-02 04:15:00 UTC is 23:15 on the 1st in New York (EST, UTC-5).
    const auto instant = ring(2099, 3, 2, 4, 15);
    const auto wall = liveOximetryWallClock(instant);

    if (old) setenv("TZ", saved.c_str(), 1); else unsetenv("TZ");
    tzset();

    EXPECT_EQ(wall, ring(2099, 3, 1, 23, 15));
    EXPECT_EQ(liveOximetryFilename(wall), "live_20990301.vld");
    EXPECT_EQ(oximetrySleepDay(wall), "2099-03-01");
    EXPECT_EQ(oximetrySleepDay(ring(2099, 3, 2, 4, 15)), "2099-03-01")
        << "a small-hours reading belongs to the night before";
}
#endif

}  // namespace
