//
// test_OximetryBackends.cpp — oximetry + checkpoint-by-folder backend parity.
//
// Why this file exists: PostgreSQL was the only backend that implemented the
// oximetry read path and getCheckpointFilesByFolder. SQLite (the default, and
// what the first-run wizard hands most users) and MySQL both carried inline
// header stubs returning {} or false, silently. The consequences were invisible
// rather than loud: O2 Ring data was written and then never reached the MQTT
// sensors (DataPublisherService), the PDF reports (BaseReportGenerator,
// RangeReportGenerator) or the daily aggregation, and the SleepHQ export guard
// in BurstCollectorService queued every folder because the checkpoint lookup
// always came back empty.
//
// So the contract is pinned here across engines: SQLite always runs, MySQL runs
// when a server is reachable and skips cleanly otherwise.
//
// SAFETY: these tests never DROP or TRUNCATE. Every row they write is namespaced
// to a synthetic per-process device_id and deleted again in TearDown, so pointing
// MYSQL_TEST_* at a populated database cannot disturb real therapy data.
//
#include <gtest/gtest.h>

#include "database/IDatabase.h"
#include "database/SQLiteDatabase.h"
#include "services/SleepHqExportService.h"
#include "utils/OximetryDevice.h"
#include "utils/TimeCompat.h"
#ifdef WITH_MYSQL
#include "database/MySQLDatabase.h"
#endif

#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <memory>
#include <string>
#include <unistd.h>
#include <vector>

using namespace hms_cpap;
using cpapdash::parser::OximetrySample;
using cpapdash::parser::OximetrySession;

namespace {

std::string envOr(const char* key, const std::string& fallback) {
    const char* v = std::getenv(key);
    return (v && *v) ? std::string(v) : fallback;
}

// SQLite's executeQuery stringifies every non-null column while the other engines
// return typed values, so aggregate reads have to accept both shapes.
int64_t asCount(const Json::Value& v) {
    if (v.isNull()) return 0;
    if (v.isString()) return std::stoll(v.asString());
    return v.asInt64();
}

// A time_point at a fixed instant, built in UTC.
//
// UTC on purpose: OximetrySession::date_str() renders cpap_session_date with
// gmtime, so anchoring in UTC is what makes the stored date equal the date the
// test asked for on any runner. Callers use midday to stay clear of either
// boundary.
std::chrono::system_clock::time_point tpUtc(int y, int mo, int d, int h, int mi) {
    std::tm tm{};
    tm.tm_year = y - 1900;
    tm.tm_mon  = mo - 1;
    tm.tm_mday = d;
    tm.tm_hour = h;
    tm.tm_min  = mi;
    const time_t t = timegm_utc(&tm);
    const auto tp  = std::chrono::system_clock::from_time_t(t);

    // libstdc++ counts NANOSECONDS, so a time_point past 2262 overflows int64.
    // to_time_t round-trips the overflowed value unchanged, which is why a bad
    // fixture looks healthy right up until something adds to it -- and then a
    // year-2999 night gets stored as 1829 and no sleep-day query finds it.
    // Adding a second is exactly that arithmetic, so it catches the fixture
    // here instead of three layers down in a database assertion.
    EXPECT_EQ(std::chrono::system_clock::to_time_t(tp + std::chrono::seconds(1)), t + 1)
        << "tpUtc(" << y << ", ...) is outside this platform's system_clock range; "
           "pick a year under 2262";
    return tp;
}

// Builds a session whose metrics are set explicitly rather than computed, so the
// assertions test storage and retrieval, not the parser's maths.
OximetrySession makeSession(const std::string& filename,
                            const std::string& date_folder,
                            int duration_seconds,
                            double avg_spo2,
                            double min_spo2,
                            int n_samples = 3) {
    OximetrySession s;
    s.filename = filename;
    // date_str() derives from start_time, and cpap_session_date must equal
    // date_folder for the read paths to find the row.
    int y  = std::stoi(date_folder.substr(0, 4));
    int mo = std::stoi(date_folder.substr(4, 2));
    int d  = std::stoi(date_folder.substr(6, 2));
    s.start_time = tpUtc(y, mo, d, 12, 0);
    s.end_time   = s.start_time + std::chrono::seconds(duration_seconds);
    s.duration_seconds = duration_seconds;
    s.sample_interval  = 4.0;

    s.metrics.avg_spo2          = avg_spo2;
    s.metrics.min_spo2          = min_spo2;
    s.metrics.spo2_baseline     = 96.0;
    s.metrics.time_below_90_pct = 1.5;
    s.metrics.time_below_88_pct = 0.5;
    s.metrics.odi_3pct          = 4.2;
    s.metrics.desat_count_3pct  = 7;
    s.metrics.avg_hr            = 61.0;
    s.metrics.min_hr            = 52;
    s.metrics.max_hr            = 88;
    s.metrics.valid_samples     = n_samples;
    s.metrics.total_samples     = n_samples;

    for (int i = 0; i < n_samples; ++i) {
        OximetrySample smp{};
        smp.timestamp    = s.start_time + std::chrono::seconds(i * 4);
        smp.spo2         = static_cast<uint8_t>(95 + (i % 3));
        smp.heart_rate   = static_cast<uint8_t>(60 + i);
        smp.invalid_flag = 0;
        smp.motion       = 0;
        smp.vibration    = 0;
        s.samples.push_back(smp);
    }
    return s;
}

enum class Engine { SQLite, MySQL };

const char* engineName(Engine e) { return e == Engine::SQLite ? "SQLite" : "MySQL"; }

class OximetryBackendTest : public ::testing::TestWithParam<Engine> {
protected:
    std::unique_ptr<IDatabase> db_;
    std::string path_;       // SQLite temp file
    std::string device_;     // synthetic, per-process

