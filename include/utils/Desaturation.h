#pragma once

/**
 * SDD-001: a session's SpO2 desaturations at the threshold set in Settings.
 *
 * The parser finds them (cpapdash::parser::detectDesaturations) at its own
 * default drop, 3 % below the rolling baseline (the AASM rule), every time it
 * computes a session's metrics. The owner may choose another drop in Settings
 * (4 % is the CMS rule), so each engine's saveSession works them out again from
 * the session's own SpO2 at that drop, and the events, the count and the ODI it
 * stores all agree with it.
 *
 * The threshold reaches here as DESAT_DROP_PCT, exported from config.json's
 * desat_drop_pct at startup and again when Settings saves, the same way the
 * collector reads CPAP_ARCHIVE_DIR at use time.
 */

#include "parsers/CpapdashBridge.h"
#include "utils/ConfigManager.h"

#include <cpapdash/parser/DesatDetector.h>

#include <cstddef>
#include <cstdlib>
#include <optional>
#include <string>
#include <vector>

namespace hms_cpap {

/// The parser's own drop, used when nothing else is set.
inline double defaultDesatDropPct() { return cpapdash::parser::DesatParams{}.drop_pct; }

/// A drop Settings accepts, in percent below the baseline.
inline bool validDesatDropPct(double pct) { return pct >= 1.0 && pct <= 10.0; }

/// The drop in force: DESAT_DROP_PCT, or the parser's default when it is unset
/// or not a drop Settings accepts.
inline double desatDropPct() {
    const std::string v = ConfigManager::get("DESAT_DROP_PCT", "");
    if (v.empty()) return defaultDesatDropPct();
    char* end = nullptr;
    const double pct = std::strtod(v.c_str(), &end);
    if (end == v.c_str() || !validDesatDropPct(pct)) return defaultDesatDropPct();
    return pct;
}

/// What a save stores for a session's desaturations.
struct SessionDesaturations {
    std::vector<DesatEvent> events;
    std::optional<int> spo2_drops;
    std::optional<double> odi;
    /// True when they were worked out from this session's SpO2, so they replace
    /// the ones stored for it before (a night saved again as it grows, or a
    /// re-parse after the threshold changed). False for a session without SpO2,
    /// whose stored ones are left as they are.
    bool from_vitals = false;
};

/// The desaturations of [s] at [drop_pct]. The count and the ODI follow the
/// parser's rule (ParsedSession::calculateMetrics): desaturations per hour of
/// therapy, else per hour of 1 Hz SpO2 samples.
inline SessionDesaturations desaturationsAt(const CPAPSession& s, double drop_pct) {
    SessionDesaturations out;
    std::size_t spo2_samples = 0;
    for (const auto& v : s.vitals)
        if (v.spo2.has_value()) ++spo2_samples;

    if (spo2_samples == 0) {
        out.events = s.desaturations;
        if (s.metrics) {
            out.spo2_drops = s.metrics->spo2_drops;
            out.odi = s.metrics->odi;
        }
        return out;
    }

    cpapdash::parser::DesatParams params;
    params.drop_pct = drop_pct;
    out.events = cpapdash::parser::detectDesaturations(s.vitals, params);
    out.from_vitals = true;
    out.spo2_drops = static_cast<int>(out.events.size());

    double hours = s.metrics ? s.metrics->usage_hours.value_or(0.0) : 0.0;
    if (hours <= 0.0) hours = static_cast<double>(spo2_samples) / 3600.0;
    if (hours > 0.0) out.odi = static_cast<double>(out.events.size()) / hours;
    return out;
}

/// [m] with the count and ODI of [d], when [d] was worked out from SpO2.
inline SessionMetrics withDesaturations(SessionMetrics m, const SessionDesaturations& d) {
    if (d.from_vitals) {
        m.spo2_drops = d.spo2_drops;
        m.odi = d.odi;
    }
    return m;
}

}  // namespace hms_cpap
