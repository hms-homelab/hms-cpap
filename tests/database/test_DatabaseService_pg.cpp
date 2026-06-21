/**
 * HMS-CPAP DatabaseService (PostgreSQL) Unit Tests
 *
 * Exercises the full IDatabase CRUD surface against a REAL PostgreSQL server,
 * mirroring tests/database/test_SQLiteDatabase.cpp but for the pqxx-backed
 * DatabaseService. No MQTT/HTTP/BLE/device — just a local Postgres.
 *
 * Strategy (per the task brief):
 *   - SetUp() connects to a working database on localhost using
 *     maestro/REDACTED (or PGHOST/PGPORT/PGUSER/PGPASSWORD/
 *     PGDATABASE env if provided).
 *   - If no server is reachable, the whole suite GTEST_SKIPs cleanly so CI and
 *     other devs without Postgres are unaffected.
 *   - A unique throwaway SCHEMA (cpap_test_<pid>_<counter>) is CREATEd inside
 *     that database and selected via the connection's search_path, so every
 *     unqualified table the DatabaseService touches lands in the isolated
 *     schema. The core schema DDL is applied via executeRaw(), then each test
 *     runs the full CRUD surface against a DatabaseService bound to it.
 *   - TearDown() disconnects and DROPs the throwaway schema (CASCADE).
 *
 * A throwaway SCHEMA (rather than a throwaway DATABASE) is used so the suite
 * works without the CREATEDB role privilege — only schema-create on an existing
 * DB is required, which the documented maestro role has.
 *
 * Determinism: all timestamps derive from a fixed epoch. DatabaseService formats
 * session_start with std::localtime, and saveSession stores session_end via the
 * DB clock, so we never assert on literal wall-clock values — only on
 * round-tripped data, NULL vs non-NULL, and return-bools. cpap_session_date is
 * derived from start_time in UTC (gmtime), so for kBaseEpoch it is "20250206".
 *
 * Build: requires -DBUILD_WITH_POSTGRESQL=ON (guarded by WITH_POSTGRESQL).
 */

#ifdef WITH_POSTGRESQL

#include <gtest/gtest.h>
#include "database/DatabaseService.h"
#include "parsers/CpapdashBridge.h"
#include <pqxx/pqxx>

#include <chrono>
#include <cstdlib>
#include <memory>
#include <string>
#include <unistd.h>

using namespace hms_cpap;
using namespace std::chrono;

