#pragma once

#include <string>

namespace hms_cpap {

// Whether one stored oximetry sample is evidence that the night's movement was
// actually recorded.
//
// The ring's own recording (source "vld") carries a movement value for every
// second. The live poll (source "live") adds a few readings a day, some of them
// moving, to the same night. A ring file imported before the parser stored its
// movement correctly has only zeros, so the live readings alone must not make
// the movement chart look available: it would draw a lone blip beside a night
// that has no movement trace.
inline bool recordsMovement(const std::string& source, int motion) {
    return source != "live" && motion > 0;
}

} // namespace hms_cpap
