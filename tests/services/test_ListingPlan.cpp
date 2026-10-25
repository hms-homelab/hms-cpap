// SDD-054: which folders a burst lists, and why. Pure, so every decision is
// tested on fixed times: 2026-10-15 03:00 UTC on a machine whose clock reads
// 4 hours behind UTC (23:00 on the 14th), so tonight is 20261014.
#include <gtest/gtest.h>

#include "services/ListingPlan.h"

#include <algorithm>

using namespace hms_cpap;

namespace {

constexpr long long kT0 = 1792033200;      // 2026-10-15 03:00:00 UTC
constexpr long long kOffset = -4 * 3600;   // the machine's clock: UTC-4
constexpr long long kMin = 60;

bool listed(const std::vector<PlannedListing>& plan, const std::string& f) {
    return std::any_of(plan.begin(), plan.end(),
                       [&](const PlannedListing& p) { return p.folder == f; });
}

ListReason reasonOf(const std::vector<PlannedListing>& plan, const std::string& f) {
    for (const auto& p : plan)
        if (p.folder == f) return p.reason;
    return ListReason::Tonight;
}

FolderWatch settled(long long listed_at) {
    FolderWatch w;
    w.last_listed = listed_at;
    w.sig = "5|2000|1";
    return w;
}

}  // namespace

TEST(ListingPlanClock, TonightIsTheMachinesNoonToNoonDay) {
    EXPECT_EQ(tonightFolder(kT0, kOffset), "20261014");
    // 11:59 on the machine's 15th is still the 14th's night; 12:00 is the 15th's.
    const long long noon15 = kT0 + 13 * 3600;   // 16:00 UTC = 12:00 machine
    EXPECT_EQ(tonightFolder(noon15 - 60, kOffset), "20261014");
    EXPECT_EQ(tonightFolder(noon15, kOffset), "20261015");
}

TEST(ListingPlanClock, TheMachinesClockNotTheHostsDecidesTonight) {
    // The same host instant on a machine set to UTC+9 is already the 15th's noon.
    EXPECT_EQ(tonightFolder(kT0, 9 * 3600), "20261015");
}

TEST(ListingPlanClock, AFolderPastTheMachinesTomorrowIsAFutureFolder) {
    EXPECT_TRUE(isFutureFolder("20991231", kT0, kOffset));
    EXPECT_FALSE(isFutureFolder("20261015", kT0, kOffset));   // the machine's tomorrow
    EXPECT_FALSE(isRecentFolder("20991231", kT0, kOffset));
    EXPECT_TRUE(isRecentFolder("20261011", kT0, kOffset));    // today - 3
    EXPECT_FALSE(isRecentFolder("20261010", kT0, kOffset));
}

TEST(ListingPlanClock, TheLastMachineNoonIsOnTheMachinesClock) {
    // machine 23:00 on the 14th: its last noon was 12:00 on the 14th = 16:00 UTC.
    EXPECT_EQ(lastMachineNoon(kT0, kOffset), kT0 - 11 * 3600);
    // Exactly at noon it is that noon.
    EXPECT_EQ(lastMachineNoon(kT0 + 13 * 3600, kOffset), kT0 + 13 * 3600);
}

TEST(ListingPlan, TonightIsListedEveryBurstEvenBeforeTheIndexNamesIt) {
    const auto plan = planFolderListings(kT0, kOffset, {"20261012", "20261013"}, {}, {});
    ASSERT_FALSE(plan.empty());
    EXPECT_EQ(plan.front().folder, "20261014");
    EXPECT_EQ(plan.front().reason, ListReason::Tonight);
}

TEST(ListingPlan, ASettledFolderIsNotListed) {
    std::map<std::string, FolderWatch> watch{{"20261013", settled(kT0 - 10 * kMin)}};
    const auto plan = planFolderListings(kT0, kOffset, {"20261013"}, {}, watch);
    EXPECT_FALSE(listed(plan, "20261013"));
    EXPECT_EQ(plan.size(), 1u);   // tonight only
}

TEST(ListingPlan, EveryRecentOrCandidateFolderGetsOneFirstLookPerRun) {
    const auto plan = planFolderListings(kT0, kOffset,
                                         {"20250101", "20261005", "20261012", "20261013"},
                                         {"20261005"}, {});
    EXPECT_TRUE(listed(plan, "20261012"));      // recent
    EXPECT_TRUE(listed(plan, "20261013"));      // recent
    EXPECT_TRUE(listed(plan, "20261005"));      // a candidate (on or after the anchor)
    EXPECT_FALSE(listed(plan, "20250101"));     // old, settled history: never listed
    EXPECT_EQ(reasonOf(plan, "20261005"), ListReason::FirstLook);
}

TEST(ListingPlan, AFutureFolderIsListedOnceAndThenLeftAlone) {
    auto plan = planFolderListings(kT0, kOffset, {"20991231"}, {}, {});
    EXPECT_TRUE(listed(plan, "20991231"));
    std::map<std::string, FolderWatch> watch{{"20991231", settled(kT0 - 9 * 3600)}};
    watch["20991231"].relist = true;   // a fault mark does not reach it either
    plan = planFolderListings(kT0, kOffset, {"20991231"}, {}, watch);
    EXPECT_FALSE(listed(plan, "20991231"));
}