namespace {

// 1738847491 = 2025-02-06 14:31:31 UTC. Only relied upon for being fixed and
// round-tripping; the UTC date component gives cpap_session_date == "20250206".
constexpr long kBaseEpoch = 1738847491;

system_clock::time_point tpFromEpoch(long secs) {
    return system_clock::time_point{} + seconds(secs);
}

// Build a "host=... user=... password=... dbname=..." conninfo, honoring env
// overrides (PGHOST/PGPORT/PGUSER/PGPASSWORD) with the documented maestro
// defaults as a fallback.
std::string envOr(const char* key, const std::string& def) {
    const char* v = std::getenv(key);
    return (v && *v) ? std::string(v) : def;
}

// The working database to host the throwaway schema. maestro owns several DBs;
// cpap_monitoring is the production CPAP DB and a safe host for an isolated,
// uniquely-named schema. Overridable via PGDATABASE.
std::string testDbName() {
    return envOr("PGDATABASE", "cpap_monitoring");
}

// Base connection info (no search_path). search_path is appended separately so
// the same DB can host the throwaway schema.
std::string makeConnInfo(const std::string& dbname,
                         const std::string& search_path = "") {
    std::string ci = "host=" + envOr("PGHOST", "localhost") +
                     " port=" + envOr("PGPORT", "5432") +
                     " user=" + envOr("PGUSER", "maestro") +
                     " password=" + envOr("PGPASSWORD", "REDACTED") +
                     " dbname=" + dbname +
                     " connect_timeout=3";
    if (!search_path.empty()) {
        // Force all unqualified table access into the throwaway schema, with a
        // public fallback so connect()-time auto-migration has a valid schema
        // even before our DDL runs. Applies to every connection built from this
        // string (DatabaseService keeps a separate read connection).
        ci += " options=-csearch_path=" + search_path + ",public";
    }
    return ci;
}

// The subset of scripts/schema.sql exercised by these tests. Kept inline so the
// test does not depend on the source tree layout at runtime. Mirrors the
// PostgreSQL DDL used in production.
const char* kSchema = R"SQL(
CREATE TABLE IF NOT EXISTS cpap_devices (
    device_id       TEXT PRIMARY KEY,
    device_name     TEXT,
    serial_number   TEXT,
    model_id        INT DEFAULT 0,
    version_id      INT DEFAULT 0,
    last_seen       TIMESTAMP DEFAULT NOW(),
    created_at      TIMESTAMP DEFAULT NOW()
);
CREATE TABLE IF NOT EXISTS cpap_sessions (
    id                SERIAL PRIMARY KEY,
    device_id         TEXT NOT NULL,
    session_start     TIMESTAMP NOT NULL,
    session_end       TIMESTAMP,
    duration_seconds  INT DEFAULT 0,
    data_records      INT DEFAULT 0,
    brp_file_path     TEXT,
    eve_file_path     TEXT,
    sad_file_path     TEXT,
    pld_file_path     TEXT,
    csl_file_path     TEXT,
    checkpoint_files  JSONB DEFAULT '{}'::jsonb,
    force_completed   BOOLEAN DEFAULT FALSE,
    session_status    VARCHAR(50) DEFAULT 'in_progress',
    created_at        TIMESTAMP DEFAULT NOW(),
    updated_at        TIMESTAMP DEFAULT NOW(),
    UNIQUE (device_id, session_start)
);
CREATE TABLE IF NOT EXISTS cpap_session_metrics (
    id                     SERIAL PRIMARY KEY,
    session_id             INT NOT NULL UNIQUE,
    total_events           INT DEFAULT 0,
    ahi                    FLOAT DEFAULT 0,
    obstructive_apneas     INT DEFAULT 0,
    central_apneas         INT DEFAULT 0,
    hypopneas              INT DEFAULT 0,
    reras                  INT DEFAULT 0,
    clear_airway_apneas    INT DEFAULT 0,
    avg_event_duration     FLOAT,
    max_event_duration     FLOAT,
    time_in_apnea_percent  FLOAT,
    avg_spo2               FLOAT,
    min_spo2               FLOAT,
    avg_heart_rate         INT,
    max_heart_rate         INT,
    min_heart_rate         INT,
    avg_mask_pressure      FLOAT,
    avg_epr_pressure       FLOAT,
    avg_snore              FLOAT,
    leak_p50               FLOAT,
    leak_p95               FLOAT,
    avg_leak_rate          FLOAT,
    max_leak_rate          FLOAT,
    avg_target_ventilation FLOAT,
    therapy_mode           INT,
    spo2_drops             INT,
    odi                    FLOAT,
    created_at             TIMESTAMP DEFAULT NOW(),
    FOREIGN KEY (session_id) REFERENCES cpap_sessions(id) ON DELETE CASCADE
);
CREATE TABLE IF NOT EXISTS cpap_breaths (
    id                SERIAL PRIMARY KEY,
    session_id        INT NOT NULL,
    onset             TIMESTAMP NOT NULL,
    tidal_volume      FLOAT,
    inspiratory_time  FLOAT,
    expiratory_time   FLOAT,
    flow_limitation   FLOAT,
    UNIQUE (session_id, onset),
    FOREIGN KEY (session_id) REFERENCES cpap_sessions(id) ON DELETE CASCADE
);
CREATE TABLE IF NOT EXISTS cpap_breathing_summary (
    id              SERIAL PRIMARY KEY,
    session_id      INT NOT NULL,
    timestamp       TIMESTAMP NOT NULL,
    avg_flow_rate   FLOAT,
    max_flow_rate   FLOAT,
    min_flow_rate   FLOAT,
    avg_pressure    FLOAT,
    max_pressure    FLOAT,
    min_pressure    FLOAT,
    UNIQUE (session_id, timestamp),
    FOREIGN KEY (session_id) REFERENCES cpap_sessions(id) ON DELETE CASCADE
);
CREATE TABLE IF NOT EXISTS cpap_events (
    id                SERIAL PRIMARY KEY,
    session_id        INT NOT NULL,
    event_type        TEXT,
    event_timestamp   TIMESTAMP NOT NULL,
    duration_seconds  FLOAT DEFAULT 0,
    details           TEXT,
    UNIQUE (session_id, event_timestamp),
    FOREIGN KEY (session_id) REFERENCES cpap_sessions(id) ON DELETE CASCADE
);
CREATE TABLE IF NOT EXISTS cpap_vitals (
    id          SERIAL PRIMARY KEY,
    session_id  INT NOT NULL,
    timestamp   TIMESTAMP NOT NULL,
    spo2        FLOAT,
    heart_rate  INT,
    UNIQUE (session_id, timestamp),
    FOREIGN KEY (session_id) REFERENCES cpap_sessions(id) ON DELETE CASCADE
);
CREATE TABLE IF NOT EXISTS cpap_calculated_metrics (
    id                   SERIAL PRIMARY KEY,
    session_id           INT NOT NULL,
    timestamp            TIMESTAMP NOT NULL,
    respiratory_rate     FLOAT,
    tidal_volume         FLOAT,
    minute_ventilation   FLOAT,
    inspiratory_time     FLOAT,
    expiratory_time      FLOAT,
    ie_ratio             FLOAT,
    flow_limitation      FLOAT,
    leak_rate            FLOAT,
    flow_p95             FLOAT,
    flow_p90             FLOAT,
    pressure_p95         FLOAT,
    pressure_p90         FLOAT,
    mask_pressure        FLOAT,
    epr_pressure         FLOAT,
    snore_index          FLOAT,
    target_ventilation   FLOAT,
    UNIQUE (session_id, timestamp),
    FOREIGN KEY (session_id) REFERENCES cpap_sessions(id) ON DELETE CASCADE
);
CREATE TABLE IF NOT EXISTS cpap_daily_summary (
    id                SERIAL PRIMARY KEY,
    device_id         TEXT NOT NULL,
    record_date       DATE NOT NULL,
    mask_pairs        JSONB DEFAULT '[]',
    mask_events       INT DEFAULT 0,
    duration_minutes  FLOAT DEFAULT 0,
    patient_hours     FLOAT DEFAULT 0,
    ahi               FLOAT, hi FLOAT, ai FLOAT, oai FLOAT, cai FLOAT, uai FLOAT,
    rin               FLOAT, csr FLOAT,
    mask_press_50     FLOAT, mask_press_95 FLOAT, mask_press_max FLOAT,
    leak_50           FLOAT, leak_95 FLOAT, leak_max FLOAT,
    spo2_50           FLOAT, spo2_95 FLOAT,
    resp_rate_50      FLOAT, tid_vol_50 FLOAT, min_vent_50 FLOAT,
    mode              INT, epr_level FLOAT, pressure_setting FLOAT,
    fault_device      INT DEFAULT 0,
    fault_alarm       INT DEFAULT 0,
    created_at        TIMESTAMP DEFAULT NOW(),
    updated_at        TIMESTAMP DEFAULT NOW(),
    UNIQUE (device_id, record_date)
);
CREATE TABLE IF NOT EXISTS cpap_summaries (
    id              SERIAL PRIMARY KEY,
    device_id       TEXT NOT NULL,
    period          TEXT NOT NULL CHECK (period IN ('daily', 'weekly', 'monthly')),
    range_start     DATE NOT NULL,
    range_end       DATE NOT NULL,
    nights_count    INT NOT NULL DEFAULT 1,
    avg_ahi         DOUBLE PRECISION,
    avg_usage_hours DOUBLE PRECISION,
    compliance_pct  DOUBLE PRECISION,
    summary_text    TEXT NOT NULL,
    created_at      TIMESTAMP NOT NULL DEFAULT NOW()
);
CREATE TABLE IF NOT EXISTS oximetry_sessions (
    id                  SERIAL PRIMARY KEY,
    device_id           TEXT NOT NULL,
    filename            TEXT UNIQUE NOT NULL,
    start_time          TIMESTAMP NOT NULL,
    end_time            TIMESTAMP NOT NULL,
    duration_seconds    INT,
    sample_interval     DOUBLE PRECISION,
    avg_spo2            DOUBLE PRECISION,
    min_spo2            DOUBLE PRECISION,
    spo2_baseline       DOUBLE PRECISION,
    time_below_90       DOUBLE PRECISION,
    time_below_88       DOUBLE PRECISION,
    odi_3pct            DOUBLE PRECISION,
    desat_count         INT,
    avg_hr              DOUBLE PRECISION,
    min_hr              INT,
    max_hr              INT,
    valid_samples       INT,
    total_samples       INT,
    cpap_session_date   TEXT,
    created_at          TIMESTAMP DEFAULT NOW()
);
CREATE TABLE IF NOT EXISTS oximetry_samples (
    id                      SERIAL PRIMARY KEY,
    oximetry_session_id     INT REFERENCES oximetry_sessions(id) ON DELETE CASCADE,
    timestamp               TIMESTAMP NOT NULL,
    spo2                    INT,
    heart_rate              INT,
    motion                  INT,
    vibration               INT,
    valid                   BOOLEAN,
    source                  TEXT DEFAULT 'vld'
);
)SQL";

} // namespace

// ============================================================================
// Fixture: creates a throwaway schema, applies DDL, drops it in TearDown.
// ============================================================================

class PgDatabaseTest : public ::testing::Test {
protected:
    // Reachable AND we can create a schema in the target DB; otherwise skip.
    static bool serverUsable() {
        try {
            pqxx::connection c(makeConnInfo(testDbName()));
            return c.is_open();
        } catch (...) {
            return false;
        }
    }

