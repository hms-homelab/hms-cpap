// SDD-078 section 4: the reference importer's conversion rules on synthetic
// Garmin sleep JSON.
#include "services/GarminSleep.h"

#include <gtest/gtest.h>

#include <ctime>

using namespace hms_cpap;
using nlohmann::json;

namespace {

// 2026-11-18 13:00:00 UTC (a night in UTC+10 starting at 23:00).
constexpr long long kStart = 1795006800LL;

std::string gmt(long long secs) {
    const std::time_t t = static_cast<std::time_t>(secs);
    std::tm tm{};
    gmtime_r(&t, &tm);
    char buf[32];
    std::strftime(buf, sizeof buf, "%Y-%m-%dT%H:%M:%S.0", &tm);
    return buf;
}

/// A finished 7-hour night with an SpO2 reading a minute, heart rate every
/// two minutes and movement in 10-minute blocks.
json night(int spo2_count = 420) {
    json j;
    j["dailySleepDTO"] = {{"id", kStart * 1000},
                          {"sleepStartTimestampGMT", kStart * 1000},
                          {"sleepEndTimestampGMT", (kStart + 7 * 3600) * 1000}};
    json s = json::array();
    for (int i = 0; i < spo2_count; ++i)
        s.push_back({{"epochTimestamp", gmt(kStart + i * 60)}, {"spo2Reading", 94 + (i % 3)}});
    j["wellnessEpochSPO2DataDTOList"] = s;
    json hr = json::array();
    for (int i = 0; i < 210; ++i) hr.push_back({{"startGMT", (kStart + i * 120) * 1000}, {"value", 58 + (i % 5)}});
    j["sleepHeartRate"] = hr;
    json mv = json::array();
    for (int i = 0; i < 42; ++i)
        mv.push_back({{"startGMT", gmt(kStart + i * 600)}, {"endGMT", gmt(kStart + (i + 1) * 600)},
                      {"activityLevel", (i % 4) + 0.4}});
    j["sleepMovement"] = mv;
    return j;
}

const auto kUtc = [](long long) { return 0LL; };
constexpr long long kMorning = kStart + 9 * 3600;

}  // namespace

TEST(GarminSleep, AFinishedNightIsOneRecordingKeyedByItsSleepPeriod) {
    const auto n = convertGarminSleep(night(), kMorning, kUtc);
    ASSERT_EQ(n.verdict, GarminNight::Verdict::Ready) << n.reason;
    EXPECT_EQ(n.session.filename, "garmin-" + std::to_string(kStart * 1000));
    EXPECT_EQ(n.session.samples.size(), 420u);
    EXPECT_EQ(n.valid_spo2, 420);
    EXPECT_EQ(n.session.duration_seconds, 7 * 3600);
    EXPECT_DOUBLE_EQ(n.session.sample_interval, 60.0);
    EXPECT_EQ(n.session.metrics.valid_samples, 420);
    EXPECT_NEAR(n.session.metrics.avg_spo2, 95.0, 0.01);
}

TEST(GarminSleep, TimesAreInstantsPutOnTheMachineClock) {
    // D5: UTC instants, put on the wall clock through the offset given, never a fixed zone.
    const auto n = convertGarminSleep(night(), kMorning, [](long long) { return 10 * 3600LL; });
    ASSERT_EQ(n.verdict, GarminNight::Verdict::Ready);
    const auto start = std::chrono::system_clock::to_time_t(n.session.start_time);
    EXPECT_EQ(start, kStart + 10 * 3600) << "23:00 on the machine's clock";
    EXPECT_EQ(std::chrono::system_clock::to_time_t(n.session.samples.front().timestamp), kStart + 10 * 3600);
}

