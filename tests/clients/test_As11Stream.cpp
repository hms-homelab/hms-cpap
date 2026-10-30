#include <gtest/gtest.h>

#include "clients/As11Stream.h"

#include <cmath>

using namespace hms_cpap::as11;
using nlohmann::json;

// SDD-057 step 4: the AirSense 11's live stream as CDS1 blocks, the one form
// both of the night writer's sources hand it. Every number here is made up.

TEST(As11Stream, Cds1RoundTrips) {
    StreamBlock b;
    b.start_ms = 1'792'000'000'123LL;
    b.serial = "23000000001";
    b.channels.push_back({kFlow, 0, {100, -200, kNull}});
    b.channels.push_back({kMaskPressure, 0, {1000, 1010, 1020}});
    b.channels.push_back({kLeak, 1, {50}});
    const auto bytes = encodeCds1(b);
    ASSERT_EQ(std::string(bytes.begin(), bytes.begin() + 4), "CDS1");

    auto d = decodeCds1(bytes);
    ASSERT_TRUE(d.has_value());
    EXPECT_EQ(d->start_ms, b.start_ms);
    EXPECT_EQ(d->serial, b.serial);
    EXPECT_EQ(d->interval_ms, 40);
    ASSERT_EQ(d->channels.size(), 3u);
    EXPECT_EQ(d->channel(kFlow)->values, (std::vector<int16_t>{100, -200, kNull}));
    EXPECT_EQ(d->channel(kLeak)->cadence, 1);
    EXPECT_EQ(d->endMs(), b.start_ms + 3 * 40);
    EXPECT_EQ(d->channel(kSpO2), nullptr);
}

TEST(As11Stream, Cds1RefusesWhatIsNotAWholeBlock) {
    StreamBlock b;
    b.start_ms = 1000;
    b.channels.push_back({kFlow, 0, {1, 2}});
    auto bytes = encodeCds1(b);
    EXPECT_FALSE(decodeCds1(bytes.data(), bytes.size() - 1).has_value());   // short
    auto longer = bytes;
    longer.push_back(0);
    EXPECT_FALSE(decodeCds1(longer).has_value());                          // trailing
    auto v2 = bytes;
    v2[4] = 2;
    EXPECT_FALSE(decodeCds1(v2).has_value());                              // a version not known
    auto bad = bytes;
    bad[0] = 'X';
    EXPECT_FALSE(decodeCds1(bad).has_value());
}

TEST(As11Stream, UnitsAreTheBridgesIntegers) {
    EXPECT_EQ(quantise(kFlow, 0.5), 500);             // L/s to mL/s
    EXPECT_EQ(quantise(kMaskPressure, 10.25), 1025);  // 0.01 cmH2O
    EXPECT_EQ(quantise(kTidalVolume, 0.45), 450);     // L to mL
    EXPECT_EQ(quantise(kRespRate, 15.3), 153);
    EXPECT_EQ(quantise(kSpO2, 96.5), 965);
    EXPECT_EQ(quantise(kHeartRate, 61), 61);
    EXPECT_EQ(quantise(kFlow, std::nan("")), kNull);
    EXPECT_EQ(quantise(kFlow, 99.0), 32767);           // clamped, never the null
    EXPECT_DOUBLE_EQ(physical(kMaskPressure, 1025), 10.25);
    EXPECT_DOUBLE_EQ(physical(kLeak, 300), 0.3);
    EXPECT_TRUE(std::isnan(physical(kFlow, kNull)));
}

TEST(As11Stream, IsoTimes) {
    EXPECT_EQ(isoToMs("1970-01-01T00:00:01.500Z"), 1500);
    EXPECT_EQ(isoToMs("2026-10-17T14:02:33.656Z"), 1792245753656LL);
    EXPECT_EQ(isoToMs("not a time"), 0);
}