    bool isMySQL() const { return GetParam() == Engine::MySQL; }

    void SetUp() override {
        device_ = "oxi_test_" + std::to_string(::getpid());

        if (GetParam() == Engine::SQLite) {
            path_ = (std::filesystem::temp_directory_path() /
                     ("hms_oxi_test_" + std::to_string(::getpid()) + ".db")).string();
            std::filesystem::remove(path_);
            auto lite = std::make_unique<SQLiteDatabase>(path_);
            ASSERT_TRUE(lite->connect());
            db_ = std::move(lite);
            return;
        }

#ifndef WITH_MYSQL
        GTEST_SKIP() << "built without MySQL (-DBUILD_WITH_MYSQL=OFF)";
#else
        // Opt-in only: with no MYSQL_TEST_HOST there is nothing to point at, and a
        // developer with no server still gets a green suite.
        std::string host = envOr("MYSQL_TEST_HOST", "");
        if (host.empty()) {
            GTEST_SKIP() << "MYSQL_TEST_HOST unset — skipping oximetry parity on MySQL. "
                            "Set MYSQL_TEST_HOST/PORT/DB/USER/PASSWORD to run it.";
        }

        auto my = std::make_unique<MySQLDatabase>(
            host,
            std::stoi(envOr("MYSQL_TEST_PORT", "3306")),
            envOr("MYSQL_TEST_USER", ""),
            envOr("MYSQL_TEST_PASSWORD", ""),
            envOr("MYSQL_TEST_DB", "hms_cpap_test"));

        if (!my->connect()) {
            GTEST_SKIP() << "No usable MySQL at " << host
                         << " — skipping oximetry parity on MySQL.";
        }
        db_ = std::move(my);
#endif
    }

    void TearDown() override {
        // Only ever removes this process's own synthetic rows.
        if (db_) {
            const std::string ph = "?";
            db_->executeQuery(
                "DELETE FROM oximetry_samples WHERE oximetry_session_id IN "
                "(SELECT id FROM oximetry_sessions WHERE device_id = " + ph + ")",
                {device_});
            db_->executeQuery("DELETE FROM oximetry_sessions WHERE device_id = " + ph,
                              {device_});
            db_->executeQuery("DELETE FROM cpap_sessions WHERE device_id = " + ph,
                              {device_});
        }
        db_.reset();
        if (!path_.empty()) std::filesystem::remove(path_);
    }

