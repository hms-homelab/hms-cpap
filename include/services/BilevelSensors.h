#pragma once
//
// SDD-030 (#33): what a bi-level machine publishes that an AirSense does not.
//
// On a ResMed the PLD's Press channel is the delivered pressure and EprPress
// the expiratory set-point. On an AirCurve those two ARE IPAP and EPAP (the
// reporter's night: Press - EprPress equals the prescribed pressure support to
// the hundredth). Whether a machine is bi-level is not in the session files; it
// is the family the parser reads off the STR's signal set, so every rule here
// takes the family and does nothing without it.
//
// Pure functions, tested without a broker, the way indexSensorsFor is.
//
#include "parsers/CpapdashBridge.h"

#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace hms_cpap {

using MachineFamily = STRDailyRecord::Family;
using NamedValues   = std::vector<std::pair<std::string, double>>;

/// The per-night sensors: `ipap`, `epap`, `pressure_support`. Empty unless the
/// machine is bi-level and the night has both channels; one without the other
/// would publish a pressure support that is not one.
inline NamedValues bilevelNightSensors(MachineFamily family, const SessionMetrics& m) {
    if (family != MachineFamily::BiLevel) return {};
    if (!m.avg_therapy_pressure || !m.avg_epr_pressure) return {};
    const double ipap = *m.avg_therapy_pressure;
    const double epap = *m.avg_epr_pressure;
    return {{"ipap", ipap}, {"epap", epap}, {"pressure_support", ipap - epap}};
}

/// The STR day's prescribed bi-level settings and daily targets. A VAuto
/// writes `S.VA.*` (a ceiling, a floor and the support); a fixed bi-level
/// writes `S.S.IPAP`/`S.S.EPAP`, whose difference is its support. Only what the
/// card wrote is returned.
inline NamedValues bilevelStrSensors(const STRDailyRecord& r) {
    NamedValues out;
    if (r.family != MachineFamily::BiLevel) return out;
    const auto ipap = r.bl_max_ipap ? r.bl_max_ipap : r.bl_ipap;
    const auto epap = r.bl_min_epap ? r.bl_min_epap : r.bl_epap;
    auto support = r.bl_ps;
    if (!support && r.bl_ipap && r.bl_epap) support = *r.bl_ipap - *r.bl_epap;
    if (ipap)           out.emplace_back("str_max_ipap", *ipap);
    if (epap)           out.emplace_back("str_min_epap", *epap);
    if (support)        out.emplace_back("str_pressure_support", *support);
    if (r.tgt_ipap_95)  out.emplace_back("str_tgt_ipap_95", *r.tgt_ipap_95);
    if (r.tgt_epap_95)  out.emplace_back("str_tgt_epap_95", *r.tgt_epap_95);
    return out;
}

/// A mode number, read through the family. On a bi-level VAuto is 8 on an
/// AirCurve 11 and 6 on an AirCurve 10 (two real cards, 2026-09-15: both with
/// only VAuto settings in their STR), where 8 on an ASV is ASV with variable
/// EPAP. A bi-level number no card has shown yet is named by its number
/// rather than guessed.
inline std::string therapyModeName(int mode, MachineFamily family) {
    if (family == MachineFamily::BiLevel)
        return (mode == 6 || mode == 8) ? "VAuto"
                                        : "Bi-level (mode " + std::to_string(mode) + ")";
    switch (mode) {
        case 0: return "CPAP";
        case 1: return "APAP";
        case 7: return "ASV (Fixed EPAP)";
        case 8: return "ASV (Variable EPAP)";
        default: return "Unknown";
    }
}

/// The standing rule applied to the mode: the session's value first, the STR's
/// when the session has none or 0. No parser fills a ResMed session's mode and
/// every engine stores "none" as 0, so on a ResMed this is the STR's mode (8 on
/// the AirCurve 11 VAuto, 6 on the AirCurve 10 VAuto, where 0 was published).
inline std::optional<int> therapyModeFor(std::optional<int> session_mode,
                                         std::optional<int> str_mode) {
    if (session_mode && *session_mode != 0) return session_mode;
    return str_mode ? str_mode : session_mode;
}

/// SDD-019's rule applied to the STR's SpO2 median: a card without an oximeter
/// writes 0 or -1 there, and neither is a saturation anyone had.
inline bool strSpo2Present(double spo2_50) { return spo2_50 > 0; }

}  // namespace hms_cpap
