/**
 * HMS-CPAP SQLiteDatabase Unit Tests
 *
 * Exercises the full IDatabase CRUD surface against a real (temp-file) SQLite
 * database. No external server, no network, no MQTT. Each test opens a unique
 * per-pid temp .db via the fixture, populates it, and asserts write/read
 * round-trips. The temp file (and its WAL/SHM siblings) is removed in TearDown.
 *
 * All timestamps use a fixed epoch-derived time_point so behaviour is
 * deterministic regardless of wall-clock. (markSessionCompleted etc. use
 * datetime('now') internally for the *value* written, but tests only assert on
 * NULL vs non-NULL / return-bool, never the literal time.)
 */

#include <gtest/gtest.h>
#include "database/SQLiteDatabase.h"
#include "parsers/CpapdashBridge.h"

#include <chrono>
#include <filesystem>
#include <string>
#include <unistd.h>

using namespace hms_cpap;
using namespace std::chrono;
namespace fs = std::filesystem;

namespace {

// A fixed, deterministic time_point used as the canonical session_start across
// most tests. 1738847491 = 2025-02-06 14:31:31 UTC (exact wall value is
// irrelevant; tests only rely on it being fixed and round-tripping).
constexpr long kBaseEpoch = 1738847491;

system_clock::time_point tpFromEpoch(long secs) {
    return system_clock::time_point{} + seconds(secs);
}

} // namespace

class SQLiteDatabaseTest : public ::testing::Test {
protected:
    void SetUp() override {
        // Unique path per pid + test counter to avoid collisions across tests.
        static int counter = 0;
        db_path_ = (fs::temp_directory_path() /
                    ("hms_cpap_sqlite_test_" + std::to_string(::getpid()) +
                     "_" + std::to_string(counter++) + ".db")).string();
        cleanupFiles();

        db_ = std::make_unique<SQLiteDatabase>(db_path_);
        ASSERT_TRUE(db_->connect());
        ASSERT_TRUE(db_->isConnected());
    }

    void TearDown() override {
        db_.reset();          // closes the connection
        cleanupFiles();
    }

    void cleanupFiles() {
        std::error_code ec;
        fs::remove(db_path_, ec);
        fs::remove(db_path_ + "-wal", ec);
        fs::remove(db_path_ + "-shm", ec);
    }

    // Build a minimal but populated session for the given start time.
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

    std::string db_path_;
    std::unique_ptr<SQLiteDatabase> db_;
};

// ============================================================================
// Connection
// ============================================================================

TEST_F(SQLiteDatabaseTest, Connect_CreatesFileAndReportsConnected) {
    EXPECT_TRUE(fs::exists(db_path_));
    EXPECT_EQ(db_->dbType(), DbType::SQLITE);
    EXPECT_NE(db_->rawConnection(), nullptr);
}

TEST_F(SQLiteDatabaseTest, Disconnect_ThenIsConnectedFalse) {
    db_->disconnect();
    EXPECT_FALSE(db_->isConnected());
    EXPECT_EQ(db_->rawConnection(), nullptr);
    // Reconnect should succeed (schema already exists -> IF NOT EXISTS).
    EXPECT_TRUE(db_->connect());
    EXPECT_TRUE(db_->isConnected());
}

// ============================================================================
// saveSession + sessionExists + getLastSessionStart
// ============================================================================

TEST_F(SQLiteDatabaseTest, SaveSession_ThenSessionExists) {
    auto start = tpFromEpoch(kBaseEpoch);
    auto s = makeSession("DEV1", start);
    ASSERT_TRUE(db_->saveSession(s));

    EXPECT_TRUE(db_->sessionExists("DEV1", start));
    // Within the +/-5s tolerance window.
    EXPECT_TRUE(db_->sessionExists("DEV1", start + seconds(3)));
    // Outside the tolerance window.
    EXPECT_FALSE(db_->sessionExists("DEV1", start + seconds(60)));
    // Different device.
    EXPECT_FALSE(db_->sessionExists("OTHER", start));
}

