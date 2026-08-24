#include <gtest/gtest.h>

#include <chrono>
#include <ctime>
#include <string>

// The reader itself now lives in the shared parser (cpapdash::parser), so
// hms-cpap and hms-cpapdash-api cannot drift apart on this format again. These
// cases stay here because they are hms-cpap's own regression cover: they are
// what this repo learned about real Wellue exports, and they should keep failing
// here if the shared reader ever loses it.
#include <cpapdash/parser/OximetryCsv.h>

namespace {

// The shape the old hms_cpap::O2RingCsvParser::parse had. Every assertion below
// is unchanged; only where the code lives has moved.
cpapdash::parser::OximetrySession parseCsv(const std::string& content,
                                           const std::string& filename) {
    return cpapdash::parser::readO2RingCsv(content, filename).session;
}

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

    auto s = parseCsv(csv, "test.csv");
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

    auto s = parseCsv(csv, "O2 Ring S.csv");
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

    auto s = parseCsv(csv, "sentinel.csv");
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

    auto s = parseCsv(csv, "mixed.csv");
    ASSERT_EQ(s.samples.size(), 2u);
    EXPECT_EQ(s.samples[0].timestamp, utc(2026, 5, 20, 23, 0, 0));
    EXPECT_EQ(s.samples[1].timestamp, utc(2026, 5, 20, 23, 0, 4));
}

TEST(O2RingCsvParser, HeaderOnlyYieldsEmptySession) {
    auto s = parseCsv("Time,SpO2(%),Pulse Rate(bpm),Motion\n", "h.csv");
    EXPECT_TRUE(s.samples.empty());
}

// ── Numeric date fields (issue #17) ─────────────────────────────────────
// The Wellue app follows the phone's locale, so the same ring exports either
// "06:53:07 Apr 12 2026" or "21:56:34 02/08/2026". Only the month-name form
// parsed, so a numeric export produced zero samples and the UI said "no O2
// data found" for a file that SleepHQ read without complaint.

TEST(O2RingCsvParserNumericDate, DayFirstResolvedFromFilenameStamp) {
    // 02/08 and 03/08 are both <= 12, so the rows alone cannot say which
    // component is the day. The filename is named after the first sample.
    std::string csv =
        "Time,Oxygen Level,Pulse Rate,Motion\r\n"
        "21:56:34 02/08/2026,94,73,13\r\n"
        "21:56:38 02/08/2026,95,73,0\r\n"
        "06:01:34 03/08/2026,96,70,1\r\n";

    auto s = parseCsv(csv, "O2Ring 0651_20260802215634.csv");
    ASSERT_EQ(s.samples.size(), 3u);
    EXPECT_EQ(s.start_time, utc(2026, 8, 2, 21, 56, 34));
    EXPECT_EQ(s.end_time, utc(2026, 8, 3, 6, 1, 34));
}

TEST(O2RingCsvParserNumericDate, ComponentAboveTwelveSettlesItWithoutFilename) {
    // 13 cannot be a month, so this is day-first no matter what the filename
    // says (or whether there is one).
    std::string csv =
        "Time,Oxygen Level,Pulse Rate,Motion\r\n"
        "22:10:00 13/08/2026,95,70,0\r\n"
        "22:10:04 13/08/2026,96,71,0\r\n";

    auto s = parseCsv(csv, "export.csv");
    ASSERT_EQ(s.samples.size(), 2u);
    EXPECT_EQ(s.start_time, utc(2026, 8, 13, 22, 10, 0));
}

TEST(O2RingCsvParserNumericDate, MonthFirstDetectedWhenSecondComponentExceedsTwelve) {
    std::string csv =
        "Time,Oxygen Level,Pulse Rate,Motion\r\n"
        "22:10:00 08/13/2026,95,70,0\r\n"
        "22:10:04 08/13/2026,96,71,0\r\n";

    auto s = parseCsv(csv, "export.csv");
    ASSERT_EQ(s.samples.size(), 2u);
    EXPECT_EQ(s.start_time, utc(2026, 8, 13, 22, 10, 0));
}

