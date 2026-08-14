//
// test_SessionFilesBackends.cpp: SDD-014's cpap_session_files, across engines.
//
// Same reason the other *Backends suites exist: 4.6.3 found four oximetry
// methods that had been inline header stubs on SQLite and MySQL for months,
// unnoticed because only PostgreSQL was ever exercised. This table is written
// by three engines and read by the SleepHQ export, so "implemented on the
// engine I happened to run" is exactly the failure to guard against.
//
// SQLite always runs (temp file, no server). MySQL runs when MYSQL_TEST_HOST is
// set and skips cleanly otherwise.
//
// SAFETY: never DROPs or TRUNCATEs. Every row is namespaced to a per-process
// device_id and removed in TearDown, so pointing MYSQL_TEST_* at a populated
// database cannot disturb real data.
//
#include <gtest/gtest.h>

#include "database/IDatabase.h"
#include "database/SQLiteDatabase.h"
#ifdef WITH_MYSQL
#include "database/MySQLDatabase.h"
#endif

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <memory>
#include <string>
#include <unistd.h>

using namespace hms_cpap;
using std::chrono::system_clock;

namespace {

std::string envOr(const char* key, const std::string& fallback) {
    const char* v = std::getenv(key);
    return (v && *v) ? std::string(v) : fallback;
}

enum class Engine { SQLite, MySQL };
const char* engineName(Engine e) { return e == Engine::SQLite ? "SQLite" : "MySQL"; }

class SessionFilesBackendTest : public ::testing::TestWithParam<Engine> {
protected:
    std::unique_ptr<IDatabase> db_;
    std::string path_;
    std::string device_;
    system_clock::time_point start_;
    static constexpr const char* kFolder = "29990101";

    void SetUp() override {
        if (GetParam() == Engine::SQLite) {
            path_ = (std::filesystem::temp_directory_path() /
                     ("hms_sessfiles_" + std::to_string(::getpid()) + ".db")).string();
            std::filesystem::remove(path_);
            auto lite = std::make_unique<SQLiteDatabase>(path_);
            ASSERT_TRUE(lite->connect());
            db_ = std::move(lite);
        } else {
#ifndef WITH_MYSQL
            GTEST_SKIP() << "built without MySQL (-DBUILD_WITH_MYSQL=OFF)";
#else
            const std::string host = envOr("MYSQL_TEST_HOST", "");
            if (host.empty()) {
                GTEST_SKIP() << "MYSQL_TEST_HOST unset, skipping session-files parity on MySQL.";
            }
            auto my = std::make_unique<MySQLDatabase>(
                host, std::stoi(envOr("MYSQL_TEST_PORT", "3306")),
                envOr("MYSQL_TEST_USER", ""), envOr("MYSQL_TEST_PASSWORD", ""),
                envOr("MYSQL_TEST_DB", "hms_cpap_test"));
            if (!my->connect()) {
                GTEST_SKIP() << "No usable MySQL at " << host;
            }
            db_ = std::move(my);
#endif
        }
        device_ = "sf_test_" + std::to_string(::getpid());
        // A fixed instant, so the +/-5s lookup is exercised against a real value
        // rather than against "now" drifting between write and read.
        std::tm tm{};
        tm.tm_year = 130; tm.tm_mon = 0; tm.tm_mday = 1;   // 2030-01-01 22:00:00
        tm.tm_hour = 22;  tm.tm_min = 0; tm.tm_sec  = 0;
        tm.tm_isdst = -1;
        start_ = system_clock::from_time_t(std::mktime(&tm));

        ASSERT_TRUE(db_->saveSession(makeSession()));
    }

    void TearDown() override {
        if (db_ && !device_.empty()) {
            db_->executeQuery(
                "DELETE FROM cpap_session_files WHERE session_id IN "
                "(SELECT id FROM cpap_sessions WHERE device_id = ?)", {device_});
            db_->executeQuery("DELETE FROM cpap_sessions WHERE device_id = ?", {device_});
        }
        db_.reset();
        if (!path_.empty()) std::filesystem::remove(path_);
    }

    CPAPSession makeSession() const {
        CPAPSession s;
        s.device_id     = device_;
        s.device_name   = "AirSense 11";
        s.serial_number = "23243570851";
        s.session_start = start_;
        s.duration_seconds = 4 * 3600;
        s.data_records  = 120;
        s.brp_file_path = std::string("DATALOG/") + kFolder + "/a_BRP.edf";
        return s;
    }

