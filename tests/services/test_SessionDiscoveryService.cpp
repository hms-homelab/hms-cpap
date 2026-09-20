#include <gtest/gtest.h>
#include "services/SessionDiscoveryService.h"
#include "clients/IDataSource.h"
#include "clients/EzShareClient.h"

#include <filesystem>
#include <fstream>
#include <string>
#ifndef _WIN32
#include <unistd.h>  // geteuid — UnreadableFolderTest skips under root
#endif

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
    EXPECT_FALSE(sessions[0].eve_files.empty()) << "the night keeps its EVE";
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
    EXPECT_EQ(sessions[0].csl_files.at(0), "20260301_220000_CSL.edf");
    EXPECT_EQ(sessions[0].eve_files.at(0), "20260301_220000_EVE.edf");

    // Session 2 should have its own CSL/EVE — NOT session 1's
    EXPECT_EQ(sessions[1].csl_files.at(0), "20260302_003000_CSL.edf");
    EXPECT_EQ(sessions[1].eve_files.at(0), "20260302_003000_EVE.edf");
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
    EXPECT_EQ(sessions[0].csl_files.at(0), "20260301_210000_CSL.edf");
    EXPECT_EQ(sessions[1].csl_files.at(0), "20260301_233000_CSL.edf");
    EXPECT_EQ(sessions[2].csl_files.at(0), "20260302_020000_CSL.edf");
}

TEST_F(CSLEVEMapTest, LastSessionGetsUnmatchedCSL) {
    // One session at 22:00, CSL written at 06:00 the next morning (user slept through).
    // CSL time doesn't match session start within 12 hours, but it should still
    // be assigned to the last (only) session via is_last_session fallback.
    touchFile(tmp_dir, "20260301_220000_BRP.edf");
    touchFile(tmp_dir, "20260302_060000_CSL.edf");

    auto sessions = SessionDiscoveryService::groupLocalFolder(tmp_dir, "20260301");

    ASSERT_EQ(sessions.size(), 1);
    EXPECT_EQ(sessions[0].csl_files.at(0), "20260302_060000_CSL.edf");
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
    EXPECT_EQ(s.csl_files.at(0), "20260301_220000_CSL.edf");
    EXPECT_EQ(s.eve_files.at(0), "20260301_220000_EVE.edf");
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
    EXPECT_EQ(sessions[0].eve_files.at(0), "20260301_200000_EVE.edf");
    EXPECT_EQ(sessions[1].eve_files.at(0), "20260301_230000_EVE.edf");
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
    EXPECT_EQ(sessions[0].csl_files.at(0), "20260301_220000_csl.edf");
}

// discoverLocalSessions' tests lived here. SDD-040 left that function with no
// caller (a local folder is an IDataSource now), so it is gone, and the rules
// it pinned are tested against discoverNewSessions below, which is the path
// every transport takes.

// ── Incident 2026-07-17: unreadable date folder crash-looped the service ─────
// DATALOG date folders uploaded root-owned 0750 made the burst cycle throw an
// uncaught std::filesystem_error from directory_iterator ("cannot open
// directory: Permission denied [/data/sdcard/DATALOG/20260627]"), terminating
// the process every cycle. Discovery must skip unreadable folders with a
// warning and keep processing the readable ones.
#ifndef _WIN32

class UnreadableFolderTest : public ::testing::Test {
protected:
    std::string root;  // simulated DATALOG dir

    void SetUp() override {
        root = "/tmp/cpap_test_unreadable_" + std::to_string(getpid());
        fs::create_directories(root);
    }

    void TearDown() override {
        // Re-add owner perms (root first, then children) so remove_all can
        // descend into folders the tests locked down.
        std::error_code ec;
        fs::permissions(root, fs::perms::owner_all, fs::perm_options::add, ec);
        for (const auto& e : fs::directory_iterator(root, ec)) {
            fs::permissions(e.path(), fs::perms::owner_all,
                            fs::perm_options::add, ec);
        }
        fs::remove_all(root, ec);
    }

    std::string makeDateFolder(const std::string& yyyymmdd) {
        std::string p = root + "/" + yyyymmdd;
        fs::create_directories(p);
        return p;
    }
};

