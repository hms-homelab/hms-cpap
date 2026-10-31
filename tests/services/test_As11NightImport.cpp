#include <gtest/gtest.h>

#include "services/As11NightImport.h"
#include "services/As11NightWriter.h"
#include "database/SQLiteDatabase.h"
#include "utils/SessionEnd.h"

#include <chrono>
#include <filesystem>
#include <memory>
#include <string>
#include <unistd.h>
#include <vector>

using namespace hms_cpap;
using namespace hms_cpap::as11;
namespace fs = std::filesystem;
using std::chrono::seconds;
using std::chrono::system_clock;

// SDD-057 D9: a night hms-cpap wrote from the machine's Bluetooth stream is a
// session like a night read from files, and D5: the card wins a stretch it
// carries. Every value is made up; no real night.

namespace {

constexpr const char* kSerial = "23000000001";
constexpr const char* kDevice = "bt_import_dev";
// 2026-10-17 03:00:00 UTC: 22:00 on the 16th at UTC-5, the night of the 16th.
constexpr int64_t kT0 = 1792206000000LL;

StreamBlock second(int64_t start) {
    StreamBlock b;
    b.start_ms = start;
    b.serial = kSerial;
    b.channels.push_back({kFlow, 0, std::vector<int16_t>(25, 300)});
    b.channels.push_back({kMaskPressure, 0, std::vector<int16_t>(25, 900)});
    return b;
}

class ImportTest : public ::testing::Test {
protected:
    fs::path dir_;
    std::shared_ptr<SQLiteDatabase> db_;