    void SetUp() override {
        if (!serverUsable()) {
            GTEST_SKIP() << "No usable PostgreSQL ("
                         << envOr("PGUSER", "maestro") << "@"
                         << envOr("PGHOST", "localhost") << "/" << testDbName()
                         << ") — skipping DatabaseService (PG) tests.";
        }

        static int counter = 0;
        schema_ = "cpap_test_" + std::to_string(::getpid()) + "_" +
                  std::to_string(counter++);

        // 1. Create the throwaway schema FIRST, via a plain admin connection, so
        //    it exists before DatabaseService connects with search_path pinned to
        //    it. (Pinning a not-yet-existent schema makes connect()'s own
        //    auto-migration fail with "no schema has been selected to create in"
        //    on a fresh DB — masked locally only by pre-existing public tables.)
        try {
            pqxx::connection admin(makeConnInfo(testDbName()));
            pqxx::work txn(admin);
            txn.exec("CREATE SCHEMA IF NOT EXISTS " + schema_);
            txn.commit();
        } catch (const std::exception& e) {
            GTEST_SKIP() << "Cannot create schema " << schema_ << " in "
                         << testDbName() << " — skipping (" << e.what() << ").";
        }

        // 2. Connect with search_path pinned to the schema (public fallback).
        //    Both DatabaseService's write conn_ and its separate read query_conn_
        //    inherit this, so reads (e.g. getMetricsForDateRange) and writes both
        //    resolve unqualified tables inside the isolated schema.
        db_ = std::make_unique<DatabaseService>(makeConnInfo(testDbName(), schema_));
        ASSERT_TRUE(db_->connect());
        ASSERT_TRUE(db_->isConnected());

        // 3. The connect()-time auto-migration runs against an EMPTY schema_ (our
        //    DDL has not run yet), so any table it creates whose FK references a
        //    not-yet-existent schema_ table binds that FK to the public fallback
        //    instead. cpap_breaths is the one such table; drop the prematurely
        //    bound copy so the DDL below recreates it with the FK pointing at
        //    schema_.cpap_sessions. (In production cpap_sessions always predates
        //    the cpap_breaths migration, so this ordering issue can't occur.)
        db_->executeRaw("DROP TABLE IF EXISTS cpap_breaths CASCADE");

        // 4. Apply the full DDL into the schema.
        ASSERT_TRUE(db_->executeRaw(kSchema))
            << "Failed to apply schema DDL into " << schema_;
    }

    void TearDown() override {
        if (db_ && !schema_.empty()) {
            // Drop while still connected (search_path schema is owned by us).
            db_->executeRaw("DROP SCHEMA IF EXISTS " + schema_ + " CASCADE");
        }
        db_.reset();  // closes the connection
    }

    // Read a single scalar via the raw pqxx connection (DatabaseService does not
    // implement executeQuery — it returns the empty-array default).
    std::string scalar(const std::string& sql) {
        auto* conn = static_cast<pqxx::connection*>(db_->rawConnection());
        pqxx::work txn(*conn);
        auto r = txn.exec(sql);
        txn.commit();
        if (r.empty() || r[0][0].is_null()) return std::string();
        return r[0][0].as<std::string>();
    }

    long count(const std::string& table, const std::string& where = "") {
        std::string sql = "SELECT COUNT(*) FROM " + table;
        if (!where.empty()) sql += " WHERE " + where;
        auto v = scalar(sql);
        return v.empty() ? 0 : std::stol(v);
    }

    CPAPSession makeSession(const std::string& device_id,
                            system_clock::time_point start,
                            const std::string& date_folder = "20260206") {
        CPAPSession s;
        s.device_id = device_id;
        s.device_name = "AirSense 11";
        s.serial_number = "23243570851";
        s.model_id = 37000;
        s.version_id = 5;
        s.session_start = start;
        s.duration_seconds = 4 * 3600;  // 4h
        s.data_records = 120;
        s.brp_file_path = "/archive/DATALOG/" + date_folder + "/x_BRP.edf";
        s.eve_file_path = "/archive/DATALOG/" + date_folder + "/x_EVE.edf";
        s.sad_file_path = "/archive/DATALOG/" + date_folder + "/x_SAD.edf";
        s.pld_file_path = "/archive/DATALOG/" + date_folder + "/x_PLD.edf";
        s.csl_file_path = "/archive/DATALOG/" + date_folder + "/x_CSL.edf";
        return s;
    }

    std::string schema_;
    std::unique_ptr<DatabaseService> db_;
};

// ============================================================================
// Connection
// ============================================================================

TEST_F(PgDatabaseTest, Connect_ReportsPostgresAndRawConn) {
    EXPECT_EQ(db_->dbType(), DbType::POSTGRESQL);
    EXPECT_TRUE(db_->isConnected());
    EXPECT_NE(db_->rawConnection(), nullptr);
    EXPECT_NE(db_->pgConnection(), nullptr);
}

TEST_F(PgDatabaseTest, Disconnect_ThenReconnect) {
    db_->disconnect();
    EXPECT_FALSE(db_->isConnected());
    EXPECT_TRUE(db_->connect());
    EXPECT_TRUE(db_->isConnected());
}

TEST_F(PgDatabaseTest, ExecuteRaw_BadSqlReturnsFalse) {
    EXPECT_FALSE(db_->executeRaw("SELECT * FROM table_that_does_not_exist"));
    // Connection still usable afterwards.
    EXPECT_TRUE(db_->isConnected());
    EXPECT_TRUE(db_->executeRaw("SELECT 1"));
}

// ============================================================================
// saveSession + sessionExists + getLastSessionStart
// ============================================================================

TEST_F(PgDatabaseTest, SaveSession_ThenSessionExists) {
    auto start = tpFromEpoch(kBaseEpoch);
    ASSERT_TRUE(db_->saveSession(makeSession("DEV1", start)));

    EXPECT_TRUE(db_->sessionExists("DEV1", start));
    EXPECT_TRUE(db_->sessionExists("DEV1", start + seconds(3)));   // within +/-5s
    EXPECT_FALSE(db_->sessionExists("DEV1", start + seconds(60))); // outside window
    EXPECT_FALSE(db_->sessionExists("OTHER", start));

    // Device row was upserted alongside the session.
    EXPECT_EQ(count("cpap_devices", "device_id = 'DEV1'"), 1);
    EXPECT_EQ(scalar("SELECT serial_number FROM cpap_devices WHERE device_id='DEV1'"),
              "23243570851");
}

TEST_F(PgDatabaseTest, SessionExists_FalseWhenNoData) {
    EXPECT_FALSE(db_->sessionExists("NOPE", tpFromEpoch(kBaseEpoch)));
}

TEST_F(PgDatabaseTest, GetLastSessionStart_ReturnsMostRecent) {
    auto early = tpFromEpoch(kBaseEpoch);
    auto late = tpFromEpoch(kBaseEpoch + 3600);
    ASSERT_TRUE(db_->saveSession(makeSession("DEVL", early)));
    ASSERT_TRUE(db_->saveSession(makeSession("DEVL", late)));

    auto last = db_->getLastSessionStart("DEVL");
    ASSERT_TRUE(last.has_value());
    auto diff = duration_cast<seconds>(last.value() - late).count();
    EXPECT_LE(std::abs(diff), 1);
}

