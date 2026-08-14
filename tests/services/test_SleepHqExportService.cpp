// SDD-003: debounced SleepHQ export — quiet-window gating, growth resets,
// retry backoff, and re-dirty semantics, driven with an injected clock and
// export hook against a temp archive.
#include <gtest/gtest.h>
#include "services/SleepHqExportService.h"
#include "utils/AppConfig.h"

#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

using namespace hms_cpap;
namespace fs = std::filesystem;
using std::chrono::minutes;

namespace {

class SleepHqExportServiceTest : public ::testing::Test {
protected:
    void SetUp() override {
        archive_ = fs::temp_directory_path() / "sleephq_test_archive";
        fs::remove_all(archive_);
        fs::create_directories(archive_ / "DATALOG" / kFolder);
        setenv("CPAP_ARCHIVE_DIR", archive_.string().c_str(), 1);

        cfg_.sleephq.enabled = true;
        cfg_.sleephq.client_id = "test-client";
        cfg_.sleephq.quiet_minutes = 10;

        auto& svc = SleepHqExportService::getInstance();
        svc.resetForTest();
        svc.initialize(&cfg_);
        svc.setExportHookForTest([this](const std::string& folder) {
            exports_.push_back(folder);
            return hook_result_;
        });

        base_ = std::chrono::steady_clock::now();
    }

    void TearDown() override {
        SleepHqExportService::getInstance().resetForTest();
        unsetenv("CPAP_ARCHIVE_DIR");
        fs::remove_all(archive_);
    }

    void writeFile(const std::string& name, size_t bytes) {
        std::ofstream f(archive_ / "DATALOG" / kFolder / name);
        f << std::string(bytes, 'x');
    }

    SleepHqExportService& svc() { return SleepHqExportService::getInstance(); }
    std::chrono::steady_clock::time_point at(int mins) { return base_ + minutes(mins); }

