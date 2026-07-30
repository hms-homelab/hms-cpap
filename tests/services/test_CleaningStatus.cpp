// SDD-007: the pure cleaning-due computation.
//
// THESE VECTORS ARE COPIED VERBATIM from hms-cpapdash-api's
// tests/unit/test_CleaningStatus.cc, and that is the entire point of the file.
// Three implementations of computeCleaningStatus now exist: the cloud's C++,
// this one, and the Dart port in the phone app. Nothing but a shared set of
// cases stops them drifting into disagreeing about when a mask is dirty, and a
// disagreement would be invisible until a user saw two different answers on two
// screens. If a case changes upstream it must change here, and vice versa.
//
// The only edit made on the way in is the namespace.
#include <gtest/gtest.h>

#include "services/CleaningStatus.h"

using namespace hms_cpap;

namespace {
constexpr long long kDay = 24LL * 60 * 60;

// 2026-07-30 00:00:00 UTC, an exact day boundary so slot maths stays readable.
constexpr long long kJul30 = 1785369600LL;

constexpr int k0830 = 8 * 60 + 30;

long long at(long long day_epoch, int minutes) {
    return day_epoch + static_cast<long long>(minutes) * 60;
}
}  // namespace

// ── disabled short-circuits ────────────────────────────────────────────────

TEST(CleaningStatus, DisabledSwitchReportsDisabled) {
    auto s = computeCleaningStatus(kJul30, 7, k0830, 0, /*enabled=*/false,
                                   at(kJul30, 12 * 60));
    EXPECT_EQ(s.state, CleaningState::Disabled);
    EXPECT_EQ(s.next_due_epoch, 0);
    EXPECT_EQ(s.days_until, 0);
}

TEST(CleaningStatus, NonPositiveIntervalIsDisabled) {
    for (int interval : {0, -1, -7}) {
        auto s = computeCleaningStatus(kJul30, interval, k0830, 0, true,
                                       at(kJul30, 12 * 60));
        EXPECT_EQ(s.state, CleaningState::Disabled) << "interval " << interval;
        EXPECT_EQ(s.next_due_epoch, 0);
    }
}

// ── never done: anchors on the start date ──────────────────────────────────

TEST(CleaningStatus, NeverDoneStartsAtTheStartDateSlot) {
    // Starts today at 08:30; it is 06:00, so it has not come round yet.
    auto s = computeCleaningStatus(kJul30, 1, k0830, 0, true,
                                   at(kJul30, 6 * 60));
    EXPECT_EQ(s.state, CleaningState::Upcoming);
    EXPECT_EQ(s.next_due_epoch, at(kJul30, k0830));
    EXPECT_EQ(s.days_until, 0);
}

TEST(CleaningStatus, NeverDoneBecomesDueOnceTheSlotPasses) {
    // Same task at 09:00, half an hour after its slot.
    auto s = computeCleaningStatus(kJul30, 1, k0830, 0, true,
                                   at(kJul30, 9 * 60));
    EXPECT_EQ(s.state, CleaningState::Due);
    EXPECT_EQ(s.next_due_epoch, at(kJul30, k0830));
}

TEST(CleaningStatus, NeverDoneStaysDueRatherThanRollingForward) {
    // Started 100 days ago and never once done. This must read Due: the user
    // who never did it is precisely the one who needs telling. Rolling the slot
    // forward would show a comfortable "upcoming" instead.
    const long long start = kJul30 - 100 * kDay;
    auto s = computeCleaningStatus(start, 1, k0830, 0, true,
                                   at(kJul30, 12 * 60));
    EXPECT_EQ(s.state, CleaningState::Due);
    EXPECT_EQ(s.next_due_epoch, at(start, k0830));
    EXPECT_LT(s.days_until, 0);
}

TEST(CleaningStatus, FutureStartDateIsUpcoming) {
    auto s = computeCleaningStatus(kJul30 + 3 * kDay, 7, k0830, 0, true,
                                   at(kJul30, 12 * 60));
    EXPECT_EQ(s.state, CleaningState::Upcoming);
    EXPECT_EQ(s.next_due_epoch, at(kJul30 + 3 * kDay, k0830));
    // 3 days minus 3.5 hours truncates to 2, matching how SupplyStatus counts
    // days_left. A partial day never rounds up.
    EXPECT_EQ(s.days_until, 2);
}

