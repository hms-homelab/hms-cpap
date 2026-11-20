#pragma once

#include <cstdint>
#include <filesystem>
#include <functional>
#include <optional>
#include <string>

namespace hms_cpap::datamove {

/**
 * SDD-067: moving the data directory, for the Home Assistant add-on.
 *
 * The add-on's run.sh decides WHEN files have to move (the first move out of
 * /data, a change from one chosen directory to another, the catch-up after a
 * fallback run) and calls `hms_cpap --move-data <from> <to>`. The rules of the
 * move live here, where the unit tests can reach them:
 *
 *   - Everything under `from` moves to the same place under `to`, except a
 *     top-level `options.json` (the Supervisor writes it to /data and nowhere
 *     else) and the marker.
 *   - A file that already exists at the destination is never overwritten; the
 *     source copy stays where it is (D9).
 *   - `<home>/config.json` is copied, not moved, so a fallback run started
 *     from `home` keeps its settings and its Sleep Bridge port (D8).
 *   - The marker `<home>/.data_dir_moved` names `to`. It is written before
 *     the first file moves, so a move that is interrupted is a catch-up on the
 *     next start rather than a directory that "already has a config.json".
 *   - Within one filesystem a file is renamed. Across filesystems (/data to
 *     /share is one) it is copied to a temporary name, synced, renamed into
 *     place, and only then removed from the source.
 *
 * Progress is reported per event: each file finished and each whole percent of
 * the bytes crossed. Nothing here runs on a clock.
 */

/// The marker, inside `home`. One line: the directory this install moved to.
inline constexpr const char* kMarkerName = ".data_dir_moved";
/// Never moved: the Supervisor writes it to /data only.
inline constexpr const char* kOptionsName = "options.json";
/// Suffix of the temporary name a file is copied under before its rename.
inline constexpr const char* kTempSuffix = ".hms-moving";

/// The add-on's private storage: where the marker and the config copy live.
inline constexpr const char* kAddonHome = "/data";

struct Progress {
    std::uint64_t files_total = 0;
    std::uint64_t files_done = 0;   ///< moved or left (exists at destination)
    std::uint64_t bytes_total = 0;
    std::uint64_t bytes_done = 0;
    std::string current;            ///< path relative to `from`, the file in hand
    bool finished = false;
    bool ok = false;                ///< meaningful once finished
    std::string error;              ///< set when finished and not ok
    std::string from;
    std::string to;
};

/// Called on every progress event. Return false to stop the move where it is
/// (what an interruption looks like to the next run).
using ProgressFn = std::function<bool(const Progress&)>;

struct Options {
    std::filesystem::path from;
    std::filesystem::path to;
    /// Holds options.json, the marker and the config.json copy. /data in the add-on.
    std::filesystem::path home = kAddonHome;
    ProgressFn on_progress;

    /// Test seams. force_copy takes the cross-filesystem path even when a
    /// rename would do; fail_after_bytes > 0 makes the copy fail with "no
    /// space left" once that many bytes were written, as a full disk does.
    bool force_copy = false;
    std::uint64_t fail_after_bytes = 0;
};

struct Result {
    bool ok = false;
    std::uint64_t moved = 0;   ///< files now only at the destination
    std::uint64_t left = 0;    ///< files that exist at the destination, left at the source
    std::uint64_t bytes = 0;
    std::string error;
};

/// Run the move. Never throws; a failure is `ok == false` with `error` set,
/// and the files not yet moved stay where they were.
Result moveData(const Options& opts);

/// The marker's content in `home`, or "" when there is none.
std::string readMarker(const std::filesystem::path& home = kAddonHome);

/// One line for standard output (the add-on's Log tab).
std::string progressLine(const Progress& p);

/// The status endpoint's JSON body. `seq` counts progress events, so a page
/// can ask for the state after the one it already shows.
std::string progressJson(const Progress& p, std::uint64_t seq = 0);

/// Whole percent of the bytes, 100 for an empty move.
int percentOf(const Progress& p);

/// D14: this run is a fallback. `wanted` is the directory that could not be
/// written (HMS_CPAP_DATA_DIR_FALLBACK), `in_use` the data directory in use.
struct Fallback {
    std::string wanted;
    std::string in_use;
};

/// Reads HMS_CPAP_DATA_DIR_FALLBACK. Empty or unset: not a fallback run.
std::optional<Fallback> fallbackFromEnv(const std::string& in_use);

}  // namespace hms_cpap::datamove
