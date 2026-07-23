#include "EquipmentStubs.h"
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

// ============================================================================
// VLDParser PURE PARSE / AGGREGATION BRANCHES (extended coverage)
// ============================================================================
//
// These tests drive additional VLDParser::parse() and calculateMetrics()
// branches that the existing tests do not reach. All inputs are hand-built
// byte buffers / sample vectors — fully deterministic, no I/O, no network.

namespace oxi_parse_more {

using cpapdash::parser::OximetrySample;
using cpapdash::parser::OximetryMetrics;
using cpapdash::parser::OximetrySession;
using cpapdash::parser::VLDParser;

// Build a minimal valid v3 header with the given duration (seconds).
static void writeHeader(std::vector<uint8_t>& d, uint16_t duration_s) {
    d[0] = 3; d[1] = 0;                 // version 3
    d[2] = 0xEA; d[3] = 0x07;           // year 2026 LE
    d[4] = 4; d[5] = 18;                // month, day
    d[6] = 22; d[7] = 0; d[8] = 0;      // hh:mm:ss
    d[18] = static_cast<uint8_t>(duration_s & 0xFF);
    d[19] = static_cast<uint8_t>((duration_s >> 8) & 0xFF);
}

// parse(): null pointer -> nullopt (the !data guard).
TEST(VLDParserMore, NullDataReturnsNullopt) {
    auto s = VLDParser::parse(nullptr, 100, "x.vld");
    EXPECT_FALSE(s.has_value());
}

// parse(): buffer shorter than the 40-byte header -> nullopt.
TEST(VLDParserMore, TooShortReturnsNullopt) {
    std::vector<uint8_t> d(10, 0);
    d[0] = 3; d[1] = 0;
    auto s = VLDParser::parse(d.data(), d.size(), "short.vld");
    EXPECT_FALSE(s.has_value());
}

// parse(): header present but ZERO records (len == HEADER_SIZE) -> nullopt
// (record_count == 0 guard).
TEST(VLDParserMore, ZeroRecordsReturnsNullopt) {
    std::vector<uint8_t> d(40, 0);
    writeHeader(d, 8);
    auto s = VLDParser::parse(d.data(), d.size(), "empty.vld");
    EXPECT_FALSE(s.has_value());
}

// parse(): duration 0 -> interval falls back to 4.0 (interval <= 0 guard).
TEST(VLDParserMore, ZeroDurationDefaultsIntervalTo4) {
    std::vector<uint8_t> d(40 + 5, 0);   // exactly 1 record
    writeHeader(d, 0);                    // duration 0 -> interval would be 0
    d[40] = 96; d[41] = 72; d[42] = 0; d[43] = 0; d[44] = 0;
    auto s = VLDParser::parse(d.data(), d.size(), "zero_dur.vld");
    ASSERT_TRUE(s.has_value());
    EXPECT_DOUBLE_EQ(s->sample_interval, 4.0) << "interval<=0 must fall back to 4.0";
    EXPECT_EQ(s->samples.size(), 1u);
}

// parse(): trailing partial record is truncated (data_len / RECORD_SIZE floors).
TEST(VLDParserMore, TruncatedTrailingRecordIgnored) {
    // 2 full records + 3 trailing bytes = 13 data bytes -> 2 records.
    std::vector<uint8_t> d(40 + 13, 0);
    writeHeader(d, 8);
    d[40] = 95; d[41] = 70; d[44] = 0;
    d[45] = 94; d[46] = 69; d[49] = 0;
    auto s = VLDParser::parse(d.data(), d.size(), "trunc.vld");
    ASSERT_TRUE(s.has_value());
    EXPECT_EQ(s->samples.size(), 2u) << "partial trailing record must be dropped";
}

// date_str(): formats the parsed start_time as YYYYMMDD (UTC).
TEST(VLDParserMore, DateStringFromHeader) {
    std::vector<uint8_t> d(40 + 5, 0);
    writeHeader(d, 4);                    // 2026-04-18 22:00:00 UTC
    d[40] = 96; d[41] = 72;
    auto s = VLDParser::parse(d.data(), d.size(), "dated.vld");
    ASSERT_TRUE(s.has_value());
    EXPECT_EQ(s->date_str(), "20260418");
}

// calculateMetrics(): empty input -> defaults preserved, early return path.
TEST(VLDParserMore, MetricsEmptySamples) {
    auto m = VLDParser::calculateMetrics({}, 4.0);
    EXPECT_EQ(m.total_samples, 0);
    EXPECT_EQ(m.valid_samples, 0);
    EXPECT_DOUBLE_EQ(m.avg_spo2, 0.0);
    EXPECT_DOUBLE_EQ(m.min_spo2, 100.0) << "default min preserved when no valid samples";
    EXPECT_DOUBLE_EQ(m.max_spo2, 0.0);
}

// calculateMetrics(): all samples invalid (off-wrist) -> early return after the
// loop (valid_spo2 empty), defaults preserved, total counted.
TEST(VLDParserMore, MetricsAllInvalid) {
    std::vector<OximetrySample> samples;
    for (int i = 0; i < 5; i++) {
        OximetrySample s{};
        s.spo2 = 0xFF; s.heart_rate = 0xFF; s.invalid_flag = 0;
        samples.push_back(s);
    }
    auto m = VLDParser::calculateMetrics(samples, 4.0);
    EXPECT_EQ(m.total_samples, 5);
    EXPECT_EQ(m.valid_samples, 0);
    EXPECT_DOUBLE_EQ(m.avg_spo2, 0.0);
}

// calculateMetrics(): invalid_flag set marks a sample invalid even with good
// spo2/hr (the invalid_flag == 0 branch of valid()).
TEST(VLDParserMore, MetricsInvalidFlagExcludesSample) {
    std::vector<OximetrySample> samples;
    OximetrySample good{}; good.spo2 = 97; good.heart_rate = 70; good.invalid_flag = 0;
    OximetrySample flagged{}; flagged.spo2 = 80; flagged.heart_rate = 60; flagged.invalid_flag = 1;
    samples.push_back(good);
    samples.push_back(flagged);
    auto m = VLDParser::calculateMetrics(samples, 4.0);
    EXPECT_EQ(m.total_samples, 2);
    EXPECT_EQ(m.valid_samples, 1) << "invalid_flag sample excluded";
    EXPECT_DOUBLE_EQ(m.avg_spo2, 97.0);
    EXPECT_DOUBLE_EQ(m.min_spo2, 97.0);
    EXPECT_DOUBLE_EQ(m.max_spo2, 97.0);
}

// calculateMetrics(): time_below_90 / time_below_88 percentage branches.
TEST(VLDParserMore, MetricsTimeBelowThresholds) {
    std::vector<OximetrySample> samples;
    // 4 valid: 96, 89 (<90), 87 (<90 and <88), 95
    for (int sp : {96, 89, 87, 95}) {
        OximetrySample s{}; s.spo2 = static_cast<uint8_t>(sp); s.heart_rate = 70; s.invalid_flag = 0;
        samples.push_back(s);
    }
    auto m = VLDParser::calculateMetrics(samples, 4.0);
    EXPECT_EQ(m.valid_samples, 4);
    // below 90: 89 and 87 -> 2/4 = 50%
    EXPECT_DOUBLE_EQ(m.time_below_90_pct, 50.0);
    // below 88: only 87 -> 1/4 = 25%
    EXPECT_DOUBLE_EQ(m.time_below_88_pct, 25.0);
    EXPECT_DOUBLE_EQ(m.min_spo2, 87.0);
    EXPECT_DOUBLE_EQ(m.max_spo2, 96.0);
}

// calculateMetrics(): stable SpO2 -> NO desaturation events (the drop < 3.0
// path keeps desat_count_3pct at 0, odi stays 0).
TEST(VLDParserMore, MetricsNoDesatWhenStable) {
    std::vector<OximetrySample> samples;
    for (int i = 0; i < 30; i++) {
        OximetrySample s{}; s.spo2 = 97; s.heart_rate = 70; s.invalid_flag = 0;
        samples.push_back(s);
    }
    auto m = VLDParser::calculateMetrics(samples, 4.0);
    EXPECT_EQ(m.desat_count_3pct, 0);
    EXPECT_DOUBLE_EQ(m.odi_3pct, 0.0);
    EXPECT_DOUBLE_EQ(m.spo2_baseline, 97.0);
    EXPECT_GT(m.recording_hours, 0.0);
}

// calculateMetrics(): a single sustained desat counted once (in_desat latch),
// then recovery (drop < 1.0) clears the latch.
TEST(VLDParserMore, MetricsSingleDesatLatched) {
    // Baseline 97 for a while, then a sustained dip to 92 (5% drop), then back.
    std::vector<uint8_t> spo2 = {
        97,97,97,97,97, 97,97,97,97,97,
        92,92,92,92,92,            // sustained drop -> one event
        97,97,97,97,97             // recovery
    };
    std::vector<OximetrySample> samples;
    for (uint8_t sp : spo2) {
        OximetrySample s{}; s.spo2 = sp; s.heart_rate = 70; s.invalid_flag = 0;
        samples.push_back(s);
    }
    auto m = VLDParser::calculateMetrics(samples, 4.0);
    EXPECT_EQ(m.desat_count_3pct, 1) << "sustained dip counts as exactly one event";
    EXPECT_GT(m.odi_3pct, 0.0);
    EXPECT_DOUBLE_EQ(m.min_spo2, 92.0);
}

// calculateMetrics(): HR min/max tracked across valid samples.
TEST(VLDParserMore, MetricsHeartRateRange) {
    std::vector<OximetrySample> samples;
    for (int hr : {60, 80, 72}) {
        OximetrySample s{}; s.spo2 = 96; s.heart_rate = static_cast<uint8_t>(hr); s.invalid_flag = 0;
        samples.push_back(s);
    }
    auto m = VLDParser::calculateMetrics(samples, 4.0);
    EXPECT_EQ(m.min_hr, 60);
    EXPECT_EQ(m.max_hr, 80);
    EXPECT_NEAR(m.avg_hr, (60 + 80 + 72) / 3.0, 1e-9);
}

}  // namespace oxi_parse_more