    void SetUp() override {
        dir_ = fs::temp_directory_path() /
               ("as11_import_" + std::to_string(::getpid()) + "_" +
                std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
        fs::create_directories(dir_);
        db_ = std::make_shared<SQLiteDatabase>((dir_ / "test.db").string());
        ASSERT_TRUE(db_->connect());
    }
    void TearDown() override {
        db_->disconnect();
        fs::remove_all(dir_);
    }

    /// One night of [secs] seconds streamed and completed, as the writer
    /// lays it out under <data dir>/as11/<serial>/DATALOG/20261016/.
    void streamNight(int secs = 60) {
        As11NightWriter w(dir_.string(), fixedOffsetClock(-300));
        for (int i = 0; i < secs; i++) w.onBlock(second(kT0 + i * 1000LL));
        w.onStreamEnd(kSerial);
    }

    As11NightImport importer() { return As11NightImport(dir_.string(), db_, kDevice, "AirSense 11"); }

    /// A card session's row, as the card path stores one.
    void saveCardSession(system_clock::time_point start, int secs) {
        CPAPSession s;
        s.device_id = kDevice;
        s.device_name = "AirSense 11";
        s.session_start = start;
        s.duration_seconds = secs;
        s.brp_file_path = "DATALOG/20261016/20261016_220010_BRP.edf";
        ASSERT_TRUE(db_->saveSession(s));
    }
};

system_clock::time_point at(long long s) { return system_clock::time_point(seconds(s)); }

}  // namespace

// ── the decisions ────────────────────────────────────────────────────────

TEST(As11NightImportDecision, OverlapIsInclusiveOnBothEnds) {
    EXPECT_TRUE(As11NightImport::overlaps(at(100), at(200), at(150), at(300)));
    EXPECT_TRUE(As11NightImport::overlaps(at(100), at(200), at(200), at(300))) << "touching counts";
    EXPECT_TRUE(As11NightImport::overlaps(at(100), at(200), at(50), at(100)));
    EXPECT_TRUE(As11NightImport::overlaps(at(100), at(200), at(120), at(130))) << "contained";
    EXPECT_FALSE(As11NightImport::overlaps(at(100), at(200), at(201), at(300)));
    EXPECT_FALSE(As11NightImport::overlaps(at(100), at(200), at(10), at(99)));
}

// The bench shape: the stream held one stretch (11:58:48 to 12:10:25), the
// machine cut its card files at noon (11:58:58 to 12:00:00, 12:00:05 to
// 12:10:00). Both card files overlap the one Bluetooth stretch.
TEST(As11NightImportDecision, TheCardHoldsAStretchItOverlapsAndOnlyACardCounts) {
    using S = As11NightImport::StoredSpan;
    const auto bt_start = at(43128), bt_end = at(43825);
    std::vector<S> stored = {
        {at(43138), at(43200), false},   // the card's first file
        {at(43205), at(43800), false},   // the card's second file
    };
    EXPECT_TRUE(As11NightImport::cardHolds(bt_start, bt_end, stored));

    std::vector<S> only_bluetooth = {{bt_start, bt_end, true}};
    EXPECT_FALSE(As11NightImport::cardHolds(bt_start, bt_end, only_bluetooth))
        << "a stored Bluetooth row is not the card";

    std::vector<S> another_night = {{at(50000), at(60000), false}};
    EXPECT_FALSE(As11NightImport::cardHolds(bt_start, bt_end, another_night));
    EXPECT_FALSE(As11NightImport::cardHolds(bt_start, bt_end, {}));
}

TEST(As11NightImportDecision, ACardSessionReplacesTheBluetoothRowsItOverlaps) {
    using S = As11NightImport::StoredSpan;
    std::vector<S> stored = {
        {at(43128), at(43825), true},    // the Bluetooth stretch
        {at(43138), at(43200), false},   // the card's own row, already stored
        {at(10000), at(20000), true},    // an earlier Bluetooth night
    };
    const auto replaced = As11NightImport::bluetoothRowsReplacedBy(at(43138), at(43200), stored);
    ASSERT_EQ(replaced.size(), 1u);
    EXPECT_EQ(replaced[0], at(43128));
    EXPECT_TRUE(As11NightImport::bluetoothRowsReplacedBy(at(30000), at(40000), stored).empty());
}

TEST(As11NightImportDecision, ABluetoothRowIsKnownByItsPath) {
    EXPECT_TRUE(As11NightImport::isBluetoothPath("as11/23000000001/DATALOG/20261016/20261016_220000_BRP.edf"));
    EXPECT_FALSE(As11NightImport::isBluetoothPath("DATALOG/20261016/20261016_220000_BRP.edf"));
    EXPECT_FALSE(As11NightImport::isBluetoothPath(""));
}

// ── the import ───────────────────────────────────────────────────────────

TEST_F(ImportTest, ANightOnDiskBecomesASessionWithItsFilesAndMetrics) {
    streamNight();
    auto imp = importer();
    const auto r = imp.run({});
    EXPECT_EQ(r.imported, 1);
    EXPECT_EQ(r.failed, 0);
    ASSERT_EQ(r.saved.size(), 1u);
    const auto& s = r.saved[0];
    ASSERT_TRUE(s.session_start.has_value());
    ASSERT_TRUE(s.brp_file_path.has_value());
    EXPECT_EQ(s.brp_file_path->rfind("as11/23000000001/DATALOG/20261016/", 0), 0u) << *s.brp_file_path;
    EXPECT_GE(s.duration_seconds.value_or(0), 59);

    EXPECT_TRUE(db_->sessionExists(kDevice, *s.session_start));
    EXPECT_TRUE(db_->getNightlyMetrics(kDevice, *s.session_start).has_value());
    const auto files = db_->getSessionFilesForDateFolder(kDevice, "20261016");
    ASSERT_FALSE(files.empty());
    for (const auto& f : files) EXPECT_TRUE(As11NightImport::isBluetoothPath(f.rel_path)) << f.rel_path;
    // Closed on its own data (SDD-037), not left open.
    EXPECT_TRUE(openSessionsStartedBefore(*db_, kDevice, *s.session_start + seconds(1)).empty());

    // Unchanged on disk: not read again.
    const auto again = imp.run({});
    EXPECT_EQ(again.imported, 0);
    EXPECT_EQ(again.grown, 0);
    EXPECT_TRUE(again.saved.empty());
}

TEST_F(ImportTest, ANewProcessReadsTheNewestStoredNightOnceAndAGrownOneAgain) {
    streamNight(30);
    EXPECT_EQ(importer().run({}).imported, 1);

    // A new importer (a restart) reads the newest stored night once, since it
    // may have grown while the process was down.
    auto imp = importer();
    const auto r = imp.run({});
    EXPECT_EQ(r.imported, 0);
    EXPECT_EQ(r.grown, 1);

    // It grew: read again.
    streamNight(60);
    EXPECT_EQ(imp.run({}).grown, 1);
    EXPECT_EQ(imp.run({}).grown, 0);
}

TEST_F(ImportTest, TheCardsCopyReplacesTheBluetoothRowItOverlaps) {
    streamNight();
    const auto r = importer().run({});
    ASSERT_EQ(r.saved.size(), 1u);
    const auto bt_start = *r.saved[0].session_start;

    // The card's file starts ten seconds later and overlaps the stretch.
    const auto card_start = bt_start + seconds(10);
    saveCardSession(card_start, 50);
    const auto end = card_start + seconds(50);
    EXPECT_EQ(As11NightImport::replaceBluetoothRows(*db_, kDevice, card_start, end), 1);
    EXPECT_FALSE(db_->sessionExists(kDevice, bt_start)) << "the Bluetooth row is gone";
    EXPECT_TRUE(db_->sessionExists(kDevice, card_start)) << "the card's row stays";
    EXPECT_EQ(As11NightImport::replaceBluetoothRows(*db_, kDevice, card_start, end), 0);

    // The next import leaves the stretch to the card.
    auto imp = importer();
    const auto after = imp.run({});
    EXPECT_EQ(after.imported, 0);
    EXPECT_EQ(after.card_wins, 1);
    EXPECT_FALSE(db_->sessionExists(kDevice, bt_start));
}

TEST_F(ImportTest, AStretchTheCardAlreadyHoldsIsNotImported) {
    streamNight();
    // The card's two files around the stream's one stretch, stored first.
    const auto probe = importer().run({});   // only to learn the stretch's start
    ASSERT_EQ(probe.saved.size(), 1u);
    const auto bt_start = *probe.saved[0].session_start;
    ASSERT_TRUE(As11NightImport::deleteSessionAt(*db_, kDevice, bt_start));
    saveCardSession(bt_start + seconds(10), 20);

    auto imp = importer();
    const auto r = imp.run({});
    EXPECT_EQ(r.imported, 0);
    EXPECT_EQ(r.card_wins, 1);
    EXPECT_FALSE(db_->sessionExists(kDevice, bt_start));
    EXPECT_TRUE(db_->sessionExists(kDevice, bt_start + seconds(10)));
}

TEST_F(ImportTest, ARemovedNightIsNotPutBack) {
    streamNight();
    const auto r = importer().run({"20261016"});
    EXPECT_EQ(r.imported, 0);
    EXPECT_TRUE(r.saved.empty());
}

TEST_F(ImportTest, NoFolderNoWork) {
    const auto r = importer().run({});
    EXPECT_EQ(r.imported, 0);
    EXPECT_EQ(r.failed, 0);
}

// ── the trigger ──────────────────────────────────────────────────────────

TEST_F(ImportTest, TheWriterSaysWhenANightIsComplete) {
    As11NightWriter w(dir_.string(), fixedOffsetClock(-300));
    std::vector<std::string> ended;
    w.setNightEndedHook([&](const std::string& serial) { ended.push_back(serial); });
    for (int i = 0; i < 5; i++) w.onBlock(second(kT0 + i * 1000LL));
    EXPECT_TRUE(ended.empty()) << "a block is not the end";
    w.onStreamEnd(kSerial);
    ASSERT_EQ(ended.size(), 1u);
    EXPECT_EQ(ended[0], kSerial);
}