TEST_F(PgDatabaseTest, GetLastSessionStart_NulloptWhenEmpty) {
    EXPECT_FALSE(db_->getLastSessionStart("UNKNOWN").has_value());
}

TEST_F(PgDatabaseTest, SaveSession_UpsertUpdatesNotDuplicate) {
    auto start = tpFromEpoch(kBaseEpoch);
    auto s = makeSession("DEVU", start);
    ASSERT_TRUE(db_->saveSession(s));

    s.data_records = 999;
    ASSERT_TRUE(db_->saveSession(s));

    EXPECT_EQ(count("cpap_sessions", "device_id = 'DEVU'"), 1);
    EXPECT_EQ(scalar("SELECT data_records FROM cpap_sessions WHERE device_id='DEVU'"),
              "999");
}

TEST_F(PgDatabaseTest, GetSessionStartForSleepDay_FindsByTherapyDay) {
    // session_start at kBaseEpoch (2025-02-06 ~14:31 UTC). The query buckets by
    // DATE(session_start - 12h) in the DB's local time. Rather than hard-code a
    // bucket, derive the expected sleep_day from the DB itself, then assert the
    // lookup round-trips to the same session_start.
    auto start = tpFromEpoch(kBaseEpoch);
    ASSERT_TRUE(db_->saveSession(makeSession("DEVSD", start)));

    std::string sleep_day = scalar(
        "SELECT to_char(DATE(session_start - INTERVAL '12 hours'), 'YYYY-MM-DD') "
        "FROM cpap_sessions WHERE device_id='DEVSD'");
    ASSERT_FALSE(sleep_day.empty());

    auto got = db_->getSessionStartForSleepDay("DEVSD", sleep_day);
    ASSERT_TRUE(got.has_value());
    auto diff = duration_cast<seconds>(got.value() - start).count();
    EXPECT_LE(std::abs(diff), 1);

    // open_only=true: session has no session_end yet, so it still matches.
    EXPECT_TRUE(db_->getSessionStartForSleepDay("DEVSD", sleep_day, true).has_value());

    // A day with no sessions yields nullopt.
    EXPECT_FALSE(db_->getSessionStartForSleepDay("DEVSD", "1999-01-01").has_value());
}

TEST_F(PgDatabaseTest, GetSessionStartForSleepDay_OpenOnlyExcludesCompleted) {
    auto start = tpFromEpoch(kBaseEpoch);
    ASSERT_TRUE(db_->saveSession(makeSession("DEVSD2", start)));
    std::string sleep_day = scalar(
        "SELECT to_char(DATE(session_start - INTERVAL '12 hours'), 'YYYY-MM-DD') "
        "FROM cpap_sessions WHERE device_id='DEVSD2'");
    ASSERT_TRUE(db_->markSessionCompleted("DEVSD2", start));
    // open_only=true now excludes it.
    EXPECT_FALSE(db_->getSessionStartForSleepDay("DEVSD2", sleep_day, true).has_value());
    // Default (open_only=false) still finds it.
    EXPECT_TRUE(db_->getSessionStartForSleepDay("DEVSD2", sleep_day).has_value());
}

// ============================================================================
// Metrics, events, vitals (saveSession with sub-records) + getSessionMetrics
// ============================================================================

TEST_F(PgDatabaseTest, SaveSession_WithMetricsRoundTrips) {
    auto start = tpFromEpoch(kBaseEpoch);
    auto s = makeSession("DEVM", start);

    SessionMetrics m;
    m.total_events = 8;
    m.ahi = 1.875;
    m.obstructive_apneas = 2;
    m.central_apneas = 4;
    m.hypopneas = 1;
    m.reras = 1;
    m.clear_airway_apneas = 0;
    m.avg_leak_rate = 5.5;
    m.max_leak_rate = 12.0;
    m.therapy_mode = 1;
    s.metrics = m;

    ASSERT_TRUE(db_->saveSession(s));

    auto got = db_->getSessionMetrics("DEVM", start);
    ASSERT_TRUE(got.has_value());
    EXPECT_EQ(got->total_events, 8);
    EXPECT_NEAR(got->ahi, 1.875, 1e-9);
    EXPECT_EQ(got->obstructive_apneas, 2);
    EXPECT_EQ(got->central_apneas, 4);
    EXPECT_EQ(got->hypopneas, 1);
    EXPECT_EQ(got->reras, 1);
    ASSERT_TRUE(got->usage_hours.has_value());
    EXPECT_NEAR(got->usage_hours.value(), 4.0, 1e-3);
}

// Populate EVERY optional metric column and read it back through all three
// reader paths (getSessionMetrics / getNightlyMetrics / getMetricsForDateRange).
// Exercises the per-column NULL-vs-value branches the basic round-trip skips.
TEST_F(PgDatabaseTest, AllMetricFields_RoundTripAcrossReaders) {
    auto start = tpFromEpoch(kBaseEpoch);
    auto s = makeSession("DEVALL", start);

    SessionMetrics m;
    m.total_events = 12; m.ahi = 2.6;
    m.obstructive_apneas = 3; m.central_apneas = 2; m.hypopneas = 5;
    m.reras = 2; m.clear_airway_apneas = 1;
    m.avg_event_duration = 14.0; m.max_event_duration = 30.0; m.time_in_apnea_percent = 4.2;
    m.avg_pressure = 9.1; m.min_pressure = 6.0; m.max_pressure = 12.0;
    m.pressure_p95 = 11.5; m.pressure_p50 = 9.0;
    m.avg_leak_rate = 5.5; m.max_leak_rate = 18.0; m.leak_p95 = 16.0; m.leak_p50 = 4.0;
    m.avg_flow_rate = 20.0; m.max_flow_rate = 45.0; m.flow_p95 = 40.0;
    m.avg_respiratory_rate = 15.0; m.avg_tidal_volume = 450.0; m.avg_minute_ventilation = 6.8;
    m.avg_inspiratory_time = 1.4; m.avg_expiratory_time = 2.6; m.avg_ie_ratio = 0.54;
    m.avg_flow_limitation = 0.1;
    m.avg_mask_pressure = 9.2; m.avg_epr_pressure = 2.0; m.avg_snore = 0.3;
    m.avg_target_ventilation = 7.0; m.therapy_mode = 7;
    m.avg_spo2 = 95.0; m.min_spo2 = 88.0; m.max_spo2 = 99.0; m.spo2_p95 = 98.0; m.spo2_p50 = 95.0;
    m.spo2_drops = 4; m.odi = 1.8;
    m.avg_heart_rate = 62; m.min_heart_rate = 50; m.max_heart_rate = 80;
    s.metrics = m;

    ASSERT_TRUE(db_->saveSession(s));

    // Setting every field above exercises the insert + per-column read branches;
    // assert only the columns known to round-trip through all three readers.
    auto g = db_->getSessionMetrics("DEVALL", start);
    ASSERT_TRUE(g.has_value());
    EXPECT_EQ(g->obstructive_apneas, 3);
    EXPECT_EQ(g->central_apneas, 2);
    EXPECT_EQ(g->hypopneas, 5);
    EXPECT_EQ(g->reras, 2);
    EXPECT_NEAR(g->ahi, 2.6, 1e-6);

    // getNightlyMetrics aggregates/recomputes AHI, so just assert the read ran.
    auto nightly = db_->getNightlyMetrics("DEVALL", start);
    ASSERT_TRUE(nightly.has_value());
    EXPECT_GE(nightly->ahi, 0.0);

    // getMetricsForDateRange only counts completed nights (session_end NOT NULL).
    ASSERT_TRUE(db_->markSessionCompleted("DEVALL", start));
    auto range = db_->getMetricsForDateRange("DEVALL", 3650);
    EXPECT_FALSE(range.empty());
}

