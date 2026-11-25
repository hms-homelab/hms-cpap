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
#include "utils/MachineClocks.h"
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
#include <functional>
#include <memory>
#include <optional>
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
            "(device_id = " + p1 + " AND filename LIKE 'live_2099%') OR filename LIKE " + p2;
        db_->executeQuery("DELETE FROM oximetry_samples WHERE oximetry_session_id IN "
                          "(SELECT id FROM oximetry_sessions WHERE " + which + ")",
                          {kOximetryDeviceId, file_ + "%"});
        db_->executeQuery("DELETE FROM oximetry_sessions WHERE " + which,
                          {kOximetryDeviceId, file_ + "%"});
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

TEST_P(LiveOximetryBackendTest, TheSessionsListReadsTheRingsFiguresForNightsTheMachineHasNone) {
    // A machine with no oximeter stores no SpO2 or heart rate, so the list
    // reads the ring's for that night in the same query (the page used to ask
    // for each night's oximetry, and stopped at the newest seven).
    const std::string cpap_dev = "o2list-cpap-" + std::to_string(::getpid());
    const auto clear_cpap = [&] {
        const auto p = sql::param(1, db_->dbType());
        db_->executeQuery("DELETE FROM cpap_session_metrics WHERE session_id IN ("
                          "SELECT id FROM cpap_sessions WHERE device_id = " + p + ")", {cpap_dev});
        db_->executeQuery("DELETE FROM cpap_sessions WHERE device_id = " + p, {cpap_dev});
    };
    clear_cpap();
    struct Guard {
        std::function<void()> f;
        ~Guard() { f(); }
    } guard{clear_cpap};

    const auto night = [&](int mo, int d, std::optional<double> spo2, std::optional<int> hr) {
        CPAPSession c;
        c.device_id = cpap_dev;
        const auto start = ring(2099, mo, d, 22, 0);
        c.session_start = start;
        c.session_end = start + std::chrono::hours(7);
        c.duration_seconds = 7 * 3600;
        c.status = CPAPSession::Status::COMPLETED;
        SessionMetrics m;
        m.ahi = 1.0;
        m.avg_spo2 = spo2;
        m.avg_heart_rate = hr;
        c.metrics = m;
        return db_->saveSession(c);
    };
    const auto file = [&](const std::string& name, std::chrono::system_clock::time_point at, int n,
                          double spo2, double hr) {
        auto f = ringFile(name, at, n);
        f.metrics.avg_spo2 = spo2;
        f.metrics.avg_hr = hr;
        return db_->saveOximetrySession(kOximetryDeviceId, f);
    };

    // Night A: no machine figures; the ring wore two stretches the same sleep
    // day, 396 s at 96 % / 60 and 96 s at 90 % / 70.
    ASSERT_TRUE(night(3, 1, std::nullopt, std::nullopt)) << eng();
    ASSERT_TRUE(file(file_, ring(2099, 3, 1, 22, 50), 100, 96, 60)) << eng();
    ASSERT_TRUE(file(file_ + "2", ring(2099, 3, 2, 3, 0), 25, 90, 70)) << eng();
    // Night B: the machine has its own figures; the ring's do not replace them.
    ASSERT_TRUE(night(3, 10, 88.0, 55)) << eng();
    ASSERT_TRUE(file(file_ + "3", ring(2099, 3, 10, 22, 50), 100, 96, 60)) << eng();
    // Night C: neither the machine nor the ring has any. A recording filed
    // under another device that night is not the ring's and is not read.
    ASSERT_TRUE(night(3, 20, std::nullopt, std::nullopt)) << eng();
    {
        auto other = ringFile(file_ + "4", ring(2099, 3, 20, 22, 50), 100);
        other.metrics.avg_spo2 = 80;
        other.metrics.avg_hr = 99;
        ASSERT_TRUE(db_->saveOximetrySession("liveo2_other_" + std::to_string(::getpid()), other))
            << eng();
    }
    // Night D: only the bridge's live stand-in so far (no stored average, the
    // ring's file has not landed): read from its samples, as the page did.
    ASSERT_TRUE(night(3, 25, std::nullopt, std::nullopt)) << eng();
    ASSERT_TRUE(live(ring(2099, 3, 26, 6, 0), 91)) << eng();
    ASSERT_TRUE(live(ring(2099, 3, 26, 6, 1), 91)) << eng();

    QueryService qs(db_, cpap_dev);
    const auto rows = qs.getSessions(200, 0);
    ASSERT_TRUE(rows.isArray()) << eng();
    const auto find = [&](const std::string& day) -> Json::Value {
        for (const auto& r : rows)
            if (r["sleep_day"].asString().rfind(day, 0) == 0 && r["oximetry_only"].asString() != "1") return r;
        return Json::Value();
    };
    const auto num = [](const Json::Value& v) {
        return v.isString() ? std::stod(v.asString()) : v.asDouble();
    };

    const auto a = find("2099-03-01");
    ASSERT_TRUE(a.isObject()) << eng() << ": night A is missing from the list";
    EXPECT_NEAR(num(a["avg_spo2"]), 94.8, 0.05) << eng() << ": weighted by each stretch's length";
    EXPECT_NEAR(num(a["avg_heart_rate"]), 62.0, 0.5) << eng();
    EXPECT_EQ(asNum(a["total_events"]), 0) << eng() << ": the ring join repeated the night's rows";

    const auto b = find("2099-03-10");
    ASSERT_TRUE(b.isObject()) << eng();
    EXPECT_NEAR(num(b["avg_spo2"]), 88.0, 0.05) << eng() << ": the machine's figure wins";
    EXPECT_NEAR(num(b["avg_heart_rate"]), 55.0, 0.5) << eng();

    const auto c = find("2099-03-20");
    ASSERT_TRUE(c.isObject()) << eng();
    EXPECT_TRUE(c["avg_spo2"].isNull()) << eng() << ": no figure is not a figure of zero";
    EXPECT_TRUE(c["avg_heart_rate"].isNull()) << eng();

    const auto d = find("2099-03-25");
    ASSERT_TRUE(d.isObject()) << eng();
    EXPECT_NEAR(num(d["avg_spo2"]), 91.0, 0.05) << eng() << ": a live-only night reads its samples";
    EXPECT_NEAR(num(d["avg_heart_rate"]), 70.0, 0.5) << eng();
}

INSTANTIATE_TEST_SUITE_P(
    Engines, LiveOximetryBackendTest,
    ::testing::Values(Engine::SQLite, Engine::MySQL, Engine::Postgres),
    [](const ::testing::TestParamInfo<Engine>& info) {
        return std::string(engineName(info.param));
    });

// ── The host clock a live sample is stamped with ───────────────────────────
//
// The live path stamps a sample with machine_clocks::nowWall (SDD-074). With no
// clock setting it must give the host's LOCAL wall clock in the ring-file
// shape (read back with gmtime), whatever the zone. Pinned under a zone that is
// not UTC, because on a UTC runner local and UTC agree and nothing is tested.

#ifndef _WIN32
TEST(LiveOximetryWallClock, IsTheHostsLocalWallClockInTheRingFileShape) {
    const char* old = std::getenv("TZ");
    const std::string saved = old ? old : "";
    setenv("TZ", "EST5EDT", 1);
    tzset();
    machine_clocks::clear();

    // 2099-03-02 04:15:00 UTC is 23:15 on the 1st in New York (EST, UTC-5).
    const auto instant = ring(2099, 3, 2, 4, 15);
    const auto wall = machine_clocks::nowWall("live_clock_test_machine", instant);

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
