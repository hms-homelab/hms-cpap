/**
 * HMS-CPAP PostgresDatabase wrapper + DatabaseService edge-case unit tests.
 *
 * Focus (distinct from tests/database/test_DatabaseService_pg.cpp, which drives
 * DatabaseService directly):
 *   - PostgresDatabase is the thin IDatabase wrapper that delegates EVERY method
 *     to an owned DatabaseService. This suite drives it THROUGH the IDatabase
 *     interface so each delegating forwarder is executed, plus its own
 *     executeQuery() libpq path (separate PGconn, not pqxx) and disconnect()
 *     teardown of that query connection.
 *   - DatabaseService error / not-found / empty / ON-CONFLICT branches not hit
 *     elsewhere (executeQuery default-empty surface via the wrapper, reopen of a
 *     never-completed session, getCheckpointFilesByFolder empty, etc.).
 *
 * Strategy (throwaway-schema pattern, copied from test_DatabaseService_pg.cpp):
 *   - Create a unique schema (cpap_edge_<pid>_<counter>) via a plain admin
 *     connection FIRST, then construct PostgresDatabase with a conninfo whose
 *     options pin search_path=<schema>,public. Both the wrapped DatabaseService
 *     (pqxx) AND the wrapper's own libpq query_conn_ inherit that search_path,
 *     because the `options=-csearch_path=...` keyword is honored by libpq too.
 *   - If no PostgreSQL is reachable / schema cannot be created, GTEST_SKIP the
 *     whole suite cleanly (counts as pass).
 *   - TearDown drops the schema CASCADE while still connected.
 *
 * Determinism: all timestamps derive from a fixed epoch (kBaseEpoch). We never
 * assert on wall-clock values, only round-tripped data / NULL-ness / booleans.
 * cpap_session_date is derived from start_time in UTC, so kBaseEpoch -> 20250206.
 *
 * Build: requires -DBUILD_WITH_POSTGRESQL=ON (guarded by WITH_POSTGRESQL).
 */

#ifdef WITH_POSTGRESQL

#include <gtest/gtest.h>
#include "database/PostgresDatabase.h"
#include "database/IDatabase.h"
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

// 1738847491 = 2025-02-06 14:31:31 UTC. Fixed; UTC date component -> "20250206".
constexpr long kBaseEpoch = 1738847491;

system_clock::time_point tpFromEpoch(long secs) {
    return system_clock::time_point{} + seconds(secs);
}

std::string envOr(const char* key, const std::string& def) {
    const char* v = std::getenv(key);
    return (v && *v) ? std::string(v) : def;
}

std::string testDbName() {
    return envOr("PGDATABASE", "cpap_monitoring");
}

// Base conninfo (no search_path). search_path appended separately so the same
// DB can host the throwaway schema, and so BOTH the pqxx (DatabaseService) and
// libpq (PostgresDatabase::query_conn_) connections resolve unqualified tables
// inside the isolated schema.
std::string makeConnInfo(const std::string& dbname,
                         const std::string& search_path = "") {
    std::string ci = "host=" + envOr("PGHOST", "localhost") +
                     " port=" + envOr("PGPORT", "5432") +
                     " user=" + envOr("PGUSER", "maestro") +
                     " password=" + envOr("PGPASSWORD", "REDACTED") +
                     " dbname=" + dbname +
                     " connect_timeout=3";
    if (!search_path.empty()) {
        ci += " options=-csearch_path=" + search_path + ",public";
    }
    return ci;
}

// Subset of scripts/schema.sql exercised here. Mirrors the PG DDL used in prod,
// kept inline so the test has no runtime dependency on the source tree.
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
    created_at             TIMESTAMP DEFAULT NOW(),
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
// Fixture: drives the PostgresDatabase wrapper through the IDatabase interface.
// ============================================================================

