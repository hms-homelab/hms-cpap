// SDD-028 (#32): the ring's .vld files from a folder beside the card and from
// the O2 upload, stored as the same row the live pull would store.
//
// A real SQLite database on a temp file, as test_OximetryBackends.cpp does:
// the dedupe is the UNIQUE filename and ON CONFLICT DO UPDATE, which only a
// real database runs. VLD bytes are built as test_OximetryService.cpp does.
#include <gtest/gtest.h>

#include "database/SQLiteDatabase.h"
#include "services/OximetryImport.h"
#include "utils/OximetryDevice.h"

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <memory>
#include <set>
#include <string>
#include <unistd.h>
#include <vector>

using namespace hms_cpap;
namespace fs = std::filesystem;

namespace {

/// A minimal valid VLD v3: header + [records] 5-byte records, starting at
/// 2026-09-12 [hour]:30:00, so two files differ by their start.
std::string vld(int records = 3, int hour = 22) {
    std::vector<uint8_t> d(40 + 5 * records, 0);
    d[0] = 3;                                  // version 3
    d[2] = 0xEA; d[3] = 0x07;                  // 2026
    d[4] = 9; d[5] = 12; d[6] = static_cast<uint8_t>(hour); d[7] = 30; d[8] = 0;
    d[18] = static_cast<uint8_t>(records * 4); // duration, 4 s a record
    for (int i = 0; i < records; ++i) {
        d[40 + 5 * i]     = static_cast<uint8_t>(95 - i);   // SpO2
        d[40 + 5 * i + 1] = 70;                              // HR
    }
    return std::string(d.begin(), d.end());
}

long long count(IDatabase& db, const std::string& sql) {
    auto rows = db.executeQuery(sql, {});
    if (!rows.isArray() || rows.empty()) return -1;
    const auto& v = rows[0u]["n"];
    return v.isString() ? std::atoll(v.asCString()) : v.asInt64();
}

class OximetryImportTest : public ::testing::Test {
protected:
    void SetUp() override {
        const auto tag = std::to_string(::getpid());
        path_ = (fs::temp_directory_path() / ("hms_vld_import_" + tag + ".db")).string();
        fs::remove(path_);
        auto lite = std::make_unique<SQLiteDatabase>(path_);
        ASSERT_TRUE(lite->connect());
        db_ = std::move(lite);
        root_ = fs::temp_directory_path() / ("hms_vld_card_" + tag);
        fs::remove_all(root_);
        fs::create_directories(root_ / "DATALOG" / "20260912");
    }
    void TearDown() override {
        db_.reset();
        fs::remove(path_);
        fs::remove_all(root_);
    }

    void put(const fs::path& rel, const std::string& bytes) {
        fs::create_directories((root_ / rel).parent_path());
        std::ofstream(root_ / rel, std::ios::binary) << bytes;
    }
    long long sessions() { return count(*db_, "SELECT COUNT(*) AS n FROM oximetry_sessions"); }

    std::string path_;
    fs::path root_;
    std::unique_ptr<IDatabase> db_;
};

}  // namespace

TEST_F(OximetryImportTest, AVldIsStoredAsTheRingsOwnSession) {
    const auto r = importVldFile(*db_, vld(3), "20260912223000.vld");
    ASSERT_TRUE(r.ok) << r.error;
    EXPECT_EQ(r.samples, 3);
    EXPECT_GT(r.avg_spo2, 90);
    EXPECT_EQ(sessions(), 1);
    // Under the device the live pull uses, by the name it would use: the same
    // file pulled off the ring later is this row, not a second night.
    EXPECT_TRUE(db_->oximetrySessionExists(kOximetryDeviceId, "20260912223000.vld"));
}

TEST_F(OximetryImportTest, AFileThatIsNotAVldIsRefusedByNameAndNothingIsWritten) {
    const auto r = importVldFile(*db_, "this is a csv, not a ring file", "bad.vld");
    EXPECT_FALSE(r.ok);
    EXPECT_NE(r.error.find("bad.vld"), std::string::npos);
    EXPECT_EQ(sessions(), 0);
}

TEST_F(OximetryImportTest, TheSameFileTwiceIsOneNight) {
    ASSERT_TRUE(importVldFile(*db_, vld(3), "a.vld").ok);
    ASSERT_TRUE(importVldFile(*db_, vld(3), "a.vld").ok);
    EXPECT_EQ(sessions(), 1);
    EXPECT_EQ(count(*db_, "SELECT COUNT(*) AS n FROM oximetry_samples"), 3)
        << "samples must not accumulate either";
}

TEST_F(OximetryImportTest, TheScanReadsTheRootAndEveryFolderButTheCardsOwn) {
    // D1: whatever the other tool names its folder. todd3835 wrote "Oxymetry".
    put("Oxymetry/a.vld", vld(3, 21));
    put("Oximetry/b.VLD", vld(3, 22));
    put("root.vld", vld(3, 23));
    put("DATALOG/20260912/x.vld", vld(3, 1));   // the card's own folder: not read
    put("SETTINGS/y.vld", vld(3, 2));           // nor this one
    put("Oxymetry/notes.txt", "not a ring file");

    std::set<std::string> refused;
    const auto scan = importVldFolder(*db_, root_.string(), refused);
    EXPECT_EQ(scan.imported, 3);
    EXPECT_EQ(scan.refused, 0);
    EXPECT_EQ(sessions(), 3);
    EXPECT_TRUE(db_->oximetrySessionExists(kOximetryDeviceId, "a.vld"));
    EXPECT_TRUE(db_->oximetrySessionExists(kOximetryDeviceId, "b.VLD"));
    EXPECT_TRUE(db_->oximetrySessionExists(kOximetryDeviceId, "root.vld"));
    EXPECT_FALSE(db_->oximetrySessionExists(kOximetryDeviceId, "x.vld"));
    EXPECT_FALSE(db_->oximetrySessionExists(kOximetryDeviceId, "y.vld"));

    // The next burst: everything is already stored, nothing is re-imported.
    const auto again = importVldFolder(*db_, root_.string(), refused);
    EXPECT_EQ(again.imported, 0);
    EXPECT_EQ(again.skipped, 3);
    EXPECT_EQ(sessions(), 3);
}

TEST_F(OximetryImportTest, AnUnreadableFileIsReportedOnceAndNotReReadEveryBurst) {
    put("Oxymetry/broken.vld", "garbage");
    std::set<std::string> refused;
    EXPECT_EQ(importVldFolder(*db_, root_.string(), refused).refused, 1);
    const auto again = importVldFolder(*db_, root_.string(), refused);
    EXPECT_EQ(again.refused, 0) << "not retried every burst";
    EXPECT_EQ(again.skipped, 1);
    EXPECT_EQ(sessions(), 0);
}

TEST_F(OximetryImportTest, AMissingOrEmptyRootIsANoOpNotAThrow) {
    std::set<std::string> refused;
    EXPECT_EQ(importVldFolder(*db_, "", refused).imported, 0);
    EXPECT_EQ(importVldFolder(*db_, (root_ / "nope").string(), refused).imported, 0);
    EXPECT_EQ(sessions(), 0);
}

TEST(OximetryImportName, OnlyAVldEndingCounts) {
    EXPECT_TRUE(isVldFilename("20260912223000.vld"));
    EXPECT_TRUE(isVldFilename("NIGHT.VLD"));
    EXPECT_FALSE(isVldFilename("O2Ring export.csv"));
    EXPECT_FALSE(isVldFilename(".vld"));
    EXPECT_FALSE(isVldFilename("x.vld.bak"));
}
