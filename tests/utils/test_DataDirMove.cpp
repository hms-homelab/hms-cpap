// SDD-067: the move rules of `--move-data` (section 5, test 11) and the
// fallback report (test 9).

#include <gtest/gtest.h>

#include "PosixCompat.h"
#include "services/SetupService.h"
#include "utils/DataDirMove.h"

#include <cerrno>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <random>
#include <set>
#include <sstream>
#include <string>
#include <vector>

namespace fs = std::filesystem;
using namespace hms_cpap::datamove;

namespace {

std::string slurp(const fs::path& p) {
    std::ifstream f(p, std::ios::binary);
    std::stringstream ss;
    ss << f.rdbuf();
    return ss.str();
}

void put(const fs::path& p, const std::string& body) {
    fs::create_directories(p.parent_path());
    std::ofstream f(p, std::ios::binary | std::ios::trunc);
    f << body;
}

bool anyTempLeft(const fs::path& root) {
    if (!fs::exists(root)) return false;
    for (const auto& e : fs::recursive_directory_iterator(root)) {
        const std::string n = e.path().filename().string();
        if (n.size() >= std::string(kTempSuffix).size() &&
            n.compare(n.size() - std::string(kTempSuffix).size(), std::string::npos, kTempSuffix) == 0)
            return true;
    }
    return false;
}

class DataDirMove : public ::testing::Test {
protected:
    void SetUp() override {
        std::mt19937_64 rng(std::random_device{}());
        root_ = fs::temp_directory_path() / ("sdd067-" + std::to_string(rng()));
        home_ = root_ / "data";
        share_ = root_ / "share";
        fs::create_directories(home_);
        fs::create_directories(share_);
    }
    void TearDown() override {
        std::error_code ec;
        fs::remove_all(root_, ec);
    }

    // An existing add-on install in home: what the Supervisor and the service left.
    void seedInstall() {
        put(home_ / "options.json", R"({"data_dir":"/share/x"})");
        put(home_ / "config.json", R"({"setup_complete":true,"sleep_bridge":{"port":8894}})");
        put(home_ / "cpap.db", std::string(300000, 'D'));
        put(home_ / "sleepbridge" / "TAP-S3-1" / "bank" / "000001.bin", std::string(200000, 'B'));
        put(home_ / "sleepbridge" / "TAP-S3-1" / "card" / "STR.edf", "STR");
        put(home_ / "cpap_archive" / "DATALOG" / "20261110" / "a_BRP.edf", std::string(5000, 'E'));
        put(home_ / "ota" / "sleepbridge" / "job.json", "{}");
    }

    Options opts(const fs::path& from, const fs::path& to) {
        Options o;
        o.from = from;
        o.to = to;
        o.home = home_;
        return o;
    }

    fs::path root_, home_, share_;
};

}  // namespace

// The first move out of /data: everything lands at the destination, /data
// keeps options.json, a copy of config.json and the marker (D4, D8, D13).
TEST_F(DataDirMove, FirstMoveLeavesOptionsConfigCopyAndMarker) {
    seedInstall();
    const std::string config = slurp(home_ / "config.json");
    const fs::path to = share_ / "x";

    const auto r = moveData(opts(home_, to));
    ASSERT_TRUE(r.ok) << r.error;

    EXPECT_EQ(slurp(to / "config.json"), config);
    EXPECT_EQ(slurp(to / "cpap.db"), std::string(300000, 'D'));
    EXPECT_EQ(slurp(to / "sleepbridge" / "TAP-S3-1" / "bank" / "000001.bin"), std::string(200000, 'B'));
    EXPECT_TRUE(fs::exists(to / "cpap_archive" / "DATALOG" / "20261110" / "a_BRP.edf"));
    EXPECT_TRUE(fs::exists(to / "ota" / "sleepbridge" / "job.json"));
    EXPECT_FALSE(fs::exists(to / "options.json"));
    EXPECT_FALSE(fs::exists(to / kMarkerName));

    std::set<std::string> left;
    for (const auto& e : fs::directory_iterator(home_)) left.insert(e.path().filename().string());
    EXPECT_EQ(left, (std::set<std::string>{"options.json", "config.json", kMarkerName}));
    EXPECT_EQ(slurp(home_ / "config.json"), config);
    EXPECT_EQ(readMarker(home_), fs::weakly_canonical(to).string());
}