TEST_F(UnreadableFolderTest, GroupLocalFolderSkipsUnreadableFolder) {
    if (::geteuid() == 0) GTEST_SKIP() << "permission bits do not bind for root";

    std::string dir = makeDateFolder("20260627");
    touchFile(dir, "20260627_234552_BRP.edf");
    fs::permissions(dir, fs::perms::none);  // mimic root-owned 0750 upload

    std::vector<SessionFileSet> sessions;
    EXPECT_NO_THROW(
        sessions = SessionDiscoveryService::groupLocalFolder(dir, "20260627"));
    EXPECT_TRUE(sessions.empty());
}

// The two cases that drove discoverLocalSessions over the same unreadable
// folders went with it. What they protected still holds here: groupLocalFolder
// degrades instead of throwing, and it is what the live path calls per folder.

#endif  // !_WIN32

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
    EXPECT_EQ(s.csl_files.at(0), "20200101_220000_CSL.edf");
    EXPECT_EQ(s.eve_files.at(0), "20200101_220000_EVE.edf");
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

// ── The anchor rules, on the path that actually runs ─────────────────────────
//
// SDD-010's retention anchor and SDD-038's catch-up were pinned only against
// discoverLocalSessions, a second copy of these rules that SDD-040 left with no
// caller. Deleting it would have taken the only tests of behaviour the live
// path still has, so they are ported here first, driven through IDataSource
// exactly as the collector drives it. Every transport goes through this one.

TEST(DiscoverNewSessionsTest, ACaughtUpFolderIsScannedBehindTheAnchor) {
    // hms-homelab/hms-cpap#34: folders older than the newest stored night are
    // invisible to every wall-clock rule. Naming one brings its sessions back.
    FakeDataSource ds;
    ds.date_folders = {"20200101"};
    ds.folder_files["20200101"] = {
        entry("20200101_220000_BRP.edf", 10),
        entry("20200101_220000_CSL.edf", 1),
    };

    std::tm tm = {};
    tm.tm_year = 2200 - 1900;
    tm.tm_mon = 5;
    tm.tm_mday = 15;
    tm.tm_hour = 12;
    tm.tm_isdst = -1;
    const auto last = std::chrono::system_clock::from_time_t(std::mktime(&tm));

    SessionDiscoveryService svc(ds);
    ASSERT_TRUE(svc.discoverNewSessions(last).empty())
        << "precondition: the anchor hides this folder";

    auto caught = svc.discoverNewSessions(last, std::nullopt, {"20200101"});
    ASSERT_EQ(caught.size(), 1u);
    EXPECT_EQ(caught[0].session_prefix, "20200101_220000");
}

TEST(DiscoverNewSessionsTest, OnlyTheNamedFoldersAreCaughtUp) {
    // The collector's bound (three per cycle on an ez Share) only means
    // anything if naming one folder does not drag its neighbours in.
    FakeDataSource ds;
    ds.date_folders = {"20200101", "20200102", "20200103"};
    for (const auto* d : {"20200101", "20200102", "20200103"}) {
        ds.folder_files[d] = {entry(std::string(d) + "_220000_BRP.edf", 10)};
    }

    std::tm tm = {};
    tm.tm_year = 2200 - 1900;
    tm.tm_mon = 5;
    tm.tm_mday = 15;
    tm.tm_isdst = -1;
    const auto last = std::chrono::system_clock::from_time_t(std::mktime(&tm));

    SessionDiscoveryService svc(ds);
    auto caught = svc.discoverNewSessions(last, std::nullopt, {"20200102"});
    ASSERT_EQ(caught.size(), 1u);
    EXPECT_EQ(caught[0].session_prefix, "20200102_220000");
}

TEST(DiscoverNewSessionsTest, ACaughtUpFolderThatIsNotThereChangesNothing) {
    FakeDataSource ds;
    ds.date_folders = {"20200101"};
    ds.folder_files["20200101"] = {entry("20200101_220000_BRP.edf", 10)};

    SessionDiscoveryService svc(ds);
    auto sessions = svc.discoverNewSessions(std::nullopt, std::nullopt, {"20190101"});
    ASSERT_EQ(sessions.size(), 1u);
    EXPECT_EQ(sessions[0].session_prefix, "20200101_220000");
}