    static constexpr const char* kFolder = "20260711";
    fs::path archive_;
    AppConfig cfg_;
    std::vector<std::string> exports_;
    bool hook_result_ = true;
    std::chrono::steady_clock::time_point base_;
};

TEST_F(SleepHqExportServiceTest, HoldsUntilQuietWindowElapses) {
    writeFile("A_BRP.edf", 100);
    svc().markDirty(kFolder);

    svc().sweep(at(0));   // first scan records the snapshot
    svc().sweep(at(5));   // quiet only 5m of 10m
    EXPECT_TRUE(exports_.empty());

    svc().sweep(at(11));
    ASSERT_EQ(exports_.size(), 1u);
    EXPECT_EQ(exports_[0], kFolder);
    EXPECT_FALSE(svc().isDirtyForTest(kFolder));

    svc().sweep(at(25));  // nothing left to export
    EXPECT_EQ(exports_.size(), 1u);
}

TEST_F(SleepHqExportServiceTest, FileGrowthResetsQuietTimer) {
    writeFile("A_BRP.edf", 100);
    svc().markDirty(kFolder);
    svc().sweep(at(0));

    writeFile("A_BRP.edf", 200);  // session still growing
    svc().sweep(at(11));          // change detected: timer restarts, no export
    EXPECT_TRUE(exports_.empty());

    svc().sweep(at(15));          // quiet only 4m since the change
    EXPECT_TRUE(exports_.empty());

    svc().sweep(at(22));
    EXPECT_EQ(exports_.size(), 1u);
}

TEST_F(SleepHqExportServiceTest, NewLateFileResetsQuietTimer) {
    writeFile("A_BRP.edf", 100);
    svc().markDirty(kFolder);
    svc().sweep(at(0));

    writeFile("A_EVE.edf", 50);   // late EVE lands after completion
    svc().sweep(at(11));
    EXPECT_TRUE(exports_.empty());

    svc().sweep(at(22));
    ASSERT_EQ(exports_.size(), 1u);
}

TEST_F(SleepHqExportServiceTest, FailureRetriesWithBackoff) {
    writeFile("A_BRP.edf", 100);
    hook_result_ = false;
    svc().markDirty(kFolder);
    svc().sweep(at(0));

    svc().sweep(at(11));               // attempt 1 fails
    EXPECT_EQ(exports_.size(), 1u);
    EXPECT_TRUE(svc().isDirtyForTest(kFolder));

    svc().sweep(at(12));               // inside 5m backoff
    EXPECT_EQ(exports_.size(), 1u);

    svc().sweep(at(17));               // attempt 2 fails (backoff now 10m)
    EXPECT_EQ(exports_.size(), 2u);

    svc().sweep(at(20));
    EXPECT_EQ(exports_.size(), 2u);

    hook_result_ = true;
    svc().sweep(at(28));               // attempt 3 succeeds
    EXPECT_EQ(exports_.size(), 3u);
    EXPECT_FALSE(svc().isDirtyForTest(kFolder));
}

TEST_F(SleepHqExportServiceTest, DisabledConfigNeverExports) {
    cfg_.sleephq.enabled = false;
    writeFile("A_BRP.edf", 100);
    svc().markDirty(kFolder);
    svc().sweep(at(0));
    svc().sweep(at(30));
    EXPECT_TRUE(exports_.empty());
}

TEST_F(SleepHqExportServiceTest, RedirtyAfterSuccessReexports) {
    writeFile("A_BRP.edf", 100);
    svc().markDirty(kFolder);
    svc().sweep(at(0));
    svc().sweep(at(11));
    ASSERT_EQ(exports_.size(), 1u);

    // Mask back on after a long break: another completion re-marks the night.
    writeFile("B_BRP.edf", 300);
    svc().markDirty(kFolder);
    svc().sweep(at(120));  // records new snapshot
    svc().sweep(at(131));
    EXPECT_EQ(exports_.size(), 2u);
}

TEST_F(SleepHqExportServiceTest, ChangeDuringUploadKeepsFolderDirty) {
    writeFile("A_BRP.edf", 100);
    svc().markDirty(kFolder);
    svc().sweep(at(0));

    // The hook simulates STR/EVE landing while the upload is in flight.
    svc().setExportHookForTest([this](const std::string& folder) {
        exports_.push_back(folder);
        writeFile("A_EVE.edf", 42);
        return true;
    });

    svc().sweep(at(11));
    EXPECT_EQ(exports_.size(), 1u);
    EXPECT_TRUE(svc().isDirtyForTest(kFolder));  // must re-export the full set

    svc().setExportHookForTest([this](const std::string& folder) {
        exports_.push_back(folder);
        return true;
    });
    svc().sweep(at(22));  // snapshot already refreshed by finishExport
    EXPECT_EQ(exports_.size(), 2u);
    EXPECT_FALSE(svc().isDirtyForTest(kFolder));
}

TEST_F(SleepHqExportServiceTest, MissingArchiveDirBlocksWithoutCrashing) {
    unsetenv("CPAP_ARCHIVE_DIR");
    svc().markDirty(kFolder);
    svc().sweep(at(0));
    svc().sweep(at(30));
    EXPECT_TRUE(exports_.empty());
    EXPECT_TRUE(svc().isDirtyForTest(kFolder));
}

} // namespace

// ── Card-root file inclusion (ported lesson from hms-cpapdash-api 4d1fb05) ───
//
// The cloud incident: "Exports from bridge devices never included STR.edf, and a
// device without Identification.* on disk produced machine-less imports that
// processed into nothing visible." hms-cpap had the same end result via a
// different cause — BackfillService passed config_.local_dir (which IS the
// DATALOG dir) as root_dir, so the probe looked for DATALOG/STR.edf, found
// nothing, and silently shipped a night with no summary and no machine.
//
// SleepHQ accepts such an import and reports success, so nothing surfaces the
// loss. These tests pin the file SET, which the folder-name hooks could not see.

namespace {

class ExportFileSet : public ::testing::Test {
protected:
    std::filesystem::path card_root_;
    std::string folder_{"20260706"};