TEST(O2RingCsvParserNumericDate, MissingReadingSentinelIsKeptButMarkedInvalid) {
    // Wellue writes "- -" for a dropped reading. The row still carries a
    // timestamp and motion, so it must not abort the import.
    std::string csv =
        "Time,Oxygen Level,Pulse Rate,Motion\r\n"
        "21:56:34 02/08/2026,94,73,13\r\n"
        "06:01:38 03/08/2026,- -,- -,2\r\n";

    auto s = parseCsv(csv, "O2Ring 0651_20260802215634.csv");
    ASSERT_EQ(s.samples.size(), 2u);
    EXPECT_EQ(s.samples[0].spo2, 94);
    EXPECT_EQ(s.samples[1].invalid_flag, 1);
}

TEST(O2RingCsvParserNumericDate, MidnightCrossingResolvesMonthFirst) {
    // US export where every component is <= 12 and the filename stamp is gone
    // (user renamed the file). The date rolls 08/05 -> 08/06 at midnight, and
    // only the day can tick up by one overnight, so the second component is
    // the day: August 5th, not May 8th.
    std::string csv =
        "Time,Oxygen Level,Pulse Rate,Motion\r\n"
        "23:59:58 08/05/2026,95,70,0\r\n"
        "00:00:02 08/06/2026,96,71,0\r\n";

    auto s = parseCsv(csv, "renamed.csv");
    ASSERT_EQ(s.samples.size(), 2u);
    EXPECT_EQ(s.start_time, utc(2026, 8, 5, 23, 59, 58));
    EXPECT_EQ(s.end_time, utc(2026, 8, 6, 0, 0, 2));
}

TEST(O2RingCsvParserNumericDate, MidnightCrossingResolvesDayFirst) {
    // Same rollover in a day-first locale: 05/08 -> 06/08 means the FIRST
    // component is the day.
    std::string csv =
        "Time,Oxygen Level,Pulse Rate,Motion\r\n"
        "23:59:58 05/08/2026,95,70,0\r\n"
        "00:00:02 06/08/2026,96,71,0\r\n";

    auto s = parseCsv(csv, "renamed.csv");
    ASSERT_EQ(s.samples.size(), 2u);
    EXPECT_EQ(s.start_time, utc(2026, 8, 5, 23, 59, 58));
}

TEST(O2RingCsvParserNumericDate, AmPmClockImpliesMonthFirstAsLastResort) {
    // Single-day US nap recording (issue seen on ticket 67: "won't take July
    // dates"): no component above 12, no midnight crossing, filename renamed.
    // The 12-hour clock is the remaining tell that the phone locale is
    // US-style, so 07/05 is July 5th, not May 7th.
    std::string csv =
        "Time,Oxygen Level,Pulse Rate,Motion\r\n"
        "\"02:10:00PM 07/05/2026\",95,70,0\r\n"
        "\"02:10:04PM 07/05/2026\",96,71,0\r\n";

    auto s = parseCsv(csv, "renamed.csv");
    ASSERT_EQ(s.samples.size(), 2u);
    EXPECT_EQ(s.start_time, utc(2026, 7, 5, 14, 10, 0));
}

TEST(O2RingCsvParserNumericDate, TwentyFourHourClockKeepsDayFirstDefault) {
    // The mirror case: 24-hour clock, nothing else to go on. Stays day-first,
    // the pre-existing default: 7 May.
    std::string csv =
        "Time,Oxygen Level,Pulse Rate,Motion\r\n"
        "14:10:00 07/05/2026,95,70,0\r\n"
        "14:10:04 07/05/2026,96,71,0\r\n";

    auto s = parseCsv(csv, "renamed.csv");
    ASSERT_EQ(s.samples.size(), 2u);
    EXPECT_EQ(s.start_time, utc(2026, 5, 7, 14, 10, 0));
}

TEST(O2RingCsvParserNumericDate, MonthNameFormatStillParses) {
    std::string csv =
        "Time,Oxygen Level,Pulse Rate,Motion\n"
        "06:53:07 Apr 12 2026,95,70,0\n"
        "06:53:11 Apr 12 2026,96,71,0\n";

    auto s = parseCsv(csv, "whatever.csv");
    ASSERT_EQ(s.samples.size(), 2u);
    EXPECT_EQ(s.start_time, utc(2026, 4, 12, 6, 53, 7));
}
