/**
 * SefamIngestion — discovery across the S.Box's two incompatible card layouts.
 *
 * The thing under test is not parsing (the parser owns that) but DISCOVERY: a
 * Sefam session is identified by a folder AND a manifest name, because the two
 * device families disagree about what a folder holds.
 *
 *   1263R  <model><serial>/DATA_<n>/DATA_<n>.INI   one session per folder
 *   1200R  <model><serial>/<YYMMDD>/<HHMMSS>.ini   one folder per DAY, holding
 *                                                  every recording that day
 *
 * A walk that assumed one session per directory would silently keep one night
 * of a 1200R card and drop the rest, which is why the multi-manifest case has a
 * test of its own rather than being folded into the happy path.
 *
 * Manifests are written with CRLF, as the device writes them.
 */

#include <gtest/gtest.h>

#include "services/SefamIngestion.h"

#include <chrono>
#include <filesystem>
#include <fstream>
#include <string>

using namespace hms_cpap;
namespace fs = std::filesystem;

namespace {

/// A manifest the parser will accept: identity, and a start time.
///
/// Only [Create Info] and [Start Record] matter to discovery -- it never opens
/// a channel file -- but one channel block is included so the fixture is a
/// plausible manifest rather than a shape that only works by accident.
std::string manifest(const std::string& created_by,
                     const std::string& serial,
                     int year, int month, int day,
                     int hour, int min, int sec) {
    std::string s;
    auto line = [&s](const std::string& t) { s += t + "\r\n"; };
    line("[Create Info]");
    line("Created By=" + created_by + " ");
    line("Serial Number=" + serial);
    line("Version=VER :A020400");
    line("Date=10/11/25 22:25:16");
    line("[Start Record]");
    line("Hour=" + std::to_string(hour));
    line("Min=" + std::to_string(min));
    line("Sec=" + std::to_string(sec));
    line("Day=" + std::to_string(day));
    line("Month=" + std::to_string(month));
    line("Year=" + std::to_string(year));
    line("Programmed Record Duration=28800");
    line("Real Record Duration=28800");
    line("[Chan0]");
    line("Name=FLW");
    line("Description=NO");
    line("Type=4");
    line("Unit=lpm");
    line("Min=-180");
    line("Max=280");
    line("Freq=25");
    line("Bit=8");
    return s;
}

class SefamIngestionTest : public ::testing::Test {
protected:
    void SetUp() override {
        root_ = fs::temp_directory_path() /
                ("hms_sefam_ingest_" + std::to_string(::testing::UnitTest::GetInstance()
                                                          ->random_seed()) +
                 "_" + std::to_string(counter_++));
        fs::remove_all(root_);
        fs::create_directories(root_);
    }

    void TearDown() override {
        std::error_code ec;
        fs::remove_all(root_, ec);
    }

    /// Write a manifest at <root>/<rel_dir>/<name>, creating the folders.
    void write(const std::string& rel_dir, const std::string& name,
               const std::string& body) {
        const auto dir = root_ / rel_dir;
        fs::create_directories(dir);
        std::ofstream f(dir / name, std::ios::binary);
        f << body;
    }

    fs::path root_;
    static int counter_;
};

int SefamIngestionTest::counter_ = 0;

} // namespace

// A folder that is not a card is not an error to shout about -- "no" is the
// honest answer, and hms-cpap tries several ingesters against one directory.
TEST_F(SefamIngestionTest, AnEmptyDirectoryIsNotACard) {
    SefamIngestion ing(root_.string());
    EXPECT_FALSE(ing.initialize());
    EXPECT_EQ(ing.sessionCount(), 0u);
}

TEST_F(SefamIngestionTest, APathThatIsNotADirectoryIsRejected) {
    SefamIngestion ing((root_ / "no_such_place").string());
    EXPECT_FALSE(ing.initialize());
}