// ============================================================================
// OximetryService::collectAndPublish() via DI seam (IO2RingClient + IDatabase)
// ============================================================================
//
// Drives the real OximetryService through its constructor-injected
// IO2RingClient and IDatabase. A FakeO2RingClient returns canned file lists
// and VLD byte buffers; a NiceMock IDatabase scripts oximetrySessionExists /
// saveOximetrySession. Fully deterministic — no BLE, no network.

namespace oxi_service {

using ::testing::_;
using ::testing::Return;
using ::testing::NiceMock;
using ::testing::AnyNumber;

// Minimal valid VLD v3 buffer with N records (avg spo2 ~96).
static std::vector<uint8_t> makeVld(int records) {
    std::vector<uint8_t> d(40 + records * 5, 0);
    d[0] = 3; d[1] = 0;
    d[2] = 0xEA; d[3] = 0x07; d[4] = 4; d[5] = 18; d[6] = 22; d[7] = 0; d[8] = 0;
    uint16_t dur = static_cast<uint16_t>(records * 4);
    d[18] = dur & 0xFF; d[19] = (dur >> 8) & 0xFF;
    for (int i = 0; i < records; i++) {
        d[40 + i*5 + 0] = 96; d[40 + i*5 + 1] = 72; d[40 + i*5 + 2] = 0;
    }
    return d;
}

class FakeO2RingClient : public IO2RingClient {
public:
    std::vector<std::string> files;
    std::map<std::string, std::vector<uint8_t>> contents;
    int battery = 88;
    int download_calls = 0;

