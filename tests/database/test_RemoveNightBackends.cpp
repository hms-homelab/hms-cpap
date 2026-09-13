/**
 * SDD-029 (#31): removing a night, on every engine.
 *
 * "Remove night" deletes one sleep night (a session's start shifted back 12 h,
 * the key the DATALOG folders and the daily summary already use) from every
 * table that holds it, in one transaction, and writes the record every
 * re-ingest path consults so the night does not come back on the next burst.
 * These pin the delete (that night, all of it, and nothing of the nights either
 * side, including the post-midnight session that belongs to the night before
 * its calendar date), the record, and the restore a Reparse performs.
 *
 * SAFETY: every row is dated 2099 and namespaced to a per-process device id or
 * filename, and deleted in TearDown. removeNight matches the ring's rows and
 * the transfer ledger by date alone (neither carries the CPAP's device id), so
 * the far-future dates are what keep an env-gated run against a populated
 * MySQL or PostgreSQL from touching a real night.
 */

#include <gtest/gtest.h>

#include "database/SQLiteDatabase.h"
#include "database/SqlDialect.h"
#include "services/RemovedNights.h"
#include "services/SyncFolderState.h"
#include "utils/OximetryDevice.h"
#include "utils/TimeCompat.h"
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
#include <unistd.h>

using namespace hms_cpap;
using namespace std::chrono;
using cpapdash::parser::OximetrySample;
using cpapdash::parser::OximetrySession;
namespace fs = std::filesystem;