// Files that are not manifests must not be mistaken for sessions.
TEST_F(SefamIngestionTest, ADirectoryOfNonManifestsIsNotACard) {
    write("DATA_1", "DATA_1.DAT", "not a manifest");
    write("DATA_1", "readme.txt", "nor this");
    SefamIngestion ing(root_.string());
    EXPECT_FALSE(ing.initialize());
}

// The 1263R layout: one session per DATA_<n> folder, manifest in UPPER case.
TEST_F(SefamIngestionTest, TheSBoxLayoutIsDiscovered) {
    write("1263R24337476/DATA_1", "DATA_1.INI",
          manifest("S.Box_AUTO", "1263R24337476", 2025, 11, 10, 22, 25, 16));

    SefamIngestion ing(root_.string());
    ASSERT_TRUE(ing.initialize());
    EXPECT_EQ(ing.sessionCount(), 1u);
    EXPECT_EQ(ing.deviceSerial(), "1263R24337476");
    EXPECT_EQ(ing.deviceModel(), "S.Box_AUTO");

    const auto found = ing.discoverSessions(std::nullopt);
    ASSERT_EQ(found.size(), 1u);
    EXPECT_EQ(found[0].stem, "DATA_1");
}

// The 1200R layout: ONE FOLDER PER DAY holding several recordings, manifest in
// lower case. This is the case a per-directory walk gets wrong -- it would find
// the folder, keep one night and drop the other two without saying so.
TEST_F(SefamIngestionTest, ADayFolderHoldingSeveralRecordingsYieldsThemAll) {
    const std::string dir = "1200R99001122/251110";
    write(dir, "222516.ini", manifest("SleepBox_AUTO", "1200R99001122",
                                      2025, 11, 10, 22, 25, 16));
    write(dir, "010203.ini", manifest("SleepBox_AUTO", "1200R99001122",
                                      2025, 11, 11, 1, 2, 3));
    write(dir, "043000.ini", manifest("SleepBox_AUTO", "1200R99001122",
                                      2025, 11, 11, 4, 30, 0));

    SefamIngestion ing(root_.string());
    ASSERT_TRUE(ing.initialize());
    EXPECT_EQ(ing.sessionCount(), 3u) << "one folder, three recordings";
    EXPECT_EQ(ing.deviceModel(), "SleepBox_AUTO");
}

// Oldest first, and across folders -- the order the ingest loop relies on.
TEST_F(SefamIngestionTest, SessionsComeBackOldestFirst) {
    write("1263R24337476/DATA_3", "DATA_3.INI",
          manifest("S.Box_AUTO", "1263R24337476", 2025, 11, 12, 23, 0, 0));
    write("1263R24337476/DATA_1", "DATA_1.INI",
          manifest("S.Box_AUTO", "1263R24337476", 2025, 11, 10, 22, 25, 16));
    write("1263R24337476/DATA_2", "DATA_2.INI",
          manifest("S.Box_AUTO", "1263R24337476", 2025, 11, 11, 21, 40, 0));

    SefamIngestion ing(root_.string());
    ASSERT_TRUE(ing.initialize());

    const auto found = ing.discoverSessions(std::nullopt);
    ASSERT_EQ(found.size(), 3u);
    EXPECT_EQ(found[0].stem, "DATA_1");
    EXPECT_EQ(found[1].stem, "DATA_2");
    EXPECT_EQ(found[2].stem, "DATA_3");
    EXPECT_LT(found[0].session_start, found[1].session_start);
    EXPECT_LT(found[1].session_start, found[2].session_start);
}