    bool isConnected() override { return true; }
    std::vector<std::string> listFiles() override { return files; }
    std::vector<uint8_t> downloadFile(const std::string& filename) override {
        ++download_calls;
        auto it = contents.find(filename);
        return it == contents.end() ? std::vector<uint8_t>{} : it->second;
    }
    LiveReading getLive() override { return {}; }
    int getBattery() const override { return battery; }
};

class MockDatabase : public IDatabase {
public:
    HMS_CPAP_STUB_EQUIPMENT_METHODS
    DbType dbType() const override { return DbType::SQLITE; }
    MOCK_METHOD(bool, connect, (), (override));
    MOCK_METHOD(void, disconnect, (), (override));
    MOCK_METHOD(bool, isConnected, (), (const, override));
    MOCK_METHOD(bool, saveSession, (const CPAPSession&), (override));
    MOCK_METHOD(bool, sessionExists, (const std::string&, const std::chrono::system_clock::time_point&), (override));
    MOCK_METHOD((std::optional<std::chrono::system_clock::time_point>), getLastSessionStart, (const std::string&), (override));
    MOCK_METHOD((std::optional<std::chrono::system_clock::time_point>), getSessionStartForSleepDay, (const std::string&, const std::string&, bool), (override));
    MOCK_METHOD((std::optional<SessionMetrics>), getSessionMetrics, (const std::string&, const std::chrono::system_clock::time_point&), (override));
    MOCK_METHOD(bool, markSessionCompleted, (const std::string&, const std::chrono::system_clock::time_point&), (override));
    MOCK_METHOD(bool, reopenSession, (const std::string&, const std::chrono::system_clock::time_point&), (override));
    MOCK_METHOD(int, deleteSessionsByDateFolder, (const std::string&, const std::string&), (override));
    MOCK_METHOD(bool, isForceCompleted, (const std::string&, const std::chrono::system_clock::time_point&), (override));
    MOCK_METHOD(bool, setForceCompleted, (const std::string&, const std::chrono::system_clock::time_point&), (override));
    MOCK_METHOD((std::map<std::string, int>), getCheckpointFileSizes, (const std::string&, const std::chrono::system_clock::time_point&), (override));
    MOCK_METHOD((std::map<std::string, int>), getCheckpointFilesByFolder, (const std::string&, const std::string&), (override));
    bool updateCheckpointFileSizes(const std::string&, const std::chrono::system_clock::time_point&, const std::map<std::string, int>&) override { return true; }
    MOCK_METHOD(bool, updateDeviceLastSeen, (const std::string&), (override));
    MOCK_METHOD(bool, saveSTRDailyRecords, (const std::vector<STRDailyRecord>&), (override));
    MOCK_METHOD((std::optional<std::string>), getLastSTRDate, (const std::string&), (override));
    MOCK_METHOD(bool, aggregateDailySummaryFromSessions, (const std::string&), (override));
    MOCK_METHOD((std::optional<SessionMetrics>), getNightlyMetrics, (const std::string&, const std::chrono::system_clock::time_point&), (override));
    MOCK_METHOD((std::vector<SessionMetrics>), getMetricsForDateRange, (const std::string&, int), (override));
    MOCK_METHOD(bool, saveSummary, (const std::string&, const std::string&, const std::string&, const std::string&, int, double, double, double, const std::string&), (override));
    MOCK_METHOD(void*, rawConnection, (), (override));
    MOCK_METHOD(bool, saveOximetrySession, (const std::string&, const cpapdash::parser::OximetrySession&), (override));
    MOCK_METHOD(bool, oximetrySessionExists, (const std::string&, const std::string&), (override));
    MOCK_METHOD(bool, saveLiveOximetrySample, (const std::string&, const std::string&, int, int, int), (override));
    OxiSummary getOximetrySummary(const std::string&, const std::string&, const std::string&) override { return {}; }
    OxiRangeSummary getOximetryRangeSummary(const std::string&, const std::string&, const std::string&) override { return {}; }
    std::vector<OxiNightlyPoint> getOximetryNightlySpo2(const std::string&, const std::string&, const std::string&) override { return {}; }
};

class OximetryServiceTest : public ::testing::Test {
protected:
    std::shared_ptr<FakeO2RingClient> client;
    std::shared_ptr<NiceMock<MockDatabase>> db;
    std::unique_ptr<OximetryService> svc;