TEST_F(SQLiteDatabaseTest, SessionExists_FalseWhenNoData) {
    EXPECT_FALSE(db_->sessionExists("NOPE", tpFromEpoch(kBaseEpoch)));
}

TEST_F(SQLiteDatabaseTest, GetLastSessionStart_ReturnsMostRecent) {
    auto early = tpFromEpoch(kBaseEpoch);
    auto late = tpFromEpoch(kBaseEpoch + 3600);  // one hour later
    ASSERT_TRUE(db_->saveSession(makeSession("DEV1", early)));
    ASSERT_TRUE(db_->saveSession(makeSession("DEV1", late)));

    auto last = db_->getLastSessionStart("DEV1");
    ASSERT_TRUE(last.has_value());

    // Round-trip through local-time formatting; compare to the later start.
    auto diff = duration_cast<seconds>(last.value() - late).count();
    EXPECT_LE(std::abs(diff), 1);
}

TEST_F(SQLiteDatabaseTest, GetLastSessionStart_NulloptWhenEmpty) {
    EXPECT_FALSE(db_->getLastSessionStart("UNKNOWN").has_value());
}

TEST_F(SQLiteDatabaseTest, SaveSession_UpsertUpdatesFilePathsNotDuplicate) {
    auto start = tpFromEpoch(kBaseEpoch);
    auto s = makeSession("DEV1", start);
    ASSERT_TRUE(db_->saveSession(s));

    // Re-save same (device_id, session_start) with a different data_records.
    s.data_records = 999;
    ASSERT_TRUE(db_->saveSession(s));

    // Still exactly one row for this device.
    auto rows = db_->executeQuery(
        "SELECT COUNT(*) AS n FROM cpap_sessions WHERE device_id = ?", {"DEV1"});
    ASSERT_EQ(rows.size(), 1u);
    EXPECT_EQ(rows[0]["n"].asString(), "1");

    auto dr = db_->executeQuery(
        "SELECT data_records FROM cpap_sessions WHERE device_id = ?", {"DEV1"});
    ASSERT_EQ(dr.size(), 1u);
    EXPECT_EQ(dr[0]["data_records"].asString(), "999");
}

// getSessionStartForSleepDay is a stub returning nullopt — assert that contract.
TEST_F(SQLiteDatabaseTest, GetSessionStartForSleepDay_StubReturnsNullopt) {
    ASSERT_TRUE(db_->saveSession(makeSession("DEV1", tpFromEpoch(kBaseEpoch))));
    EXPECT_FALSE(db_->getSessionStartForSleepDay("DEV1", "2026-02-06").has_value());
}

// ============================================================================
// Metrics, events, vitals (saveSession with sub-records) + getSessionMetrics
// ============================================================================

TEST_F(SQLiteDatabaseTest, SaveSession_WithMetricsRoundTrips) {
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
    // usage_hours derived from duration_seconds (4h).
    ASSERT_TRUE(got->usage_hours.has_value());
    EXPECT_NEAR(got->usage_hours.value(), 4.0, 1e-3);
}

TEST_F(SQLiteDatabaseTest, GetSessionMetrics_NulloptWhenNoMetrics) {
    // Session exists but has no metrics row -> JOIN yields nothing.
    auto start = tpFromEpoch(kBaseEpoch);
    ASSERT_TRUE(db_->saveSession(makeSession("DEVN", start)));
    EXPECT_FALSE(db_->getSessionMetrics("DEVN", start).has_value());
}

TEST_F(SQLiteDatabaseTest, SaveSession_EventsAndVitalsStored) {
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

    auto evs = db_->executeQuery(
        "SELECT COUNT(*) AS n FROM cpap_events");
    ASSERT_EQ(evs.size(), 1u);
    EXPECT_EQ(evs[0]["n"].asString(), "2");

    auto vit = db_->executeQuery(
        "SELECT COUNT(*) AS n FROM cpap_vitals");
    ASSERT_EQ(vit.size(), 1u);
    EXPECT_EQ(vit[0]["n"].asString(), "2");

    // Confirm an event type round-tripped through eventTypeToString.
    auto types = db_->executeQuery(
        "SELECT event_type FROM cpap_events ORDER BY event_timestamp");
    ASSERT_EQ(types.size(), 2u);
    EXPECT_FALSE(types[0]["event_type"].asString().empty());
}

