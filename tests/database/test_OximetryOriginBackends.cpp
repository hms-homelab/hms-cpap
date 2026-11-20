// SDD-078 section 4, D3: oximetry knows its source. The migration on the three
// engines (old ring rows read as "o2ring", uploads as "upload"), the origin
// stored and read back, the upload API with and without the new fields, and
// the night's oximetry saying where it came from.
//
// SQLite always runs. MySQL and PostgreSQL run when their usual variables are
// set (MYSQL_TEST_HOST..., PGHOST...). The old-shape migration on those two
// DROPS the new columns first, so it runs only with HMS_CPAP_MIGRATION_TEST=1,
// against scratch databases.
#include "database/IDatabase.h"
#include "database/SQLiteDatabase.h"
#include "database/SqlDialect.h"
#ifdef WITH_MYSQL
#include "database/MySQLDatabase.h"
#endif
#ifdef WITH_POSTGRESQL
#include "database/PostgresDatabase.h"
#endif
#include "services/OximetryImport.h"
#include "utils/OximetryDevice.h"
#include "utils/TimeCompat.h"
#include "web/QueryService.h"

#include <gtest/gtest.h>
#include <sqlite3.h>

#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <memory>
#include <vector>
#include <unistd.h>

using namespace hms_cpap;
using cpapdash::parser::OximetrySample;
using cpapdash::parser::OximetrySession;
namespace fs = std::filesystem;

namespace {

std::string envOr(const char* key, const std::string& fallback) {
    const char* v = std::getenv(key);
    return (v && *v) ? std::string(v) : fallback;
}

std::chrono::system_clock::time_point wall(int y, int mo, int d, int h, int mi) {
    std::tm tm{};
    tm.tm_year = y - 1900;
    tm.tm_mon  = mo - 1;
    tm.tm_mday = d;
    tm.tm_hour = h;
    tm.tm_min  = mi;
    return std::chrono::system_clock::from_time_t(timegm_utc(&tm));
}

OximetrySession recording(const std::string& filename, std::chrono::system_clock::time_point start, int n = 5) {
    OximetrySession s;
    s.filename = filename;
    s.start_time = start;
    s.sample_interval = 60;
    for (int i = 0; i < n; ++i) {
        OximetrySample smp{};
        smp.timestamp = start + std::chrono::seconds(i * 60);
        smp.spo2 = 95;
        smp.heart_rate = 60;
        s.samples.push_back(smp);
    }
    s.end_time = s.samples.back().timestamp;
    s.duration_seconds = (n - 1) * 60;
    s.metrics.valid_samples = n;
    s.metrics.total_samples = n;
    s.metrics.avg_spo2 = 95;
    return s;
}

std::string str(const Json::Value& v) { return v.isNull() ? "<null>" : v.asString(); }

enum class Engine { SQLite, MySQL, Postgres };

const char* engineName(Engine e) {
    switch (e) {
        case Engine::SQLite:   return "SQLite";
        case Engine::MySQL:    return "MySQL";
        case Engine::Postgres: return "Postgres";
    }
    return "?";
}

class OximetryOriginTest : public ::testing::TestWithParam<Engine> {
protected:
    std::shared_ptr<IDatabase> open() {
        switch (GetParam()) {
            case Engine::SQLite: {
                auto lite = std::make_shared<SQLiteDatabase>(path_);
                return lite->connect() ? lite : nullptr;
            }
            case Engine::MySQL: {
#ifdef WITH_MYSQL
                auto my = std::make_shared<MySQLDatabase>(
                    envOr("MYSQL_TEST_HOST", ""), std::stoi(envOr("MYSQL_TEST_PORT", "3306")),
                    envOr("MYSQL_TEST_USER", ""), envOr("MYSQL_TEST_PASSWORD", ""),
                    envOr("MYSQL_TEST_DB", "hms_cpap_test"));
                return my->connect() ? my : nullptr;
#else
                return nullptr;
#endif
            }
            case Engine::Postgres: {
#ifdef WITH_POSTGRESQL
                auto pg = std::make_shared<PostgresDatabase>(
                    "host=" + envOr("PGHOST", "") + " port=" + envOr("PGPORT", "5432") +
                    " user=" + envOr("PGUSER", "maestro") + " password=" + envOr("PGPASSWORD", "") +
                    " dbname=" + envOr("PGDATABASE", "cpap_monitoring") + " connect_timeout=3");
                return pg->connect() ? pg : nullptr;
#else
                return nullptr;
#endif
            }
        }
        return nullptr;
    }

