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
#ifdef WITH_POSTGRESQL
#include "database/PostgresDatabase.h"
#endif

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

    VldScanState state;
    const auto scan = importVldFolder(*db_, root_.string(), state);
    EXPECT_EQ(scan.found, 3);
    EXPECT_EQ(scan.imported, 3);
    EXPECT_EQ(scan.refused, 0);
    EXPECT_EQ(sessions(), 3);
    // It says where it looked and what it saw.
    EXPECT_TRUE(scan.summary_logged);
    EXPECT_NE(scan.summary.find("3 .vld file(s)"), std::string::npos) << scan.summary;
    EXPECT_NE(scan.summary.find("Oxymetry/"), std::string::npos) << scan.summary;
    EXPECT_NE(scan.summary.find("Oximetry/"), std::string::npos) << scan.summary;
    EXPECT_EQ(scan.summary.find("DATALOG/"), std::string::npos) << scan.summary;
    EXPECT_TRUE(db_->oximetrySessionExists(kOximetryDeviceId, "a.vld"));
    EXPECT_TRUE(db_->oximetrySessionExists(kOximetryDeviceId, "b.VLD"));
    EXPECT_TRUE(db_->oximetrySessionExists(kOximetryDeviceId, "root.vld"));
    EXPECT_FALSE(db_->oximetrySessionExists(kOximetryDeviceId, "x.vld"));
    EXPECT_FALSE(db_->oximetrySessionExists(kOximetryDeviceId, "y.vld"));

    // The next burst: everything is already stored, nothing is re-imported,
    // and the same summary is not logged again.
    const auto again = importVldFolder(*db_, root_.string(), state);
    EXPECT_EQ(again.imported, 0);
    EXPECT_EQ(again.reimported, 0);
    EXPECT_EQ(again.skipped, 3);
    EXPECT_FALSE(again.summary_logged);
    EXPECT_EQ(sessions(), 3);
}

TEST_F(OximetryImportTest, AnUnreadableFileIsNotReReadEveryBurst) {
    put("Oxymetry/broken.vld", "garbage");
    VldScanState state;
    const auto first = importVldFolder(*db_, root_.string(), state);
    EXPECT_EQ(first.refused, 1);
    EXPECT_NE(first.summary.find("1 unreadable"), std::string::npos) << first.summary;
    const auto again = importVldFolder(*db_, root_.string(), state);
    EXPECT_EQ(again.refused, 0) << "not retried while it is unchanged";
    EXPECT_EQ(again.skipped, 1);
    EXPECT_EQ(sessions(), 0);
}

// #32, amended 2026-09-14: a file the scan could not read is read again once
// it changes (the other tool finished writing it), not only after a restart.
TEST_F(OximetryImportTest, AnUnreadableFileIsReadAgainWhenItChanges) {
    put("Oxymetry/a.vld", "garbage");
    VldScanState state;
    EXPECT_EQ(importVldFolder(*db_, root_.string(), state).refused, 1);

    put("Oxymetry/a.vld", vld(3, 22));   // now the whole file
    const auto scan = importVldFolder(*db_, root_.string(), state);
    EXPECT_EQ(scan.imported, 1);
    EXPECT_EQ(scan.refused, 0);
    EXPECT_TRUE(state.refused.empty());
    EXPECT_EQ(sessions(), 1);
}

// #32: a file stored while the other tool was still writing it is stored again
// when it grows, so the night is not left short. Same name, same row.
TEST_F(OximetryImportTest, AStoredFileThatChangesIsStoredAgain) {
    put("Oxymetry/a.vld", vld(3, 22));
    VldScanState state;
    EXPECT_EQ(importVldFolder(*db_, root_.string(), state).imported, 1);
    EXPECT_EQ(count(*db_, "SELECT COUNT(*) AS n FROM oximetry_samples"), 3);

    put("Oxymetry/a.vld", vld(8, 22));   // the rest of the night arrived
    const auto scan = importVldFolder(*db_, root_.string(), state);
    EXPECT_EQ(scan.imported, 0);
    EXPECT_EQ(scan.reimported, 1);
    EXPECT_EQ(sessions(), 1) << "the same night, not a second one";
    EXPECT_EQ(count(*db_, "SELECT COUNT(*) AS n FROM oximetry_samples"), 8);

    // And once it stops changing, it is left alone.
    const auto settled = importVldFolder(*db_, root_.string(), state);
    EXPECT_EQ(settled.reimported, 0);
    EXPECT_EQ(settled.skipped, 1);
}