TEST_F(SQLiteDatabaseTest, SaveSession_BreathingSummaryAndCalculatedMetrics) {
    auto start = tpFromEpoch(kBaseEpoch);
    auto s = makeSession("DEVB", start);

    BreathingSummary b(start + seconds(60));
    b.avg_flow_rate = 20.0;
    b.max_flow_rate = 40.0;
    b.min_flow_rate = 5.0;
    b.avg_pressure = 9.0;
    b.max_pressure = 11.0;
    b.min_pressure = 7.0;
    b.respiratory_rate = 14.0;          // triggers calculated-metrics insert
    b.tidal_volume = 450.0;
    b.minute_ventilation = 6.3;
    s.breathing_summary = {b};

    ASSERT_TRUE(db_->saveSession(s));

    auto bs = db_->executeQuery("SELECT COUNT(*) AS n FROM cpap_breathing_summary");
    ASSERT_EQ(bs.size(), 1u);
    EXPECT_EQ(bs[0]["n"].asString(), "1");

    auto cm = db_->executeQuery("SELECT COUNT(*) AS n FROM cpap_calculated_metrics");
    ASSERT_EQ(cm.size(), 1u);
    EXPECT_EQ(cm[0]["n"].asString(), "1");
}

// A breathing summary with no calculated metrics must NOT create a
// calculated_metrics row (the early-continue branch).
TEST_F(SQLiteDatabaseTest, SaveSession_BreathingSummaryWithoutCalcMetrics) {
    auto start = tpFromEpoch(kBaseEpoch);
    auto s = makeSession("DEVB2", start);
    BreathingSummary b(start + seconds(60));
    b.avg_flow_rate = 10.0;  // raw stats only, no calculated optionals set
    s.breathing_summary = {b};
    ASSERT_TRUE(db_->saveSession(s));

    auto cm = db_->executeQuery("SELECT COUNT(*) AS n FROM cpap_calculated_metrics");
    ASSERT_EQ(cm.size(), 1u);
    EXPECT_EQ(cm[0]["n"].asString(), "0");
}

// ============================================================================
// Advanced signal analysis (F2/F3): breaths, desaturations, ODI / spo2_drops
// ============================================================================

// Per-breath records round-trip into cpap_breaths with their TV/Ti/Te/flow-lim.
TEST_F(SQLiteDatabaseTest, SaveSession_BreathsStored) {
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

    auto n = db_->executeQuery("SELECT COUNT(*) AS n FROM cpap_breaths");
    ASSERT_EQ(n.size(), 1u);
    EXPECT_EQ(n[0]["n"].asString(), "2");

    // The exact per-breath payload survived (TV + flow-limitation columns).
    auto hit = db_->executeQuery(
        "SELECT COUNT(*) AS n FROM cpap_breaths "
        "WHERE ROUND(tidal_volume,1)=480.0 AND ROUND(flow_limitation,1)=0.7 "
        "AND ROUND(inspiratory_time,1)=1.5 AND ROUND(expiratory_time,1)=2.6");
    ASSERT_EQ(hit.size(), 1u);
    EXPECT_EQ(hit[0]["n"].asString(), "1");
}

