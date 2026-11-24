#pragma once
//
// The ring's night as the Home Assistant sensors state it.
//
// The night's ODI is desaturations of 3 % or more per hour of VALID ring
// samples. The desaturation count is published as stored, never read back
// from the ODI and the session's length (finger-off time would inflate it).
//
#include <cstdio>
#include <string>

namespace hms_cpap {

/// The ODI as published: one decimal, "13.5".
inline std::string odiText(double odi) {
    char buf[32];
    std::snprintf(buf, sizeof buf, "%.1f", odi);
    return buf;
}

}  // namespace hms_cpap