    std::vector<SessionFileRef> refs() const {
        const std::string base = std::string("DATALOG/") + kFolder + "/";
        return {
            {"brp", base + "a_BRP.edf"},
            {"eve", base + "a_EVE.edf"},
            {"eve", base + "b_EVE.edf"},   // the point of the table: several EVEs
            {"eve", base + "c_EVE.edf"},
            {"csl", base + "a_CSL.edf"},
        };
    }

    static std::vector<std::string> paths(const std::vector<SessionFileRef>& v) {
        std::vector<std::string> out;
        for (const auto& r : v) out.push_back(r.rel_path);
        std::sort(out.begin(), out.end());
        return out;
    }
};

}  // namespace

TEST_P(SessionFilesBackendTest, EveryRecordedFileComesBack) {
    const char* eng = engineName(GetParam());
    ASSERT_TRUE(db_->replaceSessionFiles(device_, start_, refs())) << eng;

    auto got = db_->getSessionFilesForDateFolder(device_, kFolder);
    ASSERT_EQ(got.size(), 5u) << eng << ": a night's file set came back short";
    EXPECT_EQ(paths(got), paths(refs())) << eng;

    int eves = 0;
    for (const auto& r : got) if (r.kind == "eve") eves++;
    EXPECT_EQ(eves, 3) << eng << ": the several-EVE case is the whole reason for this table";
}

// A reparse replaces the set. Without delete-then-insert a re-run doubles every
// row, and the SleepHQ export would upload each file twice.
TEST_P(SessionFilesBackendTest, ReplacingIsNotAppending) {
    const char* eng = engineName(GetParam());
    ASSERT_TRUE(db_->replaceSessionFiles(device_, start_, refs())) << eng;
    ASSERT_TRUE(db_->replaceSessionFiles(device_, start_, refs())) << eng;

    EXPECT_EQ(db_->getSessionFilesForDateFolder(device_, kFolder).size(), 5u)
        << eng << ": a reparse doubled the night's files";
}

TEST_P(SessionFilesBackendTest, AShorterSetReplacesALongerOne) {
    const char* eng = engineName(GetParam());
    ASSERT_TRUE(db_->replaceSessionFiles(device_, start_, refs())) << eng;

    std::vector<SessionFileRef> one = {
        {"brp", std::string("DATALOG/") + kFolder + "/a_BRP.edf"}};
    ASSERT_TRUE(db_->replaceSessionFiles(device_, start_, one)) << eng;

    auto got = db_->getSessionFilesForDateFolder(device_, kFolder);
    ASSERT_EQ(got.size(), 1u) << eng << ": stale rows survived a replace";
    EXPECT_EQ(got[0].rel_path, one[0].rel_path) << eng;
}

// An empty set clears it, which is what a night that lost its files should look
// like rather than the previous set lingering.
TEST_P(SessionFilesBackendTest, AnEmptySetClearsTheNight) {
    const char* eng = engineName(GetParam());
    ASSERT_TRUE(db_->replaceSessionFiles(device_, start_, refs())) << eng;
    ASSERT_TRUE(db_->replaceSessionFiles(device_, start_, {})) << eng;
    EXPECT_TRUE(db_->getSessionFilesForDateFolder(device_, kFolder).empty()) << eng;
}

// The session start in memory can differ from the stored one by a rounding, so
// the lookup allows +/-5s exactly as sessionExists does. An exact match would
// silently record nothing.
TEST_P(SessionFilesBackendTest, ASlightlyOffStartStillFindsTheSession) {
    const char* eng = engineName(GetParam());
    EXPECT_TRUE(db_->replaceSessionFiles(device_, start_ + std::chrono::seconds(3), refs()))
        << eng;
    EXPECT_EQ(db_->getSessionFilesForDateFolder(device_, kFolder).size(), 5u) << eng;
}

// Recording against a session that does not exist must fail rather than write
// orphan rows keyed to nothing.
TEST_P(SessionFilesBackendTest, AnUnknownSessionIsRefused) {
    const char* eng = engineName(GetParam());
    EXPECT_FALSE(db_->replaceSessionFiles(device_, start_ + std::chrono::hours(72), refs()))
        << eng;
}

