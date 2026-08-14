// Issue #23: /api/upload/cpap kept only YYYYMMDD directories, so a zip of the
// card ROOT lost STR.edf on the way in and the endpoint still answered "queued".
// Backfill then derived the daily summary from sessions and the dashboard read
// AHI 0.0, while the machine's own STR record for the same night said 2.8.
//
// Reproduced against the live endpoint before this was written: STR.edf,
// Identification.tgt and SETTINGS/SET1.tgt were all absent from the card
// afterwards, and the log said "no STR.edf at <card>".
#include <gtest/gtest.h>

#include "utils/CardImport.h"

#include <filesystem>
#include <fstream>
#include <string>

using namespace hms_cpap;
namespace fs = std::filesystem;

namespace {

class CardMirror : public ::testing::Test {
protected:
    fs::path root_, zip_, card_, datalog_;

    void SetUp() override {
        root_ = fs::temp_directory_path() /
                ("hms_cardimport_" + std::to_string(::getpid()) +
                 std::to_string(reinterpret_cast<uintptr_t>(this)));
        zip_     = root_ / "extracted";
        card_    = root_ / "cardroot";
        datalog_ = card_ / "DATALOG";
        fs::create_directories(zip_);
        fs::create_directories(datalog_);
    }
    void TearDown() override { std::error_code ec; fs::remove_all(root_, ec); }

    void write(const fs::path& p, const std::string& body = "x") {
        fs::create_directories(p.parent_path());
        std::ofstream(p) << body;
    }
    void writeSized(const fs::path& p, int mb) {
        fs::create_directories(p.parent_path());
        std::ofstream f(p, std::ios::binary);
        std::string chunk(1024 * 1024, 'x');
        for (int i = 0; i < mb; ++i) f.write(chunk.data(), chunk.size());
    }
    CardImportResult run() {
        return mirrorCardInto(zip_.string(), card_.string(), datalog_.string());
    }
    bool onCard(const std::string& rel) const { return fs::exists(card_ / rel); }
};

}  // namespace

// The whole point: a zip of the card root keeps everything the card had.
TEST_F(CardMirror, ACardRootZipKeepsStrAndIdentityAndSettings) {
    write(zip_ / "DATALOG" / "20250812" / "20250812_233427_BRP.edf");
    write(zip_ / "DATALOG" / "20250812" / "20250812_233427_EVE.edf");
    write(zip_ / "STR.edf");
    write(zip_ / "Identification.tgt");
    write(zip_ / "SETTINGS" / "SET1.tgt");
    write(zip_ / "Journal.dat");

    auto r = run();

    EXPECT_TRUE(onCard("STR.edf")) << "the night's therapy summary was dropped again";
    EXPECT_TRUE(r.saw_str);
    EXPECT_TRUE(onCard("Identification.tgt"));
    EXPECT_TRUE(onCard("SETTINGS/SET1.tgt"));
    EXPECT_TRUE(onCard("Journal.dat"));
    // Date folders still land under DATALOG, which is what backfill scans.
    EXPECT_TRUE(onCard("DATALOG/20250812/20250812_233427_BRP.edf"));
    EXPECT_TRUE(onCard("DATALOG/20250812/20250812_233427_EVE.edf"));
    EXPECT_EQ(r.dates, std::set<std::string>{"20250812"});
    EXPECT_EQ(r.copied, 6);
}

// Zipping a folder rather than its contents is at least as common.
TEST_F(CardMirror, AZipThatWrapsTheCardInAFolderResolvesTheSame) {
    write(zip_ / "MyCard" / "DATALOG" / "20250812" / "20250812_233427_BRP.edf");
    write(zip_ / "MyCard" / "STR.edf");
    write(zip_ / "MyCard" / "SETTINGS" / "SET1.tgt");

    auto r = run();

    EXPECT_TRUE(onCard("STR.edf")) << "the wrapper folder was treated as the card";
    EXPECT_TRUE(onCard("SETTINGS/SET1.tgt"));
    EXPECT_TRUE(onCard("DATALOG/20250812/20250812_233427_BRP.edf"));
    EXPECT_FALSE(onCard("MyCard/STR.edf")) << "the wrapper leaked into the layout";
    EXPECT_TRUE(r.saw_str);
}

