#include <gtest/gtest.h>

#include <chrono>
#include <ctime>
#include <string>

#include "services/O2RingCsvParser.h"

using hms_cpap::O2RingCsvParser;

namespace {
std::chrono::system_clock::time_point utc(int y, int mon, int d, int h, int mi, int s) {
    std::tm tm{};
    tm.tm_year = y - 1900;
    tm.tm_mon = mon - 1;
    tm.tm_mday = d;
    tm.tm_hour = h;
    tm.tm_min = mi;
    tm.tm_sec = s;
    return std::chrono::system_clock::from_time_t(timegm(&tm));
}
}  // namespace

TEST(O2RingCsvParser, BasicMetrics24Hour) {
    std::string csv =
        "Time,Oxygen Level,Pulse Rate,Motion,O2 Reminder,PR Reminder\r\n"
        "23:00:00 May 20 2026,97,62,0,,\r\n"
        "23:00:04 May 20 2026,96,63,0,,\r\n"
        "23:00:08 May 20 2026,97,61,1,,\r\n"
        "23:00:12 May 20 2026,95,64,0,,\r\n"
        "23:00:16 May 20 2026,96,62,0,,\r\n";

    auto s = O2RingCsvParser::parse(csv, "test.csv");
    ASSERT_EQ(s.samples.size(), 5u);
    EXPECT_EQ(s.metrics.total_samples, 5);
    EXPECT_EQ(s.metrics.valid_samples, 5);
    EXPECT_NEAR(s.metrics.avg_spo2, 96.2, 0.1);
    EXPECT_EQ(s.metrics.min_spo2, 95);
    EXPECT_EQ(s.metrics.max_hr, 64);
    EXPECT_DOUBLE_EQ(s.sample_interval, 4.0);  // smallest gap detected
    EXPECT_EQ(s.duration_seconds, 20);
    EXPECT_EQ(s.start_time, utc(2026, 5, 20, 23, 0, 0));
    EXPECT_EQ(s.end_time, utc(2026, 5, 20, 23, 0, 16));
}

TEST(O2RingCsvParser, O2RingS_12HourQuotedFormat) {
    // Exact header/format from a Wellue "O2 Ring S" export (per-second, 12h
    // AM/PM, quoted timestamp with a comma after the day).
    std::string csv =
        "Time,SpO2(%),Pulse Rate(bpm),Motion,SpO2 Reminder,PR Reminder,\r\n"
        "\"11:20:29PM Jun 19, 2026\",97,60,0,0,0,\r\n"
        "\"11:20:30PM Jun 19, 2026\",96,61,0,0,0,\r\n"
        "\"12:00:00AM Jun 20, 2026\",95,62,0,0,0,\r\n";  // midnight rollover

    auto s = O2RingCsvParser::parse(csv, "O2 Ring S.csv");
    ASSERT_EQ(s.samples.size(), 3u);
    EXPECT_EQ(s.samples[0].timestamp, utc(2026, 6, 19, 23, 20, 29));  // 11:20:29 PM
    EXPECT_EQ(s.samples[2].timestamp, utc(2026, 6, 20, 0, 0, 0));     // 12:00:00 AM
    EXPECT_DOUBLE_EQ(s.sample_interval, 1.0);                         // per-second
    EXPECT_NEAR(s.metrics.avg_spo2, 96.0, 0.1);
    EXPECT_EQ(s.metrics.min_spo2, 95);
}

TEST(O2RingCsvParser, SentinelReadingsFlaggedInvalidNoOverflow) {
    // SpO2 255 / HR 65535 mean "no reading": mapped to 0xFF, excluded from
    // stats, and the 16-bit HR sentinel must not overflow the uint8_t field.
    std::string csv =
        "Time,SpO2(%),Pulse Rate(bpm),Motion\r\n"
        "\"11:20:29PM Jun 19, 2026\",97,60,0\r\n"
        "\"11:20:30PM Jun 19, 2026\",255,65535,0\r\n";

    auto s = O2RingCsvParser::parse(csv, "sentinel.csv");
    ASSERT_EQ(s.samples.size(), 2u);
    EXPECT_TRUE(s.samples[0].valid());
    EXPECT_FALSE(s.samples[1].valid());        // sentinel row invalid
    EXPECT_EQ(s.samples[1].spo2, 0xFF);
    EXPECT_EQ(s.samples[1].heart_rate, 0xFF);  // 65535 clamped, not overflowed
    EXPECT_EQ(s.metrics.valid_samples, 1);
    EXPECT_NEAR(s.metrics.avg_spo2, 97.0, 0.1);
    EXPECT_EQ(s.metrics.max_hr, 60);           // 65535 not counted
}

TEST(O2RingCsvParser, EmptyAndMalformedRows) {
    std::string csv =
        "Time,Oxygen Level,Pulse Rate,Motion\n"
        "bad timestamp,97,62,0\n"
        "23:00:00 May 20 2026,96,63,0\n"
        "incomplete\n"
        "23:00:04 May 20 2026,95,64,0\n";

    auto s = O2RingCsvParser::parse(csv, "mixed.csv");
    ASSERT_EQ(s.samples.size(), 2u);
    EXPECT_EQ(s.samples[0].timestamp, utc(2026, 5, 20, 23, 0, 0));
    EXPECT_EQ(s.samples[1].timestamp, utc(2026, 5, 20, 23, 0, 4));
}

TEST(O2RingCsvParser, HeaderOnlyYieldsEmptySession) {
    auto s = O2RingCsvParser::parse("Time,SpO2(%),Pulse Rate(bpm),Motion\n", "h.csv");
    EXPECT_TRUE(s.samples.empty());
}