class PostgresEdgesTest : public ::testing::Test {
protected:
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
                         << ") — skipping PostgresEdges tests.";
        }

        static int counter = 0;
        schema_ = "cpap_edge_" + std::to_string(::getpid()) + "_" +
                  std::to_string(counter++);

        // 1. Create the throwaway schema FIRST via a plain admin connection, so
        //    it exists before the wrapper connects with search_path pinned.
        try {
            pqxx::connection admin(makeConnInfo(testDbName()));
            pqxx::work txn(admin);
            txn.exec("CREATE SCHEMA IF NOT EXISTS " + schema_);
            txn.commit();
        } catch (const std::exception& e) {
            GTEST_SKIP() << "Cannot create schema " << schema_ << " in "
                         << testDbName() << " — skipping (" << e.what() << ").";
        }

        conninfo_ = makeConnInfo(testDbName(), schema_);

        // 2. Construct the wrapper and connect. We hold it as IDatabase* so the
        //    delegating forwarders are exercised through the interface.
        wrapper_ = std::make_unique<PostgresDatabase>(conninfo_);
        db_ = wrapper_.get();
        ASSERT_TRUE(db_->connect());
        ASSERT_TRUE(db_->isConnected());

        // 3. Apply the DDL. The wrapper has no executeRaw, so use the raw pqxx
        //    connection it exposes (DatabaseService's write conn_).
        auto* conn = static_cast<pqxx::connection*>(db_->rawConnection());
        ASSERT_NE(conn, nullptr);
        pqxx::work txn(*conn);
        txn.exec(kSchema);
        txn.commit();
    }

    void TearDown() override {
        if (db_ && !schema_.empty() && db_->isConnected()) {
            auto* conn = static_cast<pqxx::connection*>(db_->rawConnection());
            if (conn) {
                try {
                    pqxx::work txn(*conn);
                    txn.exec("DROP SCHEMA IF EXISTS " + schema_ + " CASCADE");
                    txn.commit();
                } catch (...) {}
            }
        }
        wrapper_.reset();
    }

    long count(const std::string& table, const std::string& where = "") {
        auto* conn = static_cast<pqxx::connection*>(db_->rawConnection());
        std::string sql = "SELECT COUNT(*) FROM " + table;
        if (!where.empty()) sql += " WHERE " + where;
        pqxx::work txn(*conn);
        auto r = txn.exec(sql);
        txn.commit();
        return r[0][0].as<long>();
    }

    std::string scalar(const std::string& sql) {
        auto* conn = static_cast<pqxx::connection*>(db_->rawConnection());
        pqxx::work txn(*conn);
        auto r = txn.exec(sql);
        txn.commit();
        if (r.empty() || r[0][0].is_null()) return std::string();
        return r[0][0].as<std::string>();
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
        s.duration_seconds = 4 * 3600;
        s.data_records = 120;
        s.brp_file_path = "/archive/DATALOG/" + date_folder + "/x_BRP.edf";
        s.eve_file_path = "/archive/DATALOG/" + date_folder + "/x_EVE.edf";
        s.sad_file_path = "/archive/DATALOG/" + date_folder + "/x_SAD.edf";
        s.pld_file_path = "/archive/DATALOG/" + date_folder + "/x_PLD.edf";
        s.csl_file_path = "/archive/DATALOG/" + date_folder + "/x_CSL.edf";
        return s;
    }

    std::string schema_;
    std::string conninfo_;
    std::unique_ptr<PostgresDatabase> wrapper_;
    IDatabase* db_ = nullptr;  // wrapper viewed through the interface
};

// ============================================================================
// Wrapper identity / connection lifecycle
// ============================================================================

TEST_F(PostgresEdgesTest, DbType_IsPostgresAndRawConnNonNull) {
    EXPECT_EQ(db_->dbType(), DbType::POSTGRESQL);
    EXPECT_TRUE(db_->isConnected());
    EXPECT_NE(db_->rawConnection(), nullptr);
}