    void SetUp() override {
        namespace fs = std::filesystem;
        card_root_ = fs::temp_directory_path() /
                     ("hms_shq_" + std::to_string(::getpid()) +
                      std::to_string(reinterpret_cast<uintptr_t>(this)));
        fs::create_directories(card_root_ / "DATALOG" / folder_);
        // A real card: root summary/identity files, plus the night's EDFs.
        for (const char* n : {"STR.edf", "Identification.tgt", "Identification.crc"})
            std::ofstream(card_root_ / n) << "x";
        for (const char* n : {"20260706_195339_BRP.edf", "20260706_195339_PLD.edf"})
            std::ofstream(card_root_ / "DATALOG" / folder_ / n) << "x";
    }
    void TearDown() override { std::error_code ec; std::filesystem::remove_all(card_root_, ec); }

    std::string datalog() const { return (card_root_ / "DATALOG" / folder_).string(); }
};

bool hasFile(const std::vector<SleepHqExportService::ExportFile>& v,
             const std::string& name, const std::string& import_path) {
    for (const auto& f : v)
        if (f.name == name && f.import_path == import_path) return true;
    return false;
}

} // namespace

TEST_F(ExportFileSet, CardRootYieldsStrAndIdentificationAtImportRoot) {
    auto files = SleepHqExportService::collectExportFiles(
        folder_, datalog(), card_root_.string());

    // Root files must land at the import ROOT ("") so SleepHQ sees a real card.
    EXPECT_TRUE(hasFile(files, "STR.edf", ""))
        << "STR.edf missing: the import would have no therapy summary";
    EXPECT_TRUE(hasFile(files, "Identification.tgt", ""))
        << "Identification.* missing: SleepHQ would import with no machine";
    EXPECT_TRUE(hasFile(files, "Identification.crc", ""));
    // ...and the night's own files under their card-accurate path.
    EXPECT_TRUE(hasFile(files, "20260706_195339_BRP.edf", "DATALOG/20260706"));
    EXPECT_TRUE(hasFile(files, "20260706_195339_PLD.edf", "DATALOG/20260706"));
    EXPECT_EQ(files.size(), 5u);
}

TEST_F(ExportFileSet, PassingTheDatalogDirAsRootDropsEveryRootFile) {
    // This is the bug verbatim: root_dir = the DATALOG directory. It must be
    // visibly lossy, so that if anyone reintroduces it a test fails instead of
    // SleepHQ quietly accepting a machine-less import.
    auto files = SleepHqExportService::collectExportFiles(
        folder_, datalog(), datalog());

    EXPECT_FALSE(hasFile(files, "STR.edf", ""));
    EXPECT_FALSE(hasFile(files, "Identification.tgt", ""));
    EXPECT_EQ(files.size(), 2u) << "only the night's EDFs survive - no summary, no machine";
}

TEST_F(ExportFileSet, CardRootIsTheParentOfLocalDir) {
    // BackfillService derives root_dir from config_.local_dir, which points AT
    // DATALOG. Pin the derivation the fix relies on.
    const std::string local_dir = datalog();                 // .../DATALOG/20260706
    const auto datalog_parent = std::filesystem::path(local_dir).parent_path(); // .../DATALOG
    const auto derived_root   = datalog_parent.parent_path();                   // card root
    EXPECT_EQ(derived_root.string(), card_root_.string());

    auto files = SleepHqExportService::collectExportFiles(
        folder_, datalog(), derived_root.string());
    EXPECT_TRUE(hasFile(files, "STR.edf", ""));
}

TEST_F(ExportFileSet, MissingRootFilesAreSkippedNotFatal) {
    namespace fs = std::filesystem;
    std::error_code ec;
    fs::remove(card_root_ / "STR.edf", ec);
    fs::remove(card_root_ / "Identification.tgt", ec);
    fs::remove(card_root_ / "Identification.crc", ec);

    auto files = SleepHqExportService::collectExportFiles(
        folder_, datalog(), card_root_.string());
    EXPECT_EQ(files.size(), 2u) << "a card with no root files still exports its night";
}

// ── SDD-014: the recorded file set drives the export ────────────────────────
//
// cpap_session_files knows which files a night is actually made of, which a
// directory listing cannot: a merged night is several mask-on blocks, each with
// its own EVE. These pin that the table decides, and that nothing the table does
// not track gets dropped on the way.

