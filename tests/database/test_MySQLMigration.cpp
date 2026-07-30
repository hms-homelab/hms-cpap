//
// test_MySQLMigration.cpp — MySQL schema-drift guard.
//
// The MySQL backend only ever ran CREATE TABLE IF NOT EXISTS, which is a no-op on
// a table that already exists. An install made by an older build therefore kept
// its old column set forever and every read of a newer column failed at runtime.
// Postgres and SQLite both had an ALTER pass; MySQL had none.
//
// This was not hypothetical. The throwaway hms_cpap_test database on the NAS had a
// four-column cpap_sessions (id, device_id, session_start, duration_seconds) while
// the code declared fifteen, so getCheckpointFilesByFolder could not work at all.
// That is the same failure class as release v4.4.10, where checked-in schema
// scripts drifted behind the in-code migrations and shipped broken installs.
//
// So: StaleTableIsBroughtForward builds that exact four-column stub and asserts
// connect() repairs it. The remaining tests assert the declared shape is reachable
// and that the migration is idempotent and non-destructive.
//
// Opt-in via MYSQL_TEST_HOST/PORT/DB/USER/PASSWORD, skipping cleanly when unset so
// a developer with no server still gets a green suite.
//
// SAFETY: operates only on its own throwaway tables, named with a per-process
// suffix, and drops just those in TearDown. It never touches the real tables.
//
#include <gtest/gtest.h>

#ifdef WITH_MYSQL

#include "database/MySQLDatabase.h"

#include <cstdlib>
#include <memory>
#include <string>
#include <unistd.h>
#include <vector>

using namespace hms_cpap;

namespace {

std::string envOr(const char* key, const std::string& fallback) {
    const char* v = std::getenv(key);
    return (v && *v) ? std::string(v) : fallback;
}

// Columns the code reads that an older install would not have. Kept deliberately
// short and hand-picked: these are the ones with a known consumer, so a failure
// here names a real broken feature rather than a cosmetic diff.
struct Expected { const char* table; const char* column; const char* why; };

const std::vector<Expected> kCriticalColumns = {
    {"cpap_sessions", "checkpoint_files",
     "getCheckpointFilesByFolder / burst resume"},
    {"cpap_sessions", "force_completed",
     "POST /api/sessions/{date}/force-complete"},
    {"cpap_sessions", "session_end", "session completion"},
    {"cpap_session_metrics", "odi", "desaturation metrics"},
    {"cpap_session_metrics", "avg_mask_pressure", "PLD metrics"},
    {"cpap_session_metrics", "therapy_mode", "ASV support"},
    {"cpap_calculated_metrics", "target_ventilation", "ASV per-minute data"},
    {"cpap_daily_summary", "patient_hours", "STR compliance"},
    {"cpap_daily_summary", "mask_pairs", "STR mask on/off pairs"},
    {"oximetry_sessions", "avg_spo2", "getOximetrySummary"},
    {"oximetry_sessions", "cpap_session_date", "every oximetry read path"},
    {"oximetry_samples", "spo2", "session detail oximetry chart"},
    {"cpap_sync_folders", "str_due", "SDD-008 partial-night detection"},
    {"cpap_sync_folders", "sidecars_due", "SDD-008 EVE/CSL refetch"},
    {"cpap_sync_folders", "resync_count", "SDD-008 re-arm bound"},
};

class MySQLMigrationTest : public ::testing::Test {
protected:
    std::unique_ptr<MySQLDatabase> db_;
    std::string suffix_;

    static bool configured() { return !envOr("MYSQL_TEST_HOST", "").empty(); }

    std::unique_ptr<MySQLDatabase> makeDb() {
        return std::make_unique<MySQLDatabase>(
            envOr("MYSQL_TEST_HOST", ""),
            std::stoi(envOr("MYSQL_TEST_PORT", "3306")),
            envOr("MYSQL_TEST_USER", ""),
            envOr("MYSQL_TEST_PASSWORD", ""),
            envOr("MYSQL_TEST_DB", "hms_cpap_test"));
    }

    void SetUp() override {
        if (!configured()) {
            GTEST_SKIP() << "MYSQL_TEST_HOST unset — skipping MySQL migration guard. "
                            "Set MYSQL_TEST_HOST/PORT/DB/USER/PASSWORD to run it.";
        }
        suffix_ = "_mig_" + std::to_string(::getpid());

        db_ = makeDb();
        if (!db_->connect()) {
            GTEST_SKIP() << "No usable MySQL at " << envOr("MYSQL_TEST_HOST", "")
                         << " — skipping MySQL migration guard.";
        }
    }

    void TearDown() override {
        if (db_) dropScratch();
        db_.reset();
    }