// After a restart the scan has no memory of sizes: a file already stored (by an
// earlier run or by the upload) is trusted as it is rather than re-read.
TEST_F(OximetryImportTest, AFileStoredBeforeThisRunIsTrustedAsItIs) {
    put("Oxymetry/a.vld", vld(3, 22));
    ASSERT_TRUE(importVldFile(*db_, vld(3, 22), "a.vld").ok);   // the upload, say
    VldScanState state;
    const auto scan = importVldFolder(*db_, root_.string(), state);
    EXPECT_EQ(scan.found, 1);
    EXPECT_EQ(scan.imported, 0);
    EXPECT_EQ(scan.reimported, 0);
    EXPECT_EQ(scan.skipped, 1);
    EXPECT_EQ(sessions(), 1);
}

// #32: the scan used to say nothing when it found nothing, so a log could not
// tell "no files where it looked" from "files already stored". It now says
// where it looked, and counts the folders it does not descend into.
TEST_F(OximetryImportTest, WhenItFindsNothingItSaysWhereItLooked) {
    put("Oxymetry/O2Ring 1234/2026/20260912223000.vld", vld(3, 22));   // one level too deep
    VldScanState state;
    const auto scan = importVldFolder(*db_, root_.string(), state);
    EXPECT_EQ(scan.found, 0);
    EXPECT_EQ(sessions(), 0);
    EXPECT_TRUE(scan.summary_logged);
    EXPECT_NE(scan.summary.find("no .vld files in the root, Oxymetry/ and its 1 folder(s)"),
              std::string::npos) << scan.summary;
    EXPECT_NE(scan.summary.find("1 folder(s) further down in Oxymetry/ are not searched"),
              std::string::npos) << scan.summary;
    // Logged once, not every burst.
    EXPECT_FALSE(importVldFolder(*db_, root_.string(), state).summary_logged);
}

// #32, SDD-028 §7: todd3835's card files each night in its own folder inside
// OXYMETRY, as DATALOG does. One level deeper is read; the card's own folders
// still are not, however deep.
TEST_F(OximetryImportTest, ANightPerFolderInsideTheToolsFolderIsRead) {
    put("OXYMETRY/20260913/20260913223000.vld", vld(3, 21));
    put("OXYMETRY/20260914/20260914224500.vld", vld(3, 22));
    put("OXYMETRY/20260914/notes.txt", "not a ring file");
    put("DATALOG/20260912/x.vld", vld(3, 1));
    put("SETTINGS/sub/y.vld", vld(3, 2));

    VldScanState state;
    const auto scan = importVldFolder(*db_, root_.string(), state);
    EXPECT_EQ(scan.found, 2);
    EXPECT_EQ(scan.imported, 2);
    EXPECT_EQ(sessions(), 2);
    EXPECT_TRUE(db_->oximetrySessionExists(kOximetryDeviceId, "20260913223000.vld"));
    EXPECT_TRUE(db_->oximetrySessionExists(kOximetryDeviceId, "20260914224500.vld"));
    EXPECT_FALSE(db_->oximetrySessionExists(kOximetryDeviceId, "x.vld"));
    EXPECT_FALSE(db_->oximetrySessionExists(kOximetryDeviceId, "y.vld"));
    EXPECT_NE(scan.summary.find("2 .vld file(s) in the root, OXYMETRY/ and its 2 folder(s)"),
              std::string::npos) << scan.summary;
    EXPECT_EQ(scan.summary.find("not searched;"), std::string::npos) << scan.summary;
    EXPECT_EQ(scan.summary.find("further down"), std::string::npos) << scan.summary;

    // The next burst imports nothing; the next night's folder is picked up.
    const auto again = importVldFolder(*db_, root_.string(), state);
    EXPECT_EQ(again.imported, 0);
    EXPECT_EQ(again.skipped, 2);
    put("OXYMETRY/20260915/20260915221000.vld", vld(3, 23));
    const auto next = importVldFolder(*db_, root_.string(), state);
    EXPECT_EQ(next.imported, 1);
    EXPECT_EQ(sessions(), 3);
}

// SDD-029: a night the operator removed is not brought back by the scan, and
// is not remembered as refused either, so a Reparse that restores it gets its
// ring data back on the next pass.
TEST_F(OximetryImportTest, ARemovedNightsFileIsSkippedUntilTheNightIsRestored) {
    put("Oxymetry/a.vld", vld(3, 22));   // 2026-09-12 22:30, night 20260912
    VldScanState state;
    std::set<std::string> removed{"20260912"};

    const auto scan = importVldFolder(*db_, root_.string(), state, removed);
    EXPECT_EQ(scan.imported, 0);
    EXPECT_EQ(scan.skipped, 1);
    EXPECT_EQ(scan.refused, 0) << "a removed night is not a bad file";
    EXPECT_TRUE(state.refused.empty()) << "must not be remembered past a restore";
    EXPECT_TRUE(state.stored.empty()) << "must not be remembered past a restore";
    EXPECT_EQ(sessions(), 0);

    removed.clear();   // Reparse restored it
    EXPECT_EQ(importVldFolder(*db_, root_.string(), state, removed).imported, 1);
    EXPECT_EQ(sessions(), 1);
}

