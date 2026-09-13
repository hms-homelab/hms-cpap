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
    int         samples = 0;
    int         valid_samples = 0;
    double      avg_spo2 = 0;
    double      min_spo2 = 0;
    double      sample_interval = 0;
    int         duration_seconds = 0;
};

/// Parse [bytes] as a Wellue/Viatom .vld and store it under kOximetryDeviceId.
/// A file the parser refuses, or one with no samples, is refused by name and
/// nothing is written.
VldImportResult importVldFile(IDatabase& db, const std::string& bytes,
                              const std::string& filename);

/// What one pass over a card root did.
struct VldFolderScan {
    int imported = 0;
    int skipped  = 0;   ///< already stored, or refused on an earlier pass
    int refused  = 0;   ///< failed to parse on THIS pass
};

/// SDD-028 §2.2: every `*.vld` (any case) in [card_root] and in each folder
/// directly under it except DATALOG and SETTINGS (D1: whatever the other tool
/// names its folder). A name already stored is skipped, so a steady-state pass
/// is one listing and one lookup per file. A file that will not parse is added
/// to [refused] and never read again in this process, so a bad file cannot be
/// retried every burst. Filesystem errors are logged, never thrown.
VldFolderScan importVldFolder(IDatabase& db, const std::string& card_root,
                              std::set<std::string>& refused);

/// True for a filename ending in `.vld`, any case.
bool isVldFilename(const std::string& name);

}  // namespace hms_cpap
