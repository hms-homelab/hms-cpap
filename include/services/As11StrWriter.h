#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace hms_cpap {

/**
 * STR.edf for an AirSense 11 read over Bluetooth, with no card behind it
 * (SDD-057 step 4 and its STR decision): the machine's Summary days laid into
 * the STR layout of its model, so <data_dir>/as11/<serial>/ is a card root
 * OSCAR can open.
 *
 * A layout is an STR header only (no records), cut from a card of that model
 * with the patient and recording fields blanked, kept as
 * <layout dir>/<ProductCode>.edfh. The recording field is written for this
 * machine (its serial, PlatformIdentifier as MID, VariantIdentifier as VID,
 * from Identification.json). Every Summary day is a day-off record (all -1)
 * with the Summary's figures written on it; a day between two Summary days
 * that the machine did not report stays a day-off record. The settings
 * history is not applied yet: settings fields stay -1.
 *
 * A product code with no layout writes no STR and says why.
 */
struct StrResult {
    bool written = false;
    int days = 0;            ///< records written
    std::string why;         ///< when not written
};

/// Where layouts are read from: HMS_CPAP_STR_LAYOUT_DIR when set, else
/// <data_dir>/str-layouts.
std::string strLayoutDir(const std::string& data_dir);

/// Keeps one pulled Summary spool as <root>/AS11/summary-<from>.as11.
bool keepSummary(const std::string& root, const std::string& from, const std::vector<uint8_t>& bytes);

/// Rebuilds <root>/STR.edf from every Summary kept under <root>/AS11/ (a
/// later pull wins a day both carry), the layout of the machine's product
/// code and its Identification.json.
StrResult writeStrFromSummaries(const std::string& root, const std::string& layout_dir);

/// The machine's offset east of UTC in minutes, from the kept Summaries (the
/// machine's noon as an instant); 0 and false when none tells.
bool machineOffsetFromSummaries(const std::string& root, int* offset_min);

} // namespace hms_cpap
