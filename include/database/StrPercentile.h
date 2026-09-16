#pragma once
//
// SDD-033 D1: the STR's copy of a percentile, as the three engines store it.
//
// The STR marks "none" in-band: no oximeter reads SpO2 0 (the card's -1
// scaled), and a day still recording carries negative sentinels (AHI -0.10,
// Leak.95 -0.02 on a real AirCurve 11 card). Stored as a value, such a copy
// would beat our own number on a multi-session night, so it is stored as NULL.
//
#include <optional>

namespace hms_cpap {

/// [v] as the STR's copy of a percentile column, or nullopt when the STR has
/// none. A leak of exactly 0 is a real reading ([zero_is_a_value]); a mask
/// pressure or an SpO2 of 0 is not.
inline std::optional<double> strPercentile(double v, bool zero_is_a_value) {
    if (v > 0 || (zero_is_a_value && v == 0)) return v;
    return std::nullopt;
}

}  // namespace hms_cpap
