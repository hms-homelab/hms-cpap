/**
 * HMS-CPAP SA2 Parser Unit Tests (Phase 1)
 *
 * Tests SA2.edf (oximetry) parsing using real ASV ResMed fixture data.
 * SA2 files contain SpO2 @ 1 Hz and Pulse @ 1 Hz.
 * The parser handles SA2 via the same parseSADFile path (filename match: _sa2.edf).
 *
 * NOTE: The ASV fixture SA2 has all -1 values (no oximeter connected during
 * this recording). Tests verify the parser handles this gracefully — invalid
 * readings are filtered out, vitals entries exist but have no SpO2/Pulse values.
 *
 * Since parseSADFile is private, tests go through parseSession().
 */

#include <gtest/gtest.h>
#include "parsers/EDFParser.h"
#include "models/CPAPModels.h"
#include <filesystem>
#include <cmath>

using namespace hms_cpap;

static std::string findASVFixtureDir() {
    std::vector<std::string> candidates = {
        "../tests/fixtures/asv",
        "../../tests/fixtures/asv",
        "tests/fixtures/asv",
    };
    for (const auto& p : candidates) {
        if (std::filesystem::exists(p + "/20260319_235929_SA2.edf")) return p;
    }
    std::string abs = "/home/aamat/maestro_hub/projects/hms-cpap/tests/fixtures/asv";
    if (std::filesystem::exists(abs + "/20260319_235929_SA2.edf")) return abs;
    return "";
}

class SA2ParserTest : public ::testing::Test {
protected:
    std::string fixture_dir;
    std::unique_ptr<CPAPSession> session;

    void SetUp() override {
        fixture_dir = findASVFixtureDir();
        if (fixture_dir.empty()) {
            GTEST_SKIP() << "ASV SA2 fixture not found";
        }
        session = EDFParser::parseSession(fixture_dir, "test_sa2_device", "Test SA2 Device");
        if (!session) {
            GTEST_SKIP() << "parseSession returned nullptr for ASV fixtures";
        }
    }
};

// Test: SA2 file parsed — vitals entries should be created even without valid data
TEST_F(SA2ParserTest, ParsesAsSADProducesVitals) {
    EXPECT_GT(session->vitals.size(), 0u)
        << "SA2 should produce vitals entries via parseSADFile path";
}

// Test: SpO2 values filtered correctly — this fixture has no oximeter (all -1)
// The parser filters val <= 0 and val > 100, so no spo2 values should be set
TEST_F(SA2ParserTest, SpO2InvalidValuesFiltered) {
    int spo2_count = 0;
    for (const auto& v : session->vitals) {
        if (v.spo2.has_value()) {
            double spo2 = v.spo2.value();
            // Any value that made it through filtering must be valid
            EXPECT_GT(spo2, 0.0) << "Filtered SpO2 should be > 0";
            EXPECT_LE(spo2, 100.0) << "SpO2 cannot exceed 100%";
            spo2_count++;
        }
    }
    // This fixture has no oximeter — all values are -1, so 0 valid readings expected
    EXPECT_EQ(spo2_count, 0)
        << "SA2 fixture has no oximeter (all -1), so no valid SpO2 readings expected";
}

// Test: Pulse values filtered correctly — this fixture has no oximeter (all -1)
TEST_F(SA2ParserTest, PulseInvalidValuesFiltered) {
    int pulse_count = 0;
    for (const auto& v : session->vitals) {
        if (v.heart_rate.has_value()) {
            int hr = v.heart_rate.value();
            // Any value that made it through filtering must be valid
            EXPECT_GT(hr, 0) << "Filtered heart rate should be > 0";
            EXPECT_LT(hr, 300) << "Heart rate >= 300 is unreasonable";
            pulse_count++;
        }
    }
    // This fixture has no oximeter — all values are -1, so 0 valid readings expected
    EXPECT_EQ(pulse_count, 0)
        << "SA2 fixture has no oximeter (all -1), so no valid pulse readings expected";
}

// Test: Vitals entries exist but have nullopt for SpO2 and heart_rate (no oximeter)
TEST_F(SA2ParserTest, NoOximeterAllNullopt) {
    int null_spo2 = 0;
    int null_hr = 0;
    for (const auto& v : session->vitals) {
        if (!v.spo2.has_value()) null_spo2++;
        if (!v.heart_rate.has_value()) null_hr++;
    }
    // All entries should have nullopt since fixture has no valid data
    EXPECT_EQ(null_spo2, static_cast<int>(session->vitals.size()))
        << "All SpO2 should be nullopt for no-oximeter fixture";
    EXPECT_EQ(null_hr, static_cast<int>(session->vitals.size()))
        << "All heart_rate should be nullopt for no-oximeter fixture";
}

// Test: Sample count matches expected
// 482 records * 60 samples/record = ~28920 seconds of data
TEST_F(SA2ParserTest, SampleCountReasonable) {
    // SA2 at 1 Hz, 482 records * 60s/record = ~28920 samples
    EXPECT_GE(session->vitals.size(), 25000u)
        << "Expected at least 25000 vitals samples from 482-record SA2";
    EXPECT_LE(session->vitals.size(), 35000u)
        << "Vitals count should not wildly exceed 482 records * 60 samples";
}

// Test: Timestamps are monotonically increasing
TEST_F(SA2ParserTest, TimestampsMonotonic) {
    if (session->vitals.size() < 2) GTEST_SKIP() << "Need at least 2 vitals";

    for (size_t i = 1; i < session->vitals.size(); ++i) {
        EXPECT_GE(session->vitals[i].timestamp, session->vitals[i-1].timestamp)
            << "Vitals timestamps should be monotonically non-decreasing at index " << i;
    }
}
