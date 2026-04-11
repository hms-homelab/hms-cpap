#include <gtest/gtest.h>
#include "services/SessionDiscoveryService.h"

#include <filesystem>
#include <fstream>

using namespace hms_cpap;
namespace fs = std::filesystem;

// Helper: create an empty file with the given name in the given directory
static void touchFile(const std::string& dir, const std::string& name) {
    std::ofstream ofs(dir + "/" + name);
    ofs << "dummy";
}

// ── Bug #1: Session gap should use minutes, not hours ───────────────────────

class SessionGapTest : public ::testing::Test {
protected:
    std::string tmp_dir;

    void SetUp() override {
        tmp_dir = "/tmp/cpap_test_gap_" + std::to_string(getpid());
        fs::create_directories(tmp_dir);
    }

    void TearDown() override {
        fs::remove_all(tmp_dir);
    }
};

TEST_F(SessionGapTest, SubHourGapSplitsIntoTwoSessions) {
    // Two BRP files 45 minutes apart — gap > default 60? No, but let's set threshold to 30.
    // Actually, default SESSION_GAP_MINUTES is 60. We'll use a 90-minute gap which the old
    // hours-based code would truncate to 1 hour (60 min) — still >= 60 min threshold, so it
    // would correctly split. The real bug is with gaps < 60 min.
    //
    // Scenario: Two sessions 45 minutes apart, threshold set to 30 minutes.
    // Old bug: 45 min -> cast to hours -> 0 hours -> 0 min, never >= 30 min -> NO split.
    // Fix: 45 min -> cast to minutes -> 45 min >= 30 min -> SPLIT.
    //
    // We'll use a custom config for this test.

    // Session 1: starts at 22:00
    touchFile(tmp_dir, "20260301_220000_BRP.edf");
    // Session 2: starts at 22:45 (45 minutes later)
    touchFile(tmp_dir, "20260301_224500_BRP.edf");
    // CSL for session 1
    touchFile(tmp_dir, "20260301_220000_CSL.edf");
    // CSL for session 2
    touchFile(tmp_dir, "20260301_224500_CSL.edf");

    // Set gap threshold to 30 minutes
    setenv("SESSION_GAP_MINUTES", "30", 1);

    auto sessions = SessionDiscoveryService::groupLocalFolder(tmp_dir, "20260301");

    unsetenv("SESSION_GAP_MINUTES");

    // Should split into 2 sessions (45 min gap > 30 min threshold)
    ASSERT_EQ(sessions.size(), 2);
    EXPECT_EQ(sessions[0].session_prefix, "20260301_220000");
    EXPECT_EQ(sessions[1].session_prefix, "20260301_224500");
}

TEST_F(SessionGapTest, GapUnderThresholdDoesNotSplit) {
    // Two BRP files 20 minutes apart, threshold is 30 minutes — should NOT split
    touchFile(tmp_dir, "20260301_220000_BRP.edf");
    touchFile(tmp_dir, "20260301_222000_BRP.edf");

    setenv("SESSION_GAP_MINUTES", "30", 1);

    auto sessions = SessionDiscoveryService::groupLocalFolder(tmp_dir, "20260301");

    unsetenv("SESSION_GAP_MINUTES");

    // Should be 1 session (20 min gap < 30 min threshold)
    ASSERT_EQ(sessions.size(), 1);
    EXPECT_EQ(sessions[0].brp_files.size(), 2);
}

TEST_F(SessionGapTest, NinetyMinuteGapSplitsWithDefaultThreshold) {
    // 90-minute gap with default 60-min threshold. Old bug: 90 min -> 1 hour (60 min) >= 60 min
    // which happened to work, but 89 min -> 1 hour (60 min) >= 60 would also work while
    // 59 min -> 0 hours (0 min) would NOT. This test verifies the boundary is correct.
    touchFile(tmp_dir, "20260301_220000_BRP.edf");
    touchFile(tmp_dir, "20260301_223000_BRP.edf"); // 30 min later — same session
    touchFile(tmp_dir, "20260302_000100_BRP.edf"); // 91 min after first — new session

    auto sessions = SessionDiscoveryService::groupLocalFolder(tmp_dir, "20260301");

    // Default threshold is 60 min. 91-min gap should split.
    ASSERT_EQ(sessions.size(), 2);
    EXPECT_EQ(sessions[0].brp_files.size(), 2);
    EXPECT_EQ(sessions[1].brp_files.size(), 1);
}

