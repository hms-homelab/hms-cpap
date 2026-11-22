#pragma once
//
// The ring's night as the Home Assistant sensors state it.
//
// The oximetry summary the publisher reads has the night's ODI (desaturations
// of 3 % or more per hour of the ring session) and that session's length, not
// its desaturation count. The ODI is that count over the hours, so the count
// is read back from the two.
//
#include <cmath>
#include <cstdio>
#include <string>

namespace hms_cpap {

/// Desaturations in a session of `duration_s` seconds whose ODI is `odi`
/// per hour; 0 for a session with no length.
inline long desatCountOf(double odi, int duration_s) {
    if (duration_s <= 0 || odi <= 0) return 0;
    return std::lround(odi * duration_s / 3600.0);
}

/// The ODI as published: one decimal, "13.5".
inline std::string odiText(double odi) {
    char buf[32];
    std::snprintf(buf, sizeof buf, "%.1f", odi);
    return buf;
}

}  // namespace hms_cpap