// A night is addressed by its date folder wherever the zip buried it.
TEST_F(CardMirror, DateFoldersLandUnderDatalogHoweverDeepTheyWere) {
    write(zip_ / "DATALOG" / "20250812" / "20250812_233427_BRP.edf");
    write(zip_ / "backup" / "old" / "20250701" / "20250701_220000_BRP.edf");

    auto r = run();

    EXPECT_TRUE(onCard("DATALOG/20250812/20250812_233427_BRP.edf"));
    EXPECT_TRUE(onCard("DATALOG/20250701/20250701_220000_BRP.edf"));
    EXPECT_EQ(r.dates.size(), 2u);
}

// CpapDash is not a storage cloud, and ezshare.cfg can hold WiFi credentials.
TEST_F(CardMirror, JunkAndCredentialsAreSkippedAndCounted) {
    write(zip_ / "DATALOG" / "20250812" / "20250812_233427_BRP.edf");
    write(zip_ / "ezshare.cfg", "ssid=home\npassword=hunter2\n");
    write(zip_ / "holiday.jpg");
    write(zip_ / "taxes.xlsx");
    write(zip_ / ".DS_Store");
    write(zip_ / "._AGL.tgt");

    auto r = run();

    EXPECT_FALSE(onCard("ezshare.cfg")) << "WiFi credentials were archived";
    EXPECT_FALSE(onCard("holiday.jpg"));
    EXPECT_FALSE(onCard("taxes.xlsx"));
    EXPECT_FALSE(onCard(".DS_Store"));
    EXPECT_FALSE(onCard("._AGL.tgt"));
    EXPECT_EQ(r.skipped, 5);
    EXPECT_EQ(r.copied, 1);
}

// SDD-049: the cap is the honest guard, and a whole-card container is exempt
// from it. A Prisma card carries one file; dropping it drops the upload.
TEST_F(CardMirror, TheSizeCapDropsBulkButNotALowensteinContainer) {
    write(zip_ / "DATALOG" / "20250812" / "20250812_233427_BRP.edf");
    writeSized(zip_ / "movie.mp4", 25);
    writeSized(zip_ / "huge.dat", 25);
    writeSized(zip_ / "therapy.pdat", 25);

    auto r = run();

    EXPECT_FALSE(onCard("movie.mp4"));
    EXPECT_FALSE(onCard("huge.dat")) << "a 25 MB non-container should hit the cap";
    EXPECT_TRUE(onCard("therapy.pdat"))
        << "the only file on a Prisma card was dropped by the size cap";
}

// The endpoint used to answer "queued" over a payload it had thrown away.
TEST_F(CardMirror, AZipWithNoDateFoldersReportsNoDatesRatherThanPretending) {
    write(zip_ / "STR.edf");
    write(zip_ / "Identification.tgt");

    auto r = run();

    EXPECT_TRUE(r.dates.empty()) << "the caller must be able to tell the user";
    // The root files are still mirrored: they are what the next upload needs.
    EXPECT_TRUE(onCard("STR.edf"));
    EXPECT_TRUE(r.saw_str);
}

// Re-uploading the same card must not fail or duplicate.
TEST_F(CardMirror, ReimportingTheSameCardOverwritesInPlace) {
    write(zip_ / "DATALOG" / "20250812" / "20250812_233427_BRP.edf", "first");
    run();
    write(zip_ / "DATALOG" / "20250812" / "20250812_233427_BRP.edf", "second");
    auto r = run();

    EXPECT_EQ(r.copied, 1);
    std::ifstream in(card_ / "DATALOG" / "20250812" / "20250812_233427_BRP.edf");
    std::string body((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    EXPECT_EQ(body, "second") << "a re-upload did not refresh the file";
}
