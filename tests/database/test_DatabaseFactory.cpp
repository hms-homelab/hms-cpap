// Unit tests for makeDatabaseFromConfig() — the single source of truth for DB
// backend selection. Regression guard for issue #8: the --backfill CLI used to
// hardcode PostgreSQL, so SQLite/local-directory users (e.g. Synology Docker)
// got a "connect to localhost:5432" failure. The factory must pick the backend
// from DB_TYPE, exactly like BurstCollectorService::initDatabase.
#include <gtest/gtest.h>
#include <cstdlib>

#include "database/DatabaseFactory.h"
#include "database/SQLiteDatabase.h"
#ifdef WITH_POSTGRESQL
#include "database/DatabaseService.h"
#endif

using namespace hms_cpap;

class DatabaseFactoryTest : public ::testing::Test {
protected:
    void clearEnv() {
        for (const char* k : {"DB_TYPE", "SQLITE_PATH", "DB_HOST", "DB_PORT",
                              "DB_NAME", "DB_USER", "DB_PASSWORD"})
            unsetenv(k);
    }
    void SetUp() override { clearEnv(); }
    void TearDown() override { clearEnv(); }
};

TEST_F(DatabaseFactoryTest, DefaultsToSqliteWhenUnset) {
    auto db = makeDatabaseFromConfig();
    ASSERT_NE(db, nullptr);
    EXPECT_NE(dynamic_cast<SQLiteDatabase*>(db.get()), nullptr)
        << "no DB_TYPE should select SQLite";
}

TEST_F(DatabaseFactoryTest, ExplicitSqlite) {
    setenv("DB_TYPE", "sqlite", 1);
    auto db = makeDatabaseFromConfig();
    EXPECT_NE(dynamic_cast<SQLiteDatabase*>(db.get()), nullptr);
}

TEST_F(DatabaseFactoryTest, UnknownTypeFallsBackToSqlite) {
    setenv("DB_TYPE", "bogus-engine", 1);
    auto db = makeDatabaseFromConfig();
    EXPECT_NE(dynamic_cast<SQLiteDatabase*>(db.get()), nullptr)
        << "unknown DB_TYPE must not crash or force Postgres — fall back to SQLite";
}

// The crux of issue #8: in SQLite mode the factory must NOT hand back a Postgres
// backend (which would then try localhost:5432).
TEST_F(DatabaseFactoryTest, SqliteIsNotPostgres) {
    setenv("DB_TYPE", "sqlite", 1);
    auto db = makeDatabaseFromConfig();
#ifdef WITH_POSTGRESQL
    EXPECT_EQ(dynamic_cast<DatabaseService*>(db.get()), nullptr)
        << "SQLite mode must never produce a PostgreSQL DatabaseService";
#endif
    EXPECT_NE(dynamic_cast<SQLiteDatabase*>(db.get()), nullptr);
}

#ifdef WITH_POSTGRESQL
TEST_F(DatabaseFactoryTest, PostgresSelectedWhenConfigured) {
    setenv("DB_TYPE", "postgresql", 1);
    auto db = makeDatabaseFromConfig();  // construction only; no connect()
    ASSERT_NE(db, nullptr);
    EXPECT_NE(dynamic_cast<DatabaseService*>(db.get()), nullptr);
    EXPECT_EQ(dynamic_cast<SQLiteDatabase*>(db.get()), nullptr);
}
#endif