// D9: a file that exists at the destination is never overwritten, and its
// source copy stays where it was.
TEST_F(DataDirMove, NothingAtTheDestinationIsOverwritten) {
    seedInstall();
    const fs::path to = share_ / "x";
    put(to / "sleepbridge" / "TAP-S3-1" / "card" / "STR.edf", "THEIRS");

    const auto r = moveData(opts(home_, to));
    ASSERT_TRUE(r.ok) << r.error;
    EXPECT_EQ(r.left, 1u);
    EXPECT_EQ(slurp(to / "sleepbridge" / "TAP-S3-1" / "card" / "STR.edf"), "THEIRS");
    EXPECT_EQ(slurp(home_ / "sleepbridge" / "TAP-S3-1" / "card" / "STR.edf"), "STR");
}

// /data to /share is two filesystems: copy, sync, rename, then remove. The
// copy keeps the bytes and the modification time and leaves no temporary file.
TEST_F(DataDirMove, AcrossFilesystemsCopiesThenRemoves) {
    seedInstall();
    const auto mtime = fs::last_write_time(home_ / "cpap.db");
    const fs::path to = share_ / "x";
    auto o = opts(home_, to);
    o.force_copy = true;

    const auto r = moveData(o);
    ASSERT_TRUE(r.ok) << r.error;
    EXPECT_EQ(slurp(to / "cpap.db"), std::string(300000, 'D'));
    EXPECT_EQ(fs::last_write_time(to / "cpap.db"), mtime);
    EXPECT_FALSE(fs::exists(home_ / "cpap.db"));
    EXPECT_FALSE(fs::exists(home_ / "sleepbridge"));
    EXPECT_TRUE(fs::exists(home_ / "config.json"));
    EXPECT_FALSE(anyTempLeft(to));
}

// A move cut off half way and run again completes, including the file a
// copy had renamed into place but not yet removed from the source.
TEST_F(DataDirMove, InterruptedHalfWayAndRunAgain) {
    seedInstall();
    const fs::path to = share_ / "x";
    auto o = opts(home_, to);
    o.force_copy = true;
    int files_seen = 0;
    o.on_progress = [&](const Progress& p) {
        files_seen = static_cast<int>(p.files_done);
        return p.files_done < 2;
    };
    const auto first = moveData(o);
    EXPECT_FALSE(first.ok);
    EXPECT_EQ(files_seen, 2);
    // The marker is already there, which is what makes the next start a
    // catch-up instead of "the destination already has a config.json".
    EXPECT_EQ(readMarker(home_), fs::weakly_canonical(to).string());

    // The gap between the rename and the removal, and a stray temporary file.
    const fs::path job = home_ / "ota" / "sleepbridge" / "job.json";
    if (fs::exists(job) && !fs::exists(to / "ota" / "sleepbridge" / "job.json")) {
        fs::create_directories(to / "ota" / "sleepbridge");
        fs::copy_file(job, to / "ota" / "sleepbridge" / "job.json");
        fs::last_write_time(to / "ota" / "sleepbridge" / "job.json", fs::last_write_time(job));
    }
    put(to / ("cpap.db" + std::string(kTempSuffix)), "half");

    o.on_progress = nullptr;
    const auto again = moveData(o);
    ASSERT_TRUE(again.ok) << again.error;
    // Only config.json: the first run copied it across, and the copy in
    // /data stays (D8).
    EXPECT_EQ(again.left, 1u);
    EXPECT_TRUE(fs::exists(home_ / "config.json"));
    EXPECT_EQ(slurp(to / "cpap.db"), std::string(300000, 'D'));
    EXPECT_TRUE(fs::exists(to / "ota" / "sleepbridge" / "job.json"));
    EXPECT_FALSE(fs::exists(job));
    EXPECT_FALSE(fs::exists(home_ / "cpap.db"));
    EXPECT_FALSE(anyTempLeft(to));
}

// A destination that fills up: the move fails, says why, leaves no partial
// file behind, and the files not moved (the database last of all) stay put.
TEST_F(DataDirMove, ADestinationThatFillsUpFails) {
    seedInstall();
    const fs::path to = share_ / "x";
    auto o = opts(home_, to);
    o.force_copy = true;
    o.fail_after_bytes = 100000;

    const auto r = moveData(o);
    EXPECT_FALSE(r.ok);
    EXPECT_NE(r.error.find(std::strerror(ENOSPC)), std::string::npos) << r.error;
    EXPECT_FALSE(anyTempLeft(to));
    EXPECT_EQ(slurp(home_ / "cpap.db"), std::string(300000, 'D'));
    EXPECT_TRUE(fs::exists(home_ / "config.json"));
    EXPECT_FALSE(fs::exists(to / "cpap.db"));
}