TEST(DiscoverNewSessionsTest, PrevDayFolderIsIncludedForEarlyAmSessions) {
    // ResMed files an early-morning session in the PREVIOUS day's folder, so
    // the folder cut has to reach one day further back than the anchor's date.
    FakeDataSource ds;
    ds.date_folders = {"22000614"};
    ds.folder_files["22000614"] = {
        entry("22000615_010000_BRP.edf", 10),
        entry("22000615_010000_CSL.edf", 1),
    };

    std::tm tm = {};
    tm.tm_year = 2200 - 1900;
    tm.tm_mon = 5;
    tm.tm_mday = 15;
    tm.tm_hour = 12;
    tm.tm_isdst = -1;
    const auto last = std::chrono::system_clock::from_time_t(std::mktime(&tm));

    SessionDiscoveryService svc(ds);
    auto sessions = svc.discoverNewSessions(last);
    ASSERT_EQ(sessions.size(), 1u);
    EXPECT_EQ(sessions[0].session_prefix, "22000615_010000");
    EXPECT_EQ(sessions[0].date_folder, "22000614");
}

TEST(DiscoverNewSessionsTest, RetainedSessionSurvivesEvenWhenYearsOld) {
    // SDD-010. Same shape as AlreadyStoredOldSessionIsSkipped above, with the
    // anchor supplied: the only difference is retain_from, which isolates it.
    // A night nobody re-observes never settles, and sits at Live for ever.
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
    tm.tm_hour = 12;
    tm.tm_isdst = -1;
    const auto last = std::chrono::system_clock::from_time_t(std::mktime(&tm));

    std::tm at = tm;
    at.tm_hour = 2;              // the anchor names the 02:00 session itself
    at.tm_isdst = -1;
    const auto retain = std::chrono::system_clock::from_time_t(std::mktime(&at));

    SessionDiscoveryService svc(ds);
    auto sessions = svc.discoverNewSessions(last, retain);
    ASSERT_EQ(sessions.size(), 1u) << "a retained night must be re-observable";
    EXPECT_EQ(sessions[0].session_prefix, "20200615_020000");
}

TEST(DiscoverNewSessionsTest, RetentionIsInclusiveOfTheAnchorItself) {
    // >= not >. The anchor IS the second-latest stored session, so a strict
    // comparison would retain one night where the ledger needs two.
    FakeDataSource ds;
    ds.date_folders = {"20200614", "20200615"};
    ds.folder_files["20200614"] = {
        entry("20200614_220000_BRP.edf", 10),
        entry("20200614_220000_CSL.edf", 1),
    };
    ds.folder_files["20200615"] = {
        entry("20200615_230000_BRP.edf", 10),
        entry("20200615_230000_CSL.edf", 1),
    };

    std::tm lt = {};
    lt.tm_year = 2020 - 1900; lt.tm_mon = 5; lt.tm_mday = 15; lt.tm_hour = 23;
    lt.tm_isdst = -1;
    const auto last = std::chrono::system_clock::from_time_t(std::mktime(&lt));

    std::tm rt = {};
    rt.tm_year = 2020 - 1900; rt.tm_mon = 5; rt.tm_mday = 14; rt.tm_hour = 22;
    rt.tm_isdst = -1;
    const auto retain = std::chrono::system_clock::from_time_t(std::mktime(&rt));

    SessionDiscoveryService svc(ds);
    auto sessions = svc.discoverNewSessions(last, retain);
    ASSERT_EQ(sessions.size(), 2u)
        << "both of the two newest stored nights must come back, not just one";
}

TEST(DiscoverNewSessionsTest, RetentionAnchorWidensTheFolderLevelCutToo) {
    // The folder filter runs BEFORE the per-session rule, so a retained older
    // folder would be discarded before retention was ever consulted.
    FakeDataSource ds;
    ds.date_folders = {"20200610", "20200620"};
    ds.folder_files["20200610"] = {
        entry("20200610_220000_BRP.edf", 10),
        entry("20200610_220000_CSL.edf", 1),
    };
    ds.folder_files["20200620"] = {
        entry("20200620_220000_BRP.edf", 10),
        entry("20200620_220000_CSL.edf", 1),
    };

    std::tm lt = {};
    lt.tm_year = 2020 - 1900; lt.tm_mon = 5; lt.tm_mday = 20; lt.tm_hour = 22;
    lt.tm_isdst = -1;
    const auto last = std::chrono::system_clock::from_time_t(std::mktime(&lt));

    std::tm rt = {};
    rt.tm_year = 2020 - 1900; rt.tm_mon = 5; rt.tm_mday = 10; rt.tm_hour = 22;
    rt.tm_isdst = -1;
    const auto retain = std::chrono::system_clock::from_time_t(std::mktime(&rt));

    SessionDiscoveryService svc(ds);
    auto sessions = svc.discoverNewSessions(last, retain);
    ASSERT_EQ(sessions.size(), 2u)
        << "the folder-level cut must move back to the retention anchor";
}

