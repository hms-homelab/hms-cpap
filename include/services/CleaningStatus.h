#pragma once
//
// CleaningStatus (SDD-007) — the wash half of equipment upkeep, kept
// deliberately separate from SupplyStatus: a mask is REPLACED every 90 days and
// WIPED every day, and folding those into one interval would break both.
//
// PORTED VERBATIM from hms-cpapdash-api's SDD-043 implementation, apart from the
// namespace. That is not laziness, it is the point: there are now THREE
// implementations of this function (the cloud's C++, this one, and the Dart port
// in the phone app), and the only thing stopping them drifting is that they are
// the same code tested against the same vectors. Any change here has to land in
// all three, and tests/services/test_CleaningStatus.cpp deliberately reuses the
// cloud suite's cases so a divergence fails a build instead of quietly
// disagreeing about when a mask is dirty.
//
// PURE: no DB, no I/O, no clock. `now` is a parameter so the tests are
// deterministic and so the caller owns the one conversion boundary.
//
namespace hms_cpap {

// Where a cleaning task stands right now. Computed on read, never stored.
enum class CleaningState { Due, Upcoming, Disabled };

struct CleaningStatus {
    CleaningState state{CleaningState::Disabled};
    long long     next_due_epoch{0};  // 0 when disabled
    int           days_until{0};      // negative once the slot is past
};

// Default wall-clock minute-of-day a task is due at (08:30).
inline constexpr int kCleaningDefaultTimeMinutes = 8 * 60 + 30;

// Pure due computation.
//
//   start_date_epoch: unix seconds of the task's start DATE
//   interval_days:    repeat period; <= 0 is treated as disabled
//   time_minutes:     local wall-clock minute-of-day the task is due at
//   last_done_epoch:  unix seconds the task was last marked done; <= 0 = never
//   enabled:          the user's per-task switch
//   now_epoch:        unix seconds "now"
//
// Anchoring:
//   - not enabled (or interval <= 0) -> Disabled, zeros.
//   - never done  -> due at start_date on time_minutes.
//   - done before -> due one interval after the DAY it was done, on
//     time_minutes. Marking done is what advances the clock, which is why
//     last_done anchors this rather than a raw multiple of the start date.
//
// Due when due_at <= now, and it STAYS Due until the task is marked done, the
// same way an overdue supply stays overdue. days_until goes negative meanwhile.
//
// This deliberately does NOT roll a missed slot forward. The cloud SDD corrected
// itself on exactly this point during implementation: rolling forward means a
// task the user has NEVER ONCE done could never read as Due, and that is
// precisely the user who most needs telling. Backlog is a scheduling concern,
// not a state concern.
CleaningStatus computeCleaningStatus(long long start_date_epoch,
                                     int interval_days,
                                     int time_minutes,
                                     long long last_done_epoch,
                                     bool enabled,
                                     long long now_epoch);

// Canonical lowercase state string for JSON ("due"|"upcoming"|"disabled").
const char* cleaningStateString(CleaningState s);

}  // namespace hms_cpap
