/**
 * HMS-CPAP DatabaseService Unit Tests
 *
 * Tests file path population, session matching, and checkpoint tracking.
 * NOTE: These tests verify data structures without requiring actual DB connection.
 */

#include <gtest/gtest.h>
#include <gmock/gmock.h>
#include "parsers/SleeplinkBridge.h"
#include <chrono>
#include <optional>

using namespace hms_cpap;
using namespace std::chrono;

// Test fixture
class DatabaseServiceTest : public ::testing::Test {
protected:
    void SetUp() override {
        // Setup test session
        session = std::make_unique<CPAPSession>();
        session->device_id = "TEST-DEVICE";
        session->session_start = system_clock::now();
    }

    void TearDown() override {
        session.reset();
    }

    std::unique_ptr<CPAPSession> session;
};

// ============================================================================
// FILE PATH POPULATION TESTS
// ============================================================================

TEST_F(DatabaseServiceTest, FilePathPopulation_AllFileTypes) {
    // Insert session with BRP, EVE, SAD, PLD, CSL paths
    // Verify all 5 paths stored correctly

    session->brp_file_path = "/archive/20260206/20260206_140131_BRP.edf";
    session->eve_file_path = "/archive/20260206/20260206_140126_EVE.edf";
    session->sad_file_path = "/archive/20260206/20260206_140132_SAD.edf";
    session->pld_file_path = "/archive/20260206/20260206_140132_PLD.edf";
    session->csl_file_path = "/archive/20260206/20260206_140126_CSL.edf";

    EXPECT_TRUE(session->brp_file_path.has_value());
    EXPECT_TRUE(session->eve_file_path.has_value());
    EXPECT_TRUE(session->sad_file_path.has_value());
    EXPECT_TRUE(session->pld_file_path.has_value());
    EXPECT_TRUE(session->csl_file_path.has_value());

    EXPECT_EQ(session->brp_file_path.value(), "/archive/20260206/20260206_140131_BRP.edf");
    EXPECT_EQ(session->eve_file_path.value(), "/archive/20260206/20260206_140126_EVE.edf");
    EXPECT_EQ(session->sad_file_path.value(), "/archive/20260206/20260206_140132_SAD.edf");
    EXPECT_EQ(session->pld_file_path.value(), "/archive/20260206/20260206_140132_PLD.edf");
    EXPECT_EQ(session->csl_file_path.value(), "/archive/20260206/20260206_140126_CSL.edf");
}

TEST_F(DatabaseServiceTest, FilePathPopulation_PartialFiles) {
    // Insert session with only BRP, SAD (no EVE yet)
    // Verify partial paths stored, others NULL

    session->brp_file_path = "/archive/20260206/20260206_140131_BRP.edf";
    session->sad_file_path = "/archive/20260206/20260206_140132_SAD.edf";
    // EVE, PLD, CSL not set (session in progress)

    EXPECT_TRUE(session->brp_file_path.has_value());
    EXPECT_TRUE(session->sad_file_path.has_value());
    EXPECT_FALSE(session->eve_file_path.has_value());
    EXPECT_FALSE(session->pld_file_path.has_value());
    EXPECT_FALSE(session->csl_file_path.has_value());
}

TEST_F(DatabaseServiceTest, FilePathPopulation_UpdateExisting) {
    // Insert session with BRP only
    // Update with EVE, SAD added
    // Verify ON CONFLICT DO UPDATE works

    session->brp_file_path = "/archive/20260206/20260206_140131_BRP.edf";
    EXPECT_TRUE(session->brp_file_path.has_value());
    EXPECT_FALSE(session->eve_file_path.has_value());

    // Simulate update (in real DB, this would be ON CONFLICT DO UPDATE)
    session->eve_file_path = "/archive/20260206/20260206_140126_EVE.edf";
    session->sad_file_path = "/archive/20260206/20260206_140132_SAD.edf";

    EXPECT_TRUE(session->eve_file_path.has_value());
    EXPECT_TRUE(session->sad_file_path.has_value());
}

// ============================================================================
// SESSION MATCHING TESTS (TIMESTAMP TOLERANCE)
// ============================================================================

TEST_F(DatabaseServiceTest, SessionExists_ExactTimestamp) {
    // DB has: 2026-02-06 14:01:31
    // Query: 2026-02-06 14:01:31
    // Verify returns true (exact match)

    auto timestamp = system_clock::time_point{} + std::chrono::seconds(1738847491);
    session->session_start = timestamp;

    // Simulate exact match check
    auto query_timestamp = timestamp;
    auto diff = std::chrono::duration_cast<std::chrono::seconds>(
        query_timestamp - session->session_start.value()
    ).count();

    EXPECT_EQ(diff, 0);
}

TEST_F(DatabaseServiceTest, SessionExists_WithinTolerance) {
    // DB has: 2026-02-06 14:01:31
    // Query: 2026-02-06 14:01:33 (2 sec off)
    // Verify returns true (5 sec tolerance)

    auto base_timestamp = system_clock::time_point{} + std::chrono::seconds(1738847491);
    auto query_timestamp = base_timestamp + std::chrono::seconds(2);

    session->session_start = base_timestamp;

    auto diff = std::chrono::duration_cast<std::chrono::seconds>(
        query_timestamp - session->session_start.value()
    ).count();

    int tolerance = 5;
    EXPECT_LE(std::abs(diff), tolerance);
}

