//
// test_SupplyStatus.cpp — SDD-004 wear computation.
//
// This module is a PORT, twice over: hms-cpapdash-api/src/services/SupplyStatus.cc
// is itself a port of the phone app's supply_status.dart. All three must produce
// the SAME state and days_left for the same input, or a user sees "due in 3 days"
// on their phone, "overdue" on the web, and something else again on the local
// dashboard. These fixtures are therefore deliberately shared with the cloud
// suite (tests/unit/test_EquipmentController.cc there) — if you change one,
// change all three.
//
#include <gtest/gtest.h>
#include "services/SupplyStatus.h"

using namespace hms_cpap;

namespace {

constexpr long long kNow = 1'700'000'000;   // fixed "now" so tests never drift
constexpr long long kDay = 24LL * 60 * 60;

long long daysAgo(int d) { return kNow - static_cast<long long>(d) * kDay; }

// ── Slot defaults: must equal supply_defaults.dart exactly ──────────────────

TEST(SupplyDefaults, MatchTheAppsConstants) {
    EXPECT_EQ(supplyDefaultDays("machine"),    -1) << "machine is not a consumable";
    EXPECT_EQ(supplyDefaultDays("mask"),       90);
    EXPECT_EQ(supplyDefaultDays("tubing"),     90);
    EXPECT_EQ(supplyDefaultDays("filter"),     30);
    EXPECT_EQ(supplyDefaultDays("humidifier"), 180);
    EXPECT_EQ(supplyDefaultDays("headgear"),   180);
    EXPECT_EQ(supplyDefaultDays("battery"),    -1) << "unknown slots are untracked";
}

TEST(SupplyDefaults, DueSoonWindowIsFourteenDays) {
    EXPECT_EQ(kSupplyDueSoonWindowDays, 14);
}

// ── State boundaries ────────────────────────────────────────────────────────
//
// These drive computeSupplyStatusForInterval, the one function production
// calls, and resolve the interval through supplyDefaultDays() exactly the way
// EquipmentController::intervalFor and SupplyPublisher's defaultFor do. That
// keeps the slot-default table exercised here while the boundary arithmetic is
// asserted against the live function rather than through a wrapper nothing
// shipped ever called.

TEST(SupplyStatusPure, FreshWellBeforeReplace) {
    auto s = computeSupplyStatusForInterval(daysAgo(10), supplyDefaultDays("mask"), kNow);
    EXPECT_EQ(s.state, SupplyState::Fresh);
    EXPECT_EQ(s.days_left, 80);
}

TEST(SupplyStatusPure, DueSoonInsideTheWindow) {
    // 10 left < 14
    auto s = computeSupplyStatusForInterval(daysAgo(80), supplyDefaultDays("mask"), kNow);
    EXPECT_EQ(s.state, SupplyState::DueSoon);
    EXPECT_EQ(s.days_left, 10);
}

TEST(SupplyStatusPure, ExactlyAtTheWindowEdgeIsStillFresh) {
    // 14 days left is NOT "< 14", so it must remain Fresh. Off-by-one here would
    // shift every reminder a day and desync from the app.
    auto s = computeSupplyStatusForInterval(daysAgo(76), supplyDefaultDays("mask"), kNow);
    EXPECT_EQ(s.days_left, 14);
    EXPECT_EQ(s.state, SupplyState::Fresh);
}

TEST(SupplyStatusPure, OverduePastReplace) {
    auto s = computeSupplyStatusForInterval(daysAgo(100), supplyDefaultDays("mask"), kNow);
    EXPECT_EQ(s.state, SupplyState::Overdue);
    EXPECT_LT(s.days_left, 0);
    EXPECT_DOUBLE_EQ(s.wear_fraction, 1.0) << "wear clamps at 1.0";
}

TEST(SupplyStatusPure, MachineIsAlwaysUntracked) {
    // supplyDefaultDays("machine") is -1, and a non-positive interval is untracked.
    auto s = computeSupplyStatusForInterval(daysAgo(999), supplyDefaultDays("machine"), kNow);
    EXPECT_EQ(s.state, SupplyState::Untracked);
}

TEST(SupplyStatusPure, UndatedIsUntracked) {
    auto s = computeSupplyStatusForInterval(0, supplyDefaultDays("mask"), kNow);
    EXPECT_EQ(s.state, SupplyState::Untracked);
}

TEST(SupplyStatusPure, PerItemOverrideBeatsTheDefault) {
    // Filter defaults to 30d; the item's own 200d override makes a 40-day-old
    // filter fresh. Which value wins is resolved by intervalFor() and pinned in
    // test_EquipmentController; what this asserts is that the override, once
    // resolved, is the interval the arithmetic uses.
    ASSERT_EQ(supplyDefaultDays("filter"), 30);
    auto s = computeSupplyStatusForInterval(daysAgo(40), 200, kNow);
    EXPECT_EQ(s.state, SupplyState::Fresh);
    EXPECT_EQ(s.days_left, 160);
}

TEST(SupplyStatusPure, WearAtHalfLife) {
    // 45/90
    auto s = computeSupplyStatusForInterval(daysAgo(45), supplyDefaultDays("mask"), kNow);
    EXPECT_NEAR(s.wear_fraction, 0.5, 0.01);
}

TEST(SupplyStatusPure, WearNeverGoesNegativeForAFutureStart) {
    // A user can fat-finger a start date in the future; wear must clamp at 0
    // rather than reporting a negative fraction to the UI.
    auto s = computeSupplyStatusForInterval(kNow + 5 * kDay, supplyDefaultDays("mask"), kNow);
    EXPECT_GE(s.wear_fraction, 0.0);
    EXPECT_LE(s.wear_fraction, 1.0);
}

TEST(SupplyStatusPure, ReplaceByIsStartPlusInterval) {
    auto s = computeSupplyStatusForInterval(daysAgo(10), supplyDefaultDays("mask"), kNow);
    EXPECT_EQ(s.replace_by_epoch, daysAgo(10) + 90 * kDay);
}

TEST(SupplyStatusPure, UntrackedReportsNoReplaceBy) {
    auto s = computeSupplyStatusForInterval(daysAgo(10), supplyDefaultDays("machine"), kNow);
    EXPECT_EQ(s.replace_by_epoch, 0) << "untracked must not fabricate a due date";
}

// ── Intervals the slot table cannot supply ──────────────────────────────────
// hms-cpap resolves the default from cpap_equipment_types first (so custom
// types like "battery" work) and falls back to supplyDefaultDays.

TEST(SupplyStatusInterval, SupportsCustomTypesTheSlotTableCannotKnow) {
    // "battery" has no entry in supplyDefaultDays; its 365d default lives in the
    // DB catalog. This is exactly why the interval variant exists.
    auto s = computeSupplyStatusForInterval(daysAgo(300), 365, kNow);
    EXPECT_EQ(s.state, SupplyState::Fresh);
    EXPECT_EQ(s.days_left, 65);
}

TEST(SupplyStatusInterval, NonPositiveIntervalIsUntracked) {
    EXPECT_EQ(computeSupplyStatusForInterval(daysAgo(10), 0,  kNow).state,
              SupplyState::Untracked);
    EXPECT_EQ(computeSupplyStatusForInterval(daysAgo(10), -1, kNow).state,
              SupplyState::Untracked);
}

TEST(SupplyStatusInterval, UnsetStartIsUntracked) {
    EXPECT_EQ(computeSupplyStatusForInterval(0, 90, kNow).state,
              SupplyState::Untracked);
}

// ── JSON-facing strings (the API and MQTT payloads use these verbatim) ──────

TEST(SupplyStateString, MatchesTheWireFormat) {
    EXPECT_STREQ(supplyStateString(SupplyState::Fresh),     "fresh");
    EXPECT_STREQ(supplyStateString(SupplyState::DueSoon),   "due_soon");
    EXPECT_STREQ(supplyStateString(SupplyState::Overdue),   "overdue");
    EXPECT_STREQ(supplyStateString(SupplyState::Untracked), "untracked");
}

} // namespace
