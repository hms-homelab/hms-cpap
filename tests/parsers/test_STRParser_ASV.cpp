/**
 * HMS-CPAP STR Parser ASV Unit Tests (Phase 3)
 *
 * Tests STR.edf parsing for ASV devices (Mode=7) using real fixture data.
 * The ASV STR fixture has 74 signals, 7 records (7 days), MID=46.
 */

#include <gtest/gtest.h>
#include "parsers/EDFParser.h"
#include "models/CPAPModels.h"
#include <filesystem>
#include <cmath>

using namespace hms_cpap;

static std::string findASVSTRFixture() {
    std::vector<std::string> candidates = {
        "../tests/fixtures/asv/STR.edf",
        "../../tests/fixtures/asv/STR.edf",
        "tests/fixtures/asv/STR.edf",
    };
    for (const auto& p : candidates) {
        if (std::filesystem::exists(p)) return p;
    }
    std::string abs = "/home/aamat/maestro_hub/projects/hms-cpap/tests/fixtures/asv/STR.edf";
    if (std::filesystem::exists(abs)) return abs;
    return "";
}

class STRParserASVTest : public ::testing::Test {
protected:
    std::string str_fixture;
    std::vector<STRDailyRecord> records;

    void SetUp() override {
        str_fixture = findASVSTRFixture();
        if (str_fixture.empty()) {
            GTEST_SKIP() << "ASV STR fixture not found";
        }
        records = EDFParser::parseSTRFile(str_fixture, "test_asv_device");
    }
};

// Test: Parses ASV STR file successfully
TEST_F(STRParserASVTest, ParsesSuccessfully) {
    EXPECT_GT(records.size(), 0u) << "ASV STR should have therapy days";
}

// Test: 7 records (7 days of data in fixture)
TEST_F(STRParserASVTest, SevenDaysOfData) {
    // STR fixture has 7 records total; only days with therapy are returned
    EXPECT_LE(records.size(), 7u) << "Cannot have more than 7 therapy days from 7-record STR";
    EXPECT_GE(records.size(), 1u) << "Should have at least 1 therapy day";
}

// Test: Mode is 7 (ASV) for therapy days
TEST_F(STRParserASVTest, ModeIsASV) {
    for (const auto& r : records) {
        if (r.duration_minutes > 0) {
            EXPECT_EQ(r.mode, 7) << "ASV device should have mode=7";
        }
    }
}

// Test: ASV settings populated (S.AV.EPAP, S.AV.MaxPS, etc.)
TEST_F(STRParserASVTest, ASVSettingsPopulated) {
    bool found_asv_settings = false;
    for (const auto& r : records) {
        if (r.duration_minutes > 0 && r.asv_epap.has_value()) {
            found_asv_settings = true;
            EXPECT_GT(r.asv_epap.value(), 0.0)
                << "ASV EPAP should be > 0 cmH2O";
            EXPECT_LE(r.asv_epap.value(), 25.0)
                << "ASV EPAP should be <= 25 cmH2O";

            EXPECT_TRUE(r.asv_max_ps.has_value())
                << "ASV MaxPS should be populated alongside EPAP";
            if (r.asv_max_ps.has_value()) {
                EXPECT_GT(r.asv_max_ps.value(), 0.0)
                    << "ASV MaxPS should be > 0 cmH2O";
                EXPECT_LE(r.asv_max_ps.value(), 25.0)
                    << "ASV MaxPS should be <= 25 cmH2O";
            }
        }
    }
    EXPECT_TRUE(found_asv_settings)
        << "At least one therapy day should have ASV settings populated";
}

// Test: Target IPAP/EPAP/Vent percentiles populated
TEST_F(STRParserASVTest, TargetPercentilesPopulated) {
    bool found_targets = false;
    for (const auto& r : records) {
        if (r.duration_minutes > 0 && r.tgt_ipap_50.has_value()) {
            found_targets = true;

            EXPECT_GT(r.tgt_ipap_50.value(), 0.0)
                << "TgtIPAP.50 should be > 0 cmH2O";
            EXPECT_LE(r.tgt_ipap_50.value(), 30.0)
                << "TgtIPAP.50 should be <= 30 cmH2O";

            if (r.tgt_epap_50.has_value()) {
                EXPECT_GT(r.tgt_epap_50.value(), 0.0)
                    << "TgtEPAP.50 should be > 0 cmH2O";
                // EPAP should be <= IPAP
                EXPECT_LE(r.tgt_epap_50.value(), r.tgt_ipap_50.value())
                    << "TgtEPAP.50 should be <= TgtIPAP.50";
            }

            if (r.tgt_vent_50.has_value()) {
                EXPECT_GT(r.tgt_vent_50.value(), 0.0)
                    << "TgtVent.50 should be > 0 L/min";
                EXPECT_LE(r.tgt_vent_50.value(), 25.0)
                    << "TgtVent.50 should be <= 25 L/min";
            }
        }
    }
    EXPECT_TRUE(found_targets)
        << "At least one therapy day should have target percentiles";
}

// Test: AHI values are reasonable
TEST_F(STRParserASVTest, AHIReasonable) {
    for (const auto& r : records) {
        EXPECT_GE(r.ahi, 0.0) << "AHI cannot be negative";
        EXPECT_LE(r.ahi, 100.0) << "AHI > 100 is unreasonable";
    }
}

// Test: Duration is reasonable (therapy sessions 1-16 hours)
TEST_F(STRParserASVTest, DurationReasonable) {
    for (const auto& r : records) {
        EXPECT_GT(r.duration_minutes, 0.0) << "Therapy days should have positive duration";
        EXPECT_LE(r.duration_minutes, 960.0) << "Duration > 16 hours is unreasonable";
    }
}

// Test: Pressure values in reasonable range
TEST_F(STRParserASVTest, PressureValuesReasonable) {
    for (const auto& r : records) {
        if (r.mask_press_50 > 0) {
            EXPECT_GE(r.mask_press_50, 2.0) << "Mask pressure < 2 cmH2O is too low";
            EXPECT_LE(r.mask_press_50, 25.0) << "Mask pressure > 25 cmH2O is too high";
        }
        if (r.mask_press_95 > 0) {
            EXPECT_GE(r.mask_press_95, r.mask_press_50)
                << "95th percentile should be >= 50th percentile";
        }
    }
}

// Test: Leak values converted to L/min
TEST_F(STRParserASVTest, LeakValuesConverted) {
    for (const auto& r : records) {
        EXPECT_GE(r.leak_50, 0.0) << "Leak cannot be negative";
        EXPECT_LE(r.leak_max, 180.0) << "Leak max > 180 L/min is extreme";
        if (r.leak_95 > 0 && r.leak_50 > 0) {
            EXPECT_GE(r.leak_95, r.leak_50)
                << "95th percentile leak should be >= 50th";
        }
    }
}

// Test: Device ID is set correctly
TEST_F(STRParserASVTest, DeviceIdSet) {
    for (const auto& r : records) {
        EXPECT_EQ(r.device_id, "test_asv_device");
    }
}

// Test: Invalid file returns empty
TEST_F(STRParserASVTest, InvalidFileReturnsEmpty) {
    auto empty_records = EDFParser::parseSTRFile("/nonexistent/asv_str.edf", "test");
    EXPECT_TRUE(empty_records.empty());
}