    IDatabase& db() { return *db_; }
};

// ─────────────────────────────────────────────────────────────────────────────
// Writes
// ─────────────────────────────────────────────────────────────────────────────

TEST_P(OximetryBackendTest, SaveSessionThenExists) {
    EXPECT_FALSE(db().oximetrySessionExists(device_, "night1.vld"))
        << engineName(GetParam()) << ": reported a session that was never saved";

    auto s = makeSession("night1.vld", "20260619", 28800, 96.5, 91.0);
    ASSERT_TRUE(db().saveOximetrySession(device_, s));

    EXPECT_TRUE(db().oximetrySessionExists(device_, "night1.vld"));
    // Scoped to the device, so another device's file must not match.
    EXPECT_FALSE(db().oximetrySessionExists(device_ + "_other", "night1.vld"));
}

TEST_P(OximetryBackendTest, SaveSessionIsIdempotentOnFilename) {
    auto s = makeSession("night1.vld", "20260619", 28800, 96.5, 91.0, 5);
    ASSERT_TRUE(db().saveOximetrySession(device_, s));
    ASSERT_TRUE(db().saveOximetrySession(device_, s));

    // Upsert identity is the source filename: a re-upload replaces the night
    // rather than doubling it, and samples must not accumulate either.
    auto rows = db().executeQuery(
        "SELECT COUNT(*) AS n FROM oximetry_sessions WHERE device_id = ?", {device_});
    ASSERT_TRUE(rows.isArray() && rows.size() == 1u);
    EXPECT_EQ(asCount(rows[0]["n"]), 1);

    auto samples = db().executeQuery(
        "SELECT COUNT(*) AS n FROM oximetry_samples WHERE oximetry_session_id IN "
        "(SELECT id FROM oximetry_sessions WHERE device_id = ?)", {device_});
    ASSERT_TRUE(samples.isArray() && samples.size() == 1u);
    EXPECT_EQ(asCount(samples[0]["n"]), 5);
}

TEST_P(OximetryBackendTest, UpsertReplacesMetricsRatherThanKeepingStale) {
    ASSERT_TRUE(db().saveOximetrySession(
        device_, makeSession("night1.vld", "20260619", 28800, 90.0, 80.0)));
    ASSERT_TRUE(db().saveOximetrySession(
        device_, makeSession("night1.vld", "20260619", 28800, 97.0, 93.0)));

    auto sum = db().getOximetrySummary(device_, "20260619", "20260620");
    ASSERT_TRUE(sum.found);
    EXPECT_NEAR(sum.avg_spo2, 97.0, 0.05);
    EXPECT_NEAR(sum.min_spo2, 93.0, 0.05);
}

TEST_P(OximetryBackendTest, SaveLiveOximetrySampleCreatesAndAccumulates) {
    ASSERT_TRUE(db().saveLiveOximetrySample(device_, "20260619", 96, 60, 0));
    ASSERT_TRUE(db().saveLiveOximetrySample(device_, "20260619", 95, 62, 1));

    // One synthetic session per day, both samples inside it.
    EXPECT_TRUE(db().oximetrySessionExists(device_, "live_20260619.vld"));

    auto rows = db().executeQuery(
        "SELECT total_samples, valid_samples FROM oximetry_sessions "
        "WHERE device_id = ? AND filename = ?",
        {device_, "live_20260619.vld"});
    ASSERT_TRUE(rows.isArray() && rows.size() == 1u);
    EXPECT_EQ(asCount(rows[0]["total_samples"]), 2);
    EXPECT_EQ(asCount(rows[0]["valid_samples"]), 2);
}

// ─────────────────────────────────────────────────────────────────────────────
// Timestamp fidelity
//
// The oximetry parsers read the ring's printed wall clock AS IF it were UTC
// (timegm), so the write path must render it back with gmtime. Pairing that parse
// with a localtime render shifted every oximetry timestamp by the host's UTC
// offset, putting the SpO2 charts hours away from the CPAP charts for the same
// night. These assert the wall clock survives the round trip, and they only mean
// something on a host that is not on UTC.
// ─────────────────────────────────────────────────────────────────────────────

TEST_P(OximetryBackendTest, StoredTimestampsKeepTheRingsWallClock) {
    // makeSession anchors start_time at 12:00 UTC, so 12:00:00 is what the ring
    // "displayed" and what must come back out of the column.
    ASSERT_TRUE(db().saveOximetrySession(
        device_, makeSession("wallclock.vld", "20260619", 28800, 96.5, 91.0)));

    auto rows = db().executeQuery(
        "SELECT start_time, end_time FROM oximetry_sessions "
        "WHERE device_id = ? AND filename = ?",
        {device_, "wallclock.vld"});
    ASSERT_TRUE(rows.isArray() && rows.size() == 1u);

    // Compare only the "YYYY-MM-DD HH:MM:SS" prefix: Postgres renders a
    // TIMESTAMP with extra precision, the others do not.
    EXPECT_EQ(rows[0]["start_time"].asString().substr(0, 19), "2026-06-19 12:00:00")
        << "start_time drifted from the ring's wall clock";
    EXPECT_EQ(rows[0]["end_time"].asString().substr(0, 19), "2026-06-19 20:00:00")
        << "end_time drifted from the ring's wall clock";
}

TEST_P(OximetryBackendTest, StoredSamplesShareTheSessionsClock) {
    // Samples used to be rendered by a different formatter than the session
    // header (on PostgreSQL they still disagreed inside one table), so a sample
    // could sit hours away from its own session's start_time.
    ASSERT_TRUE(db().saveOximetrySession(
        device_, makeSession("sampleclock.vld", "20260619", 28800, 96.5, 91.0, 3)));

    auto rows = db().executeQuery(
        "SELECT MIN(s.timestamp) AS first_ts FROM oximetry_samples s "
        "JOIN oximetry_sessions os ON s.oximetry_session_id = os.id "
        "WHERE os.device_id = ? AND os.filename = ?",
        {device_, "sampleclock.vld"});
    ASSERT_TRUE(rows.isArray() && rows.size() == 1u);

    // makeSession puts the first sample exactly at start_time.
    EXPECT_EQ(rows[0]["first_ts"].asString().substr(0, 19), "2026-06-19 12:00:00")
        << "the first sample does not line up with its session's start_time";
}

// ─────────────────────────────────────────────────────────────────────────────
// getOximetrySummary
// ─────────────────────────────────────────────────────────────────────────────

TEST_P(OximetryBackendTest, SummaryMissesReturnNotFoundNotZeroes) {
    auto sum = db().getOximetrySummary(device_, "20260101", "20260102");
    EXPECT_FALSE(sum.found)
        << "an absent night must be distinguishable from a night of zeroes";
}

TEST_P(OximetryBackendTest, SummaryReturnsEveryStoredField) {
    ASSERT_TRUE(db().saveOximetrySession(
        device_, makeSession("night1.vld", "20260619", 28800, 96.5, 91.0)));

    auto sum = db().getOximetrySummary(device_, "20260619", "20260620");
    ASSERT_TRUE(sum.found);
    EXPECT_NEAR(sum.avg_spo2, 96.5, 0.05);
    EXPECT_NEAR(sum.min_spo2, 91.0, 0.05);
    EXPECT_NEAR(sum.spo2_baseline, 96.0, 0.05);
    EXPECT_NEAR(sum.odi_3pct, 4.2, 0.05);
    EXPECT_NEAR(sum.time_below_90, 1.5, 0.05);
    EXPECT_NEAR(sum.time_below_88, 0.5, 0.05);
    EXPECT_NEAR(sum.avg_hr, 61.0, 0.05);
    EXPECT_EQ(sum.min_hr, 52);
    EXPECT_EQ(sum.max_hr, 88);
    EXPECT_EQ(sum.valid_samples, 3);
    EXPECT_EQ(sum.duration_seconds, 28800);
}

TEST_P(OximetryBackendTest, SummaryMatchesOnTheNextDayForMidnightCrossing) {
    // A ring session filed under the 20th is still the night the CPAP calls the
    // 19th, which is why both dates are passed.
    ASSERT_TRUE(db().saveOximetrySession(
        device_, makeSession("night2.vld", "20260620", 28800, 94.0, 88.0)));

    auto sum = db().getOximetrySummary(device_, "20260619", "20260620");
    ASSERT_TRUE(sum.found);
    EXPECT_NEAR(sum.avg_spo2, 94.0, 0.05);
}

TEST_P(OximetryBackendTest, SummaryIgnoresSubMinuteStubSessions) {
    // The ring writes a tiny session when it is picked up and put down again.
    ASSERT_TRUE(db().saveOximetrySession(
        device_, makeSession("blip.vld", "20260619", 30, 99.0, 99.0)));

    auto sum = db().getOximetrySummary(device_, "20260619", "20260620");
    EXPECT_FALSE(sum.found) << "a 30-second stub must not be reported as a night";
}

TEST_P(OximetryBackendTest, SummaryPrefersTheLongestSessionOfTheNight) {
    ASSERT_TRUE(db().saveOximetrySession(
        device_, makeSession("short.vld", "20260619", 600, 99.0, 99.0)));
    ASSERT_TRUE(db().saveOximetrySession(
        device_, makeSession("long.vld", "20260619", 28800, 94.0, 88.0)));

    auto sum = db().getOximetrySummary(device_, "20260619", "20260620");
    ASSERT_TRUE(sum.found);
    EXPECT_EQ(sum.duration_seconds, 28800);
    EXPECT_NEAR(sum.avg_spo2, 94.0, 0.05) << "the 10-minute nap won over the full night";
}

// ─────────────────────────────────────────────────────────────────────────────
// getOximetryRangeSummary
// ─────────────────────────────────────────────────────────────────────────────

TEST_P(OximetryBackendTest, RangeSummaryEmptyRangeIsNotFoundWithZeroNights) {
    auto r = db().getOximetryRangeSummary(device_, "20260101", "20260131");
    EXPECT_FALSE(r.found);
    EXPECT_EQ(r.nights, 0);
}

TEST_P(OximetryBackendTest, RangeSummaryAveragesAcrossNights) {
    ASSERT_TRUE(db().saveOximetrySession(
        device_, makeSession("n1.vld", "20260619", 28800, 94.0, 88.0)));
    ASSERT_TRUE(db().saveOximetrySession(
        device_, makeSession("n2.vld", "20260620", 28800, 96.0, 92.0)));

    auto r = db().getOximetryRangeSummary(device_, "20260619", "20260620");
    ASSERT_TRUE(r.found);
    EXPECT_EQ(r.nights, 2);
    EXPECT_NEAR(r.avg_spo2, 95.0, 0.05);
    // min across the range, not the mean of the minima.
    EXPECT_NEAR(r.min_spo2, 88.0, 0.05);
}

TEST_P(OximetryBackendTest, RangeSummaryHonoursItsBounds) {
    ASSERT_TRUE(db().saveOximetrySession(
        device_, makeSession("inside.vld", "20260619", 28800, 94.0, 88.0)));
    ASSERT_TRUE(db().saveOximetrySession(
        device_, makeSession("outside.vld", "20260701", 28800, 80.0, 70.0)));

    auto r = db().getOximetryRangeSummary(device_, "20260619", "20260620");
    ASSERT_TRUE(r.found);
    EXPECT_EQ(r.nights, 1) << "a night outside the range leaked in";
    EXPECT_NEAR(r.avg_spo2, 94.0, 0.05);
}

// ─────────────────────────────────────────────────────────────────────────────
// getOximetryNightlySpo2
// ─────────────────────────────────────────────────────────────────────────────

TEST_P(OximetryBackendTest, NightlySpo2IsEmptyWithoutData) {
    EXPECT_TRUE(db().getOximetryNightlySpo2(device_, "20260101", "20260131").empty());
}

TEST_P(OximetryBackendTest, NightlySpo2ReturnsOneAscendingPointPerNight) {
    ASSERT_TRUE(db().saveOximetrySession(
        device_, makeSession("n2.vld", "20260620", 28800, 96.0, 92.0)));
    ASSERT_TRUE(db().saveOximetrySession(
        device_, makeSession("n1.vld", "20260619", 28800, 94.0, 88.0)));

    auto pts = db().getOximetryNightlySpo2(device_, "20260619", "20260621");
    ASSERT_EQ(pts.size(), 2u);
    EXPECT_EQ(pts[0].date, "20260619");
    EXPECT_EQ(pts[1].date, "20260620");
    EXPECT_NEAR(pts[0].avg_spo2, 94.0, 0.05);
    EXPECT_NEAR(pts[1].avg_spo2, 96.0, 0.05);
    EXPECT_NEAR(pts[0].min_spo2, 88.0, 0.05);
}

TEST_P(OximetryBackendTest, NightlySpo2CollapsesTwoSessionsToTheLongest) {
    // This is what PostgreSQL says with DISTINCT ON. MySQL needs a window
    // function and SQLite leans on its bare-column-with-MAX rule, so the
    // selection is worth pinning on every engine.
    ASSERT_TRUE(db().saveOximetrySession(
        device_, makeSession("short.vld", "20260619", 600, 99.0, 99.0)));
    ASSERT_TRUE(db().saveOximetrySession(
        device_, makeSession("long.vld", "20260619", 28800, 94.0, 88.0)));

    auto pts = db().getOximetryNightlySpo2(device_, "20260619", "20260619");
    ASSERT_EQ(pts.size(), 1u) << "one night must yield exactly one point";
    EXPECT_NEAR(pts[0].avg_spo2, 94.0, 0.05)
        << "the 10-minute nap won over the full night";
}

// ─────────────────────────────────────────────────────────────────────────────
// getCheckpointFilesByFolder
//
// BurstCollectorService treats a non-empty result as "this folder was already
// ingested". Returning {} unconditionally made every folder look unparsed and
// queued it for SleepHQ export.
// ─────────────────────────────────────────────────────────────────────────────

TEST_P(OximetryBackendTest, CheckpointFilesByFolderEmptyWhenNothingIngested) {
    EXPECT_TRUE(db().getCheckpointFilesByFolder(device_, "20260619").empty());
}

TEST_P(OximetryBackendTest, CheckpointFilesByFolderFindsFilesForTheFolder) {
    ASSERT_TRUE(db().executeQuery(
        "INSERT INTO cpap_sessions (device_id, session_start, checkpoint_files) "
        "VALUES (?, ?, ?)",
        {device_, "2026-06-19 23:00:00",
         R"({"20260619_120000_BRP.edf":4096,"20260619_120000_PLD.edf":2048})"})
        .isArray());

    auto files = db().getCheckpointFilesByFolder(device_, "20260619");
    ASSERT_EQ(files.size(), 2u);
    EXPECT_EQ(files["20260619_120000_BRP.edf"], 4096);
    EXPECT_EQ(files["20260619_120000_PLD.edf"], 2048);
}

TEST_P(OximetryBackendTest, CheckpointFilesByFolderMatchesNextDayFilenames) {
    // A cross-midnight session is filed in the previous day's folder, so folder
    // 20260619 must also match files stamped 20260620.
    ASSERT_TRUE(db().executeQuery(
        "INSERT INTO cpap_sessions (device_id, session_start, checkpoint_files) "
        "VALUES (?, ?, ?)",
        {device_, "2026-06-20 01:00:00", R"({"20260620_010000_BRP.edf":8192})"})
        .isArray());

    auto files = db().getCheckpointFilesByFolder(device_, "20260619");
    ASSERT_EQ(files.size(), 1u);
    EXPECT_EQ(files["20260620_010000_BRP.edf"], 8192);
}

TEST_P(OximetryBackendTest, CheckpointFilesByFolderIgnoresOtherFolders) {
    ASSERT_TRUE(db().executeQuery(
        "INSERT INTO cpap_sessions (device_id, session_start, checkpoint_files) "
        "VALUES (?, ?, ?)",
        {device_, "2026-07-01 23:00:00", R"({"20260701_230000_BRP.edf":1024})"})
        .isArray());

    EXPECT_TRUE(db().getCheckpointFilesByFolder(device_, "20260619").empty())
        << "an unrelated folder's files leaked in";
}

TEST_P(OximetryBackendTest, CheckpointFilesByFolderRejectsMalformedFolder) {
    EXPECT_TRUE(db().getCheckpointFilesByFolder(device_, "nonsense").empty());
    EXPECT_TRUE(db().getCheckpointFilesByFolder(device_, "").empty());
}

TEST_P(OximetryBackendTest, CheckpointFilesSurviveALargeManifest) {
    // MySQL bound this JSON column with a 256-byte inline buffer, which truncated
    // the object mid-key and corrupted the parse. A real night carries far more
    // than 256 bytes of manifest.
    std::string json = "{";
    for (int i = 0; i < 40; ++i) {
        if (i) json += ",";
        json += "\"20260619_1200" + std::to_string(i) + "_BRP.edf\":" +
                std::to_string(1000 + i);
    }
    json += "}";
    ASSERT_GT(json.size(), 256u);

    ASSERT_TRUE(db().executeQuery(
        "INSERT INTO cpap_sessions (device_id, session_start, checkpoint_files) "
        "VALUES (?, ?, ?)",
        {device_, "2026-06-19 23:00:00", json}).isArray());

    auto files = db().getCheckpointFilesByFolder(device_, "20260619");
    EXPECT_EQ(files.size(), 40u)
        << "manifest was truncated: only " << files.size() << " of 40 entries parsed";
    EXPECT_EQ(files["20260619_120039_BRP.edf"], 1039)
        << "the last entry is the first casualty of a truncated buffer";
}

INSTANTIATE_TEST_SUITE_P(
    Engines, OximetryBackendTest,
    ::testing::Values(Engine::SQLite, Engine::MySQL),
    [](const ::testing::TestParamInfo<Engine>& info) {
        return std::string(engineName(info.param));
    });

}  // namespace