TEST(GarminSleep, SpO2OutsideFiftyToAHundredAndBadRecordsAreSkipped) {
    auto j = night();
    auto& s = j["wellnessEpochSPO2DataDTOList"];
    s[0]["spo2Reading"] = 49;
    s[1]["spo2Reading"] = 101;
    s[2]["spo2Reading"] = "95";       // a string is not a reading
    s[3]["spo2Reading"] = true;
    s[4].erase("epochTimestamp");
    s[5]["epochTimestamp"] = "not a time";
    s[6] = "junk";
    s[7]["spo2Reading"] = 50;         // the edges are kept
    s[8]["spo2Reading"] = 100;
    const auto n = convertGarminSleep(j, kMorning, kUtc);
    ASSERT_EQ(n.verdict, GarminNight::Verdict::Ready);
    EXPECT_EQ(n.valid_spo2, 420 - 7);
    EXPECT_EQ(n.session.samples.size(), 413u) << "nothing filled in for a skipped record";
}

TEST(GarminSleep, HeartRateIsPairedToTheNearestEpochWithinTwoMinutes) {
    auto j = night(70);
    // Heart rate only at +30 s past minute 0 and at minute 10 + 125 s.
    j["sleepHeartRate"] = json::array({{{"startGMT", (kStart + 30) * 1000}, {"value", 61}},
                                       {{"startGMT", (kStart + 600 + 125) * 1000}, {"value", 70}},
                                       {{"startGMT", (kStart + 1200) * 1000}, {"value", 25}},
                                       {{"startGMT", (kStart + 1260) * 1000}, {"value", 221}},
                                       {{"startGMT", "bad"}, {"value", 60}}});
    const auto n = convertGarminSleep(j, kMorning, kUtc);
    ASSERT_EQ(n.verdict, GarminNight::Verdict::Ready);
    const auto& smp = n.session.samples;
    EXPECT_EQ(smp[0].heart_rate, 61);
    EXPECT_EQ(smp[2].heart_rate, 61) << "90 s away: within 2 minutes";
    EXPECT_EQ(smp[3].heart_rate, kGarminNoHeartRate) << "150 s from the first and 425 s from the second";
    EXPECT_EQ(smp[12].heart_rate, 70) << "5 s past the second";
    EXPECT_EQ(smp[20].heart_rate, kGarminNoHeartRate)
        << "the rates at 20:00 (25) and 21:00 (221) are out of range and dropped";
    EXPECT_EQ(n.hr_paired, 7) << "minutes 0, 1, 2 to 0:30; 11, 12, 13, 14 to 12:05";
}

// Decided 2026-11-19 (SDD-078): validity is per field. An SpO2 reading with
// no pulse near it still counts; the pulse is only left blank.
TEST(GarminSleep, AnSpO2ReadingWithNoPulseStillCountsAndThePulseIsBlank) {
    auto j = night(70);
    // One pulse, at minute 0: minutes 0 to 2 have it, the rest do not.
    j["sleepHeartRate"] = json::array({{{"startGMT", kStart * 1000}, {"value", 60}}});
    const auto n = convertGarminSleep(j, kMorning, kUtc);
    ASSERT_EQ(n.verdict, GarminNight::Verdict::Ready);
    const auto& smp = n.session.samples;
    ASSERT_EQ(smp.size(), 70u);
    EXPECT_EQ(smp[10].heart_rate, kGarminNoHeartRate);
    EXPECT_TRUE(smp[10].valid()) << "the SpO2 reading counts on its own";
    EXPECT_EQ(n.session.metrics.valid_samples, 70) << "every SpO2 reading, pulse or not";
    EXPECT_DOUBLE_EQ(n.session.metrics.avg_hr, 60.0) << "of the samples with a pulse only";
    EXPECT_EQ(n.session.metrics.min_hr, 60) << "a blank pulse is not a 0 bpm minimum";
    EXPECT_EQ(n.session.metrics.max_hr, 60);
}

TEST(GarminSleep, EachSampleKeepsItsUtcInstant) {
    // SDD-079 D4: the stored times are the machine's wall clock; the instants
    // ride beside them for CpapDash.
    const auto n = convertGarminSleep(night(70), kMorning, [](long long) { return 10 * 3600LL; });
    ASSERT_EQ(n.verdict, GarminNight::Verdict::Ready);
    ASSERT_EQ(n.sample_utc_s.size(), n.session.samples.size());
    EXPECT_EQ(n.sample_utc_s[0], kStart);
    EXPECT_EQ(n.sample_utc_s[5], kStart + 300);
}