namespace {

// One StreamData report: `n` fast samples from `start`, and the slow values given.
json report(int64_t start_ms, int n, double flow = 0.25, double leak = NAN) {
    // Back to ISO, the way the machine sends it.
    const int64_t s = start_ms / 1000, ms = start_ms % 1000;
    const int64_t days = s / 86400, sod = s % 86400;
    // 2026-10-17 is day 20743 since 1970.
    char iso[40];
    std::snprintf(iso, sizeof(iso), "2026-10-%02lldT%02lld:%02lld:%02lld.%03lldZ",
                  static_cast<long long>(17 + (days - 20743)), static_cast<long long>(sod / 3600),
                  static_cast<long long>((sod % 3600) / 60), static_cast<long long>(sod % 60),
                  static_cast<long long>(ms));
    json flow_v = json::array(), press_v = json::array();
    for (int i = 0; i < n; i++) { flow_v.push_back(flow); press_v.push_back(8.0); }
    json data = json::array({{{"PatientFlow", flow_v}}, {{"MaskPressure", press_v}}});
    if (!std::isnan(leak)) data.push_back({{"Leak", json::array({nullptr, leak})}});
    return {{"startTime", iso}, {"intervalMs", 40}, {"data", data}};
}

constexpr int64_t kT0 = 20743LL * 86400000LL + 3 * 3600000LL;   // 2026-10-17 03:00 UTC

} // namespace

TEST(As11Stream, FiveReportsMakeASecond) {
    std::vector<StreamBlock> out;
    SecondAssembler a([&](StreamBlock&& b) { out.push_back(std::move(b)); });
    a.setSerial("23000000001");
    for (int i = 0; i < 5; i++) a.onStreamData(report(kT0 + i * 200, 5, 0.25, i == 2 ? 0.4 : NAN));
    ASSERT_EQ(out.size(), 1u);
    EXPECT_EQ(out[0].start_ms, kT0);
    EXPECT_EQ(out[0].serial, "23000000001");
    EXPECT_EQ(out[0].channel(kFlow)->values.size(), 25u);
    EXPECT_EQ(out[0].channel(kFlow)->values[0], 250);
    EXPECT_EQ(out[0].channel(kMaskPressure)->values[24], 800);
    ASSERT_NE(out[0].channel(kLeak), nullptr);
    EXPECT_EQ(out[0].channel(kLeak)->values, (std::vector<int16_t>{400}));   // the null skipped
    EXPECT_EQ(out[0].channel(kLeak)->cadence, 1);
}

TEST(As11Stream, ALostReportClosesTheSecond) {
    std::vector<StreamBlock> out;
    SecondAssembler a([&](StreamBlock&& b) { out.push_back(std::move(b)); });
    a.onStreamData(report(kT0, 5));
    a.onStreamData(report(kT0 + 200, 5));
    a.onStreamData(report(kT0 + 600, 5));   // the one at +400 never came
    ASSERT_EQ(out.size(), 1u);
    EXPECT_EQ(out[0].channel(kFlow)->values.size(), 10u);
    a.flush();
    ASSERT_EQ(out.size(), 2u);
    EXPECT_EQ(out[1].start_ms, kT0 + 600);
}

TEST(As11Stream, SamplesPastASecondCarryIntoTheNext) {
    std::vector<StreamBlock> out;
    SecondAssembler a([&](StreamBlock&& b) { out.push_back(std::move(b)); });
    for (int i = 0; i < 4; i++) a.onStreamData(report(kT0 + i * 320, 8));   // 32 samples
    ASSERT_EQ(out.size(), 1u);
    a.flush();
    ASSERT_EQ(out.size(), 2u);
    EXPECT_EQ(out[1].start_ms, kT0 + 1000);
    EXPECT_EQ(out[1].channel(kFlow)->values.size(), 7u);
}

TEST(As11Stream, KeysAndIds) {
    EXPECT_EQ(channelOfKey("PatientFlow"), kFlow);
    EXPECT_EQ(channelOfKey("_RR2"), kRespRate);
    EXPECT_EQ(channelOfKey("Something"), 0);
    EXPECT_EQ(streamIds().size(), 13u);
}