// Desaturations are persisted into cpap_events as type 'Desaturation' carrying a
// {"nadir":..,"depth":..} details blob -- and must NOT masquerade as apneas.
TEST_F(SQLiteDatabaseTest, SaveSession_DesaturationsStoredAsEvents) {
    auto start = tpFromEpoch(kBaseEpoch);
    auto s = makeSession("DEVDS", start);

    // One real respiratory event + two desats: only the apnea counts toward AHI.
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

    auto desat = db_->executeQuery(
        "SELECT COUNT(*) AS n FROM cpap_events WHERE event_type='Desaturation'");
    ASSERT_EQ(desat.size(), 1u);
    EXPECT_EQ(desat[0]["n"].asString(), "2");

    // details JSON carries nadir + depth for the chart overlay.
    auto det = db_->executeQuery(
        "SELECT COUNT(*) AS n FROM cpap_events "
        "WHERE event_type='Desaturation' AND details LIKE '%\"nadir\":88.0%' "
        "AND details LIKE '%\"depth\":6.0%'");
    ASSERT_EQ(det.size(), 1u);
    EXPECT_EQ(det[0]["n"].asString(), "1");

    // The obstructive apnea is a distinct, non-Desaturation row.
    auto apnea = db_->executeQuery(
        "SELECT COUNT(*) AS n FROM cpap_events WHERE event_type!='Desaturation'");
    ASSERT_EQ(apnea.size(), 1u);
    EXPECT_EQ(apnea[0]["n"].asString(), "1");
}

// ODI + spo2_drops persist on the session metrics row.
TEST_F(SQLiteDatabaseTest, SaveSession_MetricsOdiAndSpo2Drops) {
    auto start = tpFromEpoch(kBaseEpoch);
    auto s = makeSession("DEVODI", start);

    SessionMetrics m;
    m.total_events = 4;
    m.ahi = 1.0;
    m.spo2_drops = 3;
    m.odi = 2.5;
    s.metrics = m;

    ASSERT_TRUE(db_->saveSession(s));

    auto got = db_->executeQuery(
        "SELECT COUNT(*) AS n FROM cpap_session_metrics "
        "WHERE spo2_drops=3 AND ROUND(odi,1)=2.5");
    ASSERT_EQ(got.size(), 1u);
    EXPECT_EQ(got[0]["n"].asString(), "1");
}

// ============================================================================
// markSessionCompleted / reopenSession
// ============================================================================

TEST_F(SQLiteDatabaseTest, MarkCompleted_ThenReopen) {
    auto start = tpFromEpoch(kBaseEpoch);
    ASSERT_TRUE(db_->saveSession(makeSession("DEVC", start)));

    // session_end starts NULL.
    EXPECT_TRUE(db_->markSessionCompleted("DEVC", start));
    // Marking again is a no-op (session_end already set) -> false.
    EXPECT_FALSE(db_->markSessionCompleted("DEVC", start));

    auto ended = db_->executeQuery(
        "SELECT session_end FROM cpap_sessions WHERE device_id = ?", {"DEVC"});
    ASSERT_EQ(ended.size(), 1u);
    EXPECT_FALSE(ended[0]["session_end"].isNull());

    // Reopen clears session_end.
    EXPECT_TRUE(db_->reopenSession("DEVC", start));
    // Reopening again is a no-op -> false.
    EXPECT_FALSE(db_->reopenSession("DEVC", start));

    auto cleared = db_->executeQuery(
        "SELECT session_end FROM cpap_sessions WHERE device_id = ?", {"DEVC"});
    ASSERT_EQ(cleared.size(), 1u);
    EXPECT_TRUE(cleared[0]["session_end"].isNull());
}

TEST_F(SQLiteDatabaseTest, MarkCompleted_NoMatchReturnsFalse) {
    EXPECT_FALSE(db_->markSessionCompleted("MISSING", tpFromEpoch(kBaseEpoch)));
}

// ============================================================================
// Force-complete
// ============================================================================

TEST_F(SQLiteDatabaseTest, ForceCompleted_DefaultFalseThenSet) {
    auto start = tpFromEpoch(kBaseEpoch);
    ASSERT_TRUE(db_->saveSession(makeSession("DEVF", start)));

    EXPECT_FALSE(db_->isForceCompleted("DEVF", start));
    EXPECT_TRUE(db_->setForceCompleted("DEVF", start));
    EXPECT_TRUE(db_->isForceCompleted("DEVF", start));
}

