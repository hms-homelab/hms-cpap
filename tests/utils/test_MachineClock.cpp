// SDD-054 D3: the machine's drift and zone, learned from its own card.
#include <gtest/gtest.h>

#include "utils/MachineClock.h"

using namespace hms_cpap;

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