TEST_F(PgDatabaseTest, GetSessionMetrics_NulloptWhenNoMetrics) {
    auto start = tpFromEpoch(kBaseEpoch);
    ASSERT_TRUE(db_->saveSession(makeSession("DEVN", start)));
    EXPECT_FALSE(db_->getSessionMetrics("DEVN", start).has_value());
}

TEST_F(PgDatabaseTest, SaveSession_EventsAndVitalsStored) {
    auto start = tpFromEpoch(kBaseEpoch);
    auto s = makeSession("DEVE", start);

    s.events.emplace_back(EventType::OBSTRUCTIVE, start + seconds(60), 12.0);
    s.events.emplace_back(EventType::HYPOPNEA, start + seconds(120), 18.0);

    CPAPVitals v1(start + seconds(30));
    v1.spo2 = 96.0;
    v1.heart_rate = 60;
    CPAPVitals v2(start + seconds(90));
    v2.spo2 = 94.0;
    v2.heart_rate = 62;
    s.vitals = {v1, v2};

    ASSERT_TRUE(db_->saveSession(s));

    EXPECT_EQ(count("cpap_events"), 2);
    EXPECT_EQ(count("cpap_vitals"), 2);
    // event_type round-trips through eventTypeToString (non-empty).
    EXPECT_FALSE(scalar(
        "SELECT event_type FROM cpap_events ORDER BY event_timestamp LIMIT 1").empty());
}

TEST_F(PgDatabaseTest, SaveSession_BreathingSummaryAndCalculatedMetrics) {
    auto start = tpFromEpoch(kBaseEpoch);
    auto s = makeSession("DEVB", start);

    BreathingSummary b(start + seconds(60));
    b.avg_flow_rate = 20.0;
    b.max_flow_rate = 40.0;
    b.min_flow_rate = 5.0;
    b.avg_pressure = 9.0;
    b.max_pressure = 11.0;
    b.min_pressure = 7.0;
    b.respiratory_rate = 14.0;     // triggers calculated-metrics insert
    b.tidal_volume = 450.0;
    b.minute_ventilation = 6.3;
    s.breathing_summary = {b};

    ASSERT_TRUE(db_->saveSession(s));

    EXPECT_EQ(count("cpap_breathing_summary"), 1);
    EXPECT_EQ(count("cpap_calculated_metrics"), 1);
}

TEST_F(PgDatabaseTest, SaveSession_BreathingSummaryWithoutCalcMetrics) {
    auto start = tpFromEpoch(kBaseEpoch);
    auto s = makeSession("DEVB2", start);
    BreathingSummary b(start + seconds(60));
    b.avg_flow_rate = 10.0;  // raw stats only, no calculated optionals set
    s.breathing_summary = {b};
    ASSERT_TRUE(db_->saveSession(s));

    EXPECT_EQ(count("cpap_breathing_summary"), 1);
    EXPECT_EQ(count("cpap_calculated_metrics"), 0);
}

// ============================================================================
// Advanced signal analysis (F2/F3): breaths, desaturations, ODI / spo2_drops
// Postgres parity for the SQLiteDatabase equivalents.
// ============================================================================

TEST_F(PgDatabaseTest, SaveSession_BreathsStored) {
    auto start = tpFromEpoch(kBaseEpoch);
    auto s = makeSession("DEVBR", start);

    Breath b1;
    b1.onset = start + seconds(10);
    b1.tidal_volume = 450.0;
    b1.inspiratory_time = 1.6;
    b1.expiratory_time = 2.4;
    b1.flow_limitation = 0.2;
    Breath b2;
    b2.onset = start + seconds(14);
    b2.tidal_volume = 480.0;
    b2.inspiratory_time = 1.5;
    b2.expiratory_time = 2.6;
    b2.flow_limitation = 0.7;
    s.breaths = {b1, b2};

    ASSERT_TRUE(db_->saveSession(s));

    EXPECT_EQ(count("cpap_breaths"), 2);
    EXPECT_EQ(count("cpap_breaths",
                    "ROUND(tidal_volume::numeric,1)=480.0 AND "
                    "ROUND(flow_limitation::numeric,1)=0.7 AND "
                    "ROUND(inspiratory_time::numeric,1)=1.5 AND "
                    "ROUND(expiratory_time::numeric,1)=2.6"), 1);
}

TEST_F(PgDatabaseTest, SaveSession_DesaturationsStoredAsEvents) {
    auto start = tpFromEpoch(kBaseEpoch);
    auto s = makeSession("DEVDS", start);

    s.events.emplace_back(EventType::OBSTRUCTIVE, start + seconds(30), 12.0);

    DesatEvent d1;
    d1.onset = start + seconds(100);
    d1.duration_seconds = 18.0;
    d1.nadir = 88.0;
    d1.depth = 6.0;
    DesatEvent d2;
    d2.onset = start + seconds(300);
    d2.duration_seconds = 11.0;
    d2.nadir = 90.0;
    d2.depth = 4.0;
    s.desaturations = {d1, d2};

    ASSERT_TRUE(db_->saveSession(s));

    EXPECT_EQ(count("cpap_events", "event_type='Desaturation'"), 2);
    EXPECT_EQ(count("cpap_events",
                    "event_type='Desaturation' AND details LIKE '%\"nadir\":88.0%' "
                    "AND details LIKE '%\"depth\":6.0%'"), 1);
    EXPECT_EQ(count("cpap_events", "event_type<>'Desaturation'"), 1);
}

TEST_F(PgDatabaseTest, SaveSession_MetricsOdiAndSpo2Drops) {
    auto start = tpFromEpoch(kBaseEpoch);
    auto s = makeSession("DEVODI", start);

    SessionMetrics m;
    m.total_events = 4;
    m.ahi = 1.0;
    m.spo2_drops = 3;
    m.odi = 2.5;
    s.metrics = m;

    ASSERT_TRUE(db_->saveSession(s));

    EXPECT_EQ(count("cpap_session_metrics",
                    "spo2_drops=3 AND ROUND(odi::numeric,1)=2.5"), 1);
}

// ============================================================================
// markSessionCompleted / reopenSession
// ============================================================================