TEST_F(SQLiteDatabaseTest, ForceCompleted_NoSessionReturnsFalse) {
    EXPECT_FALSE(db_->isForceCompleted("X", tpFromEpoch(kBaseEpoch)));
    EXPECT_FALSE(db_->setForceCompleted("X", tpFromEpoch(kBaseEpoch)));
}

// ============================================================================
// Checkpoint file sizes (get/update round-trip via JSON column)
// ============================================================================

TEST_F(SQLiteDatabaseTest, CheckpointFileSizes_RoundTrip) {
    auto start = tpFromEpoch(kBaseEpoch);
    ASSERT_TRUE(db_->saveSession(makeSession("DEVK", start)));

    // Initially empty.
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

TEST_F(SQLiteDatabaseTest, CheckpointFileSizes_EmptyMapYieldsEmpty) {
    auto start = tpFromEpoch(kBaseEpoch);
    ASSERT_TRUE(db_->saveSession(makeSession("DEVK2", start)));
    ASSERT_TRUE(db_->updateCheckpointFileSizes("DEVK2", start, {}));
    EXPECT_TRUE(db_->getCheckpointFileSizes("DEVK2", start).empty());
}

// getCheckpointFilesByFolder is a stub in the SQLite backend -> always empty.
TEST_F(SQLiteDatabaseTest, GetCheckpointFilesByFolder_StubEmpty) {
    EXPECT_TRUE(db_->getCheckpointFilesByFolder("DEVK", "20260206").empty());
}

// ============================================================================
// deleteSessionsByDateFolder
// ============================================================================

TEST_F(SQLiteDatabaseTest, DeleteSessionsByDateFolder_RemovesMatching) {
    // Two sessions in folder A, one in folder B.
    ASSERT_TRUE(db_->saveSession(
        makeSession("DEVD", tpFromEpoch(kBaseEpoch), "20260206")));
    ASSERT_TRUE(db_->saveSession(
        makeSession("DEVD", tpFromEpoch(kBaseEpoch + 7200), "20260206")));
    ASSERT_TRUE(db_->saveSession(
        makeSession("DEVD", tpFromEpoch(kBaseEpoch + 86400), "20260207")));

    int deleted = db_->deleteSessionsByDateFolder("DEVD", "20260206");
    EXPECT_EQ(deleted, 2);

    // The 20260207 session survives.
    auto remaining = db_->executeQuery(
        "SELECT COUNT(*) AS n FROM cpap_sessions WHERE device_id = ?", {"DEVD"});
    ASSERT_EQ(remaining.size(), 1u);
    EXPECT_EQ(remaining[0]["n"].asString(), "1");
}

TEST_F(SQLiteDatabaseTest, DeleteSessionsByDateFolder_NoMatchReturnsZero) {
    ASSERT_TRUE(db_->saveSession(
        makeSession("DEVD2", tpFromEpoch(kBaseEpoch), "20260206")));
    EXPECT_EQ(db_->deleteSessionsByDateFolder("DEVD2", "19990101"), 0);
}

// ============================================================================
// Device last-seen
// ============================================================================

TEST_F(SQLiteDatabaseTest, UpdateDeviceLastSeen) {
    auto start = tpFromEpoch(kBaseEpoch);
    ASSERT_TRUE(db_->saveSession(makeSession("DEVL", start)));
    // Device row was upserted by saveSession; touching last_seen succeeds.
    EXPECT_TRUE(db_->updateDeviceLastSeen("DEVL"));

    auto rows = db_->executeQuery(
        "SELECT device_name, serial_number FROM cpap_devices WHERE device_id = ?",
        {"DEVL"});
    ASSERT_EQ(rows.size(), 1u);
    EXPECT_EQ(rows[0]["device_name"].asString(), "AirSense 11");
    EXPECT_EQ(rows[0]["serial_number"].asString(), "23243570851");
}

// ============================================================================
// STR daily records
// ============================================================================

TEST_F(SQLiteDatabaseTest, SaveSTRDailyRecords_AndGetLastDate) {
    std::vector<STRDailyRecord> recs;

    STRDailyRecord r1;
    r1.device_id = "DEVS";
    r1.record_date = tpFromEpoch(kBaseEpoch);  // earlier day
    r1.ahi = 2.5;
    r1.patient_hours = 6.5;
    r1.duration_minutes = 390.0;
    r1.mask_events = 1;
    r1.mode = 1;
    recs.push_back(r1);

    STRDailyRecord r2;
    r2.device_id = "DEVS";
    r2.record_date = tpFromEpoch(kBaseEpoch + 86400);  // next day
    r2.ahi = 1.1;
    r2.patient_hours = 7.2;
    recs.push_back(r2);

    ASSERT_TRUE(db_->saveSTRDailyRecords(recs));

    auto last = db_->getLastSTRDate("DEVS");
    ASSERT_TRUE(last.has_value());

    auto rows = db_->executeQuery(
        "SELECT record_date, ahi FROM cpap_daily_summary WHERE device_id = ? "
        "ORDER BY record_date ASC", {"DEVS"});
    ASSERT_EQ(rows.size(), 2u);
    EXPECT_EQ(rows[0]["ahi"].asString(), "2.5");
    // getLastSTRDate returns MAX(record_date) == the later row's date.
    EXPECT_EQ(last.value(), rows[1]["record_date"].asString());
    EXPECT_GT(rows[1]["record_date"].asString(), rows[0]["record_date"].asString());
}

TEST_F(SQLiteDatabaseTest, SaveSTRDailyRecords_EmptyVectorIsNoOp) {
    EXPECT_TRUE(db_->saveSTRDailyRecords({}));
    EXPECT_FALSE(db_->getLastSTRDate("NOBODY").has_value());
}

TEST_F(SQLiteDatabaseTest, SaveSTRDailyRecords_UpsertOnConflict) {
    STRDailyRecord r;
    r.device_id = "DEVS2";
    r.record_date = tpFromEpoch(kBaseEpoch);
    r.ahi = 3.0;
    ASSERT_TRUE(db_->saveSTRDailyRecords({r}));

    r.ahi = 9.9;  // same (device, date) -> ON CONFLICT update
    ASSERT_TRUE(db_->saveSTRDailyRecords({r}));

    auto rows = db_->executeQuery(
        "SELECT COUNT(*) AS n, MAX(ahi) AS a FROM cpap_daily_summary "
        "WHERE device_id = ?", {"DEVS2"});
    ASSERT_EQ(rows.size(), 1u);
    EXPECT_EQ(rows[0]["n"].asString(), "1");
    EXPECT_EQ(rows[0]["a"].asString(), "9.9");
}

// ============================================================================
// Nightly / range metrics
// ============================================================================

TEST_F(SQLiteDatabaseTest, GetNightlyMetrics_AggregatesSession) {
    // total_events (16) deliberately includes 8 non-AHI events (RERAs, flow
    // limitation, etc. -- as Lowenstein Prisma SMART max sessions do) so this
    // test fails loudly if AHI regresses to total_events/hours (issue #13).
    auto start = tpFromEpoch(kBaseEpoch);
    auto s = makeSession("DEVNM", start);
    SessionMetrics m;
    m.total_events = 16;
    m.obstructive_apneas = 5;
    m.hypopneas = 3;
    s.metrics = m;
    ASSERT_TRUE(db_->saveSession(s));

    auto got = db_->getNightlyMetrics("DEVNM", start);
    ASSERT_TRUE(got.has_value());
    EXPECT_EQ(got->total_events, 16);
    EXPECT_EQ(got->obstructive_apneas, 5);
    EXPECT_EQ(got->hypopneas, 3);
    ASSERT_TRUE(got->usage_hours.has_value());
    EXPECT_NEAR(got->usage_hours.value(), 4.0, 1e-3);
    // AHI = (obstructive + central + hypopnea + clear_airway) / hours
    //     = (5 + 0 + 3 + 0) / 4h = 2.0 -- NOT total_events(16) / 4h = 4.0
    EXPECT_NEAR(got->ahi, 2.0, 1e-3);
}

TEST_F(SQLiteDatabaseTest, GetNightlyMetrics_NulloptWhenNoSession) {
    EXPECT_FALSE(db_->getNightlyMetrics("NONE", tpFromEpoch(kBaseEpoch)).has_value());
}

TEST_F(SQLiteDatabaseTest, GetMetricsForDateRange_ReturnsCompletedNights) {
    // Use a recent start so it falls within the days_back cutoff window.
    auto start = system_clock::now() - hours(24);
    auto s = makeSession("DEVR", start);
    SessionMetrics m;
    m.total_events = 8;
    s.metrics = m;
    ASSERT_TRUE(db_->saveSession(s));

    // Range query only includes sessions with session_end NOT NULL.
    EXPECT_TRUE(db_->getMetricsForDateRange("DEVR", 7).empty());

    ASSERT_TRUE(db_->markSessionCompleted("DEVR", start));
    auto nights = db_->getMetricsForDateRange("DEVR", 7);
    ASSERT_EQ(nights.size(), 1u);
    EXPECT_EQ(nights[0].total_events, 8);
    EXPECT_FALSE(nights[0].sleep_day.empty());
}

TEST_F(SQLiteDatabaseTest, GetMetricsForDateRange_EmptyForUnknownDevice) {
    EXPECT_TRUE(db_->getMetricsForDateRange("GHOST", 30).empty());
}

// ============================================================================
// Summaries
// ============================================================================

TEST_F(SQLiteDatabaseTest, SaveSummary_Persisted) {
    ASSERT_TRUE(db_->saveSummary("DEVSU", "weekly",
                                 "2026-02-01", "2026-02-07",
                                 7, 2.3, 6.8, 95.0,
                                 "Good week overall."));

    auto rows = db_->executeQuery(
        "SELECT period, nights_count, summary_text, avg_ahi "
        "FROM cpap_summaries WHERE device_id = ?", {"DEVSU"});
    ASSERT_EQ(rows.size(), 1u);
    EXPECT_EQ(rows[0]["period"].asString(), "weekly");
    EXPECT_EQ(rows[0]["nights_count"].asString(), "7");
    EXPECT_EQ(rows[0]["summary_text"].asString(), "Good week overall.");
    EXPECT_EQ(rows[0]["avg_ahi"].asString(), "2.3");
}

TEST_F(SQLiteDatabaseTest, SaveSummary_InvalidPeriodRejectedByCheck) {
    // CHECK constraint on period allows only daily/weekly/monthly.
    EXPECT_FALSE(db_->saveSummary("DEVSU2", "yearly",
                                  "2026-01-01", "2026-12-31",
                                  365, 2.0, 7.0, 90.0, "bad"));
}

// ============================================================================
// Oximetry
// ============================================================================

TEST_F(SQLiteDatabaseTest, SaveOximetrySession_AndExists) {
    OximetrySession os;
    os.filename = "O2_20260206.vld";
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

    auto rows = db_->executeQuery(
        "SELECT avg_spo2, min_spo2, valid_samples FROM oximetry_sessions "
        "WHERE filename = ?", {os.filename});
    ASSERT_EQ(rows.size(), 1u);
    EXPECT_EQ(rows[0]["avg_spo2"].asString(), "95.4");
    EXPECT_EQ(rows[0]["valid_samples"].asString(), "1800");

    auto samp = db_->executeQuery("SELECT COUNT(*) AS n FROM oximetry_samples");
    ASSERT_EQ(samp.size(), 1u);
    EXPECT_EQ(samp[0]["n"].asString(), "1");
}

TEST_F(SQLiteDatabaseTest, SaveOximetrySession_UpsertReplacesSamples) {
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

    // Re-save same filename with a single sample -> old samples replaced.
    os.samples = {a};
    ASSERT_TRUE(db_->saveOximetrySession("o2ring", os));

    auto sess = db_->executeQuery("SELECT COUNT(*) AS n FROM oximetry_sessions");
    ASSERT_EQ(sess.size(), 1u);
    EXPECT_EQ(sess[0]["n"].asString(), "1");  // still one session (upsert)

    auto samp = db_->executeQuery("SELECT COUNT(*) AS n FROM oximetry_samples");
    ASSERT_EQ(samp.size(), 1u);
    EXPECT_EQ(samp[0]["n"].asString(), "1");  // samples replaced, not appended
}

TEST_F(SQLiteDatabaseTest, SaveLiveOximetrySample_CreatesAndAppends) {
    EXPECT_TRUE(db_->saveLiveOximetrySample("o2ring", "20260206", 96, 58, 0));
    EXPECT_TRUE(db_->saveLiveOximetrySample("o2ring", "20260206", 95, 60, 1));

    // One live session for the date.
    auto sess = db_->executeQuery(
        "SELECT COUNT(*) AS n FROM oximetry_sessions WHERE filename = ?",
        {"live_20260206.vld"});
    ASSERT_EQ(sess.size(), 1u);
    EXPECT_EQ(sess[0]["n"].asString(), "1");

    // Two live samples appended.
    auto samp = db_->executeQuery(
        "SELECT COUNT(*) AS n FROM oximetry_samples WHERE source = ?", {"live"});
    ASSERT_EQ(samp.size(), 1u);
    EXPECT_EQ(samp[0]["n"].asString(), "2");

    // Session counters updated.
    auto totals = db_->executeQuery(
        "SELECT total_samples, valid_samples FROM oximetry_sessions WHERE filename = ?",
        {"live_20260206.vld"});
    ASSERT_EQ(totals.size(), 1u);
    EXPECT_EQ(totals[0]["total_samples"].asString(), "2");
    EXPECT_EQ(totals[0]["valid_samples"].asString(), "2");
}

// Stub oximetry summary methods return default/not-found values.
TEST_F(SQLiteDatabaseTest, OximetrySummaries_StubsReturnDefaults) {
    auto sum = db_->getOximetrySummary("o2ring", "20260206", "20260207");
    EXPECT_FALSE(sum.found);

    auto range = db_->getOximetryRangeSummary("o2ring", "20260201", "20260207");
    EXPECT_FALSE(range.found);
    EXPECT_EQ(range.nights, 0);

    EXPECT_TRUE(db_->getOximetryNightlySpo2("o2ring", "20260201", "20260207").empty());
}

// ============================================================================
// executeQuery (generic SELECT -> JSON)
// ============================================================================

TEST_F(SQLiteDatabaseTest, ExecuteQuery_ReturnsJsonRows) {
    ASSERT_TRUE(db_->saveSession(makeSession("DEVQ", tpFromEpoch(kBaseEpoch))));

    auto rows = db_->executeQuery(
        "SELECT device_id, data_records FROM cpap_sessions WHERE device_id = ?",
        {"DEVQ"});
    ASSERT_EQ(rows.size(), 1u);
    EXPECT_EQ(rows[0]["device_id"].asString(), "DEVQ");
    EXPECT_EQ(rows[0]["data_records"].asString(), "120");
}

TEST_F(SQLiteDatabaseTest, ExecuteQuery_NullColumnSerializesAsJsonNull) {
    ASSERT_TRUE(db_->saveSession(makeSession("DEVQ2", tpFromEpoch(kBaseEpoch))));
    auto rows = db_->executeQuery(
        "SELECT session_end FROM cpap_sessions WHERE device_id = ?", {"DEVQ2"});
    ASSERT_EQ(rows.size(), 1u);
    EXPECT_TRUE(rows[0]["session_end"].isNull());
}

TEST_F(SQLiteDatabaseTest, ExecuteQuery_BadSqlReturnsEmptyArray) {
    auto rows = db_->executeQuery("SELECT * FROM table_that_does_not_exist");
    EXPECT_TRUE(rows.isArray());
    EXPECT_EQ(rows.size(), 0u);
}
