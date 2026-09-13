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

/// What one pass over a card root did.
struct VldFolderScan {
    int imported = 0;
    int skipped  = 0;   ///< already stored, refused on an earlier pass, or on a removed night
    int refused  = 0;   ///< failed to parse on THIS pass
};

/// SDD-028 §2.2: every `*.vld` (any case) in [card_root] and in each folder
/// directly under it except DATALOG and SETTINGS (D1: whatever the other tool
/// names its folder). A name already stored is skipped, so a steady-state pass
/// is one listing and one lookup per file. A file that will not parse is added
/// to [refused] and never read again in this process, so a bad file cannot be
/// retried every burst. Filesystem errors are logged, never thrown. A file on
/// a night in [removed_nights] is skipped but NOT remembered, so a Reparse that
/// restores the night brings its ring data back on the next pass.
VldFolderScan importVldFolder(IDatabase& db, const std::string& card_root,
                              std::set<std::string>& refused,
                              const std::set<std::string>& removed_nights = {});

/// True for a filename ending in `.vld`, any case.
bool isVldFilename(const std::string& name);

}  // namespace hms_cpap
