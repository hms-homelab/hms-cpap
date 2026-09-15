#pragma once
//
// SDD-028: the ring's .vld files, from anywhere but the ring itself.
//
// The live path (OximetryService) pulls a .vld off the ring and stores it; the
// same file can also sit in a folder beside the card's DATALOG, written there
// by another tool (#32, todd3835), or arrive on the O2 upload. All three store
// the SAME row for the same file: parsed by the shared VLDParser, saved under
// kOximetryDeviceId, known by its filename (UNIQUE, ON CONFLICT DO UPDATE).
//
#include <cstdint>
#include <map>
#include <set>
#include <string>

namespace hms_cpap {

class IDatabase;

/// One .vld imported, or why not.
struct VldImportResult {
    bool        ok = false;
    std::string error;          ///< set when !ok
    bool        removed_night = false;  ///< SDD-029: parsed, on a removed night, not stored
    int         samples = 0;
    int         valid_samples = 0;
    double      avg_spo2 = 0;
    double      min_spo2 = 0;
    double      sample_interval = 0;
    int         duration_seconds = 0;
};

/// Parse [bytes] as a Wellue/Viatom .vld and store it under kOximetryDeviceId.
/// A file the parser refuses, or one with no samples, is refused by name and
/// nothing is written. A file whose night is in [removed_nights] (SDD-029,
/// YYYYMMDD keys) is parsed but not stored; the upload passes none, since an
/// operator uploading a night is asking for it.
VldImportResult importVldFile(IDatabase& db, const std::string& bytes,
                              const std::string& filename,
                              const std::set<std::string>& removed_nights = {});

/// A file as the listing shows it: what decides whether it changed (the card
/// files' own rule: size, and the modified time for a change inside the same
/// size).
struct VldFileSig {
    std::uintmax_t size  = 0;
    long long      mtime = 0;   ///< filesystem clock ticks
    bool operator==(const VldFileSig& o) const { return size == o.size && mtime == o.mtime; }
    bool operator!=(const VldFileSig& o) const { return !(*this == o); }
};

/// What the scan remembers between passes, per card-folder path. In memory:
/// after a restart a file already stored is trusted as it is (§2.4 of the
/// SDD-028 amendment).
struct VldScanState {
    std::map<std::string, VldFileSig> stored;    ///< as the file was when stored (or first seen stored)
    std::map<std::string, VldFileSig> refused;   ///< as the file was when it would not parse
    std::string last_summary;                    ///< the summary last logged
};

/// What one pass over a card root did.
struct VldFolderScan {
    int found      = 0;   ///< .vld files the pass saw
    int imported   = 0;   ///< new nights stored
    int reimported = 0;   ///< stored before, changed since, stored again
    int skipped    = 0;   ///< unchanged and stored, unchanged and refused, or on a removed night
    int refused    = 0;   ///< failed to parse on THIS pass
    std::string summary;          ///< one line: where it looked, what it saw, what it did
    bool summary_logged = false;  ///< the summary differed from the last pass's and was logged
};

/// SDD-028 §2.2: every `*.vld` (any case) in [card_root] and in each folder
/// directly under it except DATALOG and SETTINGS (D1: whatever the other tool
/// names its folder). Amended 2026-09-14 (#32, 5.2.7):
/// - a stored file is read again when its size or modified time changes, so a
///   file the scan caught while the other tool was still writing it is
///   completed, not left short;
/// - a file that will not parse is retried when it changes, not only after a
///   restart;
/// - the pass logs one line saying where it looked and what it saw, whenever
///   that differs from the last pass, including when it found nothing.
/// A steady-state pass is one listing and one lookup per file. Filesystem
/// errors are reported in the summary, never thrown. A file on a night in
/// [removed_nights] is skipped but NOT remembered, so a Reparse that restores
/// the night brings its ring data back on the next pass.
VldFolderScan importVldFolder(IDatabase& db, const std::string& card_root,
                              VldScanState& state,
                              const std::set<std::string>& removed_nights = {});

/// True for a filename ending in `.vld`, any case.
bool isVldFilename(const std::string& name);

}  // namespace hms_cpap
