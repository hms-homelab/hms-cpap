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
 * with the patient and recording fields blanked: the ones built into the
 * binary (StrLayouts.h), or <layout dir>/<ProductCode>.edfh. The recording
 * field is written for this machine (its serial, PlatformIdentifier as MID,
 * VariantIdentifier as VID, from Identification.json). Every Summary day is a
 * day-off record (all -1) with the Summary's figures written on it; a day
 * between two Summary days that the machine did not report stays a day-off
 * record. The settings history is not applied yet: settings fields stay -1.
 *
 * The STR is APPENDED, never rewritten (the STR decision of 2026-10-31). A
 * folder that already holds an STR.edf, from a card copy or from earlier
 * nights, keeps every record it has, byte for byte:
 *
 *   - a Summary day after the file's last day is appended as a new record
 *     (a day the machine did not report in between gets a day-off record,
 *     so the record index stays the date);
 *   - the file's last day is rewritten in place when the Summary carries it
 *     (the machine revises the day it is on), on the file's own record of
 *     that day as the base, so what the card wrote and the Summary does not
 *     carry stays;
 *   - a day before the last is left as the file has it, and counted;
 *   - the records go first, then the header's record count: a reader that
 *     sees the old count sees a whole file.
 *
 * Nothing is appended into a file that names another machine (SRN=), or
 * whose signals differ from the layout of this product code: that is said,
 * and the file is left alone. With no STR.edf, one is created from the
 * layout; a product code with no layout writes none and says why.
 */
struct StrResult {
    bool written = false;    ///< the file changed
    bool created = false;    ///< a new STR.edf (else appended into one)
    int days = 0;            ///< records written this time: created, appended and replaced
    int appended = 0;        ///< records added at the end (gap days included)
    int replaced = 0;        ///< the last record rewritten in place (0 or 1)
    int skipped = 0;         ///< Summary days before the file's last day, left as they are
    int total = 0;           ///< records the file holds now
    std::string why;         ///< when not written
};

/// Where an owner's own layouts are read from: HMS_CPAP_STR_LAYOUT_DIR when
/// set, else <data_dir>/str-layouts. The built-in ones need no directory.
std::string strLayoutDir(const std::string& data_dir);

/// Keeps one pulled Summary spool as <root>/AS11/summary-<from>.as11.
bool keepSummary(const std::string& root, const std::string& from, const std::vector<uint8_t>& bytes);

/// Brings <root>/STR.edf up to date with every Summary kept under <root>/AS11/
/// (a later pull wins a day both carry), from the layout of the machine's
/// product code and its Identification.json, by the rules above.
StrResult writeStrFromSummaries(const std::string& root, const std::string& layout_dir);

/// The machine's offset east of UTC in minutes, from the kept Summaries (the
/// machine's noon as an instant); 0 and false when none tells.
bool machineOffsetFromSummaries(const std::string& root, int* offset_min);

/// An STR.edf's header as this writer reads it, for whoever needs to look at
/// one (tests, the status page).
struct StrHeaderInfo {
    bool ok = false;
    size_t header_bytes = 0;
    long records = 0;
    int64_t first_day = 0;              ///< days since 1970-01-01 of the first record
    size_t record_bytes = 0;
    std::vector<std::string> labels;
    std::vector<int> samples_per_record;
    std::string recording;              ///< the 80-byte recording field, trimmed
};
StrHeaderInfo readStrHeader(const std::vector<uint8_t>& file);

} // namespace hms_cpap
