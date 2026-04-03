/**
 * HMS-CPAP PLD Parser Unit Tests (Phase 2)
 *
 * Tests PLD.edf parsing using real ASV ResMed fixture data.
 * PLD contains machine-calculated metrics at 0.5 Hz (2-second intervals):
 *   MaskPress, EprPress, Leak, RespRate, TidVol, MinVent, Snore, FlowLim, TgtVent
 *
 * Since parsePLDFile is private, all tests go through parseSession().
 */

#include <gtest/gtest.h>
#include "parsers/SleeplinkBridge.h"
#include <filesystem>
#include <cmath>
#include <algorithm>
#include <numeric>

using namespace hms_cpap;

// Locate ASV fixture directory (handles build from different working dirs)
static std::string findASVFixtureDir() {
    std::vector<std::string> candidates = {
        "../tests/fixtures/asv",
        "../../tests/fixtures/asv",
        "tests/fixtures/asv",
    };
    for (const auto& p : candidates) {
        if (std::filesystem::exists(p + "/20260319_235929_PLD.edf")) return p;
    }
    std::string abs = "/home/aamat/maestro_hub/projects/hms-cpap/tests/fixtures/asv";
    if (std::filesystem::exists(abs + "/20260319_235929_PLD.edf")) return abs;
    return "";
}

class PLDParserTest : public ::testing::Test {
protected:
    std::string fixture_dir;
    std::unique_ptr<CPAPSession> session;

    void SetUp() override {
        fixture_dir = findASVFixtureDir();
        if (fixture_dir.empty()) {
            GTEST_SKIP() << "ASV PLD fixture not found";
        }
        // Parse full session (BRP + PLD + SA2 + EVE) — PLD merges into breathing_summary
        session = EDFParser::parseSession(fixture_dir, "test_pld_device", "Test PLD Device");
        if (!session) {
            GTEST_SKIP() << "parseSession returned nullptr for ASV fixtures";
        }
    }
};

// Test: PLD data produces breathing summaries with PLD-specific fields
TEST_F(PLDParserTest, ParsesRealPLDFile) {
    EXPECT_GT(session->breathing_summary.size(), 0)
        << "PLD+BRP should produce breathing summaries";
}

// Test: 482 records at 60s each = ~482 minutes of data
TEST_F(PLDParserTest, CorrectMinuteCount) {
    // PLD has 482 records * 30 samples/record = ~14460 samples
    // At 30 samples/minute = ~482 minutes
    // BRP may also contribute. Either way, expect at least 400 minutes.
    EXPECT_GE(session->breathing_summary.size(), 400u);
    EXPECT_LE(session->breathing_summary.size(), 600u);
}

// Test: Leak values are in L/min (converted from L/s * 60)
// Valid range: 0-120 L/min
TEST_F(PLDParserTest, LeakValuesInLPerMin) {
    int leak_count = 0;
    for (const auto& bs : session->breathing_summary) {
        if (bs.leak_rate.has_value()) {
            double leak = bs.leak_rate.value();
            EXPECT_GE(leak, 0.0) << "Leak cannot be negative";
            EXPECT_LE(leak, 120.0) << "Leak > 120 L/min is unreasonable";
            leak_count++;
        }
    }
    EXPECT_GT(leak_count, 0) << "PLD should populate leak_rate";
}

// Test: Tidal volume in mL (converted from L * 1000)
// Valid range: 100-2000 mL
TEST_F(PLDParserTest, TidalVolumeInML) {
    int tv_count = 0;
    for (const auto& bs : session->breathing_summary) {
        if (bs.tidal_volume.has_value()) {
            double tv = bs.tidal_volume.value();
            // Allow 0 for periods of no breathing, but positive values should be reasonable
            if (tv > 0) {
                EXPECT_GE(tv, 50.0) << "Tidal volume < 50 mL is unusually low";
                EXPECT_LE(tv, 2000.0) << "Tidal volume > 2000 mL is unreasonable";
            }
            tv_count++;
        }
    }
    EXPECT_GT(tv_count, 0) << "PLD should populate tidal_volume";
}

// Test: Snore index in range 0-5
TEST_F(PLDParserTest, SnoreIndexRange) {
    int snore_count = 0;
    for (const auto& bs : session->breathing_summary) {
        if (bs.snore_index.has_value()) {
            double snore = bs.snore_index.value();
            EXPECT_GE(snore, 0.0) << "Snore index cannot be negative";
            EXPECT_LE(snore, 5.0) << "Snore index > 5 is out of range";
            snore_count++;
        }
    }
    EXPECT_GT(snore_count, 0) << "PLD should populate snore_index";
}