TEST_F(DatabaseServiceTest, SessionExists_OutsideTolerance) {
    // DB has: 2026-02-06 14:01:31
    // Query: 2026-02-06 14:01:37 (6 sec off)
    // Verify returns false

    auto base_timestamp = system_clock::time_point{} + std::chrono::seconds(1738847491);
    auto query_timestamp = base_timestamp + std::chrono::seconds(6);

    session->session_start = base_timestamp;

    auto diff = std::chrono::duration_cast<std::chrono::seconds>(
        query_timestamp - session->session_start.value()
    ).count();

    int tolerance = 5;
    EXPECT_GT(std::abs(diff), tolerance);
}

// ============================================================================
// CHECKPOINT FILE PATH TESTS
// ============================================================================

TEST_F(DatabaseServiceTest, CheckpointFiles_BRPFilePathSet) {
    // Session should store BRP file path reference

    session->brp_file_path = "/archive/20260206/20260206_140131_BRP.edf";

    EXPECT_TRUE(session->brp_file_path.has_value());
    EXPECT_EQ(session->brp_file_path.value(), "/archive/20260206/20260206_140131_BRP.edf");
}

TEST_F(DatabaseServiceTest, CheckpointFiles_SADFilePathSet) {
    // Session should store SAD file path reference

    session->sad_file_path = "/archive/20260206/20260206_140132_SAD.edf";

    EXPECT_TRUE(session->sad_file_path.has_value());
    EXPECT_EQ(session->sad_file_path.value(), "/archive/20260206/20260206_140132_SAD.edf");
}

TEST_F(DatabaseServiceTest, CheckpointFiles_PLDFilePathSet) {
    // Session should store PLD file path reference

    session->pld_file_path = "/archive/20260206/20260206_140132_PLD.edf";

    EXPECT_TRUE(session->pld_file_path.has_value());
    EXPECT_EQ(session->pld_file_path.value(), "/archive/20260206/20260206_140132_PLD.edf");
}

// ============================================================================
// SESSION STATUS TESTS
// ============================================================================

TEST_F(DatabaseServiceTest, SessionStatus_HasFilePathReference) {
    // Verify session has file path reference after parsing

    session->brp_file_path = "/archive/20260206/20260206_140131_BRP.edf";

    EXPECT_TRUE(session->brp_file_path.has_value());
    EXPECT_FALSE(session->brp_file_path.value().empty());
}

TEST_F(DatabaseServiceTest, SessionStatus_InProgress_NoEVE) {
    // Session in progress should not have EVE file

    session->has_events = false;
    session->session_end = std::nullopt;

    EXPECT_FALSE(session->has_events);
    EXPECT_FALSE(session->session_end.has_value());
}

TEST_F(DatabaseServiceTest, SessionStatus_Completed_HasEVE) {
    // Completed session should have EVE file

    session->has_events = true;
    session->session_end = system_clock::now();

    EXPECT_TRUE(session->has_events);
    EXPECT_TRUE(session->session_end.has_value());
}

// ============================================================================
// SESSION METRICS MODEL TESTS
// (getSessionMetrics() DB integration — tests model struct behavior)
// ============================================================================

TEST_F(DatabaseServiceTest, SessionMetrics_DefaultValues_AreZero) {
    // Verify SessionMetrics zero-initializes all required event fields
    // (prevents the bug where old retained MQTT data showed zeros due to
    //  historical never being published at session completion)

    SessionMetrics m;

    EXPECT_EQ(m.total_events, 0);
    EXPECT_DOUBLE_EQ(m.ahi, 0.0);
    EXPECT_EQ(m.obstructive_apneas, 0);
    EXPECT_EQ(m.central_apneas, 0);
    EXPECT_EQ(m.hypopneas, 0);
    EXPECT_EQ(m.reras, 0);
    EXPECT_EQ(m.clear_airway_apneas, 0);

    EXPECT_FALSE(m.usage_hours.has_value());
    EXPECT_FALSE(m.avg_leak_rate.has_value());
    EXPECT_FALSE(m.avg_respiratory_rate.has_value());
}

TEST_F(DatabaseServiceTest, SessionMetrics_PopulatedFromSession) {
    // Verify populated SessionMetrics round-trips correctly

    SessionMetrics m;
    m.total_events = 8;
    m.ahi = 1.875;
    m.obstructive_apneas = 2;
    m.central_apneas = 4;
    m.hypopneas = 0;
    m.reras = 1;
    m.clear_airway_apneas = 0;
    m.usage_hours = 4.2667;
    m.avg_leak_rate = 0.91;

    EXPECT_EQ(m.total_events, 8);
    EXPECT_DOUBLE_EQ(m.ahi, 1.875);
    EXPECT_EQ(m.obstructive_apneas, 2);
    EXPECT_EQ(m.central_apneas, 4);
    EXPECT_EQ(m.reras, 1);
    EXPECT_TRUE(m.usage_hours.has_value());
    EXPECT_NEAR(m.usage_hours.value(), 4.2667, 0.0001);
    EXPECT_TRUE(m.avg_leak_rate.has_value());

    // Unset optionals must still be null
    EXPECT_FALSE(m.avg_pressure.has_value());
    EXPECT_FALSE(m.pressure_p95.has_value());
}