// D15: from one chosen directory to another. The old one ends empty of the
// files, the marker names the new one, the copy in /data is refreshed.
TEST_F(DataDirMove, FromOneChosenDirectoryToAnother) {
    seedInstall();
    const fs::path a = share_ / "a";
    ASSERT_TRUE(moveData(opts(home_, a)).ok);
    put(a / "config.json", R"({"setup_complete":true,"changed":"in a"})");

    const fs::path b = root_ / "media" / "b";
    const auto r = moveData(opts(a, b));
    ASSERT_TRUE(r.ok) << r.error;

    EXPECT_TRUE(fs::exists(b / "cpap.db"));
    EXPECT_TRUE(fs::exists(b / "sleepbridge" / "TAP-S3-1" / "bank" / "000001.bin"));
    EXPECT_TRUE(fs::is_directory(a));
    EXPECT_TRUE(fs::is_empty(a));
    EXPECT_EQ(readMarker(home_), fs::weakly_canonical(b).string());
    EXPECT_EQ(slurp(home_ / "config.json"), R"({"setup_complete":true,"changed":"in a"})");
    EXPECT_TRUE(fs::exists(home_ / "options.json"));
}

// D9 after a fallback run: what the run left in /data and is not yet in the
// chosen directory moves across; config.json and cpap.db exist on both sides
// and stay where they are.
TEST_F(DataDirMove, CatchUpAfterAFallbackRun) {
    seedInstall();
    const fs::path to = share_ / "x";
    ASSERT_TRUE(moveData(opts(home_, to)).ok);
    const std::string chosen_db = slurp(to / "cpap.db");

    put(home_ / "cpap.db", "FALLBACK DB");
    put(home_ / "config.json", R"({"fallback":true})");
    put(home_ / "sleepbridge" / "TAP-S3-1" / "bank" / "000002.bin", "NEW BATCH");

    const auto r = moveData(opts(home_, to));
    ASSERT_TRUE(r.ok) << r.error;
    EXPECT_EQ(slurp(to / "sleepbridge" / "TAP-S3-1" / "bank" / "000002.bin"), "NEW BATCH");
    EXPECT_FALSE(fs::exists(home_ / "sleepbridge"));
    EXPECT_EQ(slurp(to / "cpap.db"), chosen_db);
    EXPECT_EQ(slurp(home_ / "cpap.db"), "FALLBACK DB");
    EXPECT_EQ(slurp(home_ / "config.json"), R"({"fallback":true})");
    EXPECT_NE(slurp(to / "config.json"), R"({"fallback":true})");
}

TEST_F(DataDirMove, NothingToMoveIsCompleteAndWritesNoMarker) {
    put(home_ / "options.json", "{}");
    const auto r = moveData(opts(home_, share_ / "x"));
    EXPECT_TRUE(r.ok) << r.error;
    EXPECT_EQ(readMarker(home_), "");
}

TEST_F(DataDirMove, ADirectoryInsideTheOtherIsRefused) {
    seedInstall();
    const auto r = moveData(opts(home_, home_ / "inner"));
    EXPECT_FALSE(r.ok);
    EXPECT_TRUE(fs::exists(home_ / "cpap.db"));
}

TEST_F(DataDirMove, ProgressIsReportedAndEndsFinished) {
    seedInstall();
    std::vector<Progress> seen;
    auto o = opts(home_, share_ / "x");
    o.on_progress = [&](const Progress& p) { seen.push_back(p); return true; };
    ASSERT_TRUE(moveData(o).ok);
    ASSERT_FALSE(seen.empty());
    EXPECT_TRUE(seen.back().finished);
    EXPECT_TRUE(seen.back().ok);
    EXPECT_EQ(seen.back().files_done, seen.back().files_total);
    EXPECT_EQ(seen.back().bytes_done, seen.back().bytes_total);
    EXPECT_EQ(percentOf(seen.back()), 100);
    EXPECT_NE(progressLine(seen.back()).find("done"), std::string::npos);
    EXPECT_NE(progressJson(seen.back(), 7).find("\"seq\":7"), std::string::npos);
}

// D14 and the report in GET /api/capabilities (test 9).
TEST(DataDirFallback, ReportedOnlyWhenTheAddOnSaysSo) {
    unsetenv("HMS_CPAP_DATA_DIR_FALLBACK");
    EXPECT_FALSE(hms_cpap::SetupService::capabilities().data_dir_fallback.has_value());

    setenv("HMS_CPAP_DATA_DIR_FALLBACK", "", 1);
    EXPECT_FALSE(hms_cpap::SetupService::capabilities().data_dir_fallback.has_value());

    setenv("HMS_CPAP_DATA_DIR_FALLBACK", "/share/cpapdash", 1);
    const auto caps = hms_cpap::SetupService::capabilities();
    ASSERT_TRUE(caps.data_dir_fallback.has_value());
    EXPECT_EQ(caps.data_dir_fallback->wanted, "/share/cpapdash");
    EXPECT_EQ(caps.data_dir_fallback->in_use, caps.data_dir);
    unsetenv("HMS_CPAP_DATA_DIR_FALLBACK");
}