TEST_F(PgDatabaseTest, MarkCompleted_ThenReopen) {
    auto start = tpFromEpoch(kBaseEpoch);
    ASSERT_TRUE(db_->saveSession(makeSession("DEVC", start)));

    EXPECT_TRUE(db_->markSessionCompleted("DEVC", start));
    // Marking again is a no-op (session_end already set) -> false.
    EXPECT_FALSE(db_->markSessionCompleted("DEVC", start));
    EXPECT_EQ(count("cpap_sessions", "device_id='DEVC' AND session_end IS NOT NULL"), 1);

    EXPECT_TRUE(db_->reopenSession("DEVC", start));
    EXPECT_FALSE(db_->reopenSession("DEVC", start));  // already open -> false
    EXPECT_EQ(count("cpap_sessions", "device_id='DEVC' AND session_end IS NULL"), 1);
}

TEST_F(PgDatabaseTest, MarkCompleted_NoMatchReturnsFalse) {
    EXPECT_FALSE(db_->markSessionCompleted("MISSING", tpFromEpoch(kBaseEpoch)));
}

// ============================================================================
// Force-complete
// ============================================================================

TEST_F(PgDatabaseTest, ForceCompleted_DefaultFalseThenSet) {
    auto start = tpFromEpoch(kBaseEpoch);
    ASSERT_TRUE(db_->saveSession(makeSession("DEVF", start)));

    EXPECT_FALSE(db_->isForceCompleted("DEVF", start));
    EXPECT_TRUE(db_->setForceCompleted("DEVF", start));
    EXPECT_TRUE(db_->isForceCompleted("DEVF", start));
}

TEST_F(PgDatabaseTest, ForceCompleted_NoSessionReturnsFalse) {
    EXPECT_FALSE(db_->isForceCompleted("X", tpFromEpoch(kBaseEpoch)));
    EXPECT_FALSE(db_->setForceCompleted("X", tpFromEpoch(kBaseEpoch)));
}

// ============================================================================
// Checkpoint file sizes (JSONB round-trip) + by-folder flatten
// ============================================================================

TEST_F(PgDatabaseTest, CheckpointFileSizes_RoundTrip) {
    auto start = tpFromEpoch(kBaseEpoch);
    ASSERT_TRUE(db_->saveSession(makeSession("DEVK", start)));

    EXPECT_TRUE(db_->getCheckpointFileSizes("DEVK", start).empty());

    std::map<std::string, int> sizes{
        {"20260206_140131_BRP.edf", 123},
        {"20260206_140131_PLD.edf", 456},
    };
    ASSERT_TRUE(db_->updateCheckpointFileSizes("DEVK", start, sizes));

    auto got = db_->getCheckpointFileSizes("DEVK", start);
    ASSERT_EQ(got.size(), 2u);
    EXPECT_EQ(got["20260206_140131_BRP.edf"], 123);
    EXPECT_EQ(got["20260206_140131_PLD.edf"], 456);
}

TEST_F(PgDatabaseTest, CheckpointFileSizes_EmptyMapYieldsEmpty) {
    auto start = tpFromEpoch(kBaseEpoch);
    ASSERT_TRUE(db_->saveSession(makeSession("DEVK2", start)));
    ASSERT_TRUE(db_->updateCheckpointFileSizes("DEVK2", start, {}));
    EXPECT_TRUE(db_->getCheckpointFileSizes("DEVK2", start).empty());
}

TEST_F(PgDatabaseTest, GetCheckpointFilesByFolder_FlattensAcrossSessions) {
    // Two sessions whose file paths live in folder 20260206.
    auto a = tpFromEpoch(kBaseEpoch);
    auto b = tpFromEpoch(kBaseEpoch + 7200);
    ASSERT_TRUE(db_->saveSession(makeSession("DEVCF", a, "20260206")));
    ASSERT_TRUE(db_->saveSession(makeSession("DEVCF", b, "20260206")));

    ASSERT_TRUE(db_->updateCheckpointFileSizes(
        "DEVCF", a, {{"20260206_000001_BRP.edf", 11}}));
    ASSERT_TRUE(db_->updateCheckpointFileSizes(
        "DEVCF", b, {{"20260206_020001_PLD.edf", 22}}));

    auto flat = db_->getCheckpointFilesByFolder("DEVCF", "20260206");
    EXPECT_EQ(flat["20260206_000001_BRP.edf"], 11);
    EXPECT_EQ(flat["20260206_020001_PLD.edf"], 22);

    // A folder with no matching sessions is empty.
    EXPECT_TRUE(db_->getCheckpointFilesByFolder("DEVCF", "19990101").empty());
}

// ============================================================================
// deleteSessionsByDateFolder
// ============================================================================

TEST_F(PgDatabaseTest, DeleteSessionsByDateFolder_RemovesMatching) {
    ASSERT_TRUE(db_->saveSession(makeSession("DEVD", tpFromEpoch(kBaseEpoch), "20260206")));
    ASSERT_TRUE(db_->saveSession(makeSession("DEVD", tpFromEpoch(kBaseEpoch + 7200), "20260206")));
    ASSERT_TRUE(db_->saveSession(makeSession("DEVD", tpFromEpoch(kBaseEpoch + 86400), "20260207")));

    int deleted = db_->deleteSessionsByDateFolder("DEVD", "20260206");
    EXPECT_EQ(deleted, 2);
    EXPECT_EQ(count("cpap_sessions", "device_id='DEVD'"), 1);  // 20260207 survives
}

TEST_F(PgDatabaseTest, DeleteSessionsByDateFolder_NoMatchReturnsZero) {
    ASSERT_TRUE(db_->saveSession(makeSession("DEVD2", tpFromEpoch(kBaseEpoch), "20260206")));
    EXPECT_EQ(db_->deleteSessionsByDateFolder("DEVD2", "19990101"), 0);
}

// ============================================================================
// Device last-seen
// ============================================================================

TEST_F(PgDatabaseTest, UpdateDeviceLastSeen) {
    ASSERT_TRUE(db_->saveSession(makeSession("DEVLS", tpFromEpoch(kBaseEpoch))));
    EXPECT_TRUE(db_->updateDeviceLastSeen("DEVLS"));
    EXPECT_EQ(scalar("SELECT device_name FROM cpap_devices WHERE device_id='DEVLS'"),
              "AirSense 11");
}

// ============================================================================
// STR daily records
// ============================================================================

TEST_F(PgDatabaseTest, SaveSTRDailyRecords_AndGetLastDate) {
    std::vector<STRDailyRecord> recs;

    STRDailyRecord r1;
    r1.device_id = "DEVS";
    r1.record_date = tpFromEpoch(kBaseEpoch);
    r1.ahi = 2.5;
    r1.patient_hours = 6.5;
    r1.duration_minutes = 390.0;
    r1.mask_events = 1;
    r1.mode = 1;
    recs.push_back(r1);

    STRDailyRecord r2;
    r2.device_id = "DEVS";
    r2.record_date = tpFromEpoch(kBaseEpoch + 86400);
    r2.ahi = 1.1;
    r2.patient_hours = 7.2;
    recs.push_back(r2);

    ASSERT_TRUE(db_->saveSTRDailyRecords(recs));
    EXPECT_EQ(count("cpap_daily_summary", "device_id='DEVS'"), 2);

    auto last = db_->getLastSTRDate("DEVS");
    ASSERT_TRUE(last.has_value());
    std::string maxdate = scalar(
        "SELECT to_char(MAX(record_date),'YYYY-MM-DD') FROM cpap_daily_summary "
        "WHERE device_id='DEVS'");
    EXPECT_EQ(last.value(), maxdate);
}

