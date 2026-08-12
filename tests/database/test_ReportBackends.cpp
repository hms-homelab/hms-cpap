//
// test_ReportBackends.cpp: the cpap_reports job table, across engines.
//
// Same reason test_SyncFolderBackends.cpp and test_CleaningBackends.cpp exist,
// and this table is the sharpest example of it yet: cpap_reports was READ AND
// WRITTEN in six places and DECLARED IN NONE. Every PDF request died on a
// missing relation, in the database log, never in front of the user who asked.
// Reported by todd3835 alongside issue #21.
//
// Once it was declared, the queries were still PostgreSQL-only three ways over:
//   - `created_at::text`, which SQLite and MySQL both reject
//   - `NOW()`, which SQLite does not have
//   - `$1` placeholders and `RETURNING id`, neither of which MySQL accepts
// Each of those is exercised here on every engine, because "it compiles" says
// nothing about whether the statement parses on the backend actually in use.
//
// SQLite always runs (temp file, no server). MySQL runs when MYSQL_TEST_HOST is
// set, PostgreSQL when a server answers; both skip cleanly otherwise.
//
// SAFETY: never DROPs or TRUNCATEs. Every row is namespaced to a per-process
// device_id and deleted in TearDown, so pointing this at a populated database
// cannot disturb real data.
//
#include <gtest/gtest.h>

#include "database/IDatabase.h"
#include "database/SQLiteDatabase.h"
#include "database/SqlDialect.h"
#ifdef WITH_MYSQL
#include "database/MySQLDatabase.h"
#endif
#ifdef WITH_POSTGRESQL
#include "database/PostgresDatabase.h"
#include <pqxx/pqxx>
#endif

#include <cstdlib>
#include <filesystem>
#include <memory>
#include <string>
#include <unistd.h>

using namespace hms_cpap;