// ── SDD-014 / issue #22: a merged session keeps EVERY EVE ───────────────────
//
// A ResMed night is several mask-on blocks and each writes its own EVE. The
// matcher used to take the FIRST in prefix order and break, which is the
// earliest block -- routinely a seconds-long mask-fit check whose EVE is the
// empty 832-byte stub. The night's real annotations were never staged, never
// parsed, and the session read AHI 0.0 while OSCAR read 2.84 off the same card.

class MergedSessionSidecars : public ::testing::Test {
protected:
    std::string tmp_dir;
    void SetUp() override {
        tmp_dir = "/tmp/cpap_test_merged_" + std::to_string(getpid());
        fs::remove_all(tmp_dir);
        fs::create_directories(tmp_dir);
        setenv("SESSION_GAP_MINUTES", "60", 1);
    }
    void TearDown() override { fs::remove_all(tmp_dir); unsetenv("SESSION_GAP_MINUTES"); }
};

// The reporter's card: four blocks that merge into one session.
// Dates are deliberately >24h in the past so estimateCheckpointEnd's mtime
// plausibility gate discards the file mtimes and merging is driven purely by
// BRP size (end = start + minutes(size_kb / 6)).
TEST_F(MergedSessionSidecars, EveryBlocksEveSurvivesTheMerge) {
    touchFileSized(tmp_dir, "20250812_233427_BRP.edf", 24);     // mask-fit check
    touchFile(tmp_dir,      "20250812_233427_EVE.edf");         // the empty stub
    touchFileSized(tmp_dir, "20250812_233829_BRP.edf", 24);
    touchFile(tmp_dir,      "20250812_233829_EVE.edf");
    touchFileSized(tmp_dir, "20250812_235319_BRP.edf", 1200);   // the real night
    touchFile(tmp_dir,      "20250812_235319_EVE.edf");
    touchFileSized(tmp_dir, "20250813_034616_BRP.edf", 300);    // after a break
    touchFile(tmp_dir,      "20250813_034616_EVE.edf");

    auto sessions = SessionDiscoveryService::groupLocalFolder(tmp_dir, "20250812");

    ASSERT_EQ(sessions.size(), 1u) << "the four blocks are one night";
    EXPECT_EQ(sessions[0].brp_files.size(), 4u);
    EXPECT_EQ(sessions[0].eve_files.size(), 4u)
        << "three EVEs were dropped: the night would read AHI 0.0";
}

// The counterpart risk of taking every match: a date folder with two genuinely
// separate sessions must not have the first one swallow the second's sidecars.
// This is why matching is scoped to the session's own span rather than a flat
// 12-hour window.
TEST_F(MergedSessionSidecars, SeparateSessionsKeepTheirOwnSidecars) {
    setenv("SESSION_GAP_MINUTES", "30", 1);
    touchFileSized(tmp_dir, "20250301_200000_BRP.edf", 24);
    touchFile(tmp_dir,      "20250301_200000_EVE.edf");
    touchFile(tmp_dir,      "20250301_200000_CSL.edf");
    // Four hours later: a separate session by any threshold.
    touchFileSized(tmp_dir, "20250302_000000_BRP.edf", 24);
    touchFile(tmp_dir,      "20250302_000000_EVE.edf");
    touchFile(tmp_dir,      "20250302_000000_CSL.edf");

    auto sessions = SessionDiscoveryService::groupLocalFolder(tmp_dir, "20250301");

    ASSERT_EQ(sessions.size(), 2u);
    ASSERT_EQ(sessions[0].eve_files.size(), 1u)
        << "the first session took the second session's EVE as well";
    EXPECT_EQ(sessions[0].eve_files[0], "20250301_200000_EVE.edf");
    EXPECT_EQ(sessions[0].csl_files[0], "20250301_200000_CSL.edf");
    ASSERT_EQ(sessions[1].eve_files.size(), 1u) << "the later session got nothing";
    EXPECT_EQ(sessions[1].eve_files[0], "20250302_000000_EVE.edf");
    EXPECT_EQ(sessions[1].csl_files[0], "20250302_000000_CSL.edf");
}

