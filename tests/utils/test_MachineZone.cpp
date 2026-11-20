// SDD-074 D2: MachineZone, the one conversion between a machine's wall clock
// (held as a label) and a real instant. Pure: no database, no service. Run
// under several host zones (TZ=UTC, America/New_York, Australia/Sydney,
// Pacific/Kiritimati): every expectation below holds under each, because only
// the "same as this computer" tests read the host's zone, and those compare
// against the host's own mktime / localtime.
#include <gtest/gtest.h>

#include "utils/MachineZone.h"
#include "utils/TimeCompat.h"

#include <algorithm>
#include <ctime>

using namespace hms_cpap;

namespace {

/// A wall clock as a label.
long long L(int y, int mo, int d, int h, int mi = 0, int s = 0) {
    std::tm t{};
    t.tm_year = y - 1900;
    t.tm_mon = mo - 1;
    t.tm_mday = d;
    t.tm_hour = h;
    t.tm_min = mi;
    t.tm_sec = s;
    return wall::labelOf(t);
}

MachineZone zone(const std::string& s) {
    std::string why;
    auto z = MachineZone::parse(s, &why);
    EXPECT_TRUE(z.has_value()) << s << ": " << why;
    return z.value_or(MachineZone::host());
}

}  // namespace

TEST(MachineZone, AFixedOffsetIsTheSameInJanuaryAndJuly) {
    const auto z = zone("+10:00");
    EXPECT_EQ(z.kind(), MachineZone::Kind::Fixed);
    EXPECT_EQ(z.offsetAt(L(2026, 1, 15, 12) - 36000), 36000);
    EXPECT_EQ(z.offsetAt(L(2026, 7, 15, 12) - 36000), 36000);
    // 22:00 on the machine is 12:00 UTC, and back.
    EXPECT_EQ(z.toInstant(L(2026, 1, 15, 22)), L(2026, 1, 15, 12));
    EXPECT_EQ(z.toWall(L(2026, 1, 15, 12)), L(2026, 1, 15, 22));
}

TEST(MachineZone, SydneyHasItsTwoOffsets) {
    const auto z = zone("Australia/Sydney");
    EXPECT_EQ(z.kind(), MachineZone::Kind::Named);
    EXPECT_EQ(z.offsetAt(L(2026, 1, 15, 0)), 11 * 3600);   // AEDT
    EXPECT_EQ(z.offsetAt(L(2026, 7, 15, 0)), 10 * 3600);   // AEST
}

TEST(MachineZone, PastRulesComeFromTheDatabaseNotOnlyTodaysRule) {
    // New York moved its spring change from April to March in 2007: on
    // 2006-03-20 it was still on EST, on 2026-03-20 it is on EDT.
    const auto z = zone("America/New_York");
    EXPECT_EQ(z.offsetAt(L(2006, 3, 20, 12)), -5 * 3600);
    EXPECT_EQ(z.offsetAt(L(2026, 3, 20, 12)), -4 * 3600);
}

TEST(MachineZone, KiritimatiIsFourteenHoursAhead) {
    EXPECT_EQ(zone("Pacific/Kiritimati").offsetAt(L(2026, 11, 19, 0)), 14 * 3600);
    EXPECT_EQ(zone("+14:00").offsetAt(0), 14 * 3600);
}

TEST(MachineZone, AGapTimeMovesForwardByTheJump) {
    // Sydney springs forward on 2026-10-04 at 02:00 to 03:00: 02:30 never happens.
    const auto z = zone("Australia/Sydney");
    MachineZone::Resolution how{};
    const long long u = z.toInstant(L(2026, 10, 4, 2, 30), &how);
    EXPECT_EQ(how, MachineZone::Resolution::Gap);
    EXPECT_EQ(z.toWall(u), L(2026, 10, 4, 3, 30));
}

TEST(MachineZone, AFoldTimeIsReadAsItsFirstOccurrence) {
    // Sydney falls back on 2026-04-05 at 03:00 to 02:00: 02:30 happens twice,
    // first on AEDT (+11).
    const auto z = zone("Australia/Sydney");
    MachineZone::Resolution how{};
    const long long u = z.toInstant(L(2026, 4, 5, 2, 30), &how);
    EXPECT_EQ(how, MachineZone::Resolution::Fold);
    EXPECT_EQ(u, L(2026, 4, 5, 2, 30) - 11 * 3600);
    EXPECT_EQ(z.toWall(u), L(2026, 4, 5, 2, 30));
    // And New York's repeated 01:30 on 2026-11-01, first on EDT (-4).
    const auto ny = zone("America/New_York");
    EXPECT_EQ(ny.toInstant(L(2026, 11, 1, 1, 30), &how), L(2026, 11, 1, 1, 30) + 4 * 3600);
    EXPECT_EQ(how, MachineZone::Resolution::Fold);
}

