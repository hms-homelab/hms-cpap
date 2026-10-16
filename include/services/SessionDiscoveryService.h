#pragma once

#include "clients/IDataSource.h"
#include "clients/EzShareClient.h"
#include "parsers/CpapdashBridge.h"
#include <vector>
#include <string>
#include <chrono>
#include <optional>
#include <map>
#include <set>
#include <filesystem>

namespace hms_cpap {

/**
 * SessionDiscoveryService - Discovers and groups CPAP sessions from ez Share
 *
 * Handles:
 * - Listing date folders on ez Share SD card
 * - Filtering by last stored session timestamp
 * - Grouping files into sessions, one per mask-on stretch (groupFiles)
 * - Detecting in-progress sessions
 *
 * Solves the multi-checkpoint problem: CPAP machines write interim BRP/PLD/SAD
 * files during a session. This service groups them and selects the LARGEST
 * (final) file for each type.
 */
class SessionDiscoveryService {
public:
    explicit SessionDiscoveryService(IDataSource& data_source);

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
    /// @param retain_from SDD-010: always re-check sessions starting at or after
    ///        this point, whatever the calendar says. Callers pass the start of
    ///        the 2nd most recent STORED session, so the two newest nights are
    ///        observed every burst and can always take the second observation
    ///        that settling requires. nullopt restores pure wall-clock
    ///        behaviour. This is additive: it only ever widens the set.
    /// @param catch_up_folders SDD-038: date folders to scan WHATEVER the
    ///        anchor says, and whose sessions bypass the new/today/recent
    ///        tests. This is how a night older than everything in the database
    ///        is ever looked at: the anchor is right for a card that grows
    ///        forward and blind to history that was already there (#34, and
    ///        support 129). Empty on a cycle with nothing to catch up, which is
    ///        every cycle on a healthy install.
    std::vector<SessionFileSet> discoverNewSessions(
        std::optional<std::chrono::system_clock::time_point> last_session_start,
        std::optional<std::chrono::system_clock::time_point> retain_from = std::nullopt,
        const std::set<std::string>& catch_up_folders = {}
    );

    /**
     * Group the files of a date folder on this source into sessions.
     *
     * @param date_folder e.g., "20260203"
     * @return one session file set per mask-on stretch (see groupFiles)
     */
    std::vector<SessionFileSet> groupSessionsInFolder(const std::string& date_folder);

    /**
     * Group files in a local directory into sessions (same session gap logic).
     *
     * Static method -- no EzShareClient needed. Used by --reparse and local source mode.
     */
    static std::vector<SessionFileSet> groupLocalFolder(
        const std::string& dir_path,
        const std::string& date_folder);

    /**
     * THE grouping: one date folder's listing into sessions. Every path that
     * turns a folder into sessions comes here, whatever read the listing (an
     * ez Share or a local folder), so a folder groups
     * the same way whichever way it arrived.
     *
     * The BRP/PLD/SAD checkpoints split into mask-on stretches wherever the
     * mask was off for SESSION_GAP_MINUTES, measured end to start. Every
     * stretch lists the folder's EVE/CSL pairs and carries the window of time
     * whose events are its own (SDD-047; see SessionFileSet).
     */
    static std::vector<SessionFileSet> groupFiles(
        const std::vector<EzShareFileEntry>& files,
        const std::string& date_folder);

    // discoverLocalSessions() lived here: a second copy of discoverNewSessions'
    // rules that read the filesystem directly. SDD-040 gave a local folder an
    // IDataSource (LocalDataSource) and pointed the one cycle at it, which left
    // this with no caller. Removed 2026-09-19; its tests moved onto
    // discoverNewSessions, which is what runs.

    /// SDD-038: the date folders on the card, for the caller to compare against
    /// what the database already holds. One listing, so the catch-up costs a
    /// single request on an ez Share rather than one per folder.
    std::vector<std::string> listDateFolders();

    /// SDD-054: discoverNewSessions on listings the caller already took. The
    /// collector decides which folders a burst lists (ListingPlan) and lists
    /// each once; the same rules then pick the sessions, with no second
    /// listing of the index or of any folder.
    std::vector<SessionFileSet> discoverInListings(
        const std::map<std::string, std::vector<EzShareFileEntry>>& listings,
        std::optional<std::chrono::system_clock::time_point> last_session_start,
        std::optional<std::chrono::system_clock::time_point> retain_from = std::nullopt,
        const std::set<std::string>& catch_up_folders = {});

    /// The folders discoverNewSessions would scan from the index: every folder
    /// on or after [anchor]'s local date, plus the day before it (early-morning
    /// sessions are stored there); every folder when there is no anchor.
    static std::vector<std::string> foldersFromAnchor(
        const std::vector<std::string>& date_folders,
        std::optional<std::chrono::system_clock::time_point> anchor);

private:
    IDataSource& data_source_;

    /// The per-session test discoverNewSessions applies to one folder's groups.
    static void keepWanted(const std::string& folder,
                           const std::vector<SessionFileSet>& folder_sessions,
                           std::optional<std::chrono::system_clock::time_point> last_session_start,
                           std::optional<std::chrono::system_clock::time_point> retain_from,
                           const std::set<std::string>& catch_up_folders,
                           std::vector<SessionFileSet>& out);
};

} // namespace hms_cpap
