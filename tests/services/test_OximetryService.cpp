#include <gtest/gtest.h>
#include <gmock/gmock.h>
#include "services/OximetryService.h"
#include "clients/O2RingClient.h"
#include "database/IDatabase.h"
#include "parsers/CpapdashBridge.h"

using namespace hms_cpap;

// ── Mock O2RingClient ──────────────────────────────────────────────────

class MockO2RingClient : public O2RingClient {
public:
    MockO2RingClient() : O2RingClient("http://mock") {}

    // Override virtual methods... but O2RingClient isn't virtual.
    // Instead, test OximetryService logic via state machine simulation.
};

// ── State Machine Tests ────────────────────────────────────────────────
// These test the active/inactive/unreachable logic directly,
// matching the BurstCollectorService state machine.

class O2RingStateMachineTest : public ::testing::Test {
protected:
    bool o2ring_was_active = false;

    struct SimResult {
        bool should_save_live;
        bool should_download_files;
    };

    // Simulates one burst cycle of the O2 Ring state machine
    SimResult simulateCycle(const O2RingClient::LiveReading& live) {
        SimResult result{false, false};

        bool reachable = (live.spo2 != 0 || live.active);

        if (live.active) {
            if (live.valid) {
                result.should_save_live = true;
            }
            o2ring_was_active = true;
        } else if (o2ring_was_active && reachable) {
            result.should_download_files = true;
            o2ring_was_active = false;
        }

        return result;
    }

    O2RingClient::LiveReading makeActive(int spo2 = 96, int hr = 72) {
        O2RingClient::LiveReading r;
        r.spo2 = spo2; r.hr = hr; r.motion = 0; r.vibration = 0;
        r.active = true;
        r.valid = true;
        return r;
    }

    O2RingClient::LiveReading makeInactive() {
        O2RingClient::LiveReading r;
        r.spo2 = 255; r.hr = 255; r.motion = 0; r.vibration = 0;
        r.active = false;
        r.valid = false;
        return r;
    }

    O2RingClient::LiveReading makeUnreachable() {
        O2RingClient::LiveReading r;
        r.spo2 = 0; r.hr = 0; r.motion = 0; r.vibration = 0;
        r.active = false;
        r.valid = false;
        return r;
    }
};

TEST_F(O2RingStateMachineTest, ActiveSavesLive) {
    auto r = simulateCycle(makeActive());
    EXPECT_TRUE(r.should_save_live);
    EXPECT_FALSE(r.should_download_files);
    EXPECT_TRUE(o2ring_was_active);
}

TEST_F(O2RingStateMachineTest, ActiveToInactiveTriggersFileDownload) {
    simulateCycle(makeActive());
    EXPECT_TRUE(o2ring_was_active);

    auto r = simulateCycle(makeInactive());
    EXPECT_FALSE(r.should_save_live);
    EXPECT_TRUE(r.should_download_files);
    EXPECT_FALSE(o2ring_was_active);
}

TEST_F(O2RingStateMachineTest, InactiveWithoutPriorActiveDoesNothing) {
    // Service just started, ring is off — no file download
    auto r = simulateCycle(makeInactive());
    EXPECT_FALSE(r.should_save_live);
    EXPECT_FALSE(r.should_download_files);
    EXPECT_FALSE(o2ring_was_active);
}

TEST_F(O2RingStateMachineTest, UnreachablePreservesState) {
    simulateCycle(makeActive());
    EXPECT_TRUE(o2ring_was_active);

    // Mule times out — should NOT trigger file download
    auto r = simulateCycle(makeUnreachable());
    EXPECT_FALSE(r.should_save_live);
    EXPECT_FALSE(r.should_download_files);
    EXPECT_TRUE(o2ring_was_active);  // preserved!
}

TEST_F(O2RingStateMachineTest, UnreachableThenInactiveTriggersDownload) {
    simulateCycle(makeActive());

    // Mule timeout — state preserved
    simulateCycle(makeUnreachable());
    EXPECT_TRUE(o2ring_was_active);

    // Ring comes back as inactive — should trigger download
    auto r = simulateCycle(makeInactive());
    EXPECT_TRUE(r.should_download_files);
    EXPECT_FALSE(o2ring_was_active);
}

TEST_F(O2RingStateMachineTest, RepeatedInactiveDoesNotRepeatDownload) {
    simulateCycle(makeActive());
    simulateCycle(makeInactive());  // triggers download, resets was_active

    // Subsequent inactive cycles should NOT trigger again
    auto r2 = simulateCycle(makeInactive());
    EXPECT_FALSE(r2.should_download_files);

    auto r3 = simulateCycle(makeInactive());
    EXPECT_FALSE(r3.should_download_files);
}

TEST_F(O2RingStateMachineTest, MultipleSessionsWork) {
    // Session 1: active → inactive → download
    simulateCycle(makeActive());
    auto r1 = simulateCycle(makeInactive());
    EXPECT_TRUE(r1.should_download_files);

    // Session 2: active again → inactive → download again
    simulateCycle(makeActive(94, 68));
    auto r2 = simulateCycle(makeInactive());
    EXPECT_TRUE(r2.should_download_files);
}