// ── SDD-015: the export's own read of a night's samples ─────────────────────
//
// The chart query filters `valid` and is right to. The EXPORT must not: a
// dropped row heals the timeline over, and the interval detector then measures
// the wrong cadence off the healed timestamps, so a night with the ring off the
// finger for twenty minutes comes back shorter than it was.
//
// Seeded under the real kOximetryDeviceId, because that is what the export
// queries, and far-dated so it cannot collide with anything real in a shared
// test database. Removed again in the test itself.
//
// The year is 2099 and NOT 2999 like the date-folder strings elsewhere in these
// suites, because this is the one place that builds a real time_point. On
// libstdc++ system_clock::duration is NANOSECONDS, so the representable range
// ends in 2262; year 2999 overflows int64 silently. to_time_t happens to
// round-trip the overflowed value, so the fixture looks fine until you do
// arithmetic on it -- and every sample below is start_time + seconds(off).
// That produced a stored start_time of 1829-11-23 and a night that no sleep-day
// query could find. macOS libc++ uses microseconds and never saw it.
// Keep any tpUtc() year under 2262.

TEST_P(OximetryBackendTest, TheExportReadKeepsTheSamplesTheChartHides) {
    const char* eng = engineName(GetParam());
    const std::string folder = "20990101";

    OximetrySession s;
    s.filename = "export_read_test.vld";
    s.start_time = tpUtc(2099, 1, 1, 23, 0);
    s.sample_interval = 2.0;
    auto add = [&](int off, uint8_t spo2, uint8_t hr, uint8_t motion) {
        OximetrySample smp{};
        smp.timestamp    = s.start_time + std::chrono::seconds(off);
        smp.spo2         = spo2;
        smp.heart_rate   = hr;
        smp.invalid_flag = (spo2 == 0xFF) ? 1 : 0;
        smp.motion       = motion;
        s.samples.push_back(smp);
    };
    add(0, 97, 60, 0);
    add(2, 0xFF, 0xFF, 0);      // ring off the finger
    add(4, 0xFF, 0xFF, 0);
    add(6, 96, 61, 29);         // motion is data, not a flag
    s.end_time = s.samples.back().timestamp;
    s.duration_seconds = 8;
    s.metrics.total_samples = 4;
    s.metrics.valid_samples = 2;

    ASSERT_TRUE(db().saveOximetrySession(kOximetryDeviceId, s)) << eng;

    auto& svc = SleepHqExportService::getInstance();
    svc.initialize(nullptr, db_.get());
    const auto out = svc.oximetrySessionFor(folder);
    svc.initialize(nullptr, nullptr);   // do not leave the singleton holding this db

    // Clean up before asserting, so a failure still leaves the table tidy.
    db().executeQuery(
        "DELETE FROM oximetry_samples WHERE oximetry_session_id IN "
        "(SELECT id FROM oximetry_sessions WHERE device_id = ? AND filename = ?)",
        {kOximetryDeviceId, s.filename});
    db().executeQuery("DELETE FROM oximetry_sessions WHERE device_id = ? AND filename = ?",
                      {kOximetryDeviceId, s.filename});

    ASSERT_EQ(out.samples.size(), 4u)
        << eng << ": the unreadable samples were dropped, the night got shorter";
    EXPECT_EQ(out.samples[0].spo2, 97) << eng;
    EXPECT_FALSE(out.samples[1].valid()) << eng;
    EXPECT_FALSE(out.samples[2].valid()) << eng;
    EXPECT_TRUE(out.samples[3].valid()) << eng;
    EXPECT_EQ(out.samples[3].motion, 29) << eng;
    EXPECT_EQ(out.start_time, s.samples.front().timestamp) << eng;
}

// A night with no ring produces nothing to upload, rather than an empty file.
TEST_P(OximetryBackendTest, ANightWithNoRingYieldsNoSession) {
    auto& svc = SleepHqExportService::getInstance();
    svc.initialize(nullptr, db_.get());
    const auto out = svc.oximetrySessionFor("29990202");
    svc.initialize(nullptr, nullptr);
    EXPECT_TRUE(out.samples.empty()) << engineName(GetParam());
}
