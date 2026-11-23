#pragma once
//
// SDD-082 B10: the STR's therapy settings as the three engines store them.
//
// The parser reads the machine family (SDD-064's signal-set rule), the AutoSet
// limits (S.AS.MinPress / S.AS.MaxPress) and the bi-level prescription
// (S.VA.* / S.S.*) into every STRDailyRecord. cpap_daily_summary kept only
// mode, epr_level and pressure_setting, which on an AutoSet is the CPAP-mode
// pressure it does not use, so a settings table read from it named the wrong
// pressure. These are the columns beside them; one mapping here, so the
// engines cannot disagree on it.
//
#include "parsers/CpapdashBridge.h"

#include <optional>

namespace hms_cpap {

struct StrSettings {
    int family = 0;                     // 0 unknown 1 CPAP 2 AutoSet 3 bi-level 4 ASV
    std::optional<double> apap_min, apap_max;
    std::optional<double> bl_min_epap, bl_max_ipap, bl_ps, bl_ipap, bl_epap;
};

/// The family as the stored integer the reports read.
inline int strFamilyCode(STRDailyRecord::Family f) {
    switch (f) {
        case STRDailyRecord::Family::Cpap:    return 1;
        case STRDailyRecord::Family::AutoSet: return 2;
        case STRDailyRecord::Family::BiLevel: return 3;
        case STRDailyRecord::Family::Asv:     return 4;
        case STRDailyRecord::Family::Unknown: break;
    }
    return 0;
}

/// A pressure the STR prescribes, or nullopt: 0 is a setting the machine does
/// not have, and nothing outside (0, 30] cmH2O is one any machine delivers.
inline std::optional<double> strPressure(double v) {
    if (v > 0 && v <= 30) return v;
    return std::nullopt;
}
inline std::optional<double> strPressure(const std::optional<double>& v) {
    return v ? strPressure(*v) : std::nullopt;
}

inline StrSettings strSettingsOf(const STRDailyRecord& r) {
    StrSettings s;
    s.family      = strFamilyCode(r.family);
    s.apap_min    = strPressure(r.min_pressure);
    s.apap_max    = strPressure(r.max_pressure);
    s.bl_min_epap = strPressure(r.bl_min_epap);
    s.bl_max_ipap = strPressure(r.bl_max_ipap);
    s.bl_ps       = r.bl_ps && *r.bl_ps >= 0 && *r.bl_ps <= 30 ? r.bl_ps : std::nullopt;
    s.bl_ipap     = strPressure(r.bl_ipap);
    s.bl_epap     = strPressure(r.bl_epap);
    return s;
}

}  // namespace hms_cpap
