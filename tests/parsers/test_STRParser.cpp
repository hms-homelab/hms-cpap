/**
 * HMS-CPAP STR Parser Unit Tests
 *
 * Tests STR.edf parsing using real ResMed files (str_current.edf, str_old.edf).
 */

#include <gtest/gtest.h>
#include "parsers/EDFParser.h"
#include <filesystem>
#include <cmath>

using namespace hms_cpap;
using namespace std::chrono;

// Locate fixture files relative to build directory
static std::string findFixture(const std::string& name) {
    // Try paths relative to common build locations
    std::vector<std::string> candidates = {
        "../tests/fixtures/edf/" + name,
        "../../tests/fixtures/edf/" + name,
        "tests/fixtures/edf/" + name,
    };
    for (const auto& p : candidates) {
        if (std::filesystem::exists(p)) return p;
    }
    // Absolute fallback
    std::string abs = "/home/aamat/maestro_hub/projects/hms-cpap/tests/fixtures/edf/" + name;
    if (std::filesystem::exists(abs)) return abs;
    return "";
}

class STRParserTest : public ::testing::Test {
protected:
    std::string current_path;
    std::string old_path;

    void SetUp() override {
        current_path = findFixture("str_current.edf");
        old_path = findFixture("str_old.edf");
    }
};

// --- EDFFile-level tests ---

TEST_F(STRParserTest, OpensSTRFileValidates81Signals) {
    if (current_path.empty()) GTEST_SKIP() << "str_current.edf not found";

    EDFFile edf;
    ASSERT_TRUE(edf.open(current_path));
    EXPECT_EQ(edf.num_signals, 81);
    EXPECT_EQ(static_cast<int>(edf.record_duration), 86400);
    EXPECT_EQ(edf.actual_records, 229);
}

TEST_F(STRParserTest, FindSignalExactDistinguishesMaskOnOff) {
    if (current_path.empty()) GTEST_SKIP() << "str_current.edf not found";

    EDFFile edf;
    ASSERT_TRUE(edf.open(current_path));

    int on_idx = edf.findSignalExact("MaskOn");
    int off_idx = edf.findSignalExact("MaskOff");
    EXPECT_GE(on_idx, 0);
    EXPECT_GE(off_idx, 0);
    EXPECT_NE(on_idx, off_idx);

    // Substring match would fail here (both contain "MaskO")
    int ahi_idx = edf.findSignalExact("AHI");
    EXPECT_GE(ahi_idx, 0);

    // Exact match for nonexistent signal
    EXPECT_EQ(edf.findSignalExact("Nonexistent"), -1);
}

TEST_F(STRParserTest, FindSignalExactDurationDistinct) {
    if (current_path.empty()) GTEST_SKIP() << "str_current.edf not found";

    EDFFile edf;
    ASSERT_TRUE(edf.open(current_path));

    int dur_idx = edf.findSignalExact("Duration");
    int on_dur_idx = edf.findSignalExact("OnDuration");
    EXPECT_GE(dur_idx, 0);
    EXPECT_GE(on_dur_idx, 0);
    EXPECT_NE(dur_idx, on_dur_idx);
}

// --- parseSTRFile tests ---

TEST_F(STRParserTest, ParseCurrentReturnsTherapyDays) {
    if (current_path.empty()) GTEST_SKIP() << "str_current.edf not found";

    auto records = EDFParser::parseSTRFile(current_path, "test_device");
    // Should have therapy days (not all 229 days had therapy)
    EXPECT_GT(records.size(), 0u);
    EXPECT_LE(records.size(), 229u);

    // All returned records should have therapy
    for (const auto& r : records) {
        EXPECT_TRUE(r.hasTherapy());
        EXPECT_GT(r.duration_minutes, 0);
        EXPECT_EQ(r.device_id, "test_device");
    }
}

TEST_F(STRParserTest, ParseOldReturnsTherapyDays) {
    if (old_path.empty()) GTEST_SKIP() << "str_old.edf not found";

    auto records = EDFParser::parseSTRFile(old_path, "test_device");
    EXPECT_GT(records.size(), 0u);
    EXPECT_LE(records.size(), 199u);
}