// The night is addressed by date folder everywhere else in IDatabase, so the
// lookup must not leak another night's files into this one.
TEST_P(SessionFilesBackendTest, AnotherDateFolderIsNotReturned) {
    const char* eng = engineName(GetParam());
    ASSERT_TRUE(db_->replaceSessionFiles(device_, start_, refs())) << eng;
    EXPECT_TRUE(db_->getSessionFilesForDateFolder(device_, "29990202").empty()) << eng;
}

// Another device's night is not this device's night.
TEST_P(SessionFilesBackendTest, AnotherDeviceIsNotReturned) {
    const char* eng = engineName(GetParam());
    ASSERT_TRUE(db_->replaceSessionFiles(device_, start_, refs())) << eng;
    EXPECT_TRUE(db_->getSessionFilesForDateFolder(device_ + "_other", kFolder).empty()) << eng;
}

INSTANTIATE_TEST_SUITE_P(
    Engines, SessionFilesBackendTest,
    ::testing::Values(Engine::SQLite, Engine::MySQL),
    [](const ::testing::TestParamInfo<Engine>& i) { return engineName(i.param); });

// ── Issue 24: the sleep-day lookup is a stub off PostgreSQL ─────────────────
//
// SQLiteDatabase::getSessionStartForSleepDay and the MySQL one returned
// std::nullopt unconditionally, so on the DEFAULT backend:
//   - POST /api/sessions/<date>/generate-summary always answered "failed"
//     ("generateSummaryForDate: no session found"), and
//   - forceCompleteSession could never find a night to close.
// PostgreSQL had the real implementation, which is why it was invisible to
// anyone running the backend this project does not default to.
//
// The rule is noon-to-noon, matching PostgreSQL:
//     DATE(session_start - 12 hours) = sleep_day
// A night that starts at 22:00 on the 12th and one that starts at 02:00 on the
// 13th are the SAME sleep day, and that is the whole point of the offset.

TEST_P(SessionFilesBackendTest, TheSleepDayLookupFindsAnEveningSession) {
    const char* eng = engineName(GetParam());
    // start_ is 2030-01-01 22:00 local, so its sleep day is 2030-01-01.
    auto got = db_->getSessionStartForSleepDay(device_, "2030-01-01", false);
    ASSERT_TRUE(got.has_value())
        << eng << ": the lookup is a stub, so the AI summary can never find a night";
    EXPECT_EQ(*got, start_) << eng;
}

TEST_P(SessionFilesBackendTest, AnAfterMidnightSessionBelongsToTheNightBefore) {
    const char* eng = engineName(GetParam());
    // 02:00 on the 2nd is still the night of the 1st under the noon-to-noon rule.
    auto after_midnight = start_ + std::chrono::hours(4);   // 2030-01-02 02:00
    CPAPSession s = makeSession();
    s.session_start = after_midnight;
    ASSERT_TRUE(db_->saveSession(s)) << eng;

    auto got = db_->getSessionStartForSleepDay(device_, "2030-01-01", false);
    ASSERT_TRUE(got.has_value()) << eng;
    // Two sessions share the night; the EARLIEST is the one returned.
    EXPECT_EQ(*got, start_) << eng << ": expected the first session of the night";

    // ...and the following day owns neither of them.
    EXPECT_FALSE(db_->getSessionStartForSleepDay(device_, "2030-01-02", false).has_value())
        << eng << ": an after-midnight session was attributed to the wrong night";
}

TEST_P(SessionFilesBackendTest, OpenOnlySkipsAClosedSession) {
    const char* eng = engineName(GetParam());
    // The session seeded in SetUp has no session_end, so it is open.
    EXPECT_TRUE(db_->getSessionStartForSleepDay(device_, "2030-01-01", true).has_value())
        << eng << ": forceCompleteSession could not find the open night";

    ASSERT_TRUE(db_->markSessionCompleted(device_, start_)) << eng;
    EXPECT_FALSE(db_->getSessionStartForSleepDay(device_, "2030-01-01", true).has_value())
        << eng << ": a closed session must not answer an open_only lookup";
    EXPECT_TRUE(db_->getSessionStartForSleepDay(device_, "2030-01-01", false).has_value())
        << eng << ": it is still findable without open_only";
}

TEST_P(SessionFilesBackendTest, AnUnknownSleepDayFindsNothing) {
    EXPECT_FALSE(db_->getSessionStartForSleepDay(device_, "2030-06-15", false).has_value())
        << engineName(GetParam());
}
