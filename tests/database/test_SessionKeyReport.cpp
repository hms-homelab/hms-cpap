// SDD-034: the session key on an old database.
//
// Every backend declares UNIQUE (device_id, session_start) on cpap_sessions,
// and saveSession's upsert needs it: without it a growing night is INSERTED
// again every burst instead of updated, so the night doubles. A table created
// before the declaration has no such key and nothing adds one.
//
// This release only looks. These cases pin that it looks correctly and that
// looking writes nothing: the repair is the next release's, and it deletes
// rows, so the report is what decides whether anyone needs it.
#include <gtest/gtest.h>

#include "database/IDatabase.h"
#include "database/SQLiteDatabase.h"
#include "database/SqlDialect.h"
#ifdef WITH_MYSQL
#include "database/MySQLDatabase.h"
#endif
#ifdef WITH_POSTGRESQL
#include "database/PostgresDatabase.h"
#endif

#include <cstdlib>
#include <filesystem>
#include <memory>
#include <string>
#include <unistd.h>

using namespace hms_cpap;
namespace fs = std::filesystem;

namespace {

std::string envOr(const char* key, const std::string& fallback) {
    const char* v = std::getenv(key);
    return (v && *v) ? std::string(v) : fallback;
}

/// A database whose cpap_sessions was made the old way: no unique key over
/// (device_id, session_start). The backend's connect() leaves an existing
/// table alone (CREATE TABLE IF NOT EXISTS), which is exactly how a real
/// install carries the gap forward.
class LegacySessionsTable {
public:
    explicit LegacySessionsTable(const std::string& path) : path_(path) {
        sqlite3* raw = nullptr;
        sqlite3_open(path.c_str(), &raw);
        const char* legacy = R"(
            CREATE TABLE cpap_sessions (
                id                INTEGER PRIMARY KEY AUTOINCREMENT,
                device_id         TEXT NOT NULL,
                session_start     TEXT NOT NULL,
                session_end       TEXT,
                duration_seconds  INTEGER DEFAULT 0,
                data_records      INTEGER DEFAULT 0,
                brp_file_path     TEXT,
                eve_file_path     TEXT,
                sad_file_path     TEXT,
                pld_file_path     TEXT,
                csl_file_path     TEXT,
                created_at        TEXT DEFAULT (datetime('now')),
                updated_at        TEXT DEFAULT (datetime('now'))
            )
        )";
        sqlite3_exec(raw, legacy, nullptr, nullptr, nullptr);
        // One night stored twice, as the missing key allows, plus one stored
        // once so the count is not simply "every row".
        sqlite3_exec(raw,
                     "INSERT INTO cpap_sessions (device_id, session_start, duration_seconds)"
                     " VALUES ('dev', '2099-07-01 23:00:00', 2820),"
                     "        ('dev', '2099-07-01 23:00:00', 4260),"
                     "        ('dev', '2099-07-02 23:00:00', 3600)",
                     nullptr, nullptr, nullptr);
        sqlite3_close(raw);
    }
    ~LegacySessionsTable() {
        for (const auto* suffix : {"", "-wal", "-shm"}) {
            std::error_code ec;
            fs::remove(path_ + suffix, ec);
        }
    }

private:
    std::string path_;
};

std::string tempDbPath(const char* tag) {
    auto p = (fs::temp_directory_path() /
              (std::string("hms_cpap_") + tag + "_" + std::to_string(::getpid()) + ".db")).string();
    std::error_code ec;
    fs::remove(p, ec);
    return p;
}

}  // namespace

TEST(SessionKeyReport, SqliteSeesTheMissingKeyAndCountsTheDuplicates) {
    const auto path = tempDbPath("legacy_key");
    LegacySessionsTable legacy(path);

    SQLiteDatabase db(path);
    ASSERT_TRUE(db.connect());

    const auto report = db.inspectSessionKey();
    EXPECT_FALSE(report.key_present);
    EXPECT_EQ(report.duplicate_groups, 1);
    EXPECT_EQ(report.duplicate_rows, 2) << "the night stored twice, not the one stored once";

    // Looking changes nothing: the repair is the next release's.
    auto rows = db.executeQuery("SELECT COUNT(*) AS n FROM cpap_sessions", {});
    ASSERT_TRUE(rows.isArray() && !rows.empty());
    EXPECT_EQ(rows[0u]["n"].isString() ? std::atoi(rows[0u]["n"].asCString())
                                       : rows[0u]["n"].asInt(),
              3);
}

TEST(SessionKeyReport, SqliteSaysNothingWhenTheKeyIsThere) {
    const auto path = tempDbPath("current_key");
    SQLiteDatabase db(path);
    ASSERT_TRUE(db.connect());   // creates the table this build declares

    const auto report = db.inspectSessionKey();
    EXPECT_TRUE(report.key_present);
    EXPECT_EQ(report.duplicate_groups, 0);
    EXPECT_EQ(report.duplicate_rows, 0);

    for (const auto* suffix : {"", "-wal", "-shm"}) {
        std::error_code ec;
        fs::remove(path + suffix, ec);
    }
}

// The engines that answer from their own catalogue. Both report "present"
// against a database this build made, which is the case every install has
// after the table was declared with the key.
#ifdef WITH_MYSQL
TEST(SessionKeyReport, MySqlReportsThePresentKey) {
    const std::string host = envOr("MYSQL_TEST_HOST", "");
    if (host.empty()) GTEST_SKIP() << "MYSQL_TEST_HOST unset, skipping MySQL.";
    MySQLDatabase db(host, std::stoi(envOr("MYSQL_TEST_PORT", "3306")),
                     envOr("MYSQL_TEST_USER", ""), envOr("MYSQL_TEST_PASSWORD", ""),
                     envOr("MYSQL_TEST_DB", "hms_cpap_test"));
    if (!db.connect()) GTEST_SKIP() << "No usable MySQL at " << host;

    const auto report = db.inspectSessionKey();
    EXPECT_TRUE(report.key_present)
        << "this database has the key; a report of 'missing' here means the "
           "catalogue query is wrong, not that the install is old";
    EXPECT_EQ(report.duplicate_groups, 0);
}
#endif

#ifdef WITH_POSTGRESQL
TEST(SessionKeyReport, PostgresReportsThePresentKey) {
    const std::string host = envOr("PGHOST", "");
    if (host.empty()) GTEST_SKIP() << "PGHOST unset, skipping PostgreSQL.";
    const std::string conn =
        "host=" + host + " port=" + envOr("PGPORT", "5432") +
        " user=" + envOr("PGUSER", "maestro") +
        " password=" + envOr("PGPASSWORD", "") +
        " dbname=" + envOr("PGDATABASE", "cpap_monitoring") + " connect_timeout=3";
    PostgresDatabase db(conn);
    if (!db.connect()) GTEST_SKIP() << "No usable PostgreSQL at " << host;

    const auto report = db.inspectSessionKey();
    EXPECT_TRUE(report.key_present);
    EXPECT_EQ(report.duplicate_groups, 0);
}
#endif
