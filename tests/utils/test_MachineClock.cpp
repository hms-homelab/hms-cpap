// SDD-054 D3: the machine's drift and zone, learned from its own card.
#include <gtest/gtest.h>

#include "clients/LocalDataSource.h"
#include "utils/MachineClock.h"

#include <chrono>
#include <cstdlib>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <string>
#ifndef _WIN32
#include <unistd.h>
#else
#include <process.h>
#define getpid _getpid
#endif

using namespace hms_cpap;
namespace fs = std::filesystem;

namespace {

EzShareFileEntry entry(const std::string& name, int y, int mo, int d, int h, int mi, int s,
                       int kb = 10) {
    EzShareFileEntry e;
    e.name = name;
    e.year = y; e.month = mo; e.day = d;
    e.hour = h; e.minute = mi; e.second = s;
    e.size_kb = kb;
    e.is_dir = false;
    return e;
}

// 2026-10-14 07:10:00 UTC
constexpr long long kHostNow = 1791961800;

}  // namespace

TEST(MachineClock, AStaleListingTeachesNothing) {
    const std::vector<EzShareFileEntry> files = {
        entry("20261013_225920_BRP.edf", 2026, 10, 14, 7, 9, 0, 3000)};
    const auto prev = stampsOf(files);
    EXPECT_FALSE(learnMachineClock({}, prev, files, kHostNow).has_value());
}

TEST(MachineClock, TheCslGivesTheZoneAndTheMovedStampGivesTheDrift) {
    // An AirSense on UTC-8 whose UTC runs 15 minutes ahead of the host.
    const std::vector<EzShareFileEntry> before = {
        entry("20261013_225920_CSL.edf", 2026, 10, 14, 6, 59, 20, 1),
        entry("20261013_225920_BRP.edf", 2026, 10, 14, 7, 20, 0, 3000)};
    const std::vector<EzShareFileEntry> after = {
        entry("20261013_225920_CSL.edf", 2026, 10, 14, 6, 59, 20, 1),
        entry("20261013_225920_BRP.edf", 2026, 10, 14, 7, 25, 0, 3006)};
    const auto learned = learnMachineClock({}, stampsOf(before), after, kHostNow);
    ASSERT_TRUE(learned.has_value());
    EXPECT_TRUE(learned->zone_known);
    EXPECT_EQ(learned->zone_s, -8 * 3600);
    EXPECT_EQ(learned->drift_s, 15 * 60);
    EXPECT_EQ(machineOffset(*learned, kHostNow), -8 * 3600 + 15 * 60);
}

TEST(MachineClock, AZoneOffAStepIsNotAccepted) {
    // No CSL: the largest name - stamp is a BRP written 40 minutes after its
    // start, so the value is 40 minutes off the zone and is refused.
    const std::vector<EzShareFileEntry> before = {
        entry("20261013_225920_BRP.edf", 2026, 10, 14, 7, 35, 0, 3000)};
    const std::vector<EzShareFileEntry> after = {
        entry("20261013_225920_BRP.edf", 2026, 10, 14, 7, 39, 20, 3010)};
    const auto learned = learnMachineClock({}, stampsOf(before), after, kHostNow);
    ASSERT_TRUE(learned.has_value());
    EXPECT_FALSE(learned->zone_known);
}

TEST(MachineClock, UntilAZoneIsLearnedTheHostsZoneStandsIn) {
    MachineClock c;
    EXPECT_EQ(machineOffset(c, kHostNow), hostZoneOffset(kHostNow));
    c.drift_s = 900;
    EXPECT_EQ(machineOffset(c, kHostNow), 900 + hostZoneOffset(kHostNow));
}

TEST(MachineClock, RoundingIsToTheNearestQuarterHourBothWays) {
    EXPECT_EQ(machine_clock::roundToStep(449), 0);
    EXPECT_EQ(machine_clock::roundToStep(451), 900);
    EXPECT_EQ(machine_clock::roundToStep(-28790), -28800);
    EXPECT_EQ(machine_clock::nameTimeOf("STR.edf"), 0);
    EXPECT_EQ(machine_clock::nameTimeOf("20261013_225920_CSL.edf"), 1791932360);
}

// ── issue #95: where a learning came from, and the bridge's folders ───────

