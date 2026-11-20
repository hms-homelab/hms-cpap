#include "database/NightWrite.h"

#include "utils/SessionEnd.h"

#include <iostream>

namespace hms_cpap {

IDatabase::NightReplaceResult writeNightSteps(
    IDatabase& db, const std::string& device_id, const std::string& date_folder,
    const std::vector<IDatabase::NightSessionWrite>& sessions) {
    IDatabase::NightReplaceResult r;
    r.deleted = db.deleteSessionsByDateFolder(device_id, date_folder);
    if (r.deleted < 0) {
        std::cerr << "DB: could not delete night " << date_folder << ", kept as it was" << std::endl;
        return {};
    }
    for (const auto& s : sessions) {
        if (!db.saveSession(s.session)) {
            std::cerr << "DB: could not save a session of night " << date_folder
                      << ", the night is kept as it was" << std::endl;
            return {};
        }
        // Re-imported sessions are complete: closed at the end the data
        // carries (SDD-037 D2).
        closeWithDataEnd(db, device_id, s.start, s.session);
        db.replaceSessionFiles(device_id, s.start, s.files);              // SDD-014
        db.updateCheckpointFileSizes(device_id, s.start, s.checkpoint_sizes);   // SDD-077 D3
        ++r.saved;
    }
    r.ok = true;
    return r;
}

// Not atomic: for a test double, which has no transaction to run it in. Every
// engine overrides this with one transaction.
IDatabase::NightReplaceResult IDatabase::replaceNight(
    const std::string& device_id, const std::string& date_folder,
    const std::vector<NightSessionWrite>& sessions) {
    return writeNightSteps(*this, device_id, date_folder, sessions);
}

}  // namespace hms_cpap