    void SetUp() override {
        tag_ = "sdd078_" + std::to_string(::getpid());
        path_ = (fs::temp_directory_path() / ("hms_oxi_origin_" + std::to_string(::getpid()) + ".db")).string();
        if (GetParam() == Engine::SQLite) fs::remove(path_);
        if (GetParam() == Engine::MySQL && envOr("MYSQL_TEST_HOST", "").empty())
            GTEST_SKIP() << "MYSQL_TEST_HOST unset, skipping MySQL.";
        if (GetParam() == Engine::Postgres && envOr("PGHOST", "").empty())
            GTEST_SKIP() << "PGHOST unset, skipping PostgreSQL.";
        db_ = open();
        if (!db_) GTEST_SKIP() << "no usable " << engineName(GetParam());
        cleanup();
    }

    void TearDown() override {
        if (db_) cleanup();
        db_.reset();
        if (GetParam() == Engine::SQLite)
            for (const auto* s : {"", "-wal", "-shm"}) {
                std::error_code ec;
                fs::remove(path_ + s, ec);
            }
    }

    /// Only this process's rows (their names carry the tag), and the 2099 nights.
    void cleanup() {
        const auto p = sql::param(1, db_->dbType());
        const std::string which = "SUBSTR(filename, 1, " + std::to_string(tag_.size()) + ") = " + p +
                                  " OR filename LIKE 'live_2099%'";
        db_->executeQuery("DELETE FROM oximetry_samples WHERE oximetry_session_id IN "
                          "(SELECT id FROM oximetry_sessions WHERE " + which + ")", {tag_});
        db_->executeQuery("DELETE FROM oximetry_sessions WHERE " + which, {tag_});
    }

    Json::Value row(const std::string& filename) {
        const auto r = db_->executeQuery(
            "SELECT source, device_name FROM oximetry_sessions WHERE filename = " +
                sql::param(1, db_->dbType()), {filename});
        return r.isArray() && !r.empty() ? r[0u] : Json::Value(Json::nullValue);
    }

    std::string tag_;
    std::string path_;
    std::shared_ptr<IDatabase> db_;
};

}  // namespace

TEST_P(OximetryOriginTest, TheOriginIsStoredAndReadBack) {
    const auto f = tag_ + "-garmin";
    ASSERT_TRUE(db_->saveOximetrySessionFrom(kOximetryDeviceId, recording(f, wall(2099, 3, 1, 23, 0)),
                                             {"garmin", "Garmin Fenix 7X"}));
    EXPECT_EQ(str(row(f)["source"]), "garmin");
    EXPECT_EQ(str(row(f)["device_name"]), "Garmin Fenix 7X");
    // Saved again with another origin: the same row, relabelled.
    ASSERT_TRUE(db_->saveOximetrySessionFrom(kOximetryDeviceId, recording(f, wall(2099, 3, 1, 23, 0)),
                                             {"upload", ""}));
    EXPECT_EQ(str(row(f)["source"]), "upload");
}

TEST_P(OximetryOriginTest, TheTwoArgumentSaveIsTheRings) {
    const auto f = tag_ + "-ring.vld";
    ASSERT_TRUE(db_->saveOximetrySession(kOximetryDeviceId, recording(f, wall(2099, 3, 2, 23, 0))));
    EXPECT_EQ(str(row(f)["source"]), "o2ring");
}

TEST_P(OximetryOriginTest, OnlyTheRingsOwnFileSupersedesItsLiveSession) {
    const auto live = wall(2099, 3, 3, 23, 5);
    ASSERT_TRUE(db_->saveLiveOximetrySample(kOximetryDeviceId, live, 95, 60, 0));
    ASSERT_TRUE(db_->saveOximetrySessionFrom(kOximetryDeviceId, recording(tag_ + "-g", wall(2099, 3, 3, 23, 0)),
                                             {"garmin", "Garmin Fenix 7X"}));
    const auto p = sql::param(1, db_->dbType());
    EXPECT_EQ(db_->executeQuery("SELECT id FROM oximetry_sessions WHERE filename = " + p,
                                {"live_20990303.vld"}).size(), 1u);
    ASSERT_TRUE(db_->saveOximetrySessionFrom(kOximetryDeviceId, recording(tag_ + "-r.vld", wall(2099, 3, 3, 23, 0)),
                                             {"o2ring", ""}));
    EXPECT_EQ(db_->executeQuery("SELECT id FROM oximetry_sessions WHERE filename = " + p,
                                {"live_20990303.vld"}).size(), 0u);
}