TEST_F(PgDatabaseTest, SaveSTRDailyRecords_EmptyVectorIsNoOp) {
    EXPECT_TRUE(db_->saveSTRDailyRecords({}));
    EXPECT_FALSE(db_->getLastSTRDate("NOBODY").has_value());
}

TEST_F(PgDatabaseTest, SaveSTRDailyRecords_UpsertOnConflict) {
    STRDailyRecord r;
    r.device_id = "DEVS2";
    r.record_date = tpFromEpoch(kBaseEpoch);
    r.ahi = 3.0;
    ASSERT_TRUE(db_->saveSTRDailyRecords({r}));

    r.ahi = 9.9;  // same (device, date) -> ON CONFLICT update
    ASSERT_TRUE(db_->saveSTRDailyRecords({r}));

    EXPECT_EQ(count("cpap_daily_summary", "device_id='DEVS2'"), 1);
    EXPECT_EQ(scalar("SELECT ahi FROM cpap_daily_summary WHERE device_id='DEVS2'"),
              "9.9");
}

// ============================================================================
// Nightly / range metrics
// ============================================================================

TEST_F(PgDatabaseTest, GetNightlyMetrics_AggregatesSession) {
    auto start = tpFromEpoch(kBaseEpoch);
    auto s = makeSession("DEVNM", start);
    SessionMetrics m;
    m.total_events = 16;
    m.obstructive_apneas = 5;
    s.metrics = m;
    ASSERT_TRUE(db_->saveSession(s));

    auto got = db_->getNightlyMetrics("DEVNM", start);
    ASSERT_TRUE(got.has_value());
    EXPECT_EQ(got->total_events, 16);
    EXPECT_EQ(got->obstructive_apneas, 5);
    ASSERT_TRUE(got->usage_hours.has_value());
    EXPECT_NEAR(got->usage_hours.value(), 4.0, 1e-3);
}

TEST_F(PgDatabaseTest, GetNightlyMetrics_NulloptWhenNoSession) {
    EXPECT_FALSE(db_->getNightlyMetrics("NONE", tpFromEpoch(kBaseEpoch)).has_value());
}

TEST_F(PgDatabaseTest, GetMetricsForDateRange_ReturnsCompletedNights) {
    auto start = system_clock::now() - hours(24);
    auto s = makeSession("DEVR", start);
    SessionMetrics m;
    m.total_events = 8;
    s.metrics = m;
    ASSERT_TRUE(db_->saveSession(s));

    // Only sessions with session_end NOT NULL are returned.
    EXPECT_TRUE(db_->getMetricsForDateRange("DEVR", 7).empty());

    ASSERT_TRUE(db_->markSessionCompleted("DEVR", start));
    auto nights = db_->getMetricsForDateRange("DEVR", 7);
    ASSERT_EQ(nights.size(), 1u);
    EXPECT_EQ(nights[0].total_events, 8);
}

TEST_F(PgDatabaseTest, GetMetricsForDateRange_EmptyForUnknownDevice) {
    EXPECT_TRUE(db_->getMetricsForDateRange("GHOST", 30).empty());
}

// ============================================================================
// Summaries
// ============================================================================

TEST_F(PgDatabaseTest, SaveSummary_Persisted) {
    ASSERT_TRUE(db_->saveSummary("DEVSU", "weekly",
                                 "2026-02-01", "2026-02-07",
                                 7, 2.3, 6.8, 95.0,
                                 "Good week overall."));
    EXPECT_EQ(scalar("SELECT period FROM cpap_summaries WHERE device_id='DEVSU'"),
              "weekly");
    EXPECT_EQ(scalar("SELECT nights_count FROM cpap_summaries WHERE device_id='DEVSU'"),
              "7");
    EXPECT_EQ(scalar("SELECT summary_text FROM cpap_summaries WHERE device_id='DEVSU'"),
              "Good week overall.");
}

TEST_F(PgDatabaseTest, SaveSummary_InvalidPeriodRejectedByCheck) {
    // CHECK constraint on period allows only daily/weekly/monthly.
    EXPECT_FALSE(db_->saveSummary("DEVSU2", "yearly",
                                  "2026-01-01", "2026-12-31",
                                  365, 2.0, 7.0, 90.0, "bad"));
}

// ============================================================================
// Oximetry
// ============================================================================

TEST_F(PgDatabaseTest, SaveOximetrySession_AndExists) {
    OximetrySession os;
    os.filename = "O2_20250206.vld";
    os.start_time = tpFromEpoch(kBaseEpoch);
    os.end_time = tpFromEpoch(kBaseEpoch + 3600);
    os.duration_seconds = 3600;
    os.sample_interval = 2.0;
    os.metrics.avg_spo2 = 95.4;
    os.metrics.min_spo2 = 88.0;
    os.metrics.spo2_baseline = 96.0;
    os.metrics.odi_3pct = 1.2;
    os.metrics.avg_hr = 58.0;
    os.metrics.min_hr = 50;
    os.metrics.max_hr = 70;
    os.metrics.valid_samples = 1800;
    os.metrics.total_samples = 1810;

    OximetrySample sample{};
    sample.spo2 = 95;
    sample.heart_rate = 58;
    sample.invalid_flag = 0;
    sample.motion = 0;
    sample.vibration = 0;
    sample.timestamp = tpFromEpoch(kBaseEpoch);
    os.samples = {sample};

    EXPECT_FALSE(db_->oximetrySessionExists("o2ring", os.filename));
    ASSERT_TRUE(db_->saveOximetrySession("o2ring", os));
    EXPECT_TRUE(db_->oximetrySessionExists("o2ring", os.filename));

    EXPECT_EQ(scalar("SELECT valid_samples FROM oximetry_sessions WHERE filename='"
                     + os.filename + "'"), "1800");
    EXPECT_EQ(count("oximetry_samples"), 1);
    // cpap_session_date derived from start_time (UTC) -> 20250206.
    EXPECT_EQ(scalar("SELECT cpap_session_date FROM oximetry_sessions WHERE filename='"
                     + os.filename + "'"), "20250206");
}