// The incremental case: only what the caller has not seen. STRICTLY newer, so
// re-running with the last start returns nothing rather than re-ingesting the
// night we just stored.
TEST_F(SefamIngestionTest, OnlySessionsNewerThanTheWatermarkComeBack) {
    write("1263R24337476/DATA_1", "DATA_1.INI",
          manifest("S.Box_AUTO", "1263R24337476", 2025, 11, 10, 22, 25, 16));
    write("1263R24337476/DATA_2", "DATA_2.INI",
          manifest("S.Box_AUTO", "1263R24337476", 2025, 11, 11, 21, 40, 0));

    SefamIngestion ing(root_.string());
    ASSERT_TRUE(ing.initialize());

    const auto all = ing.discoverSessions(std::nullopt);
    ASSERT_EQ(all.size(), 2u);

    const auto after_first = ing.discoverSessions(all[0].session_start);
    ASSERT_EQ(after_first.size(), 1u);
    EXPECT_EQ(after_first[0].stem, "DATA_2");

    EXPECT_TRUE(ing.discoverSessions(all[1].session_start).empty())
        << "the newest session must not come back a second time";
}

// discoverSessions initialises on its own, so a caller cannot get an empty
// answer merely by forgetting to call initialize().
TEST_F(SefamIngestionTest, DiscoverWorksWithoutAnExplicitInitialize) {
    write("1263R24337476/DATA_1", "DATA_1.INI",
          manifest("S.Box_AUTO", "1263R24337476", 2025, 11, 10, 22, 25, 16));

    SefamIngestion ing(root_.string());
    EXPECT_EQ(ing.discoverSessions(std::nullopt).size(), 1u);
}

// A manifest with no [Start Record] cannot be placed in time, so it is skipped
// rather than landing at the epoch and sorting to the front of the card.
TEST_F(SefamIngestionTest, AManifestWithNoStartTimeIsSkipped) {
    write("1263R24337476/DATA_1", "DATA_1.INI",
          "[Create Info]\r\nCreated By=S.Box_AUTO \r\n"
          "Serial Number=1263R24337476\r\n");
    write("1263R24337476/DATA_2", "DATA_2.INI",
          manifest("S.Box_AUTO", "1263R24337476", 2025, 11, 11, 21, 40, 0));

    SefamIngestion ing(root_.string());
    ASSERT_TRUE(ing.initialize());
    ASSERT_EQ(ing.sessionCount(), 1u);
    EXPECT_EQ(ing.discoverSessions(std::nullopt)[0].stem, "DATA_2");
}

// Pointing straight at a session folder works, which is what a user who
// unzipped one night rather than a whole card will do.
TEST_F(SefamIngestionTest, PointingAtTheSessionFolderItselfWorks) {
    write("DATA_1", "DATA_1.INI",
          manifest("S.Box_AUTO", "1263R24337476", 2025, 11, 10, 22, 25, 16));

    SefamIngestion ing((root_ / "DATA_1").string());
    ASSERT_TRUE(ing.initialize());
    EXPECT_EQ(ing.sessionCount(), 1u);
}

// Identity is taken from the first manifest that carries one and then left
// alone, so a card is reported under one device rather than flickering.
TEST_F(SefamIngestionTest, TheDeviceIdentityIsTakenOnceAndKept) {
    write("1263R24337476/DATA_1", "DATA_1.INI",
          manifest("S.Box_AUTO", "1263R24337476", 2025, 11, 10, 22, 25, 16));
    write("1263R24337476/DATA_2", "DATA_2.INI",
          manifest("S.Box_AUTO", "1263R24337476", 2025, 11, 11, 21, 40, 0));

    SefamIngestion ing(root_.string());
    ASSERT_TRUE(ing.initialize());
    EXPECT_EQ(ing.deviceSerial(), "1263R24337476");
    EXPECT_EQ(ing.deviceModel(), "S.Box_AUTO");
}

// initialize() is idempotent: a second call must not walk the card again and
// double every session.
TEST_F(SefamIngestionTest, InitialiseTwiceDoesNotDoubleTheSessions) {
    write("1263R24337476/DATA_1", "DATA_1.INI",
          manifest("S.Box_AUTO", "1263R24337476", 2025, 11, 10, 22, 25, 16));

    SefamIngestion ing(root_.string());
    ASSERT_TRUE(ing.initialize());
    ASSERT_TRUE(ing.initialize());
    EXPECT_EQ(ing.sessionCount(), 1u);
}
