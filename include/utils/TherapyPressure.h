#pragma once
//
// Issue #56: which pressure a night's row reports as the machine's.
//
// cpap_daily_summary carries two pressures. mask_press_* is measured at the
// mask (MaskPress); with EPR it drops by up to the EPR level on every breath
// out, so on a minimum of 7 with EPR 3 its median sits near 5. therapy_press_*
// is what the machine delivered (PLD Press), which never goes below the
// prescribed minimum. Anything that calls itself the machine's or the
// therapy's pressure reads the therapy column, and falls back to the mask's
// only on a night that has none (no PLD minutes, or a machine without the
// channel).
//
#include <json/json.h>

#include <optional>
#include <string>

namespace hms_cpap {

/// A numeric column of a query row, or nullopt when it is NULL, empty or not
/// a number. The database layers return numbers as strings on some engines.
inline std::optional<double> rowNumber(const Json::Value& row, const char* key) {
    const Json::Value v = row.get(key, Json::nullValue);
    if (v.isNull()) return std::nullopt;
    if (v.isDouble() || v.isInt() || v.isUInt()) return v.asDouble();
    if (v.isString()) {
        const std::string s = v.asString();
        if (s.empty()) return std::nullopt;
        try { return std::stod(s); } catch (...) {}
    }
    return std::nullopt;
}

/// The machine's pressure for one statistic of one night: therapy_press_<stat>
/// when it holds a reading (> 0), else mask_press_<stat>, else 0.
/// `stat` is "50", "95" or "max".
inline double machinePressure(const Json::Value& row, const std::string& stat) {
    const auto therapy = rowNumber(row, ("therapy_press_" + stat).c_str());
    if (therapy && *therapy > 0) return *therapy;
    return rowNumber(row, ("mask_press_" + stat).c_str()).value_or(0.0);
}

}  // namespace hms_cpap