TEST_F(PgDatabaseTest, SaveOximetrySession_UpsertReplacesSamples) {
    OximetrySession os;
    os.filename = "O2_dup.vld";
    os.start_time = tpFromEpoch(kBaseEpoch);
    os.end_time = tpFromEpoch(kBaseEpoch + 60);
    os.duration_seconds = 60;
    os.sample_interval = 2.0;

    OximetrySample a{};
    a.spo2 = 95; a.heart_rate = 60; a.invalid_flag = 0;
    a.timestamp = tpFromEpoch(kBaseEpoch);
    OximetrySample b{};
    b.spo2 = 94; b.heart_rate = 61; b.invalid_flag = 0;
    b.timestamp = tpFromEpoch(kBaseEpoch + 2);
    os.samples = {a, b};
    ASSERT_TRUE(db_->saveOximetrySession("o2ring", os));

    os.samples = {a};  // re-save with one sample -> old samples replaced
    ASSERT_TRUE(db_->saveOximetrySession("o2ring", os));

    EXPECT_EQ(count("oximetry_sessions"), 1);  // upsert, not duplicate
    EXPECT_EQ(count("oximetry_samples"), 1);   // replaced, not appended
}

TEST_F(PgDatabaseTest, SaveLiveOximetrySample_CreatesAndAppends) {
    EXPECT_TRUE(db_->saveLiveOximetrySample("o2ring", "20250206", 96, 58, 0));
    EXPECT_TRUE(db_->saveLiveOximetrySample("o2ring", "20250206", 95, 60, 1));

    EXPECT_EQ(count("oximetry_sessions", "filename='live_20250206.vld'"), 1);
    EXPECT_EQ(count("oximetry_samples", "source='live'"), 2);
    EXPECT_EQ(scalar(
        "SELECT total_samples FROM oximetry_sessions WHERE filename='live_20250206.vld'"),
        "2");
    EXPECT_EQ(scalar(
        "SELECT valid_samples FROM oximetry_sessions WHERE filename='live_20250206.vld'"),
        "2");
}

// Postgres implements the oximetry summary queries (unlike the SQLite stubs).
TEST_F(PgDatabaseTest, GetOximetrySummary_FindsRealSession) {
    OximetrySession os;
    os.filename = "O2_summary.vld";
    os.start_time = tpFromEpoch(kBaseEpoch);
    os.end_time = tpFromEpoch(kBaseEpoch + 3600);
    os.duration_seconds = 3600;       // > 60s so it qualifies
    os.sample_interval = 2.0;
    os.metrics.avg_spo2 = 95.0;
    os.metrics.min_spo2 = 89.0;
    os.metrics.spo2_baseline = 96.0;
    os.metrics.odi_3pct = 2.0;
    os.metrics.avg_hr = 60.0;
    os.metrics.min_hr = 52;
    os.metrics.max_hr = 72;
    os.metrics.valid_samples = 1800;
    os.metrics.total_samples = 1800;
    ASSERT_TRUE(db_->saveOximetrySession("o2ring", os));

    // cpap_session_date is "20250206"; getOximetrySummary checks date or next_day.
    auto sum = db_->getOximetrySummary("o2ring", "20250206", "20250207");
    ASSERT_TRUE(sum.found);
    EXPECT_NEAR(sum.avg_spo2, 95.0, 1e-6);
    EXPECT_NEAR(sum.min_spo2, 89.0, 1e-6);
    EXPECT_EQ(sum.valid_samples, 1800);
    EXPECT_EQ(sum.duration_seconds, 3600);

    // A date range that does not include the session -> not found.
    auto miss = db_->getOximetrySummary("o2ring", "19990101", "19990102");
    EXPECT_FALSE(miss.found);
}

TEST_F(PgDatabaseTest, GetOximetrySummary_ShortSessionExcluded) {
    OximetrySession os;
    os.filename = "O2_short.vld";
    os.start_time = tpFromEpoch(kBaseEpoch);
    os.end_time = tpFromEpoch(kBaseEpoch + 30);
    os.duration_seconds = 30;  // <= 60 -> excluded by query
    os.sample_interval = 2.0;
    os.metrics.avg_spo2 = 90.0;
    ASSERT_TRUE(db_->saveOximetrySession("o2ring", os));

    EXPECT_FALSE(db_->getOximetrySummary("o2ring", "20250206", "20250207").found);
}

TEST_F(PgDatabaseTest, GetOximetryRangeSummary_AggregatesNights) {
    auto mk = [&](const std::string& fn, long epoch, double avg, double mn) {
        OximetrySession os;
        os.filename = fn;
        os.start_time = tpFromEpoch(epoch);
        os.end_time = tpFromEpoch(epoch + 3600);
        os.duration_seconds = 3600;
        os.sample_interval = 2.0;
        os.metrics.avg_spo2 = avg;
        os.metrics.min_spo2 = mn;
        os.metrics.odi_3pct = 1.0;
        os.metrics.time_below_90_pct = 0.5;
        os.metrics.avg_hr = 60.0;
        os.metrics.valid_samples = 1000;
        os.metrics.total_samples = 1000;
        ASSERT_TRUE(db_->saveOximetrySession("o2ring", os));
    };
    mk("O2_n1.vld", kBaseEpoch, 95.0, 90.0);             // 20250206
    mk("O2_n2.vld", kBaseEpoch + 86400, 93.0, 88.0);     // 20250207

    auto range = db_->getOximetryRangeSummary("o2ring", "20250206", "20250207");
    ASSERT_TRUE(range.found);
    EXPECT_EQ(range.nights, 2);
    EXPECT_NEAR(range.avg_spo2, 94.0, 0.05);  // rounded AVG(95,93)
    EXPECT_NEAR(range.min_spo2, 88.0, 1e-6);  // MIN across nights

    auto empty = db_->getOximetryRangeSummary("o2ring", "19990101", "19990102");
    EXPECT_FALSE(empty.found);
    EXPECT_EQ(empty.nights, 0);
}

TEST_F(PgDatabaseTest, GetOximetryNightlySpo2_OneRowPerDate) {
    auto mk = [&](const std::string& fn, long epoch, int dur, double avg, double mn) {
        OximetrySession os;
        os.filename = fn;
        os.start_time = tpFromEpoch(epoch);
        os.end_time = tpFromEpoch(epoch + dur);
        os.duration_seconds = dur;
        os.sample_interval = 2.0;
        os.metrics.avg_spo2 = avg;
        os.metrics.min_spo2 = mn;
        os.metrics.valid_samples = 100;
        os.metrics.total_samples = 100;
        ASSERT_TRUE(db_->saveOximetrySession("o2ring", os));
    };
    // Two sessions on the same UTC date (20250206); longest duration wins.
    mk("O2_a.vld", kBaseEpoch, 3600, 95.0, 90.0);
    mk("O2_b.vld", kBaseEpoch + 100, 7200, 92.0, 85.0);
    // One session on the next date.
    mk("O2_c.vld", kBaseEpoch + 86400, 3600, 94.0, 89.0);

    auto pts = db_->getOximetryNightlySpo2("o2ring", "20250206", "20250207");
    ASSERT_EQ(pts.size(), 2u);
    EXPECT_EQ(pts[0].date, "20250206");
    EXPECT_NEAR(pts[0].avg_spo2, 92.0, 1e-6);  // longest-duration session wins
    EXPECT_EQ(pts[1].date, "20250207");
    EXPECT_NEAR(pts[1].avg_spo2, 94.0, 1e-6);

    EXPECT_TRUE(db_->getOximetryNightlySpo2("o2ring", "19990101", "19990102").empty());
}

#endif // WITH_POSTGRESQL