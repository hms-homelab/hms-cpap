#pragma once
//
// SDD-032 (ticket 127): the archive carries the real EDF record count.
//
// ResMed writes num-data-records as -1, or a count lower than the data, while a
// signal file records, and the true count only on finalize. The burst resumes
// BRP/PLD/SAD with Range requests, so the header it holds is the one it read
// mid-recording. The parser computes the count from the size and never
// notices; OSCAR trusts the field and imports a minute or two of the night.
//
// The archive is what OSCAR reads and what the SleepHQ export uploads, so its
// signal files are repaired in place: the 8 bytes at offset 236, the size
// unchanged, and only when the data divides evenly into records
// (cpapdash::parser::repairEdfDataRecords decides). EVE, CSL, STR and any
// non-ResMed file are never opened for writing.
//
#include <string>

namespace hms_cpap {

/// Repair one file's record count in place, when it is a ResMed signal file
/// (by name) whose header is stale. Reads only the header. Returns true when
/// it rewrote the field.
bool repairSignalEdfFile(const std::string& path);

struct SignalEdfRepair {
    int checked  = 0;   ///< signal files looked at
    int repaired = 0;   ///< of those, rewritten
};

/// Every signal file directly in [dir] (one night's folder).
SignalEdfRepair repairSignalEdfsIn(const std::string& dir);

/// Every signal file in each DATALOG/<YYYYMMDD>/ folder under [archive_root]:
/// the startup sweep (SDD-032 D2).
SignalEdfRepair sweepArchiveSignalEdfs(const std::string& archive_root);

}  // namespace hms_cpap