TEST(MachineClock, TheSameOffsetOnASecondNightCountsTwoNights) {
    MachineClock a;
    a.zone_s = -5 * 3600;
    a.zone_known = true;
    const auto one = noteLearning({}, a, machine_clock::kFromCardStamps, "20261118");
    EXPECT_EQ(one.nights, 1);
    EXPECT_EQ(one.from, "card_stamps");
    EXPECT_EQ(noteLearning(one, a, machine_clock::kFromCardStamps, "20261118").nights, 1)
        << "the same night again is still one night";
    const auto two = noteLearning(one, a, machine_clock::kFromCardStamps, "20261119");
    EXPECT_EQ(two.nights, 2);
    // The same whole offset split differently is the same clock.
    MachineClock split = a;
    split.drift_s = 900;
    split.zone_s = -5 * 3600 - 900;
    EXPECT_EQ(noteLearning(two, split, machine_clock::kFromCardStamps, "20261120").nights, 3);
    // A different offset, or another source, starts again.
    MachineClock other = a;
    other.zone_s = 0;
    EXPECT_EQ(noteLearning(two, other, machine_clock::kFromCardStamps, "20261120").nights, 1);
    EXPECT_EQ(noteLearning(two, a, machine_clock::kFromBrpGrowth, "20261120").nights, 1);
}

namespace {

/// The host's zone for one test, put back after.
class HostTz {
public:
    explicit HostTz(const char* tz) {
        const char* old = std::getenv("TZ");
        had_ = old != nullptr;
        if (had_) old_ = old;
        put(tz);
    }
    ~HostTz() {
        if (had_) put(old_.c_str());
        else {
#ifdef _WIN32
            _putenv_s("TZ", "");
#else
            unsetenv("TZ");
#endif
            tzset_();
        }
    }

private:
    static void tzset_() {
#ifdef _WIN32
        _tzset();
#else
        tzset();
#endif
    }
    static void put(const char* tz) {
#ifdef _WIN32
        _putenv_s("TZ", tz);
#else
        setenv("TZ", tz, 1);
#endif
        tzset_();
    }
    bool had_{false};
    std::string old_;
};

/// A file's modification time set to the real instant [utc].
void stampAt(const fs::path& p, long long utc) {
    const auto sys = std::chrono::system_clock::time_point(std::chrono::seconds(utc));
    const auto ft = fs::file_time_type::clock::now() +
                    std::chrono::duration_cast<fs::file_time_type::duration>(sys - std::chrono::system_clock::now());
    fs::last_write_time(p, ft);
}

void writeBytes(const fs::path& p, size_t n) {
    std::ofstream(p, std::ios::binary) << std::string(n, 'x');
}

}  // namespace

TEST(MachineClock, ABridgeFolderIsNotTheMachinesStampsAndWouldSplitEasternIntoDriftAndUtc) {
    // Issue #95 reproduced: a Sleep Bridge folder rebuilt by hms-cpap on an
    // Eastern host (EST, UTC-5) for a machine on the same Eastern clock. The
    // files are named on the machine's clock and stamped with the host's
    // write time; the listing renders that stamp on the host's local clock.
    HostTz tz("EST5EDT");
    const fs::path root = fs::temp_directory_path() / ("hms_cpap_bridge95_" + std::to_string(::getpid()));
    fs::remove_all(root);
    const fs::path night = root / "DATALOG" / "20261119";
    fs::create_directories(night);
    fs::create_directories(root / "batches");

    // 22:10 EST on 2026-11-19 is 03:10 UTC on the 20th.
    const long long start_utc = 1795144200;   // 2026-11-20 03:10:00 UTC
    const fs::path csl = night / "20261119_221000_CSL.edf";
    const fs::path brp = night / "20261119_221000_BRP.edf";
    writeBytes(csl, 1024);
    writeBytes(brp, 4096);
    stampAt(csl, start_utc);
    stampAt(brp, start_utc + 45 * 60);

    LocalDataSource bridge(root.string());
    EXPECT_FALSE(bridge.stampsAreTheMachines());
    EXPECT_TRUE(LocalDataSource::isBridgeFolder(root.string()));
    const auto before = bridge.listFiles("20261119");

    // The bridge streams more of the night: the BRP is rewritten "now".
    const long long host_now = start_utc + 50 * 60;
    writeBytes(brp, 8192);
    stampAt(brp, host_now - 20);
    const auto after = bridge.listFiles("20261119");

    // What SDD-054 D3 made of it before the fix: the sum right (-5 h), the
    // zone +00:00 and the drift -5 h, the split the Devices page showed.
    const auto learned = learnMachineClock({}, stampsOf(before), after, host_now);
    ASSERT_TRUE(learned.has_value());
    EXPECT_TRUE(learned->zone_known);
    EXPECT_EQ(learned->zone_s, 0);
    EXPECT_EQ(learned->drift_s, -5 * 3600);
    EXPECT_EQ(learnedOffset(*learned), -5 * 3600);

    // The same files as a card (no batches/): the stamps count as the machine's.
    fs::remove_all(root / "batches");
    EXPECT_TRUE(LocalDataSource(root.string()).stampsAreTheMachines());
    fs::remove_all(root);
}
