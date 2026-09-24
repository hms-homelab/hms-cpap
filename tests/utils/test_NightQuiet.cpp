//
// test_NightQuiet.cpp: SDD-046, a night is over after an hour with no growth.
//
// The helpers against a real SQLite database, and the collector's
// announcement over the same database. SQLite always runs; the SQL is the
// same helper for MySQL and PostgreSQL, one branch for the upsert syntax.
//
#include <gtest/gtest.h>

#include "database/SQLiteDatabase.h"
#include "services/BurstCollectorService.h"
#include "services/SyncFolderState.h"
#include "utils/NightQuiet.h"

#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <memory>
#include <string>
#include <unistd.h>

using namespace hms_cpap;
using namespace std::chrono;
namespace fs = std::filesystem;

namespace {

class NightQuietTest : public ::testing::Test {
protected:
    void SetUp() override {
        path_ = (fs::temp_directory_path() /
                 ("hms_quiet_" + std::to_string(::getpid()) + "_" +
                  ::testing::UnitTest::GetInstance()->current_test_info()->name() + ".db"))
                    .string();
        fs::remove(path_);
        db_ = std::make_shared<SQLiteDatabase>(path_);
        ASSERT_TRUE(db_->connect());
        now_ = time_point_cast<seconds>(system_clock::now());
    }
    void TearDown() override {
        db_.reset();
        fs::remove(path_);
    }
    void save(const std::string& dev, system_clock::time_point start) {
        CPAPSession s;
        s.device_id = dev;
        s.device_name = "AirSense 11";
        s.session_start = start;
        s.duration_seconds = 7 * 3600;
        ASSERT_TRUE(db_->saveSession(s));
    }

    std::string path_;
    std::shared_ptr<SQLiteDatabase> db_;
    system_clock::time_point now_;
};

}  // namespace

TEST_F(NightQuietTest, TheHourRunsFromTheLastGrowth) {
    const auto start = now_ - hours(8);
    ASSERT_TRUE(noteNightGrowth(*db_, "q1", "20260923", start, now_ - minutes(59)));
    EXPECT_TRUE(quietUnannouncedNights(*db_, "q1", now_).empty()) << "59 minutes is not an hour";

    ASSERT_TRUE(noteNightGrowth(*db_, "q1", "20260923", start, now_ - minutes(60)));
    const auto quiet = quietUnannouncedNights(*db_, "q1", now_);
    ASSERT_EQ(quiet.size(), 1u);
    EXPECT_EQ(quiet[0].date_folder, "20260923");
    EXPECT_EQ(quiet[0].newest_start, night_quiet::epochOf(start));

    EXPECT_TRUE(quietUnannouncedNights(*db_, "other", now_).empty()) << "per device";
}

TEST_F(NightQuietTest, GrowthKeepsTheNewestSessionAndReopensAnAnnouncedNight) {
    const auto first = now_ - hours(8);
    const auto second = now_ - hours(2);
    ASSERT_TRUE(noteNightGrowth(*db_, "q2", "20260923", second, now_ - hours(2)));
    ASSERT_TRUE(noteNightGrowth(*db_, "q2", "20260923", first, now_ - hours(2)));
    auto quiet = quietUnannouncedNights(*db_, "q2", now_);
    ASSERT_EQ(quiet.size(), 1u);
    EXPECT_EQ(quiet[0].newest_start, night_quiet::epochOf(second))
        << "an older session must not replace the newest";

    ASSERT_TRUE(markNightAnnounced(*db_, "q2", "20260923", now_, "sig"));
    EXPECT_TRUE(quietUnannouncedNights(*db_, "q2", now_ + hours(3)).empty());
    EXPECT_TRUE(nightHourPassed(nightHourStates(*db_, "q2"), "20260923"));

    // The mask back on after it was announced: a night nobody has seen yet.
    ASSERT_TRUE(noteNightGrowth(*db_, "q2", "20260923", second, now_));
    EXPECT_FALSE(nightHourPassed(nightHourStates(*db_, "q2"), "20260923"));
    EXPECT_EQ(quietUnannouncedNights(*db_, "q2", now_ + minutes(60)).size(), 1u);
}

TEST_F(NightQuietTest, ANightSeenWithoutGrowthIsWrittenOnceAndAnOldCloseCountsAsAnnounced) {
    ASSERT_TRUE(noteNightSeen(*db_, "q3", "20260922", now_ - hours(30), now_ - hours(2),
                              /*already_closed=*/true));
    EXPECT_TRUE(quietUnannouncedNights(*db_, "q3", now_).empty())
        << "a night closed before the rule existed is not announced again";

    ASSERT_TRUE(noteNightSeen(*db_, "q3", "20260923", now_ - hours(8), now_ - hours(2),
                              /*already_closed=*/false));
    // Seen again later: the row already exists, so its hour is not restarted.
    ASSERT_TRUE(noteNightSeen(*db_, "q3", "20260923", now_ - hours(8), now_, false));
    EXPECT_EQ(quietUnannouncedNights(*db_, "q3", now_).size(), 1u);
}

TEST_F(NightQuietTest, AFolderWithNoRowPredatesTheRuleAndReadsAsPassed) {
    const std::map<std::string, bool> none;
    EXPECT_TRUE(nightHourPassed(none, "20200101"));
}

TEST_F(NightQuietTest, TheStrSignatureMovesWithTheNightsFigures) {
    STRDailyRecord r;
    r.duration_minutes = 400;
    r.ahi = 1.2;
    const auto a = strSignature(r);
    EXPECT_EQ(a, strSignature(r));
    r.duration_minutes = 431;
    EXPECT_NE(a, strSignature(r)) << "the mask-off rewrite of the day record";
}

// ── the collector ───────────────────────────────────────────────────────────

TEST_F(NightQuietTest, TheCollectorAnnouncesAQuietNightOnce) {
    setenv("CPAP_DEVICE_ID", "q_dev", 1);
    BurstCollectorService svc(300);
    unsetenv("CPAP_DEVICE_ID");
    svc.injectDependenciesForTest(db_, nullptr, nullptr);
    svc.setSourceForTest("local");

    const auto start = now_ - hours(8);
    save("q_dev", start);
    ASSERT_TRUE(noteNightGrowth(*db_, "q_dev", "20260923", start, now_ - minutes(30)));
    EXPECT_EQ(svc.announceQuietNightsForTest(now_), 0) << "inside the hour";

    EXPECT_EQ(svc.announceQuietNightsForTest(now_ + minutes(30)), 1);
    EXPECT_EQ(svc.announceQuietNightsForTest(now_ + minutes(31)), 0) << "once";
    EXPECT_TRUE(nightHourPassed(nightHourStates(*db_, "q_dev"), "20260923"));
}

TEST_F(NightQuietTest, ARemovedNightIsMarkedButNotAnnounced) {
    setenv("CPAP_DEVICE_ID", "q_gone", 1);
    BurstCollectorService svc(300);
    unsetenv("CPAP_DEVICE_ID");
    svc.injectDependenciesForTest(db_, nullptr, nullptr);
    svc.setSourceForTest("local");

    // Growth recorded, then the night's session removed (SDD-029).
    ASSERT_TRUE(noteNightGrowth(*db_, "q_gone", "20260923", now_ - hours(8), now_ - hours(2)));
    EXPECT_EQ(svc.announceQuietNightsForTest(now_), 0);
    EXPECT_TRUE(quietUnannouncedNights(*db_, "q_gone", now_).empty())
        << "it is not asked about on every burst after";
}
