// SDD-034: the session key on an old database.
//
// Every backend declares UNIQUE (device_id, session_start) on cpap_sessions,
// and saveSession's upsert needs it: without it a growing night is INSERTED
// again every burst instead of updated, so the night doubles. A table created
// before the declaration has no such key and nothing adds one.
//
// 5.2.13 only looked. These cases pin both halves: that the report counts what
// is there, and that the repair keeps the longest copy of each night, takes the
// shorter copies' child rows with them, and leaves the key behind. The repair
// deletes rows and runs unattended at startup, so what it keeps is the part
// worth holding still.
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

/// One integer out of a query, whatever type the engine chose to hand back.
int oneInt(IDatabase& db, const std::string& sql) {
    auto rows = db.executeQuery(sql, {});
    if (!rows.isArray() || rows.empty()) return -1;
    const auto& v = *rows[0u].begin();
    if (v.isNull()) return 0;
    return v.isString() ? std::atoi(v.asCString()) : v.asInt();
}

/// A database whose cpap_sessions was made the old way: no unique key over
/// (device_id, session_start). The backend's connect() leaves an existing
/// table alone (CREATE TABLE IF NOT EXISTS), which is exactly how a real
/// install carries the gap forward.
class LegacySessionsTable {
public:
    /// Default rows: one night stored twice, as the missing key allows, plus
    /// one stored once so a count is not simply "every row".
    static constexpr const char* kDefaultRows =
        "('dev', '2099-07-01 23:00:00', 2820),"
        "('dev', '2099-07-01 23:00:00', 4260),"
        "('dev', '2099-07-02 23:00:00', 3600)";