TEST_F(PostgresEdgesTest, Disconnect_ClosesQueryConnAndUnderlying) {
    // Force the libpq query_conn_ open via executeQuery, then disconnect must
    // tear it down (the wrapper's own branch, not just the delegated one).
    Json::Value one = db_->executeQuery("SELECT 1 AS v");
    ASSERT_TRUE(one.isArray());
    ASSERT_EQ(one.size(), 1u);
    EXPECT_EQ(one[0]["v"].asString(), "1");

    db_->disconnect();
    EXPECT_FALSE(db_->isConnected());

    // Reconnect for TearDown's DROP SCHEMA to have a live connection.
    ASSERT_TRUE(db_->connect());
}

// ============================================================================
// executeQuery (libpq path on the wrapper's own query_conn_)
// ============================================================================

TEST_F(PostgresEdgesTest, ExecuteQuery_ParamsAndNulls) {
    ASSERT_TRUE(db_->saveSession(makeSession("EQDEV", tpFromEpoch(kBaseEpoch))));

    // Positional param ($1) + a deliberately NULL column (session_end is NULL on
    // a freshly-saved, never-completed session) -> exercises PQgetisnull branch.
    Json::Value rows = db_->executeQuery(
        "SELECT device_id, session_end FROM cpap_sessions WHERE device_id = $1",
        {"EQDEV"});
    ASSERT_TRUE(rows.isArray());
    ASSERT_EQ(rows.size(), 1u);
    EXPECT_EQ(rows[0]["device_id"].asString(), "EQDEV");
    EXPECT_TRUE(rows[0]["session_end"].isNull());
}

TEST_F(PostgresEdgesTest, ExecuteQuery_NoRowsReturnsEmptyArray) {
    Json::Value rows = db_->executeQuery(
        "SELECT device_id FROM cpap_sessions WHERE device_id = $1", {"NOBODY"});
    ASSERT_TRUE(rows.isArray());
    EXPECT_EQ(rows.size(), 0u);
}

TEST_F(PostgresEdgesTest, ExecuteQuery_BadSqlReturnsEmptyArrayAndStaysUsable) {
    // PQexecParams fails -> wrapper returns empty array (error branch).
    Json::Value bad = db_->executeQuery("SELECT * FROM no_such_table_here");
    ASSERT_TRUE(bad.isArray());
    EXPECT_EQ(bad.size(), 0u);

    // query_conn_ is still reusable on the next call.
    Json::Value ok = db_->executeQuery("SELECT 42 AS answer");
    ASSERT_EQ(ok.size(), 1u);
    EXPECT_EQ(ok[0]["answer"].asString(), "42");
}

// ============================================================================
// Delegated session CRUD through the wrapper
// ============================================================================

TEST_F(PostgresEdgesTest, SaveSession_ExistsLastStart_Delegated) {
    auto start = tpFromEpoch(kBaseEpoch);
    ASSERT_TRUE(db_->saveSession(makeSession("WDEV", start)));

    EXPECT_TRUE(db_->sessionExists("WDEV", start));
    EXPECT_FALSE(db_->sessionExists("WDEV", start + seconds(120)));
    EXPECT_FALSE(db_->sessionExists("MISSING", start));

    auto last = db_->getLastSessionStart("WDEV");
    ASSERT_TRUE(last.has_value());
    EXPECT_LE(std::abs(duration_cast<seconds>(last.value() - start).count()), 1);

    EXPECT_FALSE(db_->getLastSessionStart("MISSING").has_value());
    EXPECT_EQ(count("cpap_devices", "device_id = 'WDEV'"), 1);
}

TEST_F(PostgresEdgesTest, SleepDayLookup_Delegated) {
    auto start = tpFromEpoch(kBaseEpoch);
    ASSERT_TRUE(db_->saveSession(makeSession("WSD", start)));

    std::string sleep_day = scalar(
        "SELECT to_char(DATE(session_start - INTERVAL '12 hours'), 'YYYY-MM-DD') "
        "FROM cpap_sessions WHERE device_id='WSD'");
    ASSERT_FALSE(sleep_day.empty());

    auto got = db_->getSessionStartForSleepDay("WSD", sleep_day);
    ASSERT_TRUE(got.has_value());
    EXPECT_TRUE(db_->getSessionStartForSleepDay("WSD", sleep_day, true).has_value());
    EXPECT_FALSE(db_->getSessionStartForSleepDay("WSD", "1990-01-01").has_value());
}