TEST_F(STRParserTest, LastRecordKnownValues) {
    if (current_path.empty()) GTEST_SKIP() << "str_current.edf not found";

    auto records = EDFParser::parseSTRFile(current_path, "test_device");
    ASSERT_FALSE(records.empty());

    // Last record (record index 228) has known values from our Python analysis
    const auto& last = records.back();
    EXPECT_NEAR(last.ahi, 0.80, 0.01);
    EXPECT_NEAR(last.hi, 0.40, 0.01);
    EXPECT_NEAR(last.oai, 0.40, 0.01);
    EXPECT_NEAR(last.cai, 0.0, 0.01);
    EXPECT_NEAR(last.rin, 0.20, 0.01);
    EXPECT_NEAR(last.csr, 0.0, 0.01);
    EXPECT_NEAR(last.duration_minutes, 270.0, 0.1);
    EXPECT_NEAR(last.patient_hours, 717.0, 0.1);
    EXPECT_EQ(last.mask_events, 2);
    EXPECT_EQ(last.mode, 1);
    EXPECT_NEAR(last.epr_level, 3.0, 0.01);
    EXPECT_NEAR(last.pressure_setting, 10.0, 0.01);
}

TEST_F(STRParserTest, MaskOnOffTimestampConversion) {
    if (current_path.empty()) GTEST_SKIP() << "str_current.edf not found";

    auto records = EDFParser::parseSTRFile(current_path, "test_device");
    ASSERT_FALSE(records.empty());

    const auto& last = records.back();
    // MaskOn=615, MaskOff=885 (minutes since noon)
    // 615 min = 10h15m past noon = 22:15 same day
    // 885 min = 14h45m past noon = 02:45 next day
    ASSERT_GE(last.mask_pairs.size(), 1u);

    auto on_time = system_clock::to_time_t(last.mask_pairs[0].first);
    auto off_time = system_clock::to_time_t(last.mask_pairs[0].second);
    std::tm on_tm = *std::localtime(&on_time);
    std::tm off_tm = *std::localtime(&off_time);

    EXPECT_EQ(on_tm.tm_hour, 22);
    EXPECT_EQ(on_tm.tm_min, 15);
    EXPECT_EQ(off_tm.tm_hour, 2);
    EXPECT_EQ(off_tm.tm_min, 45);
}

TEST_F(STRParserTest, LeakValuesInLPerMin) {
    if (current_path.empty()) GTEST_SKIP() << "str_current.edf not found";

    auto records = EDFParser::parseSTRFile(current_path, "test_device");
    ASSERT_FALSE(records.empty());

    const auto& last = records.back();
    // Raw EDF: Leak.Max = 0.04 L/s -> 2.4 L/min
    EXPECT_NEAR(last.leak_max, 0.04 * 60.0, 0.01);
    // Leak.50 = 0.0 L/s -> 0.0 L/min
    EXPECT_NEAR(last.leak_50, 0.0, 0.01);
}

TEST_F(STRParserTest, PressureValues) {
    if (current_path.empty()) GTEST_SKIP() << "str_current.edf not found";

    auto records = EDFParser::parseSTRFile(current_path, "test_device");
    ASSERT_FALSE(records.empty());

    const auto& last = records.back();
    EXPECT_NEAR(last.blow_press_95, 7.02, 0.01);
    EXPECT_NEAR(last.blow_press_5, 4.50, 0.01);
    EXPECT_NEAR(last.mask_press_50, 4.92, 0.01);
    EXPECT_NEAR(last.mask_press_95, 6.60, 0.01);
    EXPECT_NEAR(last.mask_press_max, 7.08, 0.01);
}

TEST_F(STRParserTest, SpO2NegativeOneIsZero) {
    if (current_path.empty()) GTEST_SKIP() << "str_current.edf not found";

    auto records = EDFParser::parseSTRFile(current_path, "test_device");
    ASSERT_FALSE(records.empty());

    const auto& last = records.back();
    // No oximeter: EDF has -1, should be stored as 0
    EXPECT_EQ(last.spo2_50, 0);
    EXPECT_EQ(last.spo2_95, 0);
    EXPECT_EQ(last.spo2_max, 0);
}

TEST_F(STRParserTest, InvalidFileReturnsEmpty) {
    auto records = EDFParser::parseSTRFile("/nonexistent/file.edf", "test_device");
    EXPECT_TRUE(records.empty());
}
