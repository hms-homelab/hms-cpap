//
// test_SyncFolderState.cpp: SDD-008.
//
// Table-driven and pure: no DB, no clock, no network. Every case here maps to
// something that actually went wrong in production upstream, so the comments
// name the incident rather than restating the assertion.
//
// The one property worth stating up front: there are NO grace windows. Nothing
// in this file advances state because time passed, only because something was
// observed. A test that needed to sleep would mean the design had drifted.
//
#include <gtest/gtest.h>

#include "services/SyncFolderState.h"

using namespace hms_cpap;

namespace {

FolderObservation obs(int files, long long size, bool stored = true) {
    FolderObservation o;
    o.file_count       = files;
    o.total_size       = size;
    o.all_files_stored = stored;
    return o;
}

/// A folder that has settled once already, which is the state most of the
/// interesting transitions start from.
FolderLedger settled(long long size = 1000, int files = 4) {
    FolderLedger l;
    l.date_folder     = "20260730";
    l.files_listed    = true;
    l.stable          = true;
    l.complete        = true;
    l.last_file_count = files;
    l.last_total_size = size;
    return l;
}

// ─────────────────────────────────────────────────────────────────────────────
// Growth and settling
// ─────────────────────────────────────────────────────────────────────────────

TEST(SyncFolderState, FirstSightingIsNeverImmediatelyStable) {
    // We have nothing to compare against yet, so calling it settled would close
    // a night the moment we first looked at it.
    FolderLedger fresh;
    fresh.date_folder = "20260730";

    const auto t = advanceFolder(fresh, obs(3, 500));
    EXPECT_FALSE(t.next.stable);
    EXPECT_FALSE(t.next.complete);
    EXPECT_FALSE(t.closed);
    EXPECT_TRUE(t.next.files_listed);
    // The signature is recorded, so the NEXT burst has something to compare to.
    EXPECT_EQ(t.next.last_file_count, 3);
    EXPECT_EQ(t.next.last_total_size, 500);
}

TEST(SyncFolderState, AnUnchangedSignatureSettlesTheFolder) {
    FolderLedger l;
    l.last_file_count = 3;
    l.last_total_size = 500;

    const auto t = advanceFolder(l, obs(3, 500));
    EXPECT_TRUE(t.next.stable);
    EXPECT_TRUE(t.next.complete);
    EXPECT_TRUE(t.closed) << "settling for the first time is the close edge";
}

TEST(SyncFolderState, GrowthReopensASettledFolder) {
    // A night that resumes (mask back on) must be able to flip back to
    // in-progress on its own. Half of ticket #25 was exactly this.
    const auto t = advanceFolder(settled(1000, 4), obs(5, 1400));
    EXPECT_FALSE(t.next.stable);
    EXPECT_FALSE(t.next.complete);
    EXPECT_FALSE(t.closed);
}

TEST(SyncFolderState, ShrinkingCountsAsAChangeToo) {
    // Files should only ever grow, so a smaller signature means something is
    // wrong with the card or the listing. Treating it as growth is the safe
    // reading: it reopens rather than declaring the night finished.
    const auto t = advanceFolder(settled(1000, 4), obs(2, 300));
    EXPECT_FALSE(t.next.stable);
}

TEST(SyncFolderState, StableButNotStoredIsNotComplete) {
    // Ticket #25: the folder stopped growing, but a file is still being
    // written. Reporting done here is what left a night stuck at a fraction
    // even once every file had actually arrived.
    FolderLedger l;
    l.last_file_count = 3;
    l.last_total_size = 500;

    const auto t = advanceFolder(l, obs(3, 500, /*stored=*/false));
    EXPECT_TRUE(t.next.stable)   << "it has genuinely stopped growing";
    EXPECT_FALSE(t.next.complete) << "but not every file is on disk yet";
    EXPECT_FALSE(t.closed);
    EXPECT_FALSE(t.next.str_due) << "debt must not arm before the folder closes";
}

TEST(SyncFolderState, StoringTheLastFileClosesTheFolder) {
    // The continuation of the case above: same signature, files now stored.
    FolderLedger l;
    l.last_file_count = 3;
    l.last_total_size = 500;
    l.stable = true;               // already settled on a previous burst
    l.complete = false;            // ...but was still storing

    const auto t = advanceFolder(l, obs(3, 500, /*stored=*/true));
    EXPECT_TRUE(t.next.complete);
    EXPECT_TRUE(t.closed) << "not-closed to closed is the edge that arms debt";
}

// ─────────────────────────────────────────────────────────────────────────────
// Debt
// ─────────────────────────────────────────────────────────────────────────────

TEST(SyncFolderState, ClosingArmsBothDebts) {
    FolderLedger l;
    l.last_file_count = 3;
    l.last_total_size = 500;

    const auto t = advanceFolder(l, obs(3, 500));
    EXPECT_TRUE(t.armed_debt);
    EXPECT_TRUE(t.next.str_due)      << "a close commands a fresh STR pull";
    EXPECT_TRUE(t.next.sidecars_due) << "and an EVE/CSL refetch";
}

TEST(SyncFolderState, StayingSettledDoesNotReArmEveryBurst) {
    // Debt is edge-triggered. Re-arming on every cycle would re-pull the same
    // files forever, which is the failure the resync cap exists to bound.
    auto l = advanceFolder(settled(), obs(4, 1000)).next;
    const auto again = advanceFolder(l, obs(4, 1000));
    EXPECT_FALSE(again.closed)     << "already closed; this is not a new edge";
    EXPECT_FALSE(again.armed_debt);
}

TEST(SyncFolderState, DebtSurvivesUntilExplicitlyCleared) {
    // Retry on recovery, never on a timer (ticket 39). Nothing but a successful
    // parse clears this.
    auto l = advanceFolder(settled(), obs(4, 1000)).next;
    // settled() is already closed, so no edge fires and nothing arms itself.
    // Arm BOTH by hand: the point of this test is that clearing one leaves the
    // other alone, which needs both to be set to mean anything.
    l.str_due      = true;
    l.sidecars_due = true;

    const auto next = advanceFolder(l, obs(4, 1000));
    EXPECT_TRUE(next.next.str_due) << "debt evaporated without anything clearing it";

    EXPECT_FALSE(clearStrDebt(next.next).str_due);
    EXPECT_TRUE(clearStrDebt(next.next).sidecars_due)
        << "clearing STR must not silently clear the sidecar debt too";
}

TEST(SyncFolderState, ReArmingIsCappedAtTheSameSignature) {
    // The 339-re-pulls regression. A night whose refetches keep coming back
    // different must not loop forever.
    FolderLedger l;
    l.last_file_count = 3;
    l.last_total_size = 500;

    int arms = 0;
    for (int i = 0; i < kMaxResyncArms + 3; ++i) {
        const auto t = advanceFolder(l, obs(3, 500));
        if (t.armed_debt) ++arms;
        l = t.next;
        // Model the refetch cycle: the debt is worked off and the folder
        // reopens, then settles again at the SAME signature.
        l = clearStrDebt(clearSidecarDebt(l));
        l.stable = false;
        l.complete = false;
    }
    EXPECT_EQ(arms, kMaxResyncArms)
        << "re-arming was not bounded; this is how one folder got re-pulled 339 times";
}

TEST(SyncFolderState, ExhaustionIsReportedRatherThanSilent) {
    FolderLedger l;
    l.last_file_count = 3;
    l.last_total_size = 500;
    l.resync_size  = 500;
    l.resync_count = kMaxResyncArms;   // budget already spent at this signature

    const auto t = advanceFolder(l, obs(3, 500));
    EXPECT_TRUE(t.closed);
    EXPECT_FALSE(t.armed_debt);
    EXPECT_TRUE(t.resync_exhausted)
        << "giving up quietly leaves nobody able to tell why a night stopped retrying";
}

TEST(SyncFolderState, RealProgressResetsTheReArmBudget) {
    // A different signature means the refetch actually achieved something, so
    // the folder deserves a fresh budget rather than staying locked out.
    FolderLedger l;
    l.last_file_count = 3;
    l.last_total_size = 500;
    l.resync_size  = 500;
    l.resync_count = kMaxResyncArms;

    // Growth first...
    auto grown = advanceFolder(l, obs(4, 900)).next;
    EXPECT_EQ(grown.resync_count, 0) << "the budget did not reset on real growth";

    // ...then it settles at the new signature and may arm again.
    const auto t = advanceFolder(grown, obs(4, 900));
    EXPECT_TRUE(t.armed_debt);
}

// ─────────────────────────────────────────────────────────────────────────────
// What the night reports
// ─────────────────────────────────────────────────────────────────────────────

TEST(SyncFolderState, AnIncompleteFolderReadsLive) {
    FolderLedger l;
    l.complete = false;
    EXPECT_EQ(nightState(l), NightState::Live);
}

TEST(SyncFolderState, CompleteWithOutstandingStrDebtReadsPartial) {
    // The whole point: the transfer settled and the machine's own daily record
    // never arrived.
    FolderLedger l;
    l.complete = true;
    l.str_due  = true;
    EXPECT_EQ(nightState(l), NightState::Partial);
}

TEST(SyncFolderState, CompleteWithTheDebtClearedReadsComplete) {
    FolderLedger l;
    l.complete = true;
    l.str_due  = false;
    EXPECT_EQ(nightState(l), NightState::Complete);
}

TEST(SyncFolderState, AnIncompleteFolderIsLiveEvenWithDebtOutstanding) {
    // Debt on a folder still receiving files is not a partial night, it is a
    // night in progress. Calling it partial would flag a healthy transfer.
    FolderLedger l;
    l.complete = false;
    l.str_due  = true;
    EXPECT_EQ(nightState(l), NightState::Live);
}

TEST(SyncFolderState, SidecarDebtAloneDoesNotMakeANightPartial) {
    // Missing event annotations are a data-completeness problem, not a stuck
    // night. Only the absent STR means the night never finished arriving.
    FolderLedger l;
    l.complete      = true;
    l.str_due       = false;
    l.sidecars_due  = true;
    EXPECT_EQ(nightState(l), NightState::Complete);
}

TEST(SyncFolderState, StateStringsAreCanonical) {
    EXPECT_STREQ(nightStateString(NightState::Live), "live");
    EXPECT_STREQ(nightStateString(NightState::Partial), "partial");
    EXPECT_STREQ(nightStateString(NightState::Complete), "complete");
}

// ─────────────────────────────────────────────────────────────────────────────
// Which STR day a night is waiting for
//
// This is the bug that nearly shipped: matching a folder name against an STR
// date directly. A DATALOG folder is named for the local calendar date the
// session STARTED; ResMed keys a therapy day from noon. For any night that
// crosses midnight, which is most of them, the two differ by one, so the naive
// match would leave every such night permanently partial.
// ─────────────────────────────────────────────────────────────────────────────

namespace {
/// Build a local-time instant, so these tests read as wall clock in whatever
/// zone they run in rather than assuming UTC.
std::chrono::system_clock::time_point localTime(int y, int mo, int d, int h, int mi) {
    std::tm tm{};
    tm.tm_year = y - 1900;
    tm.tm_mon  = mo - 1;
    tm.tm_mday = d;
    tm.tm_hour = h;
    tm.tm_min  = mi;
    tm.tm_isdst = -1;   // let the library decide; the rule must survive DST
    return std::chrono::system_clock::from_time_t(std::mktime(&tm));
}
}  // namespace

TEST(SyncFolderState, APostMidnightSessionBelongsToThePreviousTherapyDay) {
    // Mask on at 02:10 on the 30th. The card files it under folder 20260730,
    // but ResMed's therapy day for it is the 29th. THIS is the case the naive
    // string match got wrong.
    EXPECT_EQ(strDayForSessionStart(localTime(2026, 7, 30, 2, 10)), "20260729");
}

TEST(SyncFolderState, AnEveningSessionBelongsToTheSameTherapyDay) {
    // Mask on at 22:00 on the 30th: folder and therapy day agree here, which is
    // exactly why a broken mapping can look fine in a spot check.
    EXPECT_EQ(strDayForSessionStart(localTime(2026, 7, 30, 22, 0)), "20260730");
}

TEST(SyncFolderState, NoonIsTheBoundary) {
    // The split is noon, not midnight. One minute either side must land on
    // different therapy days.
    EXPECT_EQ(strDayForSessionStart(localTime(2026, 7, 30, 11, 59)), "20260729");
    EXPECT_EQ(strDayForSessionStart(localTime(2026, 7, 30, 12, 0)),  "20260730");
}

TEST(SyncFolderState, TheRuleCrossesMonthAndYearEnds) {
    // Naive arithmetic on the date string breaks here; going through a real
    // calendar does not.
    EXPECT_EQ(strDayForSessionStart(localTime(2026, 8, 1, 3, 0)),  "20260731");
    EXPECT_EQ(strDayForSessionStart(localTime(2027, 1, 1, 1, 0)),  "20261231");
    // Leap day: 2028 is a leap year, so March 1st rolls back to February 29th.
    EXPECT_EQ(strDayForSessionStart(localTime(2028, 3, 1, 4, 0)),  "20280229");
}

TEST(SyncFolderState, TheFolderNameAndTheTherapyDayGenuinelyDisagree) {
    // Stated as its own assertion because it is the whole reason str_day is
    // stored on the ledger instead of being re-derived from date_folder.
    const auto start = localTime(2026, 7, 30, 2, 10);
    const std::string folder = "20260730";   // what the card names it
    EXPECT_NE(strDayForSessionStart(start), folder)
        << "if these ever agree for a post-midnight session, the ledger could "
           "match on date_folder and str_day would be redundant";
}

// ─────────────────────────────────────────────────────────────────────────────
// The property that keeps the design honest
// ─────────────────────────────────────────────────────────────────────────────

TEST(SyncFolderState, NothingAdvancesWithoutAnObservation) {
    // An unreachable card produces no observation, so state must simply not
    // move. This is the anti-timer test: if someone later adds an age-based
    // rule, the only way to satisfy it is to pass time, and there is nowhere
    // here to pass it.
    const auto before = settled();
    const auto after  = advanceFolder(before, obs(4, 1000));

    // Same observation, same signature: nothing changed except the settled flags
    // it already had.
    EXPECT_EQ(after.next.last_total_size, before.last_total_size);
    EXPECT_EQ(after.next.last_file_count, before.last_file_count);
    EXPECT_EQ(after.next.str_due, before.str_due);
}

}  // namespace