TEST_F(PostgresEdgesTest, GetSessionMetrics_PresentAndAbsent_Delegated) {
    auto start = tpFromEpoch(kBaseEpoch);
    auto s = makeSession("WMET", start);
    SessionMetrics m;
    m.total_events = 5;
    m.ahi = 1.25;
    m.obstructive_apneas = 3;
    s.metrics = m;
    ASSERT_TRUE(db_->saveSession(s));

    auto got = db_->getSessionMetrics("WMET", start);
    ASSERT_TRUE(got.has_value());
    EXPECT_EQ(got->total_events, 5);
    EXPECT_NEAR(got->ahi, 1.25, 1e-9);

    // No metrics row -> nullopt.
    ASSERT_TRUE(db_->saveSession(makeSession("WMET2", start)));
    EXPECT_FALSE(db_->getSessionMetrics("WMET2", start).has_value());
}

// ============================================================================
// markSessionCompleted / reopenSession edge branches (delegated)
// ============================================================================

TEST_F(PostgresEdgesTest, MarkAndReopen_FullCycle_Delegated) {
    auto start = tpFromEpoch(kBaseEpoch);
    ASSERT_TRUE(db_->saveSession(makeSession("WMC", start)));

    EXPECT_TRUE(db_->markSessionCompleted("WMC", start));
    EXPECT_FALSE(db_->markSessionCompleted("WMC", start));  // already completed
    EXPECT_FALSE(db_->markSessionCompleted("ABSENT", start)); // no row

    EXPECT_TRUE(db_->reopenSession("WMC", start));
    EXPECT_FALSE(db_->reopenSession("WMC", start));  // already open
    EXPECT_FALSE(db_->reopenSession("ABSENT", start)); // no row
}

TEST_F(PostgresEdgesTest, Reopen_NeverCompletedSessionReturnsFalse) {
    // A freshly saved session has session_end NULL already; reopen is a no-op.
    auto start = tpFromEpoch(kBaseEpoch);
    ASSERT_TRUE(db_->saveSession(makeSession("WRO", start)));
    EXPECT_FALSE(db_->reopenSession("WRO", start));
}

// ============================================================================
// Force-complete (delegated)
// ============================================================================

TEST_F(PostgresEdgesTest, ForceComplete_Delegated) {
    auto start = tpFromEpoch(kBaseEpoch);
    ASSERT_TRUE(db_->saveSession(makeSession("WFC", start)));

    EXPECT_FALSE(db_->isForceCompleted("WFC", start));
    EXPECT_TRUE(db_->setForceCompleted("WFC", start));
    EXPECT_TRUE(db_->isForceCompleted("WFC", start));

    // No session at all -> both false.
    EXPECT_FALSE(db_->isForceCompleted("GONE", start));
    EXPECT_FALSE(db_->setForceCompleted("GONE", start));
}

// ============================================================================
// Checkpoint file sizes / by-folder (delegated)
// ============================================================================

TEST_F(PostgresEdgesTest, CheckpointFileSizes_RoundTripAndEmpty_Delegated) {
    auto start = tpFromEpoch(kBaseEpoch);
    ASSERT_TRUE(db_->saveSession(makeSession("WCK", start)));

    EXPECT_TRUE(db_->getCheckpointFileSizes("WCK", start).empty());

    std::map<std::string, int> sizes{
        {"20260206_000001_BRP.edf", 100},
        {"20260206_000001_PLD.edf", 200},
    };
    ASSERT_TRUE(db_->updateCheckpointFileSizes("WCK", start, sizes));
    auto got = db_->getCheckpointFileSizes("WCK", start);
    ASSERT_EQ(got.size(), 2u);
    EXPECT_EQ(got["20260206_000001_BRP.edf"], 100);

    // Empty map clears / yields empty.
    ASSERT_TRUE(db_->updateCheckpointFileSizes("WCK", start, {}));
    EXPECT_TRUE(db_->getCheckpointFileSizes("WCK", start).empty());
}

