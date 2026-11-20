#pragma once
//
// SDD-074 D4/D6: a session parsed from real instants (a DreamStation's chunk
// seconds, read as an epoch) put on the machine's clock as labels, the frame
// every other session is held in. Each time point goes through the zone on
// its own, so with no setting (this computer's zone) the stored text is what
// localtime gave before.
//
#include "parsers/CpapdashBridge.h"
#include "utils/MachineClocks.h"
#include "utils/MachineZone.h"
#include "utils/OximetryDevice.h"

#include <map>

namespace hms_cpap {

inline void putOnMachineClock(CPAPSession& s, const MachineZone& zone) {
    auto label = [&](std::chrono::system_clock::time_point tp) {
        const auto whole = std::chrono::time_point_cast<std::chrono::seconds>(tp);
        const auto frac = tp - whole;
        return wall::point(zone.toWall(wall::seconds(whole))) + frac;
    };
    if (s.session_start) s.session_start = label(*s.session_start);
    if (s.session_end) s.session_end = label(*s.session_end);
    for (auto& p : s.mask_on_periods) p = {label(p.first), label(p.second)};
    std::map<std::chrono::system_clock::time_point, int> spans;
    for (const auto& [at, n] : s.brp_spans) spans[label(at)] = n;
    s.brp_spans = std::move(spans);
    for (auto& e : s.events) e.timestamp = label(e.timestamp);
    for (auto& e : s.computed_events) e.timestamp = label(e.timestamp);
    for (auto& v : s.vitals) v.timestamp = label(v.timestamp);
    for (auto& b : s.breathing_summary) b.timestamp = label(b.timestamp);
    for (auto& d : s.desaturations) d.onset = label(d.onset);
    for (auto& b : s.breaths) b.onset = label(b.onset);
}

/// SDD-074 (open question 8, decided in): the O2 ring's file carries the
/// ring's own wall clock, and a night's ring and CPAP line up by wall clock.
/// When the "Ring clock" and the CPAP's "Machine clock" differ, the ring's
/// times are moved onto the CPAP's clock: ring label -> instant on the ring's
/// clock -> label on the machine's. Both unset (or equal): untouched.
inline void putRingOnMachineClock(OximetrySession& s, const MachineZone& ring,
                                  const MachineZone& machine) {
    if (ring == machine) return;
    auto move = [&](std::chrono::system_clock::time_point tp) {
        const auto whole = std::chrono::time_point_cast<std::chrono::seconds>(tp);
        const auto frac = tp - whole;
        return wall::point(machine.toWall(ring.toInstant(wall::seconds(whole)))) + frac;
    };
    s.start_time = move(s.start_time);
    s.end_time = move(s.end_time);
    for (auto& x : s.samples) x.timestamp = move(x.timestamp);
}

/// The same with the stored settings: the ring's, and the CPAP's this
/// hms-cpap stores its nights under.
inline void putRingOnMachineClock(OximetrySession& s) {
    putRingOnMachineClock(s, machine_clocks::zoneFor(kOximetryDeviceId),
                          machine_clocks::zoneFor(machine_clocks::primaryDevice()));
}

}  // namespace hms_cpap