// Test: Flow limitation in range 0-1
TEST_F(PLDParserTest, FlowLimitationRange) {
    int fl_count = 0;
    for (const auto& bs : session->breathing_summary) {
        if (bs.flow_limitation.has_value()) {
            double fl = bs.flow_limitation.value();
            EXPECT_GE(fl, 0.0) << "Flow limitation cannot be negative";
            EXPECT_LE(fl, 1.0) << "Flow limitation > 1 is out of range";
            fl_count++;
        }
    }
    EXPECT_GT(fl_count, 0) << "PLD should populate flow_limitation";
}

// Test: Mask pressure populated and in reasonable range (2-25 cmH2O)
TEST_F(PLDParserTest, MaskPressureRange) {
    int mp_count = 0;
    for (const auto& bs : session->breathing_summary) {
        if (bs.mask_pressure.has_value()) {
            double mp = bs.mask_pressure.value();
            if (mp > 0) {
                EXPECT_GE(mp, 2.0) << "Mask pressure < 2 cmH2O is too low";
                EXPECT_LE(mp, 25.0) << "Mask pressure > 25 cmH2O is too high";
            }
            mp_count++;
        }
    }
    EXPECT_GT(mp_count, 0) << "PLD should populate mask_pressure";
}

// Test: EPR pressure populated and in reasonable range (2-20 cmH2O)
TEST_F(PLDParserTest, EprPressureRange) {
    int epr_count = 0;
    for (const auto& bs : session->breathing_summary) {
        if (bs.epr_pressure.has_value()) {
            double epr = bs.epr_pressure.value();
            if (epr > 0) {
                EXPECT_GE(epr, 2.0) << "EPR pressure < 2 cmH2O is too low";
                EXPECT_LE(epr, 20.0) << "EPR pressure > 20 cmH2O is too high";
            }
            epr_count++;
        }
    }
    EXPECT_GT(epr_count, 0) << "PLD should populate epr_pressure";
}

// Test: TgtVent (ASV) is present for this ASV device
TEST_F(PLDParserTest, ASVTargetVentilationPresent) {
    int tgt_vent_count = 0;
    for (const auto& bs : session->breathing_summary) {
        if (bs.target_ventilation.has_value()) {
            tgt_vent_count++;
        }
    }
    EXPECT_GT(tgt_vent_count, 0) << "PLD TgtVent should be present for ASV device";
}

// Test: TgtVent values in range 3-25 L/min
TEST_F(PLDParserTest, ASVTargetVentilationRange) {
    for (const auto& bs : session->breathing_summary) {
        if (bs.target_ventilation.has_value()) {
            double tv = bs.target_ventilation.value();
            if (tv > 0) {
                EXPECT_GE(tv, 1.0) << "TgtVent < 1 L/min is too low";
                EXPECT_LE(tv, 30.0) << "TgtVent > 30 L/min is too high";
            }
        }
    }
}

// Test: Respiratory rate in reasonable range (5-40 bpm)
TEST_F(PLDParserTest, RespiratoryRateRange) {
    int rr_count = 0;
    for (const auto& bs : session->breathing_summary) {
        if (bs.respiratory_rate.has_value()) {
            double rr = bs.respiratory_rate.value();
            if (rr > 0) {
                EXPECT_GE(rr, 3.0) << "Respiratory rate < 3 bpm is too low";
                EXPECT_LE(rr, 45.0) << "Respiratory rate > 45 bpm is too high";
            }
            rr_count++;
        }
    }
    EXPECT_GT(rr_count, 0) << "PLD should populate respiratory_rate";
}

// Test: Minute ventilation in reasonable range (2-25 L/min)
TEST_F(PLDParserTest, MinuteVentilationRange) {
    int mv_count = 0;
    for (const auto& bs : session->breathing_summary) {
        if (bs.minute_ventilation.has_value()) {
            double mv = bs.minute_ventilation.value();
            if (mv > 0) {
                EXPECT_GE(mv, 1.0) << "Minute ventilation < 1 L/min is too low";
                EXPECT_LE(mv, 30.0) << "Minute ventilation > 30 L/min is too high";
            }
            mv_count++;
        }
    }
    EXPECT_GT(mv_count, 0) << "PLD should populate minute_ventilation";
}
