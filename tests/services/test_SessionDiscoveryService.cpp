#include <gtest/gtest.h>
#include "services/SessionDiscoveryService.h"
#include "clients/IDataSource.h"
#include "clients/EzShareClient.h"

#include <filesystem>
#include <fstream>
#include <string>

using namespace hms_cpap;
namespace fs = std::filesystem;

// Helper: create an empty file with the given name in the given directory
static void touchFile(const std::string& dir, const std::string& name) {
    std::ofstream ofs(dir + "/" + name);
    ofs << "dummy";
}

// Helper: create a file padded out to roughly `size_kb` kilobytes so that
// std::filesystem::file_size() / 1024 yields the requested size_kb.
static void touchFileSized(const std::string& dir, const std::string& name, int size_kb) {
    std::ofstream ofs(dir + "/" + name, std::ios::binary);
    std::string chunk(1024, 'x');
    for (int i = 0; i < size_kb; ++i) {
        ofs.write(chunk.data(), static_cast<std::streamsize>(chunk.size()));
    }
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

// ── Tickets 39/41: gaps are END-to-start, a long block's duration is not a gap ─

TEST_F(SessionGapTest, LongBlockThenShortMaskOffBreak_StaysOneSession) {
    // Michael's 2026-07-08 night, real sizes: a 4h07m evening block (BRP grows
    // ~6 KB/min, so 1449 KB ≈ 241 min), a ~9-minute mask-off break, a 10-minute
    // blip, another ~10-minute break, then the rest of the night. Start-to-start
    // measured the first "gap" as 255 min and split the night in half (halved
    // durations, doubled apparent AHI). End-to-start sees ~15 min and keeps ONE
    // session. File mtimes here are "now" (implausible vs the 2026-03 prefixes),
    // so the BRP size estimate is what carries the test — as it does for copied
    // data where mtimes lie.
    touchFileSized(tmp_dir, "20260301_230200_BRP.edf", 1449);
    touchFileSized(tmp_dir, "20260301_230200_PLD.edf", 133);
    touchFileSized(tmp_dir, "20260301_230200_SAD.edf", 60);
    touchFile(tmp_dir, "20260301_230200_CSL.edf");
    touchFile(tmp_dir, "20260301_230200_EVE.edf");
    touchFileSized(tmp_dir, "20260302_031800_BRP.edf", 43);
    touchFileSized(tmp_dir, "20260302_031800_PLD.edf", 7);
    touchFileSized(tmp_dir, "20260302_031800_SAD.edf", 3);
    touchFileSized(tmp_dir, "20260302_033800_BRP.edf", 1525);
    touchFileSized(tmp_dir, "20260302_033800_PLD.edf", 141);
    touchFileSized(tmp_dir, "20260302_033800_SAD.edf", 63);

    auto sessions = SessionDiscoveryService::groupLocalFolder(tmp_dir, "20260301");

    ASSERT_EQ(sessions.size(), 1) << "long block + short breaks must stay one night";
    EXPECT_EQ(sessions[0].brp_files.size(), 3);
    EXPECT_EQ(sessions[0].session_prefix, "20260301_230200");
    EXPECT_FALSE(sessions[0].eve_file.empty()) << "the night keeps its EVE";
}

TEST_F(SessionGapTest, RealGapAfterLongBlock_StillSplits) {
    // A genuinely separate second session after a long block must still split:
    // 1449 KB block starting 22:00 ends ~01:41; next start 05:00 -> ~199 min
    // real mask-off gap >= 60 min threshold.
    touchFileSized(tmp_dir, "20260301_220000_BRP.edf", 1449);
    touchFileSized(tmp_dir, "20260302_050000_BRP.edf", 100);

    auto sessions = SessionDiscoveryService::groupLocalFolder(tmp_dir, "20260301");

    ASSERT_EQ(sessions.size(), 2);
    EXPECT_EQ(sessions[0].brp_files.size(), 1);
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

// ── groupLocalFolder: file-type collection, sizing, and edge cases ───────────

class GroupLocalFolderTest : public ::testing::Test {
protected:
    std::string tmp_dir;

    void SetUp() override {
        tmp_dir = "/tmp/cpap_test_glf_" + std::to_string(getpid());
        fs::create_directories(tmp_dir);
    }

    void TearDown() override {
        fs::remove_all(tmp_dir);
        unsetenv("SESSION_GAP_MINUTES");
    }
};

TEST_F(GroupLocalFolderTest, NonexistentDirReturnsEmpty) {
    auto sessions = SessionDiscoveryService::groupLocalFolder(
        tmp_dir + "/does_not_exist", "20260301");
    EXPECT_TRUE(sessions.empty());
}

TEST_F(GroupLocalFolderTest, EmptyDirReturnsEmpty) {
    auto sessions = SessionDiscoveryService::groupLocalFolder(tmp_dir, "20260301");
    EXPECT_TRUE(sessions.empty());
}

TEST_F(GroupLocalFolderTest, FolderWithOnlyCslNoCheckpointsReturnsEmpty) {
    // CSL/EVE present but no BRP/PLD/SAD checkpoint files -> no sessions
    touchFile(tmp_dir, "20260301_220000_CSL.edf");
    touchFile(tmp_dir, "20260301_220000_EVE.edf");

    auto sessions = SessionDiscoveryService::groupLocalFolder(tmp_dir, "20260301");
    EXPECT_TRUE(sessions.empty());
}

TEST_F(GroupLocalFolderTest, FilesWithoutValidPrefixAreSkipped) {
    // Garbage / non-conforming filenames must be ignored, leaving one real session.
    touchFile(tmp_dir, "random.txt");
    touchFile(tmp_dir, "notes_BRP.edf");          // no YYYYMMDD_HHMMSS prefix
    touchFile(tmp_dir, "STR.edf");                // no prefix
    touchFile(tmp_dir, "20260301_220000_BRP.edf"); // the only valid checkpoint

    auto sessions = SessionDiscoveryService::groupLocalFolder(tmp_dir, "20260301");

    ASSERT_EQ(sessions.size(), 1);
    EXPECT_EQ(sessions[0].brp_files.size(), 1);
    EXPECT_EQ(sessions[0].brp_files[0], "20260301_220000_BRP.edf");
}

TEST_F(GroupLocalFolderTest, BrpPldSadAllCollectedInOneSession) {
    // A single session containing every checkpoint type plus CSL/EVE.
    touchFile(tmp_dir, "20260301_220000_BRP.edf");
    touchFile(tmp_dir, "20260301_220000_PLD.edf");
    touchFile(tmp_dir, "20260301_220000_SAD.edf");
    touchFile(tmp_dir, "20260301_220000_CSL.edf");
    touchFile(tmp_dir, "20260301_220000_EVE.edf");

    auto sessions = SessionDiscoveryService::groupLocalFolder(tmp_dir, "20260301");

    ASSERT_EQ(sessions.size(), 1);
    const auto& s = sessions[0];
    EXPECT_EQ(s.brp_files.size(), 1);
    EXPECT_EQ(s.pld_files.size(), 1);
    EXPECT_EQ(s.sad_files.size(), 1);
    EXPECT_EQ(s.csl_file, "20260301_220000_CSL.edf");
    EXPECT_EQ(s.eve_file, "20260301_220000_EVE.edf");
    EXPECT_TRUE(s.hasData());
    EXPECT_TRUE(s.isComplete());
    EXPECT_EQ(s.date_folder, "20260301");
}

TEST_F(GroupLocalFolderTest, Sa2OximetryFileTreatedAsSad) {
    // _SA2.edf is the newer-device oximetry suffix; should be collected as SAD.
    touchFile(tmp_dir, "20260301_220000_BRP.edf");
    touchFile(tmp_dir, "20260301_220000_SA2.edf");

    auto sessions = SessionDiscoveryService::groupLocalFolder(tmp_dir, "20260301");

    ASSERT_EQ(sessions.size(), 1);
    ASSERT_EQ(sessions[0].sad_files.size(), 1);
    EXPECT_EQ(sessions[0].sad_files[0], "20260301_220000_SA2.edf");
}

TEST_F(GroupLocalFolderTest, MultipleCheckpointsAccumulateSizesAndCount) {
    // Several BRP checkpoints in one session: ALL are kept (not deduped) and the
    // total_size_kb accumulates every file size plus the CSL size.
    touchFileSized(tmp_dir, "20260301_220000_BRP.edf", 2);   // ~2 KB
    touchFileSized(tmp_dir, "20260301_221000_BRP.edf", 5);   // ~5 KB, 10 min later
    touchFileSized(tmp_dir, "20260301_222000_BRP.edf", 3);   // ~3 KB, 20 min later
    touchFileSized(tmp_dir, "20260301_220000_CSL.edf", 1);   // ~1 KB

    setenv("SESSION_GAP_MINUTES", "60", 1);

    auto sessions = SessionDiscoveryService::groupLocalFolder(tmp_dir, "20260301");

    ASSERT_EQ(sessions.size(), 1);
    const auto& s = sessions[0];
    EXPECT_EQ(s.brp_files.size(), 3);
    // per-file sizes recorded
    EXPECT_EQ(s.file_sizes_kb.at("20260301_220000_BRP.edf"), 2);
    EXPECT_EQ(s.file_sizes_kb.at("20260301_221000_BRP.edf"), 5);
    EXPECT_EQ(s.file_sizes_kb.at("20260301_222000_BRP.edf"), 3);
    EXPECT_EQ(s.file_sizes_kb.at("20260301_220000_CSL.edf"), 1);
    // total = 2 + 5 + 3 + 1 (CSL) = 11
    EXPECT_EQ(s.total_size_kb, 11);
}

TEST_F(GroupLocalFolderTest, EveMatchedByTimeNotOnlyLastSession) {
    // Two sessions; first session's EVE should match by 12-hour time proximity
    // (not just the is_last_session fallback) and be assigned to session 0.
    touchFile(tmp_dir, "20260301_200000_BRP.edf");
    touchFile(tmp_dir, "20260301_200000_EVE.edf");
    touchFile(tmp_dir, "20260301_230000_BRP.edf"); // 3h later -> new session
    touchFile(tmp_dir, "20260301_230000_EVE.edf");

    auto sessions = SessionDiscoveryService::groupLocalFolder(tmp_dir, "20260301");

    ASSERT_EQ(sessions.size(), 2);
    EXPECT_EQ(sessions[0].eve_file, "20260301_200000_EVE.edf");
    EXPECT_EQ(sessions[1].eve_file, "20260301_230000_EVE.edf");
}

TEST_F(GroupLocalFolderTest, LowercaseSuffixesAreRecognized) {
    // Suffix matching is case-insensitive.
    touchFile(tmp_dir, "20260301_220000_brp.edf");
    touchFile(tmp_dir, "20260301_220000_pld.edf");
    touchFile(tmp_dir, "20260301_220000_csl.edf");

    auto sessions = SessionDiscoveryService::groupLocalFolder(tmp_dir, "20260301");

    ASSERT_EQ(sessions.size(), 1);
    EXPECT_EQ(sessions[0].brp_files.size(), 1);
    EXPECT_EQ(sessions[0].pld_files.size(), 1);
    EXPECT_EQ(sessions[0].csl_file, "20260301_220000_csl.edf");
}

// ── discoverLocalSessions: folder enumeration & date filtering ───────────────

class DiscoverLocalSessionsTest : public ::testing::Test {
protected:
    std::string root;  // simulated DATALOG dir

    void SetUp() override {
        root = "/tmp/cpap_test_dls_" + std::to_string(getpid());
        fs::create_directories(root);
    }

    void TearDown() override {
        fs::remove_all(root);
    }

    std::string makeDateFolder(const std::string& yyyymmdd) {
        std::string p = root + "/" + yyyymmdd;
        fs::create_directories(p);
        return p;
    }
};

TEST_F(DiscoverLocalSessionsTest, NonexistentDirReturnsEmpty) {
    auto sessions = SessionDiscoveryService::discoverLocalSessions(
        root + "/missing", std::nullopt);
    EXPECT_TRUE(sessions.empty());
}

TEST_F(DiscoverLocalSessionsTest, DirWithNoDateFoldersReturnsEmpty) {
    // Non-date subdirectory must be ignored by the YYYYMMDD regex.
    fs::create_directories(root + "/SETTINGS");
    fs::create_directories(root + "/not_a_date");

    auto sessions = SessionDiscoveryService::discoverLocalSessions(root, std::nullopt);
    EXPECT_TRUE(sessions.empty());
}

TEST_F(DiscoverLocalSessionsTest, FirstRunScansAllFoldersAndGroups) {
    // No last_session_start -> scan every date folder. is_new is true whenever
    // last_session_start is nullopt, so any date passes the per-session filter.
    std::string f1 = makeDateFolder("20200101");
    touchFile(f1, "20200101_220000_BRP.edf");
    touchFile(f1, "20200101_220000_CSL.edf");

    std::string f2 = makeDateFolder("20200102");
    touchFile(f2, "20200102_213000_BRP.edf");

    auto sessions = SessionDiscoveryService::discoverLocalSessions(root, std::nullopt);

    // Both folders contribute one session each; is_new == true (nullopt).
    ASSERT_EQ(sessions.size(), 2);
    // Sessions are returned in sorted-folder order.
    EXPECT_EQ(sessions[0].session_prefix, "20200101_220000");
    EXPECT_EQ(sessions[1].session_prefix, "20200102_213000");
}

TEST_F(DiscoverLocalSessionsTest, OldFoldersBeforeLastDateAreFilteredOut) {
    // last_session_start in the year 2200 -> all old folders are < last_date and
    // not the prev-day folder, so nothing should be scanned/returned.
    std::string f1 = makeDateFolder("20200101");
    touchFile(f1, "20200101_220000_BRP.edf");
    touchFile(f1, "20200101_220000_CSL.edf");

    std::tm tm = {};
    tm.tm_year = 2200 - 1900;
    tm.tm_mon = 5;   // June
    tm.tm_mday = 15;
    tm.tm_hour = 12;
    tm.tm_isdst = -1;
    auto last = std::chrono::system_clock::from_time_t(std::mktime(&tm));

    auto sessions = SessionDiscoveryService::discoverLocalSessions(root, last);
    EXPECT_TRUE(sessions.empty());
}

TEST_F(DiscoverLocalSessionsTest, PrevDayFolderIsIncludedForEarlyAmSessions) {
    // last_session_start = 2200-06-15. The folder for the PREVIOUS day (22000614)
    // should be pulled in even though it's < last_date, because ResMed stores
    // early-AM sessions in the prior day's folder.
    std::tm tm = {};
    tm.tm_year = 2200 - 1900;
    tm.tm_mon = 5;   // June (0-based)
    tm.tm_mday = 15;
    tm.tm_hour = 12;
    tm.tm_isdst = -1;
    auto last = std::chrono::system_clock::from_time_t(std::mktime(&tm));

    // Folder for the day BEFORE last_session_start: 22000614
    std::string prev = makeDateFolder("22000614");
    // Session start AFTER last_session_start so is_new keeps it.
    touchFile(prev, "22000615_010000_BRP.edf");
    touchFile(prev, "22000615_010000_CSL.edf");

    auto sessions = SessionDiscoveryService::discoverLocalSessions(root, last);

    // prev-day folder included; its session (start 22000615_010000 > last) is new.
    ASSERT_EQ(sessions.size(), 1);
    EXPECT_EQ(sessions[0].session_prefix, "22000615_010000");
    EXPECT_EQ(sessions[0].date_folder, "22000614");
}

TEST_F(DiscoverLocalSessionsTest, OlderSessionInRelevantFolderIsFilteredOut) {
    // The folder is >= last_date (so it's scanned), but the only session inside
    // started BEFORE last_session_start and is far in the PAST (well beyond 48h
    // ago and not today), so it must be filtered out:
    //   is_new=false (start < last), is_recent=false (>48h ago), is_today=false.
    // We use 2020 dates so the session is genuinely older than 48h vs wall clock.
    std::string f = makeDateFolder("20200615");
    // Session at 02:00, last_session_start is 12:00 same day -> session is older.
    touchFile(f, "20200615_020000_BRP.edf");
    touchFile(f, "20200615_020000_CSL.edf");

    std::tm tm = {};
    tm.tm_year = 2020 - 1900;
    tm.tm_mon = 5;
    tm.tm_mday = 15;
    tm.tm_hour = 12;
    tm.tm_isdst = -1;
    auto last = std::chrono::system_clock::from_time_t(std::mktime(&tm));

    auto sessions = SessionDiscoveryService::discoverLocalSessions(root, last);
    EXPECT_TRUE(sessions.empty());
}

// ── discoverNewSessions: ezShare path via a fake IDataSource ─────────────────

namespace {
// In-memory fake data source. Only listDateFolders/listFiles are exercised by
// discoverNewSessions / groupSessionsInFolder; download methods are no-ops.
class FakeDataSource : public IDataSource {
public:
    std::vector<std::string> date_folders;
    std::map<std::string, std::vector<EzShareFileEntry>> folder_files;

    std::vector<std::string> listDateFolders() override { return date_folders; }

    std::vector<EzShareFileEntry> listFiles(const std::string& date_folder) override {
        auto it = folder_files.find(date_folder);
        if (it == folder_files.end()) return {};
        return it->second;
    }

    bool downloadFile(const std::string&, const std::string&, const std::string&) override {
        return false;
    }
    bool downloadFileRange(const std::string&, const std::string&, const std::string&,
                           size_t, size_t&) override { return false; }
    bool downloadRootFile(const std::string&, const std::string&) override { return false; }
};

EzShareFileEntry entry(const std::string& name, int size_kb) {
    EzShareFileEntry e;
    e.name = name;
    e.size_kb = size_kb;
    return e;
}
}  // namespace

TEST(DiscoverNewSessionsTest, NoDateFoldersReturnsEmpty) {
    FakeDataSource ds;  // empty
    SessionDiscoveryService svc(ds);
    auto sessions = svc.discoverNewSessions(std::nullopt);
    EXPECT_TRUE(sessions.empty());
}

TEST(DiscoverNewSessionsTest, FirstRunGroupsCheckpointsAndPicksLargest) {
    FakeDataSource ds;
    ds.date_folders = {"20200101"};
    ds.folder_files["20200101"] = {
        entry("20200101_220000_BRP.edf", 10),
        entry("20200101_221000_BRP.edf", 40),  // 10 min later, same session
        entry("20200101_220000_PLD.edf", 5),
        entry("20200101_220000_CSL.edf", 1),
        entry("20200101_220000_EVE.edf", 2),
    };

    SessionDiscoveryService svc(ds);
    auto sessions = svc.discoverNewSessions(std::nullopt);  // nullopt -> all new

    ASSERT_EQ(sessions.size(), 1);
    const auto& s = sessions[0];
    EXPECT_EQ(s.session_prefix, "20200101_220000");
    EXPECT_EQ(s.brp_files.size(), 2);
    EXPECT_EQ(s.pld_files.size(), 1);
    EXPECT_EQ(s.csl_file, "20200101_220000_CSL.edf");
    EXPECT_EQ(s.eve_file, "20200101_220000_EVE.edf");
    // 10 + 40 + 5 + 1 + 2 = 58
    EXPECT_EQ(s.total_size_kb, 58);
}

TEST(DiscoverNewSessionsTest, GapSplitsIntoTwoSessionsOverEzShare) {
    FakeDataSource ds;
    ds.date_folders = {"20200101"};
    ds.folder_files["20200101"] = {
        entry("20200101_200000_BRP.edf", 10),
        entry("20200101_230000_BRP.edf", 12),  // 3h later -> new session
    };
    setenv("SESSION_GAP_MINUTES", "60", 1);

    SessionDiscoveryService svc(ds);
    auto sessions = svc.discoverNewSessions(std::nullopt);

    unsetenv("SESSION_GAP_MINUTES");

    ASSERT_EQ(sessions.size(), 2);
    EXPECT_EQ(sessions[0].session_prefix, "20200101_200000");
    EXPECT_EQ(sessions[1].session_prefix, "20200101_230000");
}

TEST(DiscoverNewSessionsTest, FolderWithNoCheckpointsYieldsNoSessions) {
    FakeDataSource ds;
    ds.date_folders = {"20200101"};
    ds.folder_files["20200101"] = {
        entry("20200101_220000_CSL.edf", 1),
        entry("20200101_220000_EVE.edf", 2),
    };

    SessionDiscoveryService svc(ds);
    auto sessions = svc.discoverNewSessions(std::nullopt);
    EXPECT_TRUE(sessions.empty());
}

TEST(DiscoverNewSessionsTest, FoldersBeforeLastDateAreFilteredOut) {
    // last_session_start far in the future -> the only (old) folder is excluded,
    // so no folders are scanned and the result is empty.
    FakeDataSource ds;
    ds.date_folders = {"20200101"};
    ds.folder_files["20200101"] = {
        entry("20200101_220000_BRP.edf", 10),
    };

    std::tm tm = {};
    tm.tm_year = 2200 - 1900;
    tm.tm_mon = 5;
    tm.tm_mday = 15;
    tm.tm_hour = 12;
    tm.tm_isdst = -1;
    auto last = std::chrono::system_clock::from_time_t(std::mktime(&tm));

    SessionDiscoveryService svc(ds);
    auto sessions = svc.discoverNewSessions(last);
    EXPECT_TRUE(sessions.empty());
}

TEST(DiscoverNewSessionsTest, AlreadyStoredOldSessionIsSkipped) {
    // Folder >= last_date so it's scanned, but the session predates
    // last_session_start and is ancient (not today, not within 48h) -> skipped.
    FakeDataSource ds;
    ds.date_folders = {"20200615"};
    ds.folder_files["20200615"] = {
        entry("20200615_020000_BRP.edf", 10),
        entry("20200615_020000_CSL.edf", 1),
    };

    std::tm tm = {};
    tm.tm_year = 2020 - 1900;
    tm.tm_mon = 5;
    tm.tm_mday = 15;
    tm.tm_hour = 12;  // after the 02:00 session
    tm.tm_isdst = -1;
    auto last = std::chrono::system_clock::from_time_t(std::mktime(&tm));

    SessionDiscoveryService svc(ds);
    auto sessions = svc.discoverNewSessions(last);
    EXPECT_TRUE(sessions.empty());
}