TEST(ListingPlan, AGrowingFolderIsListedEveryBurst) {
    std::map<std::string, FolderWatch> watch{{"20261013", settled(kT0 - kMin)}};
    watch["20261013"].growing = true;
    const auto plan = planFolderListings(kT0, kOffset, {"20261013"}, {}, watch);
    EXPECT_EQ(reasonOf(plan, "20261013"), ListReason::Live);
}

TEST(ListingPlan, AfterGrowthStopsTheCheckpointsAre5And35And65Minutes) {
    FolderWatch w = settled(kT0);
    w.stopped_at = kT0;
    std::map<std::string, FolderWatch> watch{{"20261013", w}};

    EXPECT_FALSE(listed(planFolderListings(kT0 + 4 * kMin, kOffset, {}, {}, watch), "20261013"));
    auto plan = planFolderListings(kT0 + 5 * kMin, kOffset, {}, {}, watch);
    EXPECT_EQ(reasonOf(plan, "20261013"), ListReason::Checkpoint);

    watch["20261013"].last_listed = kT0 + 5 * kMin;
    EXPECT_FALSE(listed(planFolderListings(kT0 + 34 * kMin, kOffset, {}, {}, watch), "20261013"));
    EXPECT_TRUE(listed(planFolderListings(kT0 + 35 * kMin, kOffset, {}, {}, watch), "20261013"));

    watch["20261013"].last_listed = kT0 + 35 * kMin;
    EXPECT_TRUE(listed(planFolderListings(kT0 + 65 * kMin, kOffset, {}, {}, watch), "20261013"));

    watch["20261013"].last_listed = kT0 + 65 * kMin;
    EXPECT_FALSE(listed(planFolderListings(kT0 + 3 * 3600, kOffset, {}, {}, watch), "20261013"));
}

TEST(ListingPlan, AFaultMarkListsTheFolderOnce) {
    std::map<std::string, FolderWatch> watch{{"20261012", settled(kT0 - kMin)}};
    watch["20261012"].relist = true;
    EXPECT_EQ(reasonOf(planFolderListings(kT0, kOffset, {}, {}, watch), "20261012"),
              ListReason::Relist);
    watch["20261012"] = observeFolder(watch["20261012"], "5|2000|1", false, kT0);
    EXPECT_FALSE(listed(planFolderListings(kT0 + kMin, kOffset, {}, {}, watch), "20261012"));
}

TEST(ListingPlan, NoRecentFolderGoesEightHoursUnlisted) {
    std::map<std::string, FolderWatch> watch{{"20261012", settled(kT0 - 8 * 3600)},
                                             {"20250101", settled(kT0 - 30 * 3600)}};
    const auto plan = planFolderListings(kT0, kOffset, {}, {}, watch);
    EXPECT_EQ(reasonOf(plan, "20261012"), ListReason::Ceiling);
    EXPECT_FALSE(listed(plan, "20250101"));   // not recent: no ceiling
}

TEST(ListingPlan, ADoneFolderIsNeverListedForAnyReason) {
    FolderWatch w = settled(kT0 - 9 * 3600);
    w.relist = true;
    w.growing = true;
    std::map<std::string, FolderWatch> watch{{"20261012", w}};
    const auto plan = planFolderListings(kT0, kOffset, {"20261012", "20250101"},
                                         {"20250101"}, watch, {"20261012", "20250101"});
    EXPECT_FALSE(listed(plan, "20261012"));
    EXPECT_FALSE(listed(plan, "20250101"));
    // Tonight is never done.
    EXPECT_TRUE(listed(planFolderListings(kT0, kOffset, {}, {}, {}, {"20261014"}), "20261014"));
}

TEST(ListingPlanObserve, ANewSignatureOrAStoredFileIsGrowth) {
    FolderWatch w = observeFolder({}, "1|10|5", false, kT0);
    EXPECT_FALSE(w.growing) << "a first look is a baseline, not growth";
    w = observeFolder(w, "2|20|6", false, kT0 + kMin);
    EXPECT_TRUE(w.growing);
    EXPECT_EQ(w.last_growth, kT0 + kMin);
    w = observeFolder(w, "2|20|6", false, kT0 + 2 * kMin);
    EXPECT_FALSE(w.growing);
    EXPECT_EQ(w.stopped_at, kT0 + 2 * kMin);
    w = observeFolder(w, "2|20|6", true, kT0 + 3 * kMin);
    EXPECT_TRUE(w.growing) << "a file stored from it is growth";
    EXPECT_EQ(w.stopped_at, 0);
}