namespace {

std::string envOr(const char* key, const std::string& fallback) {
    const char* v = std::getenv(key);
    return (v && *v) ? std::string(v) : fallback;
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

#ifdef WITH_POSTGRESQL
std::string pgEnvOr(const char* k, const std::string& d) { return envOr(k, d); }
std::string pgConnInfo() {
    return "host=" + pgEnvOr("PGHOST", "localhost") +
           " port=" + pgEnvOr("PGPORT", "5432") +
           " user=" + pgEnvOr("PGUSER", "maestro") +
           " password=" + pgEnvOr("PGPASSWORD", "") +
           " dbname=" + pgEnvOr("PGDATABASE", "cpap_monitoring") +
           " connect_timeout=3";
}
#endif

class ReportBackend : public ::testing::TestWithParam<Engine> {
protected:
    std::unique_ptr<IDatabase> db_;
    std::string path_;
    std::string device_;   // per-process, so concurrent runs cannot collide

    void SetUp() override {
        device_ = "test-report-dev-" + std::to_string(::getpid());

        switch (GetParam()) {
            case Engine::SQLite: {
                path_ = (std::filesystem::temp_directory_path() /
                         ("hms_reports_" + std::to_string(::getpid()) + ".db")).string();
                std::filesystem::remove(path_);
                auto lite = std::make_unique<SQLiteDatabase>(path_);
                ASSERT_TRUE(lite->connect());
                db_ = std::move(lite);
                break;
            }
            case Engine::MySQL: {
#ifndef WITH_MYSQL
                GTEST_SKIP() << "built without MySQL (-DBUILD_WITH_MYSQL=OFF)";
#else
                const std::string host = envOr("MYSQL_TEST_HOST", "");
                if (host.empty()) GTEST_SKIP() << "MYSQL_TEST_HOST unset";
                auto my = std::make_unique<MySQLDatabase>(
                    host, std::stoi(envOr("MYSQL_TEST_PORT", "3306")),
                    envOr("MYSQL_TEST_USER", ""), envOr("MYSQL_TEST_PASSWORD", ""),
                    envOr("MYSQL_TEST_DB", "hms_cpap_test"));
                if (!my->connect()) GTEST_SKIP() << "No usable MySQL at " << host;
                db_ = std::move(my);
#endif
                break;
            }
            case Engine::Postgres: {
#ifndef WITH_POSTGRESQL
                GTEST_SKIP() << "built without PostgreSQL";
#else
                try {
                    pqxx::connection probe(pgConnInfo());
                    if (!probe.is_open()) throw std::runtime_error("not open");
                } catch (const std::exception& e) {
                    GTEST_SKIP() << "No usable PostgreSQL (" << e.what() << ")";
                }
                auto pg = std::make_unique<PostgresDatabase>(pgConnInfo());
                if (!pg->connect()) GTEST_SKIP() << "PostgresDatabase connect failed";
                db_ = std::move(pg);
#endif
                break;
            }
        }
    }

    void TearDown() override {
        if (db_ && !device_.empty()) {
            db_->executeQuery("DELETE FROM cpap_reports WHERE device_id=" +
                                  sql::param(1, db_->dbType()),
                              {device_});
        }
        db_.reset();
        if (!path_.empty()) std::filesystem::remove(path_);
    }

    IDatabase& db() { return *db_; }
    DbType dt() { return db_->dbType(); }

    // Exactly the INSERT ReportGeneratorService::triggerReport runs.
    int insertJob(const std::string& start, const std::string& end,
                  const std::string& filename) {
        return db().insertReturningId(
            "INSERT INTO cpap_reports (device_id, range_start, range_end, filename, filepath, status)"
            " VALUES (" + sql::param(1, dt()) + "," + sql::param(2, dt()) + "," +
            sql::param(3, dt()) + "," + sql::param(4, dt()) + "," + sql::param(5, dt()) +
            ",'pending')",
            {device_, start, end, filename, "/tmp/" + filename});
    }

    // Exactly the SELECT ReportGeneratorService::listReports runs.
    Json::Value listJobs() {
        return db().executeQuery(
            "SELECT id, device_id, range_start, range_end, nights_count, filename, filepath,"
            " status, error_msg, " + sql::tsText("created_at", dt()) + " AS created_at, "
            + sql::tsText("completed_at", dt()) + " AS completed_at"
            " FROM cpap_reports WHERE device_id=" + sql::param(1, dt()) +
            " ORDER BY created_at DESC LIMIT 50",
            {device_});
    }
};

} // namespace

// The table has to EXIST. This is the whole bug: six call sites, no declaration,
// so every report request failed in the log and nowhere else.
TEST_P(ReportBackend, TheReportsTableExists) {
    Json::Value rows = db().executeQuery(
        "SELECT id FROM cpap_reports WHERE device_id=" + sql::param(1, dt()),
        {device_});
    EXPECT_TRUE(rows.isArray()) << engineName(GetParam());
}

// INSERT ... RETURNING id is PostgreSQL-only; MySQL has no RETURNING and SQLite
// only gained it in 3.35. Every engine must still hand back a usable id.
TEST_P(ReportBackend, InsertReturnsANewId) {
    const int id = insertJob("2026-08-01", "2026-08-07", "r1.pdf");
    EXPECT_GT(id, 0) << engineName(GetParam());

    const int id2 = insertJob("2026-08-08", "2026-08-14", "r2.pdf");
    EXPECT_GT(id2, 0);
    EXPECT_NE(id, id2) << "each insert must get its own id";
}

// The SELECT that made reports PostgreSQL-only: created_at::text parses nowhere
// else. It must return rows, with the timestamp readable as text, on all three.
TEST_P(ReportBackend, ListReturnsRowsWithReadableTimestamps) {
    ASSERT_GT(insertJob("2026-08-01", "2026-08-07", "r1.pdf"), 0);

    Json::Value rows = listJobs();
    ASSERT_TRUE(rows.isArray()) << engineName(GetParam());
    ASSERT_EQ(rows.size(), 1u) << engineName(GetParam());

    EXPECT_EQ(rows[0]["filename"].asString(), "r1.pdf");
    EXPECT_EQ(rows[0]["status"].asString(), "pending");
    // created_at must come back as usable text, not empty and not a raw blob.
    const std::string created = rows[0]["created_at"].asString();
    EXPECT_FALSE(created.empty()) << engineName(GetParam());
    EXPECT_GE(created.size(), 10u) << "expected a date-like string, got: " << created;
}

// completed_at is set with NOW(), which SQLite does not have. The status
// transition has to work on every engine or a finished report never reports it.
TEST_P(ReportBackend, StatusTransitionToReadySetsCompletedAt) {
    const int id = insertJob("2026-08-01", "2026-08-07", "r1.pdf");
    ASSERT_GT(id, 0);

    const std::string ts = sql::now(dt());
    db().executeQuery(
        "UPDATE cpap_reports SET status=" + sql::param(1, dt()) +
        ", error_msg=" + sql::param(2, dt()) + ", completed_at=" + ts +
        " WHERE id=" + std::to_string(id),
        {"ready", ""});

    Json::Value rows = listJobs();
    ASSERT_EQ(rows.size(), 1u);
    EXPECT_EQ(rows[0]["status"].asString(), "ready") << engineName(GetParam());
    EXPECT_FALSE(rows[0]["completed_at"].asString().empty())
        << "completed_at must be set on " << engineName(GetParam());
}

// The error path writes through the same placeholders.
TEST_P(ReportBackend, ErrorStatusRoundTrips) {
    const int id = insertJob("2026-08-01", "2026-08-07", "r1.pdf");
    ASSERT_GT(id, 0);

    db().executeQuery(
        "UPDATE cpap_reports SET status='error', error_msg=" + sql::param(1, dt()) +
        " WHERE id=" + sql::param(2, dt()),
        {"gnuplot exploded", std::to_string(id)});

    Json::Value rows = listJobs();
    ASSERT_EQ(rows.size(), 1u);
    EXPECT_EQ(rows[0]["status"].asString(), "error") << engineName(GetParam());
    EXPECT_EQ(rows[0]["error_msg"].asString(), "gnuplot exploded");
}

TEST_P(ReportBackend, NightsCountUpdates) {
    const int id = insertJob("2026-08-01", "2026-08-07", "r1.pdf");
    ASSERT_GT(id, 0);

    db().executeQuery("UPDATE cpap_reports SET nights_count=" + sql::param(1, dt()) +
                      " WHERE id=" + sql::param(2, dt()),
                      {"7", std::to_string(id)});

    Json::Value rows = listJobs();
    ASSERT_EQ(rows.size(), 1u);
    EXPECT_EQ(rows[0]["nights_count"].asString(), "7") << engineName(GetParam());
}

// Reports are listed per device; another device's jobs must not leak in.
TEST_P(ReportBackend, ListingIsScopedToTheDevice) {
    ASSERT_GT(insertJob("2026-08-01", "2026-08-07", "mine.pdf"), 0);
    db().insertReturningId(
        "INSERT INTO cpap_reports (device_id, range_start, range_end, filename, filepath, status)"
        " VALUES (" + sql::param(1, dt()) + "," + sql::param(2, dt()) + "," +
        sql::param(3, dt()) + "," + sql::param(4, dt()) + "," + sql::param(5, dt()) +
        ",'pending')",
        {device_ + "-other", "2026-08-01", "2026-08-07", "theirs.pdf", "/tmp/theirs.pdf"});

    Json::Value rows = listJobs();
    ASSERT_EQ(rows.size(), 1u) << engineName(GetParam());
    EXPECT_EQ(rows[0]["filename"].asString(), "mine.pdf");

    db().executeQuery("DELETE FROM cpap_reports WHERE device_id=" + sql::param(1, dt()),
                      {device_ + "-other"});
}

INSTANTIATE_TEST_SUITE_P(
    Engines, ReportBackend,
    ::testing::Values(Engine::SQLite, Engine::MySQL, Engine::Postgres),
    [](const ::testing::TestParamInfo<Engine>& info) {
        return std::string(engineName(info.param));
    });