    explicit LegacySessionsTable(const std::string& path,
                                 const std::string& rows = kDefaultRows)
        : path_(path) {
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
        const std::string insert =
            "INSERT INTO cpap_sessions (device_id, session_start, duration_seconds)"
            " VALUES " + rows;
        sqlite3_exec(raw, insert.c_str(), nullptr, nullptr, nullptr);
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

    // Looking changes nothing. The repair is a separate call, and startup only
    // makes it after this one has said there is something to repair.
    EXPECT_EQ(oneInt(db, "SELECT COUNT(*) AS n FROM cpap_sessions"), 3);
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

// --- the repair ------------------------------------------------------------

TEST(SessionKeyRepair, SqliteKeepsTheLongestCopyAndAddsTheKey) {
    const auto path = tempDbPath("repair_key");
    LegacySessionsTable legacy(path);

    SQLiteDatabase db(path);
    ASSERT_TRUE(db.connect());

    // The child rows the losing copy owns. They have to go with it: left
    // behind they point at a session id that no longer exists, and every query
    // that joins them would count the night's events twice.
    const int loser = oneInt(db, "SELECT id FROM cpap_sessions WHERE duration_seconds = 2820");
    const int winner = oneInt(db, "SELECT id FROM cpap_sessions WHERE duration_seconds = 4260");
    const int other = oneInt(db, "SELECT id FROM cpap_sessions WHERE duration_seconds = 3600");
    ASSERT_GT(loser, 0);
    ASSERT_GT(winner, 0);
    for (int id : {loser, winner, other}) {
        db.executeQuery("INSERT INTO cpap_events (session_id, event_type, event_timestamp) "
                        "VALUES (" + std::to_string(id) + ", 'Apnea', '2099-07-01 23:30:00')",
                        {});
        db.executeQuery("INSERT INTO cpap_session_files (session_id, kind, rel_path) "
                        "VALUES (" + std::to_string(id) + ", 'brp', 'f" + std::to_string(id) +
                        ".edf')",
                        {});
    }

    const auto fixed = db.repairSessionKey();
    EXPECT_TRUE(fixed.ok);
    EXPECT_TRUE(fixed.key_added);
    EXPECT_EQ(fixed.groups, 1);
    EXPECT_EQ(fixed.rows_deleted, 1) << "one copy of one night, and only that";

    EXPECT_TRUE(db.inspectSessionKey().key_present);
    EXPECT_EQ(oneInt(db, "SELECT COUNT(*) AS n FROM cpap_sessions"), 2);
    EXPECT_EQ(oneInt(db, "SELECT duration_seconds AS d FROM cpap_sessions "
                         "WHERE session_start = '2099-07-01 23:00:00'"),
              4260)
        << "the longest copy is the most complete one: a session row only grows";

    // The loser's children left with it; the other two kept theirs.
    EXPECT_EQ(oneInt(db, "SELECT COUNT(*) AS n FROM cpap_events WHERE session_id = " +
                             std::to_string(loser)),
              0);
    EXPECT_EQ(oneInt(db, "SELECT COUNT(*) AS n FROM cpap_session_files WHERE session_id = " +
                             std::to_string(loser)),
              0);
    EXPECT_EQ(oneInt(db, "SELECT COUNT(*) AS n FROM cpap_events"), 2);
    EXPECT_EQ(oneInt(db, "SELECT COUNT(*) AS n FROM cpap_session_files"), 2);

    // Startup calls this on every boot. The second one must be free.
    const auto again = db.repairSessionKey();
    EXPECT_TRUE(again.ok);
    EXPECT_TRUE(again.key_added);
    EXPECT_EQ(again.rows_deleted, 0);
    EXPECT_EQ(oneInt(db, "SELECT COUNT(*) AS n FROM cpap_sessions"), 2);
}

TEST(SessionKeyRepair, SqliteBreaksATieOnTheNewerRow) {
    const auto path = tempDbPath("repair_tie");
    // Same night, same duration, three times: nothing separates them but the
    // order they were written in, so the last one written is the one kept.
    LegacySessionsTable legacy(path,
                               "('dev', '2099-08-01 23:00:00', 3600),"
                               "('dev', '2099-08-01 23:00:00', 3600),"
                               "('dev', '2099-08-01 23:00:00', 3600)");

    SQLiteDatabase db(path);
    ASSERT_TRUE(db.connect());
    const int newest = oneInt(db, "SELECT MAX(id) AS n FROM cpap_sessions");

    const auto fixed = db.repairSessionKey();
    EXPECT_TRUE(fixed.ok);
    EXPECT_TRUE(fixed.key_added);
    EXPECT_EQ(fixed.rows_deleted, 2);
    EXPECT_EQ(oneInt(db, "SELECT COUNT(*) AS n FROM cpap_sessions"), 1);
    EXPECT_EQ(oneInt(db, "SELECT id AS n FROM cpap_sessions"), newest);
}

TEST(SessionKeyRepair, SqliteTouchesNothingWhenTheKeyIsAlreadyThere) {
    const auto path = tempDbPath("repair_noop");
    SQLiteDatabase db(path);
    ASSERT_TRUE(db.connect());   // the table this build declares, key included
    db.executeQuery("INSERT INTO cpap_sessions (device_id, session_start, duration_seconds) "
                    "VALUES ('dev', '2099-09-01 23:00:00', 3600)",
                    {});

    const auto fixed = db.repairSessionKey();
    EXPECT_TRUE(fixed.ok);
    EXPECT_TRUE(fixed.key_added);
    EXPECT_EQ(fixed.groups, 0);
    EXPECT_EQ(fixed.rows_deleted, 0);
    EXPECT_EQ(oneInt(db, "SELECT COUNT(*) AS n FROM cpap_sessions"), 1);

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

// MySQL is the engine the repair can go wrong on twice over: it refuses a
// DELETE whose subquery reads the table being deleted from (error 1093), and
// ALTER TABLE commits by itself, so the deletes and the key cannot share one
// transaction. Both are only visible against a real server.
TEST(SessionKeyRepair, MySqlCollapsesDuplicatesAndAddsTheKey) {
    const std::string host = envOr("MYSQL_TEST_HOST", "");
    if (host.empty()) GTEST_SKIP() << "MYSQL_TEST_HOST unset, skipping MySQL.";
    MySQLDatabase db(host, std::stoi(envOr("MYSQL_TEST_PORT", "3306")),
                     envOr("MYSQL_TEST_USER", ""), envOr("MYSQL_TEST_PASSWORD", ""),
                     envOr("MYSQL_TEST_DB", "hms_cpap_test"));
    if (!db.connect()) GTEST_SKIP() << "No usable MySQL at " << host;

    const std::string dev = "sdd034_mysql_" + std::to_string(::getpid());
    // Put the key back only if the repair did not, so a failed run leaves this
    // shared database the way the next suite expects it.
    auto cleanup = [&]() {
        db.executeQuery("DELETE FROM cpap_sessions WHERE device_id = '" + dev + "'", {});
        if (!db.inspectSessionKey().key_present) {
            db.executeQuery("ALTER TABLE cpap_sessions "
                            "ADD UNIQUE KEY uq_device_session (device_id, session_start)",
                            {});
        }
    };

    // Put the table back the way an old install has it, then the duplicates the
    // missing key allows.
    db.executeQuery("DELETE FROM cpap_sessions WHERE device_id = '" + dev + "'", {});
    db.executeQuery("ALTER TABLE cpap_sessions DROP INDEX uq_device_session", {});
    ASSERT_FALSE(db.inspectSessionKey().key_present)
        << "could not drop the key, so there is nothing to repair; not testing a no-op";
    db.executeQuery("INSERT INTO cpap_sessions (device_id, session_start, duration_seconds) "
                    "VALUES ('" + dev + "', '2099-07-01 23:00:00', 2820), "
                    "       ('" + dev + "', '2099-07-01 23:00:00', 4260), "
                    "       ('" + dev + "', '2099-07-02 23:00:00', 3600)",
                    {});

    const auto fixed = db.repairSessionKey();
    EXPECT_TRUE(fixed.ok);
    EXPECT_TRUE(fixed.key_added);
    EXPECT_TRUE(db.inspectSessionKey().key_present);
    EXPECT_EQ(oneInt(db, "SELECT COUNT(*) AS n FROM cpap_sessions WHERE device_id = '" + dev + "'"),
              2);
    EXPECT_EQ(oneInt(db, "SELECT duration_seconds AS d FROM cpap_sessions WHERE device_id = '" +
                             dev + "' AND session_start = '2099-07-01 23:00:00'"),
              4260);

    cleanup();
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

TEST(SessionKeyRepair, PostgresCollapsesDuplicatesAndAddsTheKey) {
    const std::string host = envOr("PGHOST", "");
    if (host.empty()) GTEST_SKIP() << "PGHOST unset, skipping PostgreSQL.";
    const std::string conn =
        "host=" + host + " port=" + envOr("PGPORT", "5432") +
        " user=" + envOr("PGUSER", "maestro") +
        " password=" + envOr("PGPASSWORD", "") +
        " dbname=" + envOr("PGDATABASE", "cpap_monitoring") + " connect_timeout=3";
    PostgresDatabase db(conn);
    if (!db.connect()) GTEST_SKIP() << "No usable PostgreSQL at " << host;

    const std::string dev = "sdd034_pg_" + std::to_string(::getpid());
    db.executeQuery("DELETE FROM cpap_sessions WHERE device_id = '" + dev + "'", {});
    // The declared key is a table constraint; the repair adds a plain unique
    // index. Both satisfy the ON CONFLICT saveSession does, and both are what
    // inspectSessionKey looks for, so either may be the one in the way.
    db.executeQuery("ALTER TABLE cpap_sessions DROP CONSTRAINT IF EXISTS "
                    "cpap_sessions_device_id_session_start_key",
                    {});
    db.executeQuery("DROP INDEX IF EXISTS uq_device_session", {});
    ASSERT_FALSE(db.inspectSessionKey().key_present)
        << "could not drop the key, so there is nothing to repair; not testing a no-op";
    db.executeQuery("INSERT INTO cpap_sessions (device_id, session_start, duration_seconds) "
                    "VALUES ('" + dev + "', '2099-07-01 23:00:00', 2820), "
                    "       ('" + dev + "', '2099-07-01 23:00:00', 4260), "
                    "       ('" + dev + "', '2099-07-02 23:00:00', 3600)",
                    {});

    const auto fixed = db.repairSessionKey();
    EXPECT_TRUE(fixed.ok);
    EXPECT_TRUE(fixed.key_added);
    EXPECT_TRUE(db.inspectSessionKey().key_present);
    EXPECT_EQ(oneInt(db, "SELECT COUNT(*) AS n FROM cpap_sessions WHERE device_id = '" + dev + "'"),
              2);
    EXPECT_EQ(oneInt(db, "SELECT duration_seconds AS d FROM cpap_sessions WHERE device_id = '" +
                             dev + "' AND session_start = '2099-07-01 23:00:00'"),
              4260);

    db.executeQuery("DELETE FROM cpap_sessions WHERE device_id = '" + dev + "'", {});
}
#endif