namespace {

std::string envOr(const char* key, const std::string& fallback) {
    const char* v = std::getenv(key);
    return (v && *v) ? std::string(v) : fallback;
}

// SQLite stringifies every column; the others return typed values.
long long asCount(const Json::Value& v) {
    if (v.isNull()) return -1;
    if (v.isString()) return std::stoll(v.asString());
    return v.asInt64();
}

/// A CPAP start on the LOCAL clock, as the EDF wall clock is read (mktime).
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

/// A ring start: its printed wall clock read AS IF it were UTC (timegm).
system_clock::time_point ring(int y, int mo, int d, int h, int mi = 0) {
    std::tm tm{};
    tm.tm_year = y - 1900;
    tm.tm_mon = mo - 1;
    tm.tm_mday = d;
    tm.tm_hour = h;
    tm.tm_min = mi;
    return system_clock::from_time_t(timegm_utc(&tm));
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

class RemoveNightBackendTest : public ::testing::TestWithParam<Engine> {
protected:
    // The night removed, and the nights either side of it.
    static constexpr const char* kBefore = "20990609";
    static constexpr const char* kNight  = "20990610";
    static constexpr const char* kAfter  = "20990611";

    void SetUp() override {
        device_ = "rmn_test_" + std::to_string(::getpid());
        oxi_prefix_ = "rmn" + std::to_string(::getpid()) + "_";
        switch (GetParam()) {
            case Engine::SQLite: {
                path_ = (fs::temp_directory_path() /
                         ("hms_cpap_rmn_" + std::to_string(::getpid()) + ".db")).string();
                fs::remove(path_);
                auto lite = std::make_shared<SQLiteDatabase>(path_);
                ASSERT_TRUE(lite->connect()) << "SQLite connect failed";
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
            db_->executeQuery("DELETE FROM cpap_session_files WHERE session_id IN "
                              "(SELECT id FROM cpap_sessions WHERE device_id = " + p + ")",
                              {device_});
            for (const char* t : {"cpap_sessions", "cpap_daily_summary", "cpap_summaries",
                                  "cpap_removed_nights"})
                db_->executeQuery(std::string("DELETE FROM ") + t + " WHERE device_id = " + p,
                                  {device_});
            db_->executeQuery("DELETE FROM oximetry_samples WHERE oximetry_session_id IN "
                              "(SELECT id FROM oximetry_sessions WHERE filename LIKE " + p + ")",
                              {oxi_prefix_ + "%"});
            db_->executeQuery("DELETE FROM oximetry_sessions WHERE filename LIKE " + p,
                              {oxi_prefix_ + "%"});
            db_->executeQuery("DELETE FROM cpap_sync_folders WHERE date_folder LIKE " + p,
                              {std::string("2099%")});
        }
        db_.reset();
        if (!path_.empty()) {
            for (const auto* suffix : {"", "-wal", "-shm"}) {
                std::error_code ec;
                fs::remove(path_ + suffix, ec);
            }
        }
    }

    void saveSession(system_clock::time_point start, bool with_files = true) {
        CPAPSession s;
        s.device_id = device_;
        s.device_name = "AirSense 11";
        s.serial_number = "SDD029";
        s.session_start = start;
        s.duration_seconds = 3600;
        s.data_records = 60;
        SessionMetrics m;
        m.ahi = 1.0;
        s.metrics = m;
        ASSERT_TRUE(db_->saveSession(s)) << engineName(GetParam());
        if (with_files) {
            const auto night = strDayForSessionStart(start);
            ASSERT_TRUE(db_->replaceSessionFiles(
                device_, start, {{"brp", "DATALOG/" + night + "/x_BRP.edf"}}));
        }
    }

    /// An STR day, stamped at local noon as the parser stamps it.
    void saveStrDay(int d) {
        STRDailyRecord r;
        r.device_id = device_;
        r.record_date = local(2099, 6, d, 12);
        r.duration_minutes = 60;
        ASSERT_TRUE(db_->saveSTRDailyRecords({r})) << engineName(GetParam());
    }

    void saveRingNight(const std::string& tag, system_clock::time_point start) {
        OximetrySession s;
        s.filename = oxi_prefix_ + tag + ".vld";
        s.start_time = start;
        s.end_time = start + seconds(12);
        s.duration_seconds = 12;
        s.sample_interval = 4.0;
        for (int i = 0; i < 3; ++i) {
            OximetrySample smp{};
            smp.timestamp = start + seconds(i * 4);
            smp.spo2 = 96;
            smp.heart_rate = 60;
            s.samples.push_back(smp);
        }
        s.metrics.valid_samples = 3;
        s.metrics.total_samples = 3;
        ASSERT_TRUE(db_->saveOximetrySession(kOximetryDeviceId, s)) << engineName(GetParam());
    }

    void saveLedger(const std::string& folder) {
        FolderLedger f;
        f.date_folder = folder;
        f.files_listed = true;
        ASSERT_TRUE(db_->upsertSyncFolder(f)) << engineName(GetParam());
    }

    /// The three nights, fully populated.
    void populate() {
        saveSession(local(2099, 6, 9, 22));        // kBefore
        saveSession(local(2099, 6, 10, 22));       // kNight
        saveSession(local(2099, 6, 11, 2));        // kNight: after midnight, before noon
        saveSession(local(2099, 6, 11, 13));       // kAfter: after noon
        saveSession(local(2099, 6, 11, 22));       // kAfter
        for (int d : {9, 10, 11}) saveStrDay(d);
        saveRingNight("0610", ring(2099, 6, 10, 23));
        saveRingNight("0611", ring(2099, 6, 11, 23));
        for (const char* f : {kBefore, kNight, kAfter}) saveLedger(f);
        ASSERT_TRUE(db_->saveSummary(device_, "daily", "2099-06-10", "2099-06-10",
                                     1, 1.0, 1.0, 100.0, "the night"));
        ASSERT_TRUE(db_->saveSummary(device_, "daily", "2099-06-11", "2099-06-11",
                                     1, 1.0, 1.0, 100.0, "the night after"));
        ASSERT_TRUE(db_->saveSummary(device_, "weekly", "2099-06-07", "2099-06-13",
                                     5, 1.0, 1.0, 100.0, "the week"));
    }

    long long count(const std::string& sql, const std::vector<std::string>& args) {
        const auto rows = db_->executeQuery(sql, args);
        if (!rows.isArray() || rows.empty()) return -1;
        return asCount(rows[0]["n"]);
    }
    std::string p(int i) const { return sql::param(i, db_->dbType()); }

    long long sessionsOn(const std::string& night) {
        long long n = 0;
        const auto rows = db_->executeQuery(
            "SELECT session_start FROM cpap_sessions WHERE device_id = " + p(1), {device_});
        for (const auto& r : rows) {
            // Re-derived here, not with the backend's own date maths, so the
            // assertion does not share the code it checks.
            const auto s = r["session_start"].asString().substr(0, 16);
            std::tm tm{};
            tm.tm_year = std::stoi(s.substr(0, 4)) - 1900;
            tm.tm_mon = std::stoi(s.substr(5, 2)) - 1;
            tm.tm_mday = std::stoi(s.substr(8, 2));
            tm.tm_hour = std::stoi(s.substr(11, 2));
            tm.tm_min = std::stoi(s.substr(14, 2));
            tm.tm_isdst = -1;
            if (strDayForSessionStart(system_clock::from_time_t(std::mktime(&tm))) == night) ++n;
        }
        return n;
    }
    long long dailyRows() {
        return count("SELECT COUNT(*) AS n FROM cpap_daily_summary WHERE device_id = " + p(1),
                     {device_});
    }
    long long ringRows(const std::string& tag) {
        return count("SELECT COUNT(*) AS n FROM oximetry_sessions WHERE filename = " + p(1),
                     {oxi_prefix_ + tag + ".vld"});
    }
    long long fileRows() {
        return count("SELECT COUNT(*) AS n FROM cpap_session_files WHERE rel_path LIKE " + p(1),
                     {std::string("DATALOG/2099%")});
    }
    long long summaryRows(const std::string& period) {
        return count("SELECT COUNT(*) AS n FROM cpap_summaries WHERE device_id = " + p(1) +
                         " AND period = " + p(2),
                     {device_, period});
    }

    std::string path_;
    std::string device_;
    std::string oxi_prefix_;
    std::shared_ptr<IDatabase> db_;
};

}  // namespace

TEST_P(RemoveNightBackendTest, RemovesEveryRowOfThatNightAndNothingEitherSide) {
    populate();
    ASSERT_EQ(sessionsOn(kNight), 2);
    ASSERT_EQ(dailyRows(), 3);
    ASSERT_EQ(fileRows(), 5);

    const auto r = db_->removeNight(device_, kNight);
    ASSERT_TRUE(r.ok) << engineName(GetParam());

    // That night: both sessions (the 02:00 one belongs to it, not to the 11th),
    // its daily row, its ring night, its ledger row, its daily summary.
    EXPECT_EQ(r.sessions, 2) << engineName(GetParam());
    EXPECT_EQ(r.daily, 1) << engineName(GetParam());
    EXPECT_EQ(r.oximetry, 1) << engineName(GetParam());
    EXPECT_EQ(r.ledger, 1) << engineName(GetParam());
    EXPECT_EQ(r.summaries, 1) << engineName(GetParam());
    EXPECT_EQ(sessionsOn(kNight), 0) << engineName(GetParam());
    EXPECT_EQ(ringRows("0610"), 0) << engineName(GetParam());
    EXPECT_FALSE(db_->getSyncFolder(kNight).has_value()) << engineName(GetParam());

    // Either side: untouched.
    EXPECT_EQ(sessionsOn(kBefore), 1) << engineName(GetParam());
    EXPECT_EQ(sessionsOn(kAfter), 2) << engineName(GetParam());
    EXPECT_EQ(dailyRows(), 2) << engineName(GetParam());
    EXPECT_EQ(ringRows("0611"), 1) << engineName(GetParam());
    EXPECT_TRUE(db_->getSyncFolder(kBefore).has_value()) << engineName(GetParam());
    EXPECT_TRUE(db_->getSyncFolder(kAfter).has_value()) << engineName(GetParam());
    EXPECT_EQ(summaryRows("daily"), 1) << engineName(GetParam());
    EXPECT_EQ(summaryRows("weekly"), 1)
        << engineName(GetParam()) << ": a range summary is not the night's to delete";
}

TEST_P(RemoveNightBackendTest, LeavesNoOrphanFileRows) {
    populate();
    ASSERT_TRUE(db_->removeNight(device_, kNight).ok);
    // Two sessions of the night each had one file row; the other three stay.
    EXPECT_EQ(fileRows(), 3)
        << engineName(GetParam()) << ": cpap_session_files is not ON DELETE CASCADE";
}

TEST_P(RemoveNightBackendTest, WritesTheRecordAndReparseClearsIt) {
    populate();
    EXPECT_TRUE(db_->removedNights(device_).empty());

    ASSERT_TRUE(db_->removeNight(device_, kNight).ok);
    const auto removed = db_->removedNights(device_);
    ASSERT_EQ(removed.size(), 1u) << engineName(GetParam());
    EXPECT_EQ(removed[0], kNight);
    // Scoped to the device.
    EXPECT_TRUE(db_->removedNights(device_ + "_other").empty()) << engineName(GetParam());

    EXPECT_TRUE(restoreNightByDate(*db_, device_, "2099-06-10")) << engineName(GetParam());
    EXPECT_TRUE(db_->removedNights(device_).empty()) << engineName(GetParam());
    // Nothing left to restore.
    EXPECT_FALSE(restoreNightByDate(*db_, device_, "2099-06-10")) << engineName(GetParam());
}

TEST_P(RemoveNightBackendTest, RemovingTwiceIsOneRecordAndNothingMoreToDelete) {
    populate();
    ASSERT_TRUE(db_->removeNight(device_, kNight).ok);
    const auto again = db_->removeNight(device_, kNight);
    ASSERT_TRUE(again.ok) << engineName(GetParam());
    EXPECT_EQ(again.sessions, 0);
    EXPECT_EQ(again.daily, 0);
    EXPECT_EQ(db_->removedNights(device_).size(), 1u) << engineName(GetParam());
}

TEST_P(RemoveNightBackendTest, TheStrRewriteDoesNotBringTheDayBack) {
    populate();
    ASSERT_TRUE(db_->removeNight(device_, kNight).ok);

    // What the local burst does every cycle: re-write the WHOLE STR history,
    // through the filter every STR writer now applies.
    std::vector<STRDailyRecord> history;
    for (int d : {9, 10, 11}) {
        STRDailyRecord r;
        r.device_id = device_;
        r.record_date = local(2099, 6, d, 12);
        r.duration_minutes = 60;
        history.push_back(r);
    }
    const auto kept = withoutRemovedNights(history, removedNightSet(*db_, device_));
    EXPECT_EQ(kept.size(), 2u);
    ASSERT_TRUE(db_->saveSTRDailyRecords(kept));
    EXPECT_EQ(dailyRows(), 2) << engineName(GetParam());
}

TEST_P(RemoveNightBackendTest, NotADateRemovesNothing) {
    populate();
    EXPECT_FALSE(removeNightByDate(*db_, device_, "yesterday").ok);
    EXPECT_FALSE(removeNightByDate(*db_, device_, "2099-06").ok);
    EXPECT_TRUE(db_->removedNights(device_).empty());
    EXPECT_EQ(sessionsOn(kNight), 2);
}

INSTANTIATE_TEST_SUITE_P(
    Engines, RemoveNightBackendTest,
    ::testing::Values(Engine::SQLite, Engine::MySQL, Engine::Postgres),
    [](const ::testing::TestParamInfo<Engine>& info) {
        return std::string(engineName(info.param));
    });

// ── The keys the re-ingest paths use ────────────────────────────────────────

TEST(RemovedNightKeys, ASessionsNightIsItsStartShiftedBackTwelveHours) {
    const std::set<std::string> removed{"20990610"};
    EXPECT_TRUE(isRemovedNight(removed, local(2099, 6, 10, 22)));
    EXPECT_TRUE(isRemovedNight(removed, local(2099, 6, 11, 11, 59)));
    EXPECT_FALSE(isRemovedNight(removed, local(2099, 6, 11, 12, 1)));
    EXPECT_FALSE(isRemovedNight(removed, local(2099, 6, 10, 11)));
    EXPECT_FALSE(isRemovedNight({}, local(2099, 6, 10, 22)));
}

TEST(RemovedNightKeys, TheRingsNightIsOnItsOwnClock) {
    // 13:30 on the ring's display is the 10th's night whatever the host's zone.
    // The local rule would call it the 9th on any host west of UTC.
    EXPECT_EQ(oximetryNightOf(ring(2099, 6, 10, 13, 30)), "20990610");
    EXPECT_EQ(oximetryNightOf(ring(2099, 6, 11, 3)), "20990610");
    EXPECT_EQ(oximetryNightOf(ring(2099, 6, 10, 11)), "20990609");
}

TEST(RemovedNightKeys, NightKeyOfTakesEitherForm) {
    EXPECT_EQ(nightKeyOf("2099-06-10"), "20990610");
    EXPECT_EQ(nightKeyOf("20990610"), "20990610");
    EXPECT_EQ(nightKeyOf("2099-06"), "");
    EXPECT_EQ(nightKeyOf(""), "");
}