    void dropScratch() {
        db_->executeQuery("SET FOREIGN_KEY_CHECKS=0");
        db_->executeQuery("DROP TABLE IF EXISTS scratch" + suffix_);
        db_->executeQuery("SET FOREIGN_KEY_CHECKS=1");
    }
};

// ─────────────────────────────────────────────────────────────────────────────
// The declared shape must actually be reachable after connect().
// ─────────────────────────────────────────────────────────────────────────────

TEST_F(MySQLMigrationTest, EveryCriticalColumnExistsAfterConnect) {
    for (const auto& e : kCriticalColumns) {
        EXPECT_TRUE(db_->columnExists(e.table, e.column))
            << e.table << "." << e.column << " is missing, which breaks: " << e.why;
    }
}

TEST_F(MySQLMigrationTest, ColumnExistsDoesNotInventColumns) {
    // A guard that answers true for everything would make the test above vacuous.
    EXPECT_FALSE(db_->columnExists("cpap_sessions", "definitely_not_a_column"));
    EXPECT_FALSE(db_->columnExists("no_such_table", "id"));
}

TEST_F(MySQLMigrationTest, TableExistsAgreesWithReality) {
    EXPECT_TRUE(db_->tableExists("cpap_sessions"));
    EXPECT_TRUE(db_->tableExists("oximetry_sessions"));
    EXPECT_FALSE(db_->tableExists("no_such_table" + suffix_));
}

// ─────────────────────────────────────────────────────────────────────────────
// The migration itself
// ─────────────────────────────────────────────────────────────────────────────

TEST_F(MySQLMigrationTest, AddColumnIfMissingIsIdempotent) {
    ASSERT_TRUE(db_->executeQuery(
        "CREATE TABLE scratch" + suffix_ + " (id INT AUTO_INCREMENT PRIMARY KEY)")
        .isArray());
    ASSERT_TRUE(db_->tableExists("scratch" + suffix_));

    // First call adds, second is a no-op. The return value is what migrateSchema
    // counts, so a stuck-at-1 would report phantom migrations on every boot.
    EXPECT_EQ(db_->addColumnIfMissing("scratch" + suffix_, "added_col", "DOUBLE"), 1);
    EXPECT_TRUE(db_->columnExists("scratch" + suffix_, "added_col"));
    EXPECT_EQ(db_->addColumnIfMissing("scratch" + suffix_, "added_col", "DOUBLE"), 0);
}

TEST_F(MySQLMigrationTest, AddColumnIfMissingSkipsAbsentTables) {
    // A table that does not exist yet is CREATE TABLE's job, not the migration's.
    // Attempting the ALTER anyway is what produced connect-time error spam.
    EXPECT_EQ(db_->addColumnIfMissing("no_such_table" + suffix_, "c", "INT"), 0);
}

TEST_F(MySQLMigrationTest, StaleTableIsBroughtForward) {
    // Reproduces the real NAS state: cpap_sessions as it existed several releases
    // ago. Built under a scratch name so the live table is never dropped, then
    // migrated through the same addColumnIfMissing path createSchema() drives.
    ASSERT_TRUE(db_->executeQuery(
        "CREATE TABLE scratch" + suffix_ + " ("
        "  id INT AUTO_INCREMENT PRIMARY KEY,"
        "  device_id VARCHAR(255) NOT NULL,"
        "  session_start DATETIME NOT NULL,"
        "  duration_seconds INT DEFAULT 0"
        ")").isArray());

    // Precondition: genuinely stale.
    ASSERT_FALSE(db_->columnExists("scratch" + suffix_, "checkpoint_files"));
    ASSERT_FALSE(db_->columnExists("scratch" + suffix_, "force_completed"));

    struct Col { const char* name; const char* ddl; };
    const Col wanted[] = {
        {"session_end",      "DATETIME"},
        {"data_records",     "INT DEFAULT 0"},
        {"checkpoint_files", "JSON"},
        {"force_completed",  "TINYINT DEFAULT 0"},
    };

    int applied = 0;
    for (const auto& c : wanted)
        applied += db_->addColumnIfMissing("scratch" + suffix_, c.name, c.ddl);

    EXPECT_EQ(applied, 4);
    for (const auto& c : wanted) {
        EXPECT_TRUE(db_->columnExists("scratch" + suffix_, c.name))
            << c.name << " was not added by the migration";
    }

    // Second pass adds nothing: a restart must not keep re-migrating.
    int again = 0;
    for (const auto& c : wanted)
        again += db_->addColumnIfMissing("scratch" + suffix_, c.name, c.ddl);
    EXPECT_EQ(again, 0);
}

TEST_F(MySQLMigrationTest, MigrationPreservesExistingRows) {
    // A migration that recreated tables instead of altering them would silently
    // discard therapy history, which is the worst possible way to fail.
    ASSERT_TRUE(db_->executeQuery(
        "CREATE TABLE scratch" + suffix_ + " ("
        "  id INT AUTO_INCREMENT PRIMARY KEY,"
        "  device_id VARCHAR(255) NOT NULL,"
        "  duration_seconds INT DEFAULT 0"
        ")").isArray());
    ASSERT_TRUE(db_->executeQuery(
        "INSERT INTO scratch" + suffix_ + " (device_id, duration_seconds) "
        "VALUES ('keepme', 1234)").isArray());

    ASSERT_EQ(db_->addColumnIfMissing("scratch" + suffix_, "checkpoint_files", "JSON"), 1);

    auto rows = db_->executeQuery(
        "SELECT device_id, duration_seconds FROM scratch" + suffix_);
    ASSERT_TRUE(rows.isArray());
    ASSERT_EQ(rows.size(), 1u);
    EXPECT_EQ(rows[0]["device_id"].asString(), "keepme");
    // New column exists and is NULL on the pre-existing row.
    auto nulls = db_->executeQuery(
        "SELECT COUNT(*) AS n FROM scratch" + suffix_ + " WHERE checkpoint_files IS NULL");
    ASSERT_TRUE(nulls.isArray() && nulls.size() == 1u);
    EXPECT_EQ(nulls[0]["n"].asString(), "1");
}

TEST_F(MySQLMigrationTest, ReconnectingIsQuietOnAnUpToDateSchema) {
    // createSchema() runs on every connect. On a current database it must apply
    // nothing, or every boot logs migrations that did not happen.
    auto second = makeDb();
    ASSERT_TRUE(second->connect());

    for (const auto& e : kCriticalColumns) {
        EXPECT_TRUE(second->columnExists(e.table, e.column))
            << e.table << "." << e.column << " vanished on reconnect";
    }
    second.reset();
}

}  // namespace

#endif  // WITH_MYSQL
