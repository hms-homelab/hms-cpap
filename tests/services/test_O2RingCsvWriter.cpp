// SDD-015: the ring's oximetry goes to SleepHQ as the CSV its own app exports.
//
// The writer is the parser's inverse, so the test that matters is the round
// trip: parse(write(x)) == x. Both halves are ours, so anything that drifts
// between them shows up here rather than in a SleepHQ import that quietly
// contains the wrong night.
//
// Fixtures are written by hand in the shape of the real exports. The three
// files that settled the format are a customer's therapy data sitting in a
// support ticket and do not belong in this repo.
#include <gtest/gtest.h>

#include "services/O2RingCsvWriter.h"
#include "services/O2RingCsvParser.h"
#include "utils/TimeCompat.h"

#include <algorithm>   // std::count -- GCC 14 dropped the transitive include
#include <chrono>
#include <string>

using namespace hms_cpap;
using cpapdash::parser::OximetrySample;
using cpapdash::parser::OximetrySession;

namespace {

// UTC, because that is the clock O2RingCsvParser reads these strings in
// (timegm), and therefore the clock the writer has to render them in.
std::chrono::system_clock::time_point utcTime(int y, int mon, int d,
                                              int h, int mi, int s) {
    std::tm tm{};
    tm.tm_year = y - 1900; tm.tm_mon = mon - 1; tm.tm_mday = d;
    tm.tm_hour = h; tm.tm_min = mi; tm.tm_sec = s;
    return std::chrono::system_clock::from_time_t(timegm_utc(&tm));
}

OximetrySample sample(std::chrono::system_clock::time_point ts,
                      uint8_t spo2, uint8_t hr, uint8_t motion = 0) {
    OximetrySample s{};
    s.timestamp = ts;
    s.spo2 = spo2;
    s.heart_rate = hr;
    s.invalid_flag = (spo2 == 0xFF) ? 1 : 0;
    s.motion = motion;
    s.vibration = 0;
    return s;
}

OximetrySession night() {
    OximetrySession s;
    auto t0 = utcTime(2026, 6, 19, 23, 20, 29);
    for (int i = 0; i < 6; ++i) {
        s.samples.push_back(sample(t0 + std::chrono::seconds(i * 4),
                                   static_cast<uint8_t>(95 + (i % 3)),
                                   static_cast<uint8_t>(60 + i),
                                   static_cast<uint8_t>(i)));
    }
    s.start_time = s.samples.front().timestamp;
    s.end_time   = s.samples.back().timestamp;
    return s;
}

}  // namespace

TEST(O2RingCsvWriter, HeaderMatchesARealExport) {
    const std::string csv = O2RingCsvWriter::write(night());
    EXPECT_EQ(csv.substr(0, csv.find("\r\n")),
              "Time,Oxygen Level,Pulse Rate,Motion,O2 Reminder,PR Reminder");
}

TEST(O2RingCsvWriter, RowsHaveSixColumnsAndNoTrailingComma) {
    const std::string csv = O2RingCsvWriter::write(night());
    const size_t first = csv.find("\r\n") + 2;
    const std::string row = csv.substr(first, csv.find("\r\n", first) - first);

    EXPECT_EQ(std::count(row.begin(), row.end(), ','), 5)
        << "row was: " << row;
    EXPECT_NE(row.back(), ',') << "the Checkme dialect has no trailing comma";
    EXPECT_EQ(row.find('"'), std::string::npos) << "this dialect is unquoted";
}

// The test this file exists for.
TEST(O2RingCsvWriter, RoundTripsThroughTheParser) {
    const OximetrySession in = night();
    const auto out = O2RingCsvParser::parse(O2RingCsvWriter::write(in), "x.csv");

    ASSERT_EQ(out.samples.size(), in.samples.size());
    for (size_t i = 0; i < in.samples.size(); ++i) {
        EXPECT_EQ(out.samples[i].timestamp,  in.samples[i].timestamp)  << "at " << i;
        EXPECT_EQ(out.samples[i].spo2,       in.samples[i].spo2)       << "at " << i;
        EXPECT_EQ(out.samples[i].heart_rate, in.samples[i].heart_rate) << "at " << i;
        EXPECT_EQ(out.samples[i].motion,     in.samples[i].motion)     << "at " << i;
    }
    EXPECT_EQ(out.start_time, in.start_time);
    EXPECT_EQ(out.end_time,   in.end_time);
    EXPECT_DOUBLE_EQ(out.sample_interval, 4.0);
}