TEST_P(OximetryOriginTest, ARowWithNoSourceIsBackfilledAsTheRingsOrAnUpload) {
    // A row as an older build (or the live writer) leaves it: no source.
    const auto p1 = sql::param(1, db_->dbType());
    const auto p2 = sql::param(2, db_->dbType());
    for (const std::string& f : {tag_ + "-old.vld", tag_ + "-old.csv"})
        db_->executeQuery("INSERT INTO oximetry_sessions (device_id, filename, start_time, end_time) VALUES (" +
                              p1 + ", " + p2 + ", '2099-03-04 23:00:00', '2099-03-05 06:00:00')",
                          {kOximetryDeviceId, f});
    EXPECT_EQ(str(row(tag_ + "-old.vld")["source"]), "<null>");
    db_->executeQuery(sql::oxiSourceBackfill(), {});
    EXPECT_EQ(str(row(tag_ + "-old.vld")["source"]), "o2ring");
    EXPECT_EQ(str(row(tag_ + "-old.csv")["source"]), "upload");
}

TEST_P(OximetryOriginTest, AnOldTableIsMigrated) {
    if (GetParam() != Engine::SQLite && envOr("HMS_CPAP_MIGRATION_TEST", "") != "1")
        GTEST_SKIP() << "drops the columns first: set HMS_CPAP_MIGRATION_TEST=1 on a scratch database";
    const std::vector<std::string> old_rows{tag_ + "-a.vld", tag_ + "-b.csv", "live_20990305.vld"};
    if (GetParam() == Engine::SQLite) {
        // An install from before: the table without the two columns, and its rows.
        db_.reset();
        fs::remove(path_);
        sqlite3* raw = nullptr;
        ASSERT_EQ(sqlite3_open(path_.c_str(), &raw), SQLITE_OK);
        ASSERT_EQ(sqlite3_exec(raw,
                               "CREATE TABLE oximetry_sessions (id INTEGER PRIMARY KEY AUTOINCREMENT,"
                               " device_id TEXT NOT NULL, filename TEXT UNIQUE NOT NULL, start_time TEXT NOT NULL,"
                               " end_time TEXT NOT NULL, duration_seconds INTEGER, sample_interval REAL,"
                               " avg_spo2 REAL, min_spo2 REAL, spo2_baseline REAL, time_below_90 REAL,"
                               " time_below_88 REAL, odi_3pct REAL, desat_count INTEGER, avg_hr REAL,"
                               " min_hr INTEGER, max_hr INTEGER, valid_samples INTEGER, total_samples INTEGER,"
                               " cpap_session_date TEXT, created_at TEXT DEFAULT (datetime('now')));",
                               nullptr, nullptr, nullptr), SQLITE_OK);
        for (const auto& f : old_rows) {
            const std::string q = "INSERT INTO oximetry_sessions (device_id, filename, start_time, end_time)"
                                  " VALUES ('o2ring', '" + f + "', '2099-03-05 23:00:00', '2099-03-06 06:00:00')";
            ASSERT_EQ(sqlite3_exec(raw, q.c_str(), nullptr, nullptr, nullptr), SQLITE_OK);
        }
        sqlite3_close(raw);
    } else {
        // The table as an older build left it, on a scratch database.
        db_->executeQuery("ALTER TABLE oximetry_sessions DROP COLUMN source", {});
        db_->executeQuery("ALTER TABLE oximetry_sessions DROP COLUMN device_name", {});
        const auto p1 = sql::param(1, db_->dbType());
        const auto p2 = sql::param(2, db_->dbType());
        for (const auto& f : old_rows)
            db_->executeQuery("INSERT INTO oximetry_sessions (device_id, filename, start_time, end_time) VALUES (" +
                                  p1 + ", " + p2 + ", '2099-03-05 23:00:00', '2099-03-06 06:00:00')",
                              {kOximetryDeviceId, f});
        EXPECT_TRUE(row(old_rows[0]).isNull()) << "the columns are gone before the restart";
        db_.reset();
    }
    // The restart: connect runs the migration.
    db_ = open();
    ASSERT_TRUE(db_);
    EXPECT_EQ(str(row(tag_ + "-a.vld")["source"]), "o2ring") << engineName(GetParam());
    EXPECT_EQ(str(row(tag_ + "-b.csv")["source"]), "upload");
    EXPECT_EQ(str(row("live_20990305.vld")["source"]), "o2ring");
    // And the new columns take the new writers.
    ASSERT_TRUE(db_->saveOximetrySessionFrom(kOximetryDeviceId, recording(tag_ + "-c", wall(2099, 3, 6, 23, 0)),
                                             {"garmin", "Garmin Fenix 7X"}));
    EXPECT_EQ(str(row(tag_ + "-c")["device_name"]), "Garmin Fenix 7X");
}

