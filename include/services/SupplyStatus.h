#pragma once
//
// SupplyStatus — port of cpapdash-app's supply_status.dart +
// supply_defaults.dart (hms-cpap SDD-004, mirroring hms-cpapdash-api SDD-035). PURE: no DB, no I/O, so it is unit
// testable in isolation and produces the SAME due dates / states the app shows.
//
// Keep the constants here identical to the client's supply_defaults.dart: a
// mismatch would make the web/observer/push views disagree with the app.
//
#include <string>

namespace hms_cpap {

// Wear state of one equipment item. Computed, never stored.
enum class SupplyState { Fresh, DueSoon, Overdue, Untracked };

// Default replacement interval per slot, in days. -1 = not a consumable, never
// tracked (matches supply_defaults.dart: machine null; mask/tubing 90; filter
// 30; humidifier/headgear 180). Unknown slot strings return -1 (untracked).
int supplyDefaultDays(const std::string& slot);

// "Replace soon" window: within this many days of the replace-by date.
inline constexpr int kSupplyDueSoonWindowDays = 14;

struct SupplyStatus {
    SupplyState state{SupplyState::Untracked};
    long long   replace_by_epoch{0};  // startedUsingAt + interval; 0 when untracked
    double      wear_fraction{0.0};    // 0.0 fresh -> 1.0 at replace-by (clamped)
    int         days_left{0};          // days until replace_by; negative when overdue
};

// Pure wear computation, mirroring supplyStatus(item, now) in the client.
//
// The effective interval is resolved by the caller (per-item override, else the
// type's default from equipment_types, else supplyDefaultDays) the way
// EquipmentController::intervalFor and SupplyPublisher do, so this needs no
// slot lookup and works for custom types like "battery" whose default lives
// only in the DB.
//   started_epoch: unix seconds of started_using_at; <= 0 == untracked (no date)
//   interval_days: the resolved replacement interval; <= 0 == untracked
//   now_epoch:     unix seconds "now"
SupplyStatus computeSupplyStatusForInterval(long long started_epoch,
                                            int interval_days,
                                            long long now_epoch);

// Canonical lowercase state string for JSON ("fresh"|"due_soon"|"overdue"|"untracked").
const char* supplyStateString(SupplyState s);

} // namespace hms_cpap