TEST(GarminSleep, MovementIsGivenByInterval) {
    auto j = night(70);
    j["sleepMovement"] = json::array({{{"startGMT", gmt(kStart)}, {"endGMT", gmt(kStart + 120)}, {"activityLevel", 2.7}},
                                      {{"startGMT", gmt(kStart + 300)}, {"endGMT", gmt(kStart + 240)}, {"activityLevel", 5}},
                                      {{"startGMT", gmt(kStart + 600)}, {"endGMT", gmt(kStart + 660)}, {"activityLevel", "x"}}});
    const auto n = convertGarminSleep(j, kMorning, kUtc);
    ASSERT_EQ(n.verdict, GarminNight::Verdict::Ready);
    EXPECT_EQ(n.session.samples[0].motion, 2) << "as the CSV path reads 2.7";
    EXPECT_EQ(n.session.samples[1].motion, 2);
    EXPECT_EQ(n.session.samples[2].motion, 0) << "the interval is [start, end)";
    EXPECT_EQ(n.session.samples[5].motion, 0) << "an interval that ends before it starts is skipped";
    EXPECT_EQ(n.session.samples[10].motion, 0) << "an unreadable level is skipped";
    EXPECT_EQ(n.motion_paired, 2);
}

TEST(GarminSleep, SixtySamplesAtLeast) {
    EXPECT_EQ(convertGarminSleep(night(59), kMorning, kUtc).verdict, GarminNight::Verdict::TooFewSamples);
    EXPECT_EQ(convertGarminSleep(night(60), kMorning, kUtc).verdict, GarminNight::Verdict::Ready);
}

TEST(GarminSleep, AnIncompleteSleepPeriodIsLeftForTheNextEvent) {
    // Still asleep: the period's end is after now.
    EXPECT_EQ(convertGarminSleep(night(), kStart + 3 * 3600, kUtc).verdict, GarminNight::Verdict::Incomplete);
    auto j = night();
    j["dailySleepDTO"].erase("sleepEndTimestampGMT");
    EXPECT_EQ(convertGarminSleep(j, kMorning, kUtc).verdict, GarminNight::Verdict::Incomplete);
    j = night();
    j["dailySleepDTO"]["sleepEndTimestampGMT"] = (kStart + 1800) * 1000;   // under an hour
    EXPECT_EQ(convertGarminSleep(j, kMorning, kUtc).verdict, GarminNight::Verdict::Incomplete);
    EXPECT_EQ(convertGarminSleep(json::object(), kMorning, kUtc).verdict, GarminNight::Verdict::Incomplete);
    EXPECT_EQ(convertGarminSleep(json(nullptr), kMorning, kUtc).verdict, GarminNight::Verdict::Incomplete);
}

TEST(GarminSleep, MissingArraysAreEmpty) {
    auto j = night();
    j.erase("sleepHeartRate");
    j["sleepMovement"] = "not a list";
    const auto n = convertGarminSleep(j, kMorning, kUtc);
    ASSERT_EQ(n.verdict, GarminNight::Verdict::Ready);
    EXPECT_EQ(n.hr_paired, 0);
    EXPECT_EQ(n.session.samples[0].heart_rate, kGarminNoHeartRate);
    EXPECT_EQ(n.session.metrics.valid_samples, 420) << "no pulse at all: every SpO2 reading still counts";
}

TEST(GarminSleep, GmtStringsAreReadAsTheReferenceReadsThem) {
    EXPECT_EQ(parseGarminGmt("2026-11-18T13:00:00.0"), kStart * 1000) << "no offset: UTC";
    EXPECT_EQ(parseGarminGmt("2026-11-18T13:00:00Z"), kStart * 1000);
    EXPECT_EQ(parseGarminGmt("2026-11-18T23:00:00+10:00"), kStart * 1000) << "an explicit offset is kept";
    EXPECT_EQ(parseGarminGmt("2026-11-18 13:00:00.250"), kStart * 1000 + 250);
    EXPECT_FALSE(parseGarminGmt("2026-11-18"));
    EXPECT_FALSE(parseGarminGmt(12345));
    EXPECT_EQ(parseGarminEpochMs("1795006800000"), kStart * 1000);
    EXPECT_FALSE(parseGarminEpochMs(true));
}
