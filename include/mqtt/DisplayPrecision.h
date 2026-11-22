#pragma once
//
// How many decimals Home Assistant should show for one of our sensors.
//
// The MQTT payloads carry whatever digits the database holds (a mean pressure
// of 9.193142, a leak of 3.897860), and without a hint Home Assistant prints
// them all, in the entity row, the gauge and the glance alike. Discovery can
// carry `suggested_display_precision`; Home Assistant then rounds the display
// everywhere (the stored state is untouched, and a user's own choice of
// precision in the entity's settings still wins).
//
// Chosen by unit, then, for the unitless numbers, by what they are:
//
//   cmH2O, L/min, %, breaths/min, min   1      9.2 cmH2O, 3.9 L/min, 94.0 %
//   SpO2 (%)                            0      94 %
//   events/h                            2      0.13 events/h
//   mL, bpm, d                          0      518 mL, 81 bpm
//   snore, flow limitation, I:E ratio   2      0.07
//   event and apnea counts, index       0      a count has no fraction
//
// Nothing for a time (device_class duration or timestamp: Home Assistant
// converts those itself) or for a sensor that is text.
//
#include <optional>
#include <string>

namespace hms_cpap {

inline std::optional<int> displayPrecision(const std::string& name, const std::string& unit,
                                           const std::string& device_class) {
    if (device_class == "duration" || device_class == "timestamp") return std::nullopt;

    // SpO2 is a whole number on every oximeter, so a decimal would be noise.
    if (unit == "%" && name.find("spo2") != std::string::npos) return 0;
    if (unit == "cmH2O" || unit == "L/min" || unit == "%" || unit == "breaths/min" || unit == "min")
        return 1;
    if (unit == "events/h") return 2;
    if (unit == "mL" || unit == "bpm" || unit == "d") return 0;
    if (!unit.empty()) return std::nullopt;   // a unit we have no view on: leave it be

    auto has = [&name](const char* part) { return name.find(part) != std::string::npos; };
    if (has("snore") || has("flow_limitation") || has("ie_ratio")) return 2;
    if (has("apneas") || has("hypopneas") || has("reras") || has("total_events") ||
        has("mask_events") || name == "sleep_index" || name == "str_sleep_index" || name == "motion")
        return 0;
    return std::nullopt;   // text (a summary, a band, a status) or a mode
}

}  // namespace hms_cpap
