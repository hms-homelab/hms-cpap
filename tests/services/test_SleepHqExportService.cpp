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
