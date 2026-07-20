#include "services/SupplyStatus.h"

#include <algorithm>

namespace hms_cpap {

// Mirrors supply_defaults.dart. null (never tracked) is represented as -1.
int supplyDefaultDays(const std::string& slot) {
    if (slot == "machine")    return -1;   // not a consumable
    if (slot == "mask")       return 90;
    if (slot == "tubing")     return 90;
    if (slot == "filter")     return 30;
    if (slot == "humidifier") return 180;
    if (slot == "headgear")   return 180;
    return -1;                              // unknown slot -> untracked
}

const char* supplyStateString(SupplyState s) {
    switch (s) {
        case SupplyState::Fresh:    return "fresh";
        case SupplyState::DueSoon:  return "due_soon";
        case SupplyState::Overdue:  return "overdue";
        case SupplyState::Untracked:
        default:                    return "untracked";
    }
}

SupplyStatus computeSupplyStatus(const std::string& slot,
                                 long long started_epoch,
                                 int replace_after_days,
                                 long long now_epoch) {
    // Per-item override (>= 0) beats the slot default; a negative value means
    // "no override" (client NULL) -> fall back to the slot default.
    const int intervalDays =
        (replace_after_days >= 0) ? replace_after_days : supplyDefaultDays(slot);
    return computeSupplyStatusForInterval(started_epoch, intervalDays, now_epoch);
}

SupplyStatus computeSupplyStatusForInterval(long long started_epoch,
                                            int intervalDays,
                                            long long now_epoch) {
    // Machines, undated items, and non-consumables are untracked (client parity:
    // intervalDays == null/<=0 || startedUsingAt == null).
    if (intervalDays <= 0 || started_epoch <= 0) {
        return SupplyStatus{};  // Untracked, zeros
    }

    constexpr long long kSecondsPerDay = 24LL * 60 * 60;
    const long long replaceBy = started_epoch + static_cast<long long>(intervalDays) * kSecondsPerDay;
    const long long total     = static_cast<long long>(intervalDays) * kSecondsPerDay;
    const long long elapsed   = now_epoch - started_epoch;

    // clamp(elapsed / total, 0..1)
    double wear = static_cast<double>(elapsed) / static_cast<double>(total);
    wear = std::clamp(wear, 0.0, 1.0);

    // Dart's Duration.inDays truncates toward zero; integer division matches for
    // both signs.
    const int daysLeft = static_cast<int>((replaceBy - now_epoch) / kSecondsPerDay);

    SupplyState state;
    if (now_epoch > replaceBy) {
        state = SupplyState::Overdue;
    } else if (daysLeft < kSupplyDueSoonWindowDays) {
        state = SupplyState::DueSoon;
    } else {
        state = SupplyState::Fresh;
    }

    SupplyStatus out;
    out.state = state;
    out.replace_by_epoch = replaceBy;
    out.wear_fraction = wear;
    out.days_left = daysLeft;
    return out;
}

} // namespace hms_cpap
