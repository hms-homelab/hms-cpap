#include "services/CleaningStatus.h"

namespace hms_cpap {

namespace {
constexpr long long kSecondsPerDay = 24LL * 60 * 60;

// Floor the instant to its day boundary, then add the wall-clock minute the
// task is due at. Floor division (not truncation) keeps this correct for
// pre-1970 inputs, which only tests will ever produce.
long long slotOnDayOf(long long epoch, int time_minutes) {
    long long days = epoch / kSecondsPerDay;
    if (epoch < 0 && epoch % kSecondsPerDay != 0) --days;
    return days * kSecondsPerDay + static_cast<long long>(time_minutes) * 60;
}
}  // namespace

const char* cleaningStateString(CleaningState s) {
    switch (s) {
        case CleaningState::Due:      return "due";
        case CleaningState::Upcoming: return "upcoming";
        case CleaningState::Disabled:
        default:                      return "disabled";
    }
}

CleaningStatus computeCleaningStatus(long long start_date_epoch,
                                     int interval_days,
                                     int time_minutes,
                                     long long last_done_epoch,
                                     bool enabled,
                                     long long now_epoch) {
    if (!enabled || interval_days <= 0) return CleaningStatus{};

    const long long interval =
        static_cast<long long>(interval_days) * kSecondsPerDay;

    // Never done anchors on the start date; otherwise one interval past the day
    // the user last marked it done.
    const long long due =
        (last_done_epoch > 0)
            ? slotOnDayOf(last_done_epoch, time_minutes) + interval
            : slotOnDayOf(start_date_epoch, time_minutes);

    CleaningStatus out;
    out.next_due_epoch = due;
    // Truncating division matches SupplyStatus::days_left for both signs.
    out.days_until = static_cast<int>((due - now_epoch) / kSecondsPerDay);
    out.state = (due <= now_epoch) ? CleaningState::Due : CleaningState::Upcoming;
    return out;
}

}  // namespace hms_cpap