// ── done before: anchors on completion ─────────────────────────────────────

TEST(CleaningStatus, DoneTodayPushesAWeeklyTaskOutAWeek) {
    const long long done = at(kJul30, 10 * 60);  // done at 10:00 today
    auto s = computeCleaningStatus(kJul30 - 30 * kDay, 7, k0830, done, true,
                                   at(kJul30, 11 * 60));
    EXPECT_EQ(s.state, CleaningState::Upcoming);
    EXPECT_EQ(s.next_due_epoch, at(kJul30 + 7 * kDay, k0830));
}

TEST(CleaningStatus, CompletionBeatsAnAncientStartDate) {
    // A start date years back must not drag a freshly-done task back to Due.
    const long long done = at(kJul30, 9 * 60);
    auto s = computeCleaningStatus(kJul30 - 900 * kDay, 30, k0830, done, true,
                                   at(kJul30, 20 * 60));
    EXPECT_EQ(s.state, CleaningState::Upcoming);
    EXPECT_EQ(s.days_until, 29);
}

TEST(CleaningStatus, DueAgainOneIntervalAfterCompletion) {
    const long long done = at(kJul30 - 7 * kDay, 10 * 60);
    // Exactly on the slot: due is inclusive.
    auto s = computeCleaningStatus(kJul30 - 30 * kDay, 7, k0830,
                                   done, true, at(kJul30, k0830));
    EXPECT_EQ(s.state, CleaningState::Due);
    EXPECT_EQ(s.next_due_epoch, at(kJul30, k0830));
    EXPECT_EQ(s.days_until, 0);
}

TEST(CleaningStatus, OneSecondBeforeTheSlotIsStillUpcoming) {
    const long long done = at(kJul30 - 7 * kDay, 10 * 60);
    auto s = computeCleaningStatus(kJul30 - 30 * kDay, 7, k0830,
                                   done, true, at(kJul30, k0830) - 1);
    EXPECT_EQ(s.state, CleaningState::Upcoming);
}

// ── the daily case, which is the whole point of the feature ────────────────

TEST(CleaningStatus, DailyTaskComesBackTheNextDay) {
    const long long done = at(kJul30, 21 * 60);  // wiped the mask at bedtime
    auto s = computeCleaningStatus(kJul30, 1, k0830, done, true,
                                   at(kJul30, 22 * 60));
    EXPECT_EQ(s.state, CleaningState::Upcoming);
    EXPECT_EQ(s.next_due_epoch, at(kJul30 + kDay, k0830));

    // ... and is due again the following morning.
    auto next = computeCleaningStatus(kJul30, 1, k0830, done, true,
                                      at(kJul30 + kDay, 9 * 60));
    EXPECT_EQ(next.state, CleaningState::Due);
}

// ── time-of-day is honoured, not rounded to midnight ───────────────────────

TEST(CleaningStatus, TimeOfDayShiftsTheSlot) {
    const int k2130 = 21 * 60 + 30;  // a deliberate bedtime cleaning slot
    auto morning = computeCleaningStatus(kJul30, 1, k2130, 0, true,
                                         at(kJul30, 9 * 60));
    EXPECT_EQ(morning.state, CleaningState::Upcoming)
        << "a 21:30 task must not read due at 09:00";
    EXPECT_EQ(morning.next_due_epoch, at(kJul30, k2130));

    auto evening = computeCleaningStatus(kJul30, 1, k2130, 0, true,
                                         at(kJul30, 22 * 60));
    EXPECT_EQ(evening.state, CleaningState::Due);
}

TEST(CleaningStatus, MidnightSlotIsSupported) {
    auto s = computeCleaningStatus(kJul30, 1, 0, 0, true, kJul30);
    EXPECT_EQ(s.state, CleaningState::Due);
    EXPECT_EQ(s.next_due_epoch, kJul30);
}

// ── state strings ──────────────────────────────────────────────────────────

TEST(CleaningStatus, StateStringsAreCanonical) {
    EXPECT_STREQ(cleaningStateString(CleaningState::Due), "due");
    EXPECT_STREQ(cleaningStateString(CleaningState::Upcoming), "upcoming");
    EXPECT_STREQ(cleaningStateString(CleaningState::Disabled), "disabled");
}