// A leftover sidecar with no session of its own still has to land somewhere:
// the last group sweeps it, which is the pre-existing catch-all.
TEST_F(MergedSessionSidecars, AnOrphanSidecarIsSweptByTheLastSession) {
    touchFileSized(tmp_dir, "20250301_220000_BRP.edf", 24);
    touchFile(tmp_dir,      "20250301_220000_EVE.edf");
    // Written a day later than anything on the card: inside no session's span.
    touchFile(tmp_dir,      "20250302_235959_EVE.edf");

    auto sessions = SessionDiscoveryService::groupLocalFolder(tmp_dir, "20250301");

    ASSERT_EQ(sessions.size(), 1u);
    EXPECT_EQ(sessions[0].eve_files.size(), 2u) << "an EVE was orphaned entirely";
}

// ── SDD-033: the 11 series' TCV rides with its checkpoint ───────────────────
//
// An AirCurve 11 writes a *_TCV.edf beside each BRP/PLD/SA2. It is not
// parsed, but it grows all night and the archive is what OSCAR and SleepHQ
// read, so it belongs to the session rather than to the residue sweep that
// re-downloaded every one of them whole on every burst.

class TcvFiles : public ::testing::Test {
protected:
    std::string tmp_dir;
    void SetUp() override {
        tmp_dir = "/tmp/cpap_test_tcv_" + std::to_string(getpid());
        fs::remove_all(tmp_dir);
        fs::create_directories(tmp_dir);
        setenv("SESSION_GAP_MINUTES", "60", 1);
    }
    void TearDown() override { fs::remove_all(tmp_dir); unsetenv("SESSION_GAP_MINUTES"); }
};

TEST_F(TcvFiles, EachSessionTakesTheTcvOfItsOwnCheckpoints) {
    // Two blocks, five hours apart: two sessions, each with its own TCV.
    touchFileSized(tmp_dir, "20260911_225616_BRP.edf", 1712);
    touchFileSized(tmp_dir, "20260911_225616_PLD.edf", 192);
    touchFileSized(tmp_dir, "20260911_225616_TCV.edf", 856);
    touchFile(tmp_dir,      "20260911_225604_EVE.edf");
    touchFileSized(tmp_dir, "20260912_045142_BRP.edf", 604);
    touchFileSized(tmp_dir, "20260912_045142_TCV.edf", 302);

    auto sessions = SessionDiscoveryService::groupLocalFolder(tmp_dir, "20260911");

    ASSERT_EQ(sessions.size(), 2u);
    ASSERT_EQ(sessions[0].tcv_files.size(), 1u);
    EXPECT_EQ(sessions[0].tcv_files[0], "20260911_225616_TCV.edf");
    ASSERT_EQ(sessions[1].tcv_files.size(), 1u);
    EXPECT_EQ(sessions[1].tcv_files[0], "20260912_045142_TCV.edf");
    // Counted like any other file of the session, so the ledger and the
    // archive check know it has to be there.
    EXPECT_EQ(sessions[0].file_sizes_kb.count("20260911_225616_TCV.edf"), 1u);
}

TEST_F(TcvFiles, ATcvNeverMakesASessionOfItsOwn) {
    // A TCV with no checkpoint beside it is not therapy data; grouping is the
    // BRP/PLD/SAD story and must not change because a machine writes one.
    touchFileSized(tmp_dir, "20260911_225616_TCV.edf", 856);

    auto sessions = SessionDiscoveryService::groupLocalFolder(tmp_dir, "20260911");
    EXPECT_TRUE(sessions.empty());
}

TEST_F(TcvFiles, AnAirSenseNightHasNoTcvAndIsUnchanged) {
    touchFileSized(tmp_dir, "20260911_225616_BRP.edf", 1712);
    touchFileSized(tmp_dir, "20260911_225616_PLD.edf", 192);

    auto sessions = SessionDiscoveryService::groupLocalFolder(tmp_dir, "20260911");
    ASSERT_EQ(sessions.size(), 1u);
    EXPECT_TRUE(sessions[0].tcv_files.empty());
}