TEST(ListingPlanIndex, DueUntilOneSucceedsThenOncePerMachineNoon) {
    IndexState s;
    EXPECT_TRUE(indexDue(s, kT0, kOffset, false));
    s.ok_this_run = true;
    s.last_index = kT0;
    EXPECT_FALSE(indexDue(s, kT0 + 3600, kOffset, false));
    const long long next_noon = kT0 + 13 * 3600;
    EXPECT_FALSE(indexDue(s, next_noon - 60, kOffset, false));
    EXPECT_TRUE(indexDue(s, next_noon, kOffset, false));
    EXPECT_FALSE(indexDue(s, next_noon, kOffset, true)) << "not while a night is written";
    s.reason = true;
    EXPECT_TRUE(indexDue(s, kT0 + 60, kOffset, true));
}

TEST(ListingPlanIndex, LocalIndexIsDueWhenTheFoldersOnDiskChange) {
    IndexState s;
    s.ok_this_run = true;
    s.last_index = kT0;
    const std::vector<std::string> known{"20261021", "20261022"};
    EXPECT_FALSE(indexDueLocal(s, kT0 + 3600, kOffset, false, known, known))
        << "same folders, same day: nothing to read";
    EXPECT_TRUE(indexDueLocal(s, kT0 + 3600, kOffset, false, known,
                              {"20261021", "20261022", "20261023"}))
        << "a night copied in after the index was read";
    EXPECT_TRUE(indexDueLocal(s, kT0 + 3600, kOffset, true, known, {"20261022"}))
        << "a folder removed, even while a night is written";
    EXPECT_TRUE(indexDueLocal(s, kT0 + 13 * 3600, kOffset, false, known, known))
        << "the daily cadence still applies";
}

TEST(ListingPlanSettings, FirstAndOnReasonsAndOnFloors) {
    SettingsState s;
    EXPECT_TRUE(settingsDue(s, kT0, false, false, false)) << "once per run, at start";
    s.last_listed = kT0;
    EXPECT_FALSE(settingsDue(s, kT0 + 14 * kMin, true, false, false));
    EXPECT_TRUE(settingsDue(s, kT0 + 15 * kMin, true, false, false));   // night floor
    EXPECT_FALSE(settingsDue(s, kT0 + 59 * kMin, false, false, false));
    EXPECT_TRUE(settingsDue(s, kT0 + 60 * kMin, false, false, false));  // idle floor
    EXPECT_TRUE(settingsDue(s, kT0 + kMin, true, true, false));          // therapy start
    EXPECT_TRUE(settingsDue(s, kT0 + kMin, false, false, true));         // a checkpoint
}

TEST(CardHealth, OkThenNotAnsweringThenTheHourAndTheTwelveHours) {
    EXPECT_EQ(cardHealth(true, kT0, kT0, kT0), "OK");
    EXPECT_EQ(cardHealth(false, kT0, kT0 + 59 * kMin, kT0), "Not answering");
    EXPECT_EQ(cardHealth(false, kT0, kT0 + 3600, kT0), "Not answering for 1 h");
    EXPECT_EQ(cardHealth(false, kT0, kT0 + 12 * 3600, kT0), "Not answering for 12 h");
    // Never answered since the start: counted from the start.
    EXPECT_EQ(cardHealth(false, 0, kT0 + 2 * 3600, kT0), "Not answering for 1 h");
}

TEST(ListingPlanSettings, AnAirSense11CardIsListedOnlyOnReasons) {
    SettingsState s;
    s.last_listed = kT0;
    s.logs_on_card = false;
    EXPECT_FALSE(settingsDue(s, kT0 + 5 * 3600, true, true, true));
    s.reason = true;
    EXPECT_TRUE(settingsDue(s, kT0 + kMin, false, false, false));
    EXPECT_FALSE(settingsListingHasLogs({"CurrentSettings.json", "CurrentSettings.crc"}));
    EXPECT_TRUE(settingsListingHasLogs({"CurrentSettings.json", "DLL.log"}));
}

TEST(ListingPlanSettings, ACrcWaitsWhenItsFileWasFetchedOnTheSameBurst) {
    EXPECT_EQ(fileStem("DLL.log"), "DLL");
    EXPECT_EQ(fileStem("20261016_235259_BRP.crc"), "20261016_235259_BRP");
    EXPECT_EQ(fileStem("Journal"), "Journal");
    EXPECT_TRUE(isCrcFile("AGL.crc"));
    EXPECT_TRUE(isCrcFile("Identification.CRC"));
    EXPECT_FALSE(isCrcFile("AGL.tgt"));
    EXPECT_FALSE(isCrcFile("crc"));

    const std::set<std::string> fetched = {"AGL", "20261016_235259_BRP"};
    EXPECT_TRUE(crcWaitsForNextBurst("AGL.crc", fetched));
    EXPECT_TRUE(crcWaitsForNextBurst("20261016_235259_BRP.crc", fetched));
    EXPECT_FALSE(crcWaitsForNextBurst("BGL.crc", fetched)) << "its file did not change";
    EXPECT_FALSE(crcWaitsForNextBurst("AGL.tgt", fetched)) << "not a checksum";
}
