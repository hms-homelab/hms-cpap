/**
 * HMS-CPAP ASV Integration Tests
 *
 * End-to-end test that runs the full parseSession() pipeline on real ASV data:
 *   BRP (breathing waveforms) + PLD (machine metrics) + SA2 (oximetry) + EVE (events)
 *
 * Also tests STR parsing for ASV device summary data.
 */

#include <gtest/gtest.h>
#include "parsers/CpapdashBridge.h"
#include <filesystem>
#include <cmath>
#include <algorithm>

using namespace hms_cpap;

static std::string findASVFixtureDir() {
    std::vector<std::string> candidates = {
        "../tests/fixtures/asv",
        "../../tests/fixtures/asv",
        "tests/fixtures/asv",
    };
    for (const auto& p : candidates) {
        if (std::filesystem::exists(p + "/20260319_235929_BRP.edf")) return p;
    }
    std::string abs = "/home/aamat/maestro_hub/projects/hms-cpap/tests/fixtures/asv";
    if (std::filesystem::exists(abs + "/20260319_235929_BRP.edf")) return abs;
    return "";
}

class ASVIntegrationTest : public ::testing::Test {
protected:
    std::string fixture_dir;

    void SetUp() override {
        fixture_dir = findASVFixtureDir();
        if (fixture_dir.empty()) {
            GTEST_SKIP() << "ASV fixtures not found";
        }
    }
};

// Full pipeline: parseSession with BRP + PLD + SA2 + EVE
TEST_F(ASVIntegrationTest, FullSessionParse) {
    auto session = EDFParser::parseSession(fixture_dir, "test_asv", "Test ASV Device");
    ASSERT_NE(session, nullptr) << "parseSession should succeed for ASV fixture dir";

    // BRP: breathing summaries present
    EXPECT_GT(session->breathing_summary.size(), 0u)
        << "BRP should produce breathing summaries";

    // SA2: vitals present (SpO2/Pulse)
    EXPECT_GT(session->vitals.size(), 0u)
        << "SA2 should produce vitals (SpO2/Pulse)";

    // EVE: events present
    EXPECT_TRUE(session->has_events)
        << "EVE file should mark has_events=true";

    // PLD: mask_pressure populated in at least some summaries
    int mask_press_count = 0;
    int snore_count = 0;
    int tgt_vent_count = 0;
    int leak_count = 0;
    int rr_count = 0;
    int tv_count = 0;
    int mv_count = 0;

    for (const auto& bs : session->breathing_summary) {
        if (bs.mask_pressure.has_value()) mask_press_count++;
        if (bs.snore_index.has_value()) snore_count++;
        if (bs.target_ventilation.has_value()) tgt_vent_count++;
        if (bs.leak_rate.has_value()) leak_count++;
        if (bs.respiratory_rate.has_value()) rr_count++;
        if (bs.tidal_volume.has_value()) tv_count++;
        if (bs.minute_ventilation.has_value()) mv_count++;
    }

    EXPECT_GT(mask_press_count, 0) << "PLD mask_pressure should be populated";
    EXPECT_GT(snore_count, 0) << "PLD snore_index should be populated";
    EXPECT_GT(tgt_vent_count, 0) << "PLD TgtVent should be populated (ASV device)";
    EXPECT_GT(leak_count, 0) << "PLD leak_rate should be populated";
    EXPECT_GT(rr_count, 0) << "PLD respiratory_rate should be populated";
    EXPECT_GT(tv_count, 0) << "PLD tidal_volume should be populated";
    EXPECT_GT(mv_count, 0) << "PLD minute_ventilation should be populated";
}

// Test: calculateMetrics aggregates PLD and SA2 data correctly
TEST_F(ASVIntegrationTest, MetricsCalculation) {
    auto session = EDFParser::parseSession(fixture_dir, "test_asv", "Test ASV Device");
    ASSERT_NE(session, nullptr);

    session->calculateMetrics();
    ASSERT_TRUE(session->metrics.has_value()) << "Metrics should be computed";

    const auto& m = session->metrics.value();

    // PLD-derived averages should be populated
    EXPECT_TRUE(m.avg_mask_pressure.has_value()) << "avg_mask_pressure should be set";
    EXPECT_TRUE(m.avg_snore.has_value()) << "avg_snore should be set";
    EXPECT_TRUE(m.avg_target_ventilation.has_value())
        << "avg_target_ventilation should be set for ASV device";
    EXPECT_TRUE(m.avg_leak_rate.has_value()) << "avg_leak_rate should be set";
    EXPECT_TRUE(m.leak_p50.has_value()) << "leak_p50 should be set";

    // Validate metric ranges
    if (m.avg_mask_pressure.has_value()) {
        EXPECT_GE(m.avg_mask_pressure.value(), 2.0);
        EXPECT_LE(m.avg_mask_pressure.value(), 25.0);
    }
    if (m.avg_target_ventilation.has_value()) {
        EXPECT_GE(m.avg_target_ventilation.value(), 1.0);
        EXPECT_LE(m.avg_target_ventilation.value(), 25.0);
    }
    if (m.avg_leak_rate.has_value()) {
        EXPECT_GE(m.avg_leak_rate.value(), 0.0);
        EXPECT_LE(m.avg_leak_rate.value(), 120.0);
    }

    // SpO2/HR from SA2
    if (m.avg_spo2.has_value()) {
        EXPECT_GE(m.avg_spo2.value(), 60.0);
        EXPECT_LE(m.avg_spo2.value(), 100.0);
    }
    if (m.avg_heart_rate.has_value()) {
        EXPECT_GE(m.avg_heart_rate.value(), 25);
        EXPECT_LE(m.avg_heart_rate.value(), 200);
    }
}