TEST_F(PostgresEdgesTest, GetCheckpointFilesByFolder_FlattenAndEmpty_Delegated) {
    auto a = tpFromEpoch(kBaseEpoch);
    auto b = tpFromEpoch(kBaseEpoch + 7200);
    ASSERT_TRUE(db_->saveSession(makeSession("WCF", a, "20260206")));
    ASSERT_TRUE(db_->saveSession(makeSession("WCF", b, "20260206")));
    ASSERT_TRUE(db_->updateCheckpointFileSizes("WCF", a, {{"20260206_000001_BRP.edf", 11}}));
    ASSERT_TRUE(db_->updateCheckpointFileSizes("WCF", b, {{"20260206_020001_PLD.edf", 22}}));

    auto flat = db_->getCheckpointFilesByFolder("WCF", "20260206");
    EXPECT_EQ(flat["20260206_000001_BRP.edf"], 11);
    EXPECT_EQ(flat["20260206_020001_PLD.edf"], 22);

    // Folder with no matching sessions -> empty.
    EXPECT_TRUE(db_->getCheckpointFilesByFolder("WCF", "19990101").empty());
}

// ============================================================================
// deleteSessionsByDateFolder / device last-seen (delegated)
// ============================================================================

TEST_F(PostgresEdgesTest, DeleteByDateFolder_Delegated) {
    ASSERT_TRUE(db_->saveSession(makeSession("WDF", tpFromEpoch(kBaseEpoch), "20260206")));
    ASSERT_TRUE(db_->saveSession(makeSession("WDF", tpFromEpoch(kBaseEpoch + 86400), "20260207")));

    EXPECT_EQ(db_->deleteSessionsByDateFolder("WDF", "20260206"), 1);
    EXPECT_EQ(db_->deleteSessionsByDateFolder("WDF", "19990101"), 0);  // no match
    EXPECT_EQ(count("cpap_sessions", "device_id='WDF'"), 1);
}

TEST_F(PostgresEdgesTest, UpdateDeviceLastSeen_Delegated) {
    ASSERT_TRUE(db_->saveSession(makeSession("WLS", tpFromEpoch(kBaseEpoch))));
    EXPECT_TRUE(db_->updateDeviceLastSeen("WLS"));
}

// ============================================================================
// STR daily records: empty no-op + ON CONFLICT update (delegated)
// ============================================================================

TEST_F(PostgresEdgesTest, STRDaily_EmptyNoOpAndLastDate_Delegated) {
    EXPECT_TRUE(db_->saveSTRDailyRecords({}));        // empty vector no-op
    EXPECT_FALSE(db_->getLastSTRDate("WSTR").has_value());

    STRDailyRecord r;
    r.device_id = "WSTR";
    r.record_date = tpFromEpoch(kBaseEpoch);
    r.ahi = 2.0;
    r.patient_hours = 6.0;
    ASSERT_TRUE(db_->saveSTRDailyRecords({r}));

    auto last = db_->getLastSTRDate("WSTR");
    ASSERT_TRUE(last.has_value());
}

TEST_F(PostgresEdgesTest, STRDaily_OnConflictUpdate_Delegated) {
    STRDailyRecord r;
    r.device_id = "WSTR2";
    r.record_date = tpFromEpoch(kBaseEpoch);
    r.ahi = 1.0;
    ASSERT_TRUE(db_->saveSTRDailyRecords({r}));

    r.ahi = 8.8;  // same (device, date) -> ON CONFLICT update, no duplicate row
    ASSERT_TRUE(db_->saveSTRDailyRecords({r}));

    EXPECT_EQ(count("cpap_daily_summary", "device_id='WSTR2'"), 1);
    EXPECT_EQ(scalar("SELECT ahi FROM cpap_daily_summary WHERE device_id='WSTR2'"), "8.8");
}