TEST_F(ExportFileSet, EveryRecordedEveIsExported) {
    namespace fs = std::filesystem;
    // Three blocks of one night, the shape of issue #22.
    for (const char* n : {"20260706_195339_EVE.edf", "20260706_211200_EVE.edf",
                          "20260706_034500_EVE.edf"})
        std::ofstream(card_root_ / "DATALOG" / folder_ / n) << "x";

    std::vector<SessionFileRef> recorded = {
        {"brp", "DATALOG/20260706/20260706_195339_BRP.edf"},
        {"eve", "DATALOG/20260706/20260706_195339_EVE.edf"},
        {"eve", "DATALOG/20260706/20260706_211200_EVE.edf"},
        {"eve", "DATALOG/20260706/20260706_034500_EVE.edf"},
    };

    auto files = SleepHqExportService::collectExportFiles(
        folder_, datalog(), card_root_.string(), recorded);

    EXPECT_TRUE(hasFile(files, "20260706_195339_EVE.edf", "DATALOG/20260706"));
    EXPECT_TRUE(hasFile(files, "20260706_211200_EVE.edf", "DATALOG/20260706"));
    EXPECT_TRUE(hasFile(files, "20260706_034500_EVE.edf", "DATALOG/20260706"));
    EXPECT_TRUE(hasFile(files, "STR.edf", ""));
}

// The table records the five analytical kinds. A night's .crc sidecar has no
// other source, and SleepHQ is supposed to receive the card as it is.
TEST_F(ExportFileSet, SidecarsInTheDateFolderStillShipWhenTheTableDrives) {
    std::ofstream(card_root_ / "DATALOG" / folder_ / "20260706_195339_BRP.crc") << "x";
    std::ofstream(card_root_ / "DATALOG" / folder_ / "Journal.dat") << "x";

    std::vector<SessionFileRef> recorded = {
        {"brp", "DATALOG/20260706/20260706_195339_BRP.edf"},
    };
    auto files = SleepHqExportService::collectExportFiles(
        folder_, datalog(), card_root_.string(), recorded);

    EXPECT_TRUE(hasFile(files, "20260706_195339_BRP.crc", "DATALOG/20260706"))
        << "the night's checksum was dropped";
    EXPECT_TRUE(hasFile(files, "Journal.dat", "DATALOG/20260706"));
}

// Root files stop being four hardcoded names: the whole card root ships.
TEST_F(ExportFileSet, TheWholeCardRootIsExportedNotFourHardcodedNames) {
    namespace fs = std::filesystem;
    fs::create_directories(card_root_ / "SETTINGS");
    std::ofstream(card_root_ / "SETTINGS" / "SET1.tgt") << "x";
    std::ofstream(card_root_ / "Journal.dat") << "x";
    std::ofstream(card_root_ / "AGL.crc") << "x";

    auto files = SleepHqExportService::collectExportFiles(
        folder_, datalog(), card_root_.string());

    EXPECT_TRUE(hasFile(files, "SET1.tgt", "SETTINGS"));
    EXPECT_TRUE(hasFile(files, "Journal.dat", ""));
    EXPECT_TRUE(hasFile(files, "AGL.crc", ""));
}

// This is the one code path that sends card contents to a third party.
TEST_F(ExportFileSet, EzshareCfgIsNeverExported) {
    std::ofstream(card_root_ / "ezshare.cfg") << "ssid=home\npassword=hunter2\n";
    std::ofstream(card_root_ / "holiday.jpg") << "x";

    auto files = SleepHqExportService::collectExportFiles(
        folder_, datalog(), card_root_.string());

    EXPECT_FALSE(hasFile(files, "ezshare.cfg", ""))
        << "ezshare.cfg can hold WiFi credentials and must never leave the machine";
    EXPECT_FALSE(hasFile(files, "holiday.jpg", ""));
}

// A Lowenstein card is one container. The 20 MB cap must not drop it.
TEST_F(ExportFileSet, ALowensteinContainerIsExportedEvenWhenLarge) {
    {
        std::ofstream f(card_root_ / "therapy.pdat", std::ios::binary);
        std::string chunk(1024 * 1024, 'x');
        for (int i = 0; i < 22; ++i) f.write(chunk.data(), chunk.size());  // 22 MB
    }
    auto files = SleepHqExportService::collectExportFiles(
        folder_, datalog(), card_root_.string());
    EXPECT_TRUE(hasFile(files, "therapy.pdat", ""))
        << "the only file on a Prisma card was dropped by the size cap";
}

