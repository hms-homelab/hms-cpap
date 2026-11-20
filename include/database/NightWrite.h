#pragma once

#include "database/IDatabase.h"

#include <string>
#include <vector>

namespace hms_cpap {

/// The steps of IDatabase::replaceNight(), through [db]'s own calls: delete the
/// folder's sessions, then save, close, record the files and the checkpoint
/// sizes of each session. Stops at the first failed delete or save and
/// returns ok = false; the caller's transaction rolls the night back.
///
/// Each engine runs this inside the one transaction it opens for the night,
/// with its own calls joining that transaction instead of opening their own.
IDatabase::NightReplaceResult writeNightSteps(
    IDatabase& db, const std::string& device_id, const std::string& date_folder,
    const std::vector<IDatabase::NightSessionWrite>& sessions);

}  // namespace hms_cpap