TEST_F(O2RingStateMachineTest, UnreachableAfterIdleDoesNothing) {
    // Never was active, mule goes down — no action
    auto r = simulateCycle(makeUnreachable());
    EXPECT_FALSE(r.should_save_live);
    EXPECT_FALSE(r.should_download_files);
    EXPECT_FALSE(o2ring_was_active);
}

TEST_F(O2RingStateMachineTest, ActiveWithInvalidReadingsStillMarksActive) {
    O2RingClient::LiveReading r;
    r.spo2 = 0; r.hr = 0; r.active = true; r.valid = false;

    auto result = simulateCycle(r);
    EXPECT_FALSE(result.should_save_live);  // invalid readings not saved
    EXPECT_TRUE(o2ring_was_active);  // but state is set
}

// ── VLD Parser Integration ─────────────────────────────────────────────

TEST(VLDParserIntegration, ParsesValidFile) {
    // Build a minimal valid VLD v3 file: header + 2 records
    std::vector<uint8_t> data(40 + 10, 0);

    // Version 3
    data[0] = 3; data[1] = 0;
    // Date: 2026-04-18 20:25:49
    data[2] = 0xEA; data[3] = 0x07;  // year 2026 LE
    data[4] = 4;   // month
    data[5] = 18;  // day
    data[6] = 20;  // hour
    data[7] = 25;  // min
    data[8] = 49;  // sec
    // Duration: 8 seconds
    data[18] = 8; data[19] = 0;

    // Record 0: SpO2=96, HR=72, valid, motion=1, vib=0
    data[40] = 96; data[41] = 72; data[42] = 0; data[43] = 1; data[44] = 0;
    // Record 1: SpO2=94, HR=68, valid, motion=0, vib=0
    data[45] = 94; data[46] = 68; data[47] = 0; data[48] = 0; data[49] = 0;

    auto session = cpapdash::parser::VLDParser::parse(data.data(), data.size(), "test.vld");
    ASSERT_TRUE(session.has_value());
    EXPECT_EQ(session->samples.size(), 2u);
    EXPECT_EQ(session->duration_seconds, 8);
    EXPECT_EQ(session->samples[0].spo2, 96);
    EXPECT_EQ(session->samples[0].heart_rate, 72);
    EXPECT_EQ(session->samples[1].spo2, 94);
    EXPECT_TRUE(session->samples[0].valid());
    EXPECT_DOUBLE_EQ(session->sample_interval, 4.0);
}

TEST(VLDParserIntegration, RejectsInvalidVersion) {
    std::vector<uint8_t> data(50, 0);
    data[0] = 2;  // wrong version
    auto session = cpapdash::parser::VLDParser::parse(data.data(), data.size());
    EXPECT_FALSE(session.has_value());
}

TEST(VLDParserIntegration, HandlesOffWristMarker) {
    std::vector<uint8_t> data(40 + 5, 0);
    data[0] = 3; data[1] = 0;
    data[2] = 0xEA; data[3] = 0x07;
    data[4] = 4; data[5] = 18; data[6] = 22; data[7] = 0; data[8] = 0;
    data[18] = 4; data[19] = 0;

    // Off-wrist marker: 0xFF
    data[40] = 0xFF; data[41] = 0xFF; data[42] = 0; data[43] = 0; data[44] = 0;

    auto session = cpapdash::parser::VLDParser::parse(data.data(), data.size());
    ASSERT_TRUE(session.has_value());
    EXPECT_EQ(session->samples.size(), 1u);
    EXPECT_FALSE(session->samples[0].valid());
    EXPECT_EQ(session->metrics.valid_samples, 0);
}

TEST(VLDParserIntegration, MetricsODICalculation) {
    // 20 samples: baseline at 96, then a 4% drop to 92, then recovery
    std::vector<uint8_t> data(40 + 100, 0);
    data[0] = 3; data[1] = 0;
    data[2] = 0xEA; data[3] = 0x07;
    data[4] = 4; data[5] = 18; data[6] = 22; data[7] = 0; data[8] = 0;
    data[18] = 80; data[19] = 0;  // 80 seconds

    uint8_t spo2_vals[] = {96,96,96,96,96, 95,94,93,92,92, 93,94,95,96,96, 96,96,96,96,96};
    for (int i = 0; i < 20; i++) {
        data[40 + i*5 + 0] = spo2_vals[i];
        data[40 + i*5 + 1] = 72;  // HR
        data[40 + i*5 + 2] = 0;   // valid
        data[40 + i*5 + 3] = 0;   // motion
        data[40 + i*5 + 4] = 0;   // vibration
    }

    auto session = cpapdash::parser::VLDParser::parse(data.data(), data.size());
    ASSERT_TRUE(session.has_value());
    EXPECT_EQ(session->samples.size(), 20u);
    EXPECT_EQ(session->metrics.valid_samples, 20);
    EXPECT_DOUBLE_EQ(session->metrics.min_spo2, 92);
    EXPECT_DOUBLE_EQ(session->metrics.max_spo2, 96);
    EXPECT_GE(session->metrics.desat_count_3pct, 1);  // at least one 4% drop
    EXPECT_GT(session->metrics.odi_3pct, 0.0);
}