// ── The export must not be decided by what parsed ───────────────────────────
//
// cpap_session_files is written INSIDE `if (saveSession(...))`, so a session
// that fails to parse records no rows at all. If the table were the only source
// of session files, that night's EDFs would never reach SleepHQ or an OSCAR
// download -- the card would be on disk and the export would quietly omit half
// of it.
//
// These pin the two nets that prevent it: an unrecorded night falls back to the
// directory walk, and a PARTIALLY recorded folder still ships the files the
// table does not know about.

TEST_F(ExportFileSet, FilesFromAnUnPARSEDSessionStillShip) {
    namespace fs = std::filesystem;
    // Session A parsed and was recorded. Session B never parsed, so it has no
    // rows -- a corrupt EDF, a crash mid-backfill, anything.
    for (const char* n : {"20260706_195339_EVE.edf",
                          "20260706_233000_BRP.edf",   // session B, unrecorded
                          "20260706_233000_EVE.edf",
                          "20260706_233000_PLD.edf"})
        std::ofstream(card_root_ / "DATALOG" / folder_ / n) << "x";

    std::vector<SessionFileRef> recorded = {
        {"brp", "DATALOG/20260706/20260706_195339_BRP.edf"},
        {"eve", "DATALOG/20260706/20260706_195339_EVE.edf"},
    };

    auto files = SleepHqExportService::collectExportFiles(
        folder_, datalog(), card_root_.string(), recorded);

    // The recorded session, obviously.
    EXPECT_TRUE(hasFile(files, "20260706_195339_BRP.edf", "DATALOG/20260706"));
    EXPECT_TRUE(hasFile(files, "20260706_195339_EVE.edf", "DATALOG/20260706"));
    // ...and everything the table never heard of.
    EXPECT_TRUE(hasFile(files, "20260706_233000_BRP.edf", "DATALOG/20260706"))
        << "an unparsed session's flow never reached SleepHQ";
    EXPECT_TRUE(hasFile(files, "20260706_233000_EVE.edf", "DATALOG/20260706"))
        << "an unparsed session's events never reached SleepHQ";
    EXPECT_TRUE(hasFile(files, "20260706_233000_PLD.edf", "DATALOG/20260706"));
}

// A night where NOTHING parsed has an empty table and must still export in full.
TEST_F(ExportFileSet, ANightThatParsedNothingAtAllStillExportsEveryFile) {
    std::ofstream(card_root_ / "DATALOG" / folder_ / "20260706_195339_EVE.edf") << "x";

    auto files = SleepHqExportService::collectExportFiles(
        folder_, datalog(), card_root_.string(), /* recorded */ {});

    EXPECT_TRUE(hasFile(files, "20260706_195339_BRP.edf", "DATALOG/20260706"));
    EXPECT_TRUE(hasFile(files, "20260706_195339_PLD.edf", "DATALOG/20260706"));
    EXPECT_TRUE(hasFile(files, "20260706_195339_EVE.edf", "DATALOG/20260706"));
    EXPECT_TRUE(hasFile(files, "STR.edf", ""));
}

// Whatever else the card carries in the night's folder goes too, even when the
// name means nothing to us. The card is the record, not our understanding of it.
TEST_F(ExportFileSet, UnrecognisedFilesInTheNightFolderAreNotDropped) {
    for (const char* n : {"20260706_195339_BRP.crc", "SOMETHING.UNKNOWN", "Journal.dat"})
        std::ofstream(card_root_ / "DATALOG" / folder_ / n) << "x";

    std::vector<SessionFileRef> recorded = {
        {"brp", "DATALOG/20260706/20260706_195339_BRP.edf"}};

    auto files = SleepHqExportService::collectExportFiles(
        folder_, datalog(), card_root_.string(), recorded);

    EXPECT_TRUE(hasFile(files, "20260706_195339_BRP.crc", "DATALOG/20260706"));
    EXPECT_TRUE(hasFile(files, "SOMETHING.UNKNOWN", "DATALOG/20260706"));
    EXPECT_TRUE(hasFile(files, "Journal.dat", "DATALOG/20260706"));
}