TEST_P(OximetryOriginTest, TheUploadApiTakesTheNewFieldsOrNone) {
    const std::string csv =
        "Time,Oxygen Level,Pulse Rate,Motion,O2 Reminder,PR Reminder\n"
        "23:00:00 Mar 07 2099,95,60,0,,\n23:00:04 Mar 07 2099,94,61,0,,\n";
    auto r = importOximetryUpload(*db_, csv, tag_ + "-plain.csv", uploadOrigin("", ""));
    ASSERT_FALSE(r.isMember("error")) << r["error"].asString();
    EXPECT_EQ(str(row(tag_ + "-plain.csv")["source"]), "upload");
    EXPECT_EQ(r["source"].asString(), "upload");

    r = importOximetryUpload(*db_, csv, tag_ + "-labelled.csv", uploadOrigin("garmin", "  Garmin Fenix 7X  "));
    ASSERT_FALSE(r.isMember("error"));
    EXPECT_EQ(str(row(tag_ + "-labelled.csv")["source"]), "garmin");
    EXPECT_EQ(str(row(tag_ + "-labelled.csv")["device_name"]), "Garmin Fenix 7X");

    r = importOximetryUpload(*db_, csv, tag_ + "-odd.csv", uploadOrigin("Not A Source!", ""));
    EXPECT_EQ(str(row(tag_ + "-odd.csv")["source"]), "upload") << "a malformed source is an upload";
}

INSTANTIATE_TEST_SUITE_P(Engines, OximetryOriginTest,
                         ::testing::Values(Engine::SQLite, Engine::MySQL, Engine::Postgres),
                         [](const ::testing::TestParamInfo<Engine>& i) { return engineName(i.param); });

TEST(OximetryOrigin, UploadOriginNormalisesTheFields) {
    EXPECT_EQ(uploadOrigin("", "").source, "upload");
    EXPECT_EQ(uploadOrigin("GARMIN", "").source, "garmin");
    EXPECT_EQ(uploadOrigin("my-watch_2", "").source, "my-watch_2");
    EXPECT_EQ(uploadOrigin("has space", "").source, "upload");
    EXPECT_EQ(uploadOrigin(std::string(33, 'a'), "").source, "upload");
    EXPECT_EQ(uploadOrigin("", std::string(200, 'x')).device_name.size(), 128u);
    EXPECT_EQ(uploadOrigin("", "A\nB").device_name, "AB");
}

TEST(OximetryOrigin, TheNightSaysWhereItsOximetryCameFrom) {
    const auto path = (fs::temp_directory_path() / ("hms_oxi_q_" + std::to_string(::getpid()) + ".db")).string();
    fs::remove(path);
    {
        auto db = std::make_shared<SQLiteDatabase>(path);
        ASSERT_TRUE(db->connect());
        ASSERT_TRUE(db->saveOximetrySessionFrom(kOximetryDeviceId, recording("garmin-1", wall(2099, 4, 1, 23, 0)),
                                                {"garmin", "Garmin Fenix 7X"}));
        ASSERT_TRUE(db->saveOximetrySession(kOximetryDeviceId, recording("ring.vld", wall(2099, 4, 2, 23, 0))));
        QueryService q(db, "cpap");
        auto o = q.getSessionOximetry("2099-04-01", 4);
        EXPECT_EQ(o["source"].asString(), "garmin");
        EXPECT_EQ(o["device_name"].asString(), "Garmin Fenix 7X");
        ASSERT_EQ(o["sources"].size(), 1u);
        EXPECT_EQ(o["timestamps"].size(), 5u);
        o = q.getSessionOximetry("2099-04-02", 4);
        EXPECT_EQ(o["source"].asString(), "o2ring");
        EXPECT_EQ(o["device_name"].asString(), "");
        o = q.getSessionOximetry("2099-04-03", 4);
        EXPECT_TRUE(o["source"].isNull()) << "no recording, no source";
    }
    for (const auto* s : {"", "-wal", "-shm"}) {
        std::error_code ec;
        fs::remove(path + s, ec);
    }
}
