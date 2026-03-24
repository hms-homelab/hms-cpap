#pragma once
#include <string>
#include <algorithm>

namespace hms_cpap {

/// Returns true if the filename represents an oximetry file (_SAD.edf or _SA2.edf).
/// SA2 is used by ASV and newer ResMed devices; SAD by older ones. Both contain
/// SpO2 + pulse oximetry data in the same EDF format.
inline bool isOximetryFile(const std::string& filename) {
    std::string lower = filename;
    std::transform(lower.begin(), lower.end(), lower.begin(), ::tolower);
    return lower.find("_sad.edf") != std::string::npos ||
           lower.find("_sa2.edf") != std::string::npos;
}

} // namespace hms_cpap