// ============================================================================
// Nightly / range metrics (delegated)
// ============================================================================

TEST_F(PostgresEdgesTest, NightlyAndRangeMetrics_Delegated) {
    auto start = tpFromEpoch(kBaseEpoch);
    auto s = makeSession("WNM", start);
    SessionMetrics m;
    m.total_events = 12;
    m.obstructive_apneas = 4;
    s.metrics = m;
    ASSERT_TRUE(db_->saveSession(s));

    auto night = db_->getNightlyMetrics("WNM", start);
    ASSERT_TRUE(night.has_value());
    EXPECT_EQ(night->total_events, 12);
    EXPECT_FALSE(db_->getNightlyMetrics("NONE", start).has_value());

    EXPECT_TRUE(db_->getMetricsForDateRange("GHOST", 30).empty());
}

// ============================================================================
// Summaries: insert + CHECK-constraint failure (delegated)
// ============================================================================

TEST_F(PostgresEdgesTest, SaveSummary_OkAndBadPeriod_Delegated) {
    EXPECT_TRUE(db_->saveSummary("WSU", "monthly", "2026-02-01", "2026-02-28",
                                 28, 2.1, 6.5, 92.0, "Decent month."));
    EXPECT_EQ(scalar("SELECT period FROM cpap_summaries WHERE device_id='WSU'"),
              "monthly");

    // CHECK constraint rejects 'yearly' -> delegated saveSummary returns false.
    EXPECT_FALSE(db_->saveSummary("WSU2", "yearly", "2026-01-01", "2026-12-31",
                                  365, 2.0, 7.0, 90.0, "bad period"));
}

// ============================================================================
// Oximetry inline forwarders (delegated through the wrapper)
// ============================================================================

TEST_F(PostgresEdgesTest, Oximetry_SaveExistsSummaryRangeNightly_Delegated) {
    cpapdash::parser::OximetrySession os;
    os.filename = "O2_edge.vld";
    os.start_time = tpFromEpoch(kBaseEpoch);
    os.end_time = tpFromEpoch(kBaseEpoch + 3600);
    os.duration_seconds = 3600;
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

    EXPECT_FALSE(db_->oximetrySessionExists("o2ring", os.filename));
    ASSERT_TRUE(db_->saveOximetrySession("o2ring", os));
    EXPECT_TRUE(db_->oximetrySessionExists("o2ring", os.filename));

    auto sum = db_->getOximetrySummary("o2ring", "20250206", "20250207");
    ASSERT_TRUE(sum.found);
    EXPECT_NEAR(sum.avg_spo2, 95.0, 1e-6);

    auto range = db_->getOximetryRangeSummary("o2ring", "20250206", "20250207");
    ASSERT_TRUE(range.found);
    EXPECT_EQ(range.nights, 1);

    auto pts = db_->getOximetryNightlySpo2("o2ring", "20250206", "20250207");
    ASSERT_EQ(pts.size(), 1u);
    EXPECT_EQ(pts[0].date, "20250206");

    // A range with no data -> not found / empty.
    EXPECT_FALSE(db_->getOximetrySummary("o2ring", "19990101", "19990102").found);
    EXPECT_FALSE(db_->getOximetryRangeSummary("o2ring", "19990101", "19990102").found);
    EXPECT_TRUE(db_->getOximetryNightlySpo2("o2ring", "19990101", "19990102").empty());
}

TEST_F(PostgresEdgesTest, SaveLiveOximetrySample_Delegated) {
    EXPECT_TRUE(db_->saveLiveOximetrySample("o2ring", "20250206", 96, 58, 0));
    EXPECT_TRUE(db_->saveLiveOximetrySample("o2ring", "20250206", 95, 60, 1));
    EXPECT_EQ(count("oximetry_sessions", "filename='live_20250206.vld'"), 1);
    EXPECT_EQ(count("oximetry_samples", "source='live'"), 2);
}

#endif // WITH_POSTGRESQL
