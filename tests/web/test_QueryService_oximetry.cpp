/**
 * QueryService oximetry night-assignment tests.
 *
 * Regression cover for the bug where every SpO2 recording was returned for two
 * consecutive nights. getSessionOximetry used to match a session if ANY of six
 * predicates held — cpap_session_date equal to the requested night OR the next
 * one, plus four filename LIKE patterns against both dates — and nothing ever
 * narrowed that back down. A single import showed the identical trace on both
 * nights, which is how an owner found it.
 *
 * A recording belongs to exactly one night: date(start_time - 12 hours), the
 * same rule the CPAP side uses, so the SpO2 chart and the therapy chart agree
 * on which night they are drawing.
 *
 * Oximetry timestamps are written with gmtime (see the comment above
 * SQLiteDatabase::fmtOximetryTimestamp — the parsers read the ring's printed
 * time AS IF it were UTC), so every epoch below is chosen for its UTC wall
 * clock and these assertions hold in any host timezone.
 */

#include <gtest/gtest.h>
#include "database/SQLiteDatabase.h"
#include "web/QueryService.h"

#include <chrono>
#include <filesystem>
#include <string>
#include <unistd.h>

using namespace hms_cpap;
using namespace std::chrono;
namespace fs = std::filesystem;

namespace {

system_clock::time_point tpFromEpoch(long secs) {
    return system_clock::time_point{} + seconds(secs);
}

// Three recordings across two nights. The middle one is the whole point: it
// starts after midnight, so its calendar date is the 6th but it belongs to the
// night of the 5th.
constexpr long kEveningOf5th = 1778020200;  // 2026-05-05 22:30:00 UTC -> night of the 5th
constexpr long kSmallHoursOf6th = 1778043404;  // 2026-05-06 04:56:44 UTC -> night of the 5th
constexpr long kAfternoonOf6th = 1778072400;  // 2026-05-06 13:00:00 UTC -> night of the 6th

class QueryServiceOximetryTest : public ::testing::Test {
protected:
    void SetUp() override {
        static int counter = 0;
        db_path_ = (fs::temp_directory_path() /
                    ("hms_cpap_qs_oxi_test_" + std::to_string(::getpid()) +
                     "_" + std::to_string(counter++) + ".db")).string();
        cleanupFiles();

        db_ = std::make_shared<SQLiteDatabase>(db_path_);
        ASSERT_TRUE(db_->connect());
        qs_ = std::make_unique<QueryService>(db_, "cpap-test");
    }

    void TearDown() override {
        qs_.reset();
        db_.reset();
        cleanupFiles();
    }

    void cleanupFiles() {
        for (const auto* suffix : {"", "-wal", "-shm"}) {
            std::error_code ec;
            fs::remove(db_path_ + suffix, ec);
        }
    }

    // Saves a session of `sample_count` valid samples one second apart, and
    // deliberately sets cpap_session_date to the recording's own calendar date
    // — which is what the ring does, and what the old query was trying (and
    // failing) to compensate for.
    void addSession(const std::string& filename, long start_epoch, int sample_count) {
        OximetrySession os;
        os.filename = filename;
        os.start_time = tpFromEpoch(start_epoch);
        os.end_time = tpFromEpoch(start_epoch + sample_count);
        os.duration_seconds = sample_count;
        os.sample_interval = 1.0;
        os.metrics.valid_samples = sample_count;
        os.metrics.total_samples = sample_count;

        for (int i = 0; i < sample_count; ++i) {
            OximetrySample s{};
            s.spo2 = 95;
            s.heart_rate = 58;
            s.invalid_flag = 0;
            s.motion = 0;
            s.vibration = 0;
            s.timestamp = tpFromEpoch(start_epoch + i);
            os.samples.push_back(s);
        }
        ASSERT_TRUE(db_->saveOximetrySession("o2ring", os));
    }

    int sampleCountFor(const std::string& night) {
        auto r = qs_->getSessionOximetry(night, 1);
        return static_cast<int>(r["spo2"].size());
    }

    std::string db_path_;
    std::shared_ptr<SQLiteDatabase> db_;
    std::unique_ptr<QueryService> qs_;
};

// The regression. Before the fix this session was returned for BOTH nights.
TEST_F(QueryServiceOximetryTest, AnAfterMidnightRecordingBelongsOnlyToTheNightItStarted) {
    addSession("20260506045644", kSmallHoursOf6th, 7);

    EXPECT_EQ(sampleCountFor("2026-05-05"), 7)
        << "a 04:56 recording belongs to the night of the 5th";
    EXPECT_EQ(sampleCountFor("2026-05-06"), 0)
        << "and must NOT also be served for the 6th";
}

TEST_F(QueryServiceOximetryTest, AnEveningRecordingBelongsToThatSameEvening) {
    addSession("20260505223000", kEveningOf5th, 5);

    EXPECT_EQ(sampleCountFor("2026-05-05"), 5);
    EXPECT_EQ(sampleCountFor("2026-05-04"), 0)
        << "the previous night must not absorb it either";
}

// Both sides of midnight are one night, and the next night is untouched.
TEST_F(QueryServiceOximetryTest, RecordingsEitherSideOfMidnightGroupIntoOneNight) {
    addSession("20260505223000", kEveningOf5th, 5);
    addSession("20260506045644", kSmallHoursOf6th, 7);
    addSession("20260506130000", kAfternoonOf6th, 3);

    EXPECT_EQ(sampleCountFor("2026-05-05"), 12) << "the evening plus the small hours";
    EXPECT_EQ(sampleCountFor("2026-05-06"), 3) << "only the afternoon recording";
}

// The night boundary is noon, matching sql::sleepDay everywhere else.
TEST_F(QueryServiceOximetryTest, NoonIsTheBoundaryBetweenNights) {
    addSession("before_noon", kAfternoonOf6th - 3600 * 2, 1);  // 11:00 -> night of the 5th
    addSession("after_noon", kAfternoonOf6th - 3600, 1);       // 12:00 -> night of the 6th

    EXPECT_EQ(sampleCountFor("2026-05-05"), 1);
    EXPECT_EQ(sampleCountFor("2026-05-06"), 1);
}

// The route hands us whatever punctuation the caller used.
TEST_F(QueryServiceOximetryTest, TheNightAcceptsBothDateSpellings) {
    addSession("20260506045644", kSmallHoursOf6th, 7);

    EXPECT_EQ(sampleCountFor("2026-05-05"), 7);
    EXPECT_EQ(sampleCountFor("20260505"), 7);
}

// Filenames used to be matched with LIKE '%date%', so a name carrying an
// unrelated date could pull a recording onto the wrong night entirely.
TEST_F(QueryServiceOximetryTest, TheFilenameNoLongerDecidesTheNight) {
    addSession("export_20260601_to_20260630.csv", kSmallHoursOf6th, 7);

    EXPECT_EQ(sampleCountFor("2026-05-05"), 7) << "start_time decides, not the name";
    EXPECT_EQ(sampleCountFor("2026-06-01"), 0);
    EXPECT_EQ(sampleCountFor("2026-06-30"), 0);
}

TEST_F(QueryServiceOximetryTest, ANightWithNoRecordingsReturnsEmptyRatherThanBorrowing) {
    addSession("20260506045644", kSmallHoursOf6th, 7);

    EXPECT_EQ(sampleCountFor("2026-05-07"), 0);
    EXPECT_EQ(sampleCountFor("2026-05-08"), 0);
}

} // namespace