TEST(MachineZone, AnOrdinaryTimeIsExact) {
    MachineZone::Resolution how{};
    zone("Australia/Sydney").toInstant(L(2026, 11, 19, 22), &how);
    EXPECT_EQ(how, MachineZone::Resolution::Exact);
}

TEST(MachineZone, SameAsThisComputerIsExactlyMktimeAndLocaltime) {
    const auto host = MachineZone::host();
    EXPECT_TRUE(host.isHost());
    EXPECT_EQ(host.setting(), "");
    for (long long wl : {L(2026, 1, 15, 22), L(2026, 3, 8, 2, 30), L(2026, 4, 5, 2, 30),
                         L(2026, 7, 4, 23, 59, 59), L(2026, 10, 4, 2, 30), L(2026, 11, 1, 1, 30)}) {
        std::tm t = wall::fieldsOf(wl);
        t.tm_isdst = -1;
        const long long expected = static_cast<long long>(std::mktime(&t));
        EXPECT_EQ(host.toInstant(wl), expected) << wall::text(wl);
        const std::time_t tt = static_cast<std::time_t>(expected);
        std::tm lt{};
        localtime_r(&tt, &lt);
        EXPECT_EQ(host.toWall(expected), static_cast<long long>(timegm_utc(&lt))) << wall::text(wl);
    }
}

TEST(MachineZone, SettingsParseToTheirCanonicalText) {
    EXPECT_EQ(zone("").setting(), "");
    EXPECT_EQ(zone("+10:00").setting(), "+10:00");
    EXPECT_EQ(zone("UTC+10").setting(), "+10:00");
    EXPECT_EQ(zone("-05:00").setting(), "-05:00");
    EXPECT_EQ(zone("+0545").setting(), "+05:45");
    EXPECT_EQ(zone("-12:00").setting(), "-12:00");
    EXPECT_EQ(zone("Australia/Sydney").setting(), "Australia/Sydney");
}

TEST(MachineZone, AnInvalidSettingIsRefusedWithAReason) {
    for (const char* bad : {"+10:07", "+14:15", "-12:15", "Not/AZone", "../etc/passwd", "+1:2:3"}) {
        std::string why;
        EXPECT_FALSE(MachineZone::parse(bad, &why).has_value()) << bad;
        EXPECT_FALSE(why.empty()) << bad;
    }
}

TEST(MachineZone, IsoTextCarriesTheMachinesOffsetAtThatInstant) {
    // D8: the issue's example, 22:00 on a fixed UTC+10 machine.
    EXPECT_EQ(zone("+10:00").isoText(L(2026, 1, 15, 12)), "2026-01-15T22:00:00+10:00");
    EXPECT_EQ(zone("Australia/Sydney").isoText(L(2026, 1, 15, 12)), "2026-01-15T23:00:00+11:00");
    EXPECT_EQ(zone("-05:30").isoText(L(2026, 1, 15, 12)), "2026-01-15T06:30:00-05:30");
}

TEST(MachineZone, TheZoneListHoldsTheCommonZones) {
    const auto names = zonedb::names();
    EXPECT_NE(std::find(names.begin(), names.end(), "Australia/Sydney"), names.end());
    EXPECT_NE(std::find(names.begin(), names.end(), "America/New_York"), names.end());
    EXPECT_TRUE(std::is_sorted(names.begin(), names.end()));
    EXPECT_NE(zonedb::source(), "none");
}

TEST(WallLabel, TextAndParseAreAPair) {
    const long long l = L(2026, 11, 19, 22, 5, 9);
    EXPECT_EQ(wall::text(l), "2026-11-19 22:05:09");
    EXPECT_EQ(wall::parseText("2026-11-19 22:05:09"), l);
    EXPECT_EQ(wall::parseText("2026-11-19T22:05:09.000Z"), l);
    EXPECT_EQ(wall::parseText("2026-11-19"), L(2026, 11, 19, 0));
    EXPECT_FALSE(wall::parseText("not a time").has_value());
}
