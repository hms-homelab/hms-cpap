#pragma once

#include "clients/EzShareClient.h"
#include "models/CPAPModels.h"
#include <vector>
#include <string>
#include <chrono>
#include <optional>

namespace hms_cpap {

/**
 * SessionDiscoveryService - Discovers and groups CPAP sessions from ez Share
 *
 * Handles:
 * - Listing date folders on ez Share SD card
 * - Filtering by last stored session timestamp
 * - Grouping files into sessions (latest BRP/PLD/SAD per session prefix)
 * - Detecting in-progress sessions
 *
 * Solves the multi-checkpoint problem: CPAP machines write interim BRP/PLD/SAD
 * files during a session. This service groups them and selects the LARGEST
 * (final) file for each type.
 */
class SessionDiscoveryService {
public:
    explicit SessionDiscoveryService(EzShareClient& ezshare_client);

    /**
     * Discover new sessions since last stored timestamp
     *
     * Algorithm:
     * 1. List all date folders on ez Share
     * 2. Filter folders >= last session date (or all if nullopt)
     * 3. For each folder: group files into sessions
     * 4. Filter sessions newer than last_session_start
     *
     * @param last_session_start Last stored session (nullopt = get all)
     * @return Vector of session file sets to download
     */
    std::vector<SessionFileSet> discoverNewSessions(
        std::optional<std::chrono::system_clock::time_point> last_session_start
    );

    /**
     * Group files in a date folder into sessions
     *
     * Groups by session prefix (YYYYMMDD_HHMMSS from filename).
     * For multiple BRP/PLD/SAD files with same prefix, keeps largest.
     *
     * @param date_folder e.g., "20260203"
     * @return Vector of session file sets (grouped by CSL timestamp)
     */
    std::vector<SessionFileSet> groupSessionsInFolder(const std::string& date_folder);

private:
    EzShareClient& ezshare_client_;

    /**
     * Extract session prefix from filename (YYYYMMDD_HHMMSS)
     *
     * Example: "20260204_001809_CSL.edf" → "20260204_001809"
     *
     * @param filename Filename from ez Share listing
     * @return Session prefix or empty string if invalid format
     */
    std::string extractSessionPrefix(const std::string& filename);

    /**
     * Parse session start time from filename prefix
     *
     * Example: "20260204_001809" → 2026-02-04 00:18:09
     *
     * @param prefix Session prefix (YYYYMMDD_HHMMSS)
     * @return Time point for session start
     */
    std::chrono::system_clock::time_point parseSessionTime(const std::string& prefix);

    /**
     * Find largest file of a given type in a file list
     *
     * @param files Files from ez Share listing
     * @param prefix Session prefix to match
     * @param suffix File type suffix (e.g., "_BRP.edf")
     * @return Filename of largest matching file, or empty if none found
     */
    std::string findLargestFile(const std::vector<EzShareFileEntry>& files,
                                 const std::string& prefix,
                                 const std::string& suffix);
};

} // namespace hms_cpap