// Test: Session has device info from EDF headers
TEST_F(ASVIntegrationTest, DeviceInfo) {
    auto session = EDFParser::parseSession(fixture_dir, "test_asv", "Test ASV Device");
    ASSERT_NE(session, nullptr);

    EXPECT_EQ(session->device_id, "test_asv");
    EXPECT_EQ(session->device_name, "Test ASV Device");
    // Serial number should be parsed from EDF recording field
    EXPECT_FALSE(session->serial_number.empty())
        << "Serial number should be parsed from EDF header";
}

// Test: BRP and PLD minute counts are consistent
TEST_F(ASVIntegrationTest, BRPAndPLDMinuteConsistency) {
    auto session = EDFParser::parseSession(fixture_dir, "test_asv", "Test ASV Device");
    ASSERT_NE(session, nullptr);

    // Count how many breathing summaries have PLD data vs BRP data
    int with_mask_press = 0;
    int with_avg_flow = 0;
    for (const auto& bs : session->breathing_summary) {
        if (bs.mask_pressure.has_value()) with_mask_press++;
        if (bs.avg_flow_rate != 0) with_avg_flow++;
    }

    // Both BRP and PLD have 482 records each, so they should overlap substantially
    EXPECT_GT(with_mask_press, 300)
        << "PLD should populate mask_pressure for most minutes";
    EXPECT_GT(with_avg_flow, 300)
        << "BRP should populate flow data for most minutes";
}

// STR parsing for ASV device
TEST_F(ASVIntegrationTest, STRParsing) {
    std::string str_path = fixture_dir + "/STR.edf";
    if (!std::filesystem::exists(str_path)) {
        GTEST_SKIP() << "ASV STR fixture not found";
    }

    auto records = EDFParser::parseSTRFile(str_path, "test_asv");
    ASSERT_GT(records.size(), 0u) << "STR should return therapy days";

    // All therapy days should be Mode=7 (ASV)
    for (const auto& r : records) {
        if (r.duration_minutes > 0) {
            EXPECT_EQ(r.mode, 7) << "ASV device STR should have mode=7";
            EXPECT_TRUE(r.asv_epap.has_value())
                << "ASV device STR should have asv_epap populated";
        }
    }
}

// Test: Session timestamps are reasonable
TEST_F(ASVIntegrationTest, SessionTimestamps) {
    auto session = EDFParser::parseSession(fixture_dir, "test_asv", "Test ASV Device");
    ASSERT_NE(session, nullptr);

    // Breathing summary timestamps should span hours (not seconds)
    if (session->breathing_summary.size() >= 2) {
        auto first_ts = session->breathing_summary.front().timestamp;
        auto last_ts = session->breathing_summary.back().timestamp;
        auto span = std::chrono::duration_cast<std::chrono::hours>(last_ts - first_ts);
        EXPECT_GE(span.count(), 4)
            << "482-minute session should span at least 4 hours";
    }
}

// Test: Vitals timestamps span the same period as breathing data
TEST_F(ASVIntegrationTest, VitalsTimestampSpan) {
    auto session = EDFParser::parseSession(fixture_dir, "test_asv", "Test ASV Device");
    ASSERT_NE(session, nullptr);

    if (session->vitals.size() >= 2 && session->breathing_summary.size() >= 2) {
        auto vitals_first = session->vitals.front().timestamp;
        auto brp_first = session->breathing_summary.front().timestamp;

        // Vitals should start approximately when breathing data starts (within 5 min)
        auto start_diff = std::chrono::duration_cast<std::chrono::minutes>(
            vitals_first > brp_first ? vitals_first - brp_first : brp_first - vitals_first
        );
        EXPECT_LE(start_diff.count(), 5)
            << "SA2 and BRP should start within 5 minutes of each other";
    }
}