TEST_F(OximetryImportTest, ARemovedNightLeavesTheOtherNightsAlone) {
    put("Oxymetry/a.vld", vld(3, 22));   // night 20260912
    put("Oxymetry/b.vld", vld(3, 10));   // 2026-09-12 10:30, night 20260911
    VldScanState state;
    const auto scan = importVldFolder(*db_, root_.string(), state, {"20260912"});
    EXPECT_EQ(scan.imported, 1);
    EXPECT_EQ(scan.skipped, 1);
}

TEST_F(OximetryImportTest, TheUploadNamesNoRemovedNightsSoItStores) {
    // The upload passes none: an operator uploading a night is asking for it.
    const auto r = importVldFile(*db_, vld(), "20260912223000.vld");
    EXPECT_TRUE(r.ok);
    const auto skipped = importVldFile(*db_, vld(3, 21), "other.vld", {"20260912"});
    EXPECT_FALSE(skipped.ok);
    EXPECT_TRUE(skipped.removed_night);
    EXPECT_EQ(sessions(), 1);
}

TEST_F(OximetryImportTest, AMissingOrEmptyRootIsANoOpNotAThrow) {
    VldScanState state;
    EXPECT_EQ(importVldFolder(*db_, "", state).imported, 0);
    const auto missing = importVldFolder(*db_, (root_ / "nope").string(), state);
    EXPECT_EQ(missing.imported, 0);
    EXPECT_NE(missing.summary.find("not a folder"), std::string::npos) << missing.summary;
    EXPECT_EQ(sessions(), 0);
}

// #32 runs PostgreSQL. The re-read of a changed file is an upsert on the
// filename, and the night's samples must be replaced, not added to. Runs when
// PGHOST is set; the rows are namespaced to this process and deleted.
TEST(OximetryImportPostgres, AStoredFileThatChangesReplacesItsSamples) {
#ifndef WITH_POSTGRESQL
    GTEST_SKIP() << "built without PostgreSQL";
#else
    const char* host = std::getenv("PGHOST");
    if (!host || !*host) GTEST_SKIP() << "PGHOST unset, skipping PostgreSQL.";
    auto env = [](const char* k, const char* d) {
        const char* v = std::getenv(k);
        return std::string(v && *v ? v : d);
    };
    PostgresDatabase db("host=" + std::string(host) + " port=" + env("PGPORT", "5432") +
                        " user=" + env("PGUSER", "maestro") + " password=" +
                        env("PGPASSWORD", "") + " dbname=" + env("PGDATABASE", "cpap_monitoring") +
                        " connect_timeout=3");
    if (!db.connect()) GTEST_SKIP() << "No usable PostgreSQL at " << host;

    const auto tag = std::to_string(::getpid());
    const fs::path root = fs::temp_directory_path() / ("hms_vld_pg_" + tag);
    fs::remove_all(root);
    fs::create_directories(root / "Oxymetry");
    const std::string name = "pg" + tag + ".vld";
    auto write = [&](int records) {
        std::ofstream(root / "Oxymetry" / name, std::ios::binary) << vld(records, 22);
    };
    auto samples = [&] {
        return count(db, "SELECT COUNT(*) AS n FROM oximetry_samples WHERE oximetry_session_id IN "
                         "(SELECT id FROM oximetry_sessions WHERE filename = '" + name + "')");
    };

    VldScanState state;
    write(3);
    EXPECT_EQ(importVldFolder(db, root.string(), state).imported, 1);
    EXPECT_EQ(samples(), 3);
    write(8);
    EXPECT_EQ(importVldFolder(db, root.string(), state).reimported, 1);
    EXPECT_EQ(samples(), 8) << "replaced, not 11";

    db.executeQuery("DELETE FROM oximetry_sessions WHERE filename = $1", {name});
    fs::remove_all(root);
#endif
}

TEST(OximetryImportName, OnlyAVldEndingCounts) {
    EXPECT_TRUE(isVldFilename("20260912223000.vld"));
    EXPECT_TRUE(isVldFilename("NIGHT.VLD"));
    EXPECT_FALSE(isVldFilename("O2Ring export.csv"));
    EXPECT_FALSE(isVldFilename(".vld"));
    EXPECT_FALSE(isVldFilename("x.vld.bak"));
}