// ── Bug #2: CSL/EVE map entries must be erased after matching ────────────────

class CSLEVEMapTest : public ::testing::Test {
protected:
    std::string tmp_dir;

    void SetUp() override {
        tmp_dir = "/tmp/cpap_test_csl_" + std::to_string(getpid());
        fs::create_directories(tmp_dir);
    }

    void TearDown() override {
        fs::remove_all(tmp_dir);
    }
};

TEST_F(CSLEVEMapTest, MultipleSessionsGetCorrectCSLFiles) {
    // Two sessions 2 hours apart, each with their own CSL and EVE files.
    // Old bug: last session would steal the first session's CSL because matched
    // entries were never erased from the map.

    // Session 1: 22:00
    touchFile(tmp_dir, "20260301_220000_BRP.edf");
    touchFile(tmp_dir, "20260301_220000_CSL.edf");
    touchFile(tmp_dir, "20260301_220000_EVE.edf");

    // Session 2: 00:30 (2.5 hours later)
    touchFile(tmp_dir, "20260302_003000_BRP.edf");
    touchFile(tmp_dir, "20260302_003000_CSL.edf");
    touchFile(tmp_dir, "20260302_003000_EVE.edf");

    auto sessions = SessionDiscoveryService::groupLocalFolder(tmp_dir, "20260301");

    ASSERT_EQ(sessions.size(), 2);

    // Session 1 should have its own CSL/EVE
    EXPECT_EQ(sessions[0].csl_file, "20260301_220000_CSL.edf");
    EXPECT_EQ(sessions[0].eve_file, "20260301_220000_EVE.edf");

    // Session 2 should have its own CSL/EVE — NOT session 1's
    EXPECT_EQ(sessions[1].csl_file, "20260302_003000_CSL.edf");
    EXPECT_EQ(sessions[1].eve_file, "20260302_003000_EVE.edf");
}

TEST_F(CSLEVEMapTest, ThreeSessionsEachGetOwnCSL) {
    // Three sessions in one night — stress test the erase logic
    touchFile(tmp_dir, "20260301_210000_BRP.edf");
    touchFile(tmp_dir, "20260301_210000_CSL.edf");

    touchFile(tmp_dir, "20260301_233000_BRP.edf");
    touchFile(tmp_dir, "20260301_233000_CSL.edf");

    touchFile(tmp_dir, "20260302_020000_BRP.edf");
    touchFile(tmp_dir, "20260302_020000_CSL.edf");

    auto sessions = SessionDiscoveryService::groupLocalFolder(tmp_dir, "20260301");

    ASSERT_EQ(sessions.size(), 3);
    EXPECT_EQ(sessions[0].csl_file, "20260301_210000_CSL.edf");
    EXPECT_EQ(sessions[1].csl_file, "20260301_233000_CSL.edf");
    EXPECT_EQ(sessions[2].csl_file, "20260302_020000_CSL.edf");
}

TEST_F(CSLEVEMapTest, LastSessionGetsUnmatchedCSL) {
    // One session at 22:00, CSL written at 06:00 the next morning (user slept through).
    // CSL time doesn't match session start within 12 hours, but it should still
    // be assigned to the last (only) session via is_last_session fallback.
    touchFile(tmp_dir, "20260301_220000_BRP.edf");
    touchFile(tmp_dir, "20260302_060000_CSL.edf");

    auto sessions = SessionDiscoveryService::groupLocalFolder(tmp_dir, "20260301");

    ASSERT_EQ(sessions.size(), 1);
    EXPECT_EQ(sessions[0].csl_file, "20260302_060000_CSL.edf");
}