// A gap must stay a gap. Dropping unreadable samples would heal the timeline
// over and hand the interval detector the wrong cadence.
TEST(O2RingCsvWriter, OffFingerSamplesSurviveAsGapsNotAsMissingRows) {
    OximetrySession in;
    auto t0 = utcTime(2026, 6, 19, 23, 0, 0);
    in.samples.push_back(sample(t0,                              97, 60));
    in.samples.push_back(sample(t0 + std::chrono::seconds(2),  0xFF, 0xFF));  // off finger
    in.samples.push_back(sample(t0 + std::chrono::seconds(4),  0xFF, 0xFF));
    in.samples.push_back(sample(t0 + std::chrono::seconds(6),    96, 61));
    in.start_time = t0;
    in.end_time   = t0 + std::chrono::seconds(6);

    const std::string csv = O2RingCsvWriter::write(in);
    EXPECT_NE(csv.find(",255,255,"), std::string::npos)
        << "an unreadable sample must be written as the sentinel, not skipped";

    const auto out = O2RingCsvParser::parse(csv, "x.csv");
    ASSERT_EQ(out.samples.size(), 4u) << "rows were dropped, the night got shorter";
    EXPECT_EQ(out.samples[1].spo2,       0xFF);
    EXPECT_EQ(out.samples[1].heart_rate, 0xFF);
    EXPECT_FALSE(out.samples[1].valid());
    EXPECT_TRUE(out.samples[3].valid());
    // The parser defines duration as samples * interval (each sample covers
    // one interval), so four 2-second samples is 8s, not the 6s between the
    // first and last. The point of this assertion is that the gap SURVIVES:
    // drop the two unreadable rows and this becomes 2 samples = 4s.
    EXPECT_EQ(out.duration_seconds, 8);
    EXPECT_DOUBLE_EQ(out.sample_interval, 2.0);
}

// Per-second exports are what the real files are; the interval must not inflate.
TEST(O2RingCsvWriter, PerSecondSessionsKeepTheirInterval) {
    OximetrySession in;
    auto t0 = utcTime(2026, 6, 26, 0, 16, 55);
    for (int i = 0; i < 10; ++i)
        in.samples.push_back(sample(t0 + std::chrono::seconds(i), 95, 86, 29));
    in.start_time = t0;
    in.end_time   = t0 + std::chrono::seconds(9);

    const auto out = O2RingCsvParser::parse(O2RingCsvWriter::write(in), "x.csv");
    EXPECT_DOUBLE_EQ(out.sample_interval, 1.0);
    EXPECT_EQ(out.duration_seconds, 10);   // samples * interval, see above
    EXPECT_EQ(out.samples.front().motion, 29) << "motion is data, not a flag";
}

TEST(O2RingCsvWriter, AnEmptySessionWritesAHeaderAndNothingElse) {
    OximetrySession empty;
    const std::string csv = O2RingCsvWriter::write(empty);
    EXPECT_EQ(csv, "Time,Oxygen Level,Pulse Rate,Motion,O2 Reminder,PR Reminder\r\n");
}

TEST(O2RingCsvWriter, FilenameFollowsTheRingsOwnConvention) {
    OximetrySession s = night();   // starts 2026-06-19 23:20:29 UTC
    EXPECT_EQ(O2RingCsvWriter::filenameFor(s, "O2Ring S"),
              "O2Ring S_20260619232029.csv");
    EXPECT_EQ(O2RingCsvWriter::filenameFor(s), "O2Ring_20260619232029.csv");
}

// The device name can come from the database, so it is not trusted to be tame.
TEST(O2RingCsvWriter, ADeviceNameCannotEscapeTheFilename) {
    OximetrySession s = night();
    const std::string name = O2RingCsvWriter::filenameFor(s, "evil/../\"name");
    EXPECT_EQ(name.find('/'),  std::string::npos);
    EXPECT_EQ(name.find('\\'), std::string::npos);
    EXPECT_EQ(name.find('"'),  std::string::npos);
}

// The reader stays permissive on purpose: real O2Ring S files are quoted,
// 12-hour, and carry a trailing comma. Pin that the writer's narrowness did
// not creep into the parser.
TEST(O2RingCsvWriter, TheReaderStillAcceptsTheOtherDialect) {
    const std::string real_shape =
        "Time,SpO2(%),Pulse Rate(bpm),Motion,SpO2 Reminder,PR Reminder,\r\n"
        "\"11:20:29PM Jun 19, 2026\",89,60,0,0,0,\r\n"
        "\"11:20:30PM Jun 19, 2026\",90,61,0,0,0,\r\n";
    const auto s = O2RingCsvParser::parse(real_shape, "O2Ring S_20260619232029.csv");
    ASSERT_EQ(s.samples.size(), 2u);
    EXPECT_EQ(s.samples[0].spo2, 89);
    EXPECT_EQ(s.samples[1].heart_rate, 61);
}