    void SetUp() override {
        client = std::make_shared<FakeO2RingClient>();
        db = std::make_shared<NiceMock<MockDatabase>>();
        svc = std::make_unique<OximetryService>(client, db);
    }
};

// No files on device -> early return false, never touches DB.
TEST_F(OximetryServiceTest, NoFiles_ReturnsFalse) {
    client->files.clear();
    EXPECT_CALL(*db, oximetrySessionExists(_, _)).Times(0);
    EXPECT_FALSE(svc->collectAndPublish());
}

// New file, valid VLD, not in DB, save succeeds -> returns true, saved once.
TEST_F(OximetryServiceTest, NewFile_ParsedAndSaved) {
    client->files = {"20260418_220000.vld"};
    client->contents["20260418_220000.vld"] = makeVld(20);

    EXPECT_CALL(*db, oximetrySessionExists("o2ring", "20260418_220000.vld"))
        .WillRepeatedly(Return(false));
    EXPECT_CALL(*db, saveOximetrySession("o2ring", _)).Times(1).WillOnce(Return(true));

    EXPECT_TRUE(svc->collectAndPublish());
    EXPECT_EQ(client->download_calls, 1);
}

// File already in DB -> marked processed, skipped, no download/save, returns false.
TEST_F(OximetryServiceTest, AlreadyInDb_SkipsDownload) {
    client->files = {"old.vld"};
    client->contents["old.vld"] = makeVld(20);

    EXPECT_CALL(*db, oximetrySessionExists("o2ring", "old.vld"))
        .WillRepeatedly(Return(true));
    EXPECT_CALL(*db, saveOximetrySession(_, _)).Times(0);

    EXPECT_FALSE(svc->collectAndPublish());
    EXPECT_EQ(client->download_calls, 0) << "DB-existing file must not download";
}

// Download returns empty bytes -> parse skipped, save skipped, returns false.
TEST_F(OximetryServiceTest, EmptyDownload_Skipped) {
    client->files = {"gone.vld"};
    // No entry in contents -> downloadFile returns empty.
    EXPECT_CALL(*db, oximetrySessionExists("o2ring", "gone.vld"))
        .WillRepeatedly(Return(false));
    EXPECT_CALL(*db, saveOximetrySession(_, _)).Times(0);

    EXPECT_FALSE(svc->collectAndPublish());
    EXPECT_EQ(client->download_calls, 1);
}

// Downloaded bytes are not a valid VLD -> parse fails, save skipped, false.
TEST_F(OximetryServiceTest, UnparseableFile_Skipped) {
    client->files = {"junk.vld"};
    client->contents["junk.vld"] = std::vector<uint8_t>{'n','o','t','v','l','d'};

    EXPECT_CALL(*db, oximetrySessionExists("o2ring", "junk.vld"))
        .WillRepeatedly(Return(false));
    EXPECT_CALL(*db, saveOximetrySession(_, _)).Times(0);

    EXPECT_FALSE(svc->collectAndPublish());
}

// Valid file parsed but DB save fails -> not marked processed, returns false.
TEST_F(OximetryServiceTest, SaveFails_ReturnsFalse) {
    client->files = {"savefail.vld"};
    client->contents["savefail.vld"] = makeVld(10);

    EXPECT_CALL(*db, oximetrySessionExists("o2ring", "savefail.vld"))
        .WillRepeatedly(Return(false));
    EXPECT_CALL(*db, saveOximetrySession("o2ring", _)).Times(1).WillOnce(Return(false));

    EXPECT_FALSE(svc->collectAndPublish());
}

// Mixed batch: one new (saved), one already-in-DB (skipped) -> returns true,
// only the new file downloaded.
TEST_F(OximetryServiceTest, MixedBatch_OnlyNewSaved) {
    client->files = {"existing.vld", "fresh.vld"};
    client->contents["existing.vld"] = makeVld(10);
    client->contents["fresh.vld"] = makeVld(15);

    EXPECT_CALL(*db, oximetrySessionExists("o2ring", "existing.vld"))
        .WillRepeatedly(Return(true));
    EXPECT_CALL(*db, oximetrySessionExists("o2ring", "fresh.vld"))
        .WillRepeatedly(Return(false));
    EXPECT_CALL(*db, saveOximetrySession("o2ring", _)).Times(1).WillOnce(Return(true));

    EXPECT_TRUE(svc->collectAndPublish());
    EXPECT_EQ(client->download_calls, 1) << "only the fresh file is downloaded";
}

// Second collect in the same service instance: a file saved on the first pass
// is now in processed_files_ -> skipped without consulting the DB again.
TEST_F(OximetryServiceTest, ProcessedFileCache_SkipsOnSecondPass) {
    client->files = {"cached.vld"};
    client->contents["cached.vld"] = makeVld(12);

    EXPECT_CALL(*db, oximetrySessionExists("o2ring", "cached.vld"))
        .Times(1)               // consulted exactly once (first pass)
        .WillOnce(Return(false));
    EXPECT_CALL(*db, saveOximetrySession("o2ring", _)).Times(1).WillOnce(Return(true));

    EXPECT_TRUE(svc->collectAndPublish());   // first pass: download + save
    EXPECT_FALSE(svc->collectAndPublish());  // second pass: cached -> skipped
    EXPECT_EQ(client->download_calls, 1) << "cached file not re-downloaded";
}

// pollLive() returns and stores the client's live reading.
TEST_F(OximetryServiceTest, PollLive_StoresLastReading) {
    // FakeO2RingClient::getLive returns a default (all-zero) reading; verify
    // pollLive surfaces it and getLastLive matches.
    auto live = svc->pollLive();
    EXPECT_FALSE(live.active);
    EXPECT_EQ(live.spo2, 0);
    EXPECT_EQ(svc->getLastLive().spo2, live.spo2);
    EXPECT_EQ(svc->getLastLive().active, live.active);
}

}  // namespace oxi_service
