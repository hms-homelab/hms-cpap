#pragma once
//
// SDD-077 D3: one checkpoint for every writer.
//
// cpap_sessions.checkpoint_files is written by the collector, by the SD/ZIP
// backfill and by the reparse, and read by the collector to decide whether a
// stored session changed. The backfill used to write KB while the collector
// wrote exact bytes, so every imported session read as changed on the next
// burst, was downloaded again and reopened. Every writer calls
// checkpointSizes() now, and the collector compares through
// compareCheckpoints(), so the shape written is the shape read.
//
#include "parsers/CpapdashBridge.h"
#include "utils/FileUtils.h"

#include <algorithm>
#include <cstdint>
#include <map>
#include <string>

namespace hms_cpap {

/// The files a checkpoint is made of: BRP, PLD and the oximetry files (SAD,
/// SA2). EVE and CSL are under a kilobyte and are judged by their stamps.
inline bool isCheckpointFile(const std::string& filename) {
    return filename.find("_BRP.edf") != std::string::npos ||
           filename.find("_PLD.edf") != std::string::npos || isOximetryFile(filename);
}

/// The sizes a night is judged "still growing" by, as the source lists them:
/// exact bytes from a source that lists them (a FlashAir, a local folder, the
/// backfill's own folder), KB from an ez Share, which lists nothing finer. In
/// KB alone a file still being written can grow between two bursts and read
/// as unchanged. An int holds a card file's bytes: residualSkip caps them at
/// 20 MB.
inline std::map<std::string, int> checkpointSizes(const SessionFileSet& session) {
    std::map<std::string, int> sizes;
    for (const auto& [filename, size_kb] : session.file_sizes_kb) {
        if (!isCheckpointFile(filename)) continue;
        const auto exact = session.file_sizes_bytes.find(filename);
        sizes[filename] = exact != session.file_sizes_bytes.end()
                              ? static_cast<int>(std::min<uint64_t>(exact->second, 0x7fffffff))
                              : size_kb;
    }
    return sizes;
}

/// Is [kb] what [bytes] reads as in kilobytes, rounded down or up? Sources
/// differ (a folder and a FlashAir round down, an ez Share's own listing may
/// not), and either is the same file.
inline bool isKbOf(long long bytes, long long kb) {
    return kb == bytes / 1024 || kb == (bytes + 1023) / 1024;
}

/// What a stored checkpoint says about the session as the source lists it now.
struct CheckpointCompare {
    bool all_unchanged = true;
    bool has_new_files = false;
    /// A stored size matched only as the KB of the file's exact bytes: a row
    /// written in the old KB shape. Unchanged, once; the caller rewrites the
    /// row in bytes so the next compare is exact.
    bool legacy_kb = false;
};

inline CheckpointCompare compareCheckpoints(const std::map<std::string, int>& stored,
                                            const SessionFileSet& session) {
    CheckpointCompare c;
    const auto current = checkpointSizes(session);
    for (const auto& [filename, was] : stored) {
        const auto it = current.find(filename);
        if (it == current.end()) {
            c.all_unchanged = false;
            break;
        }
        if (it->second == was) continue;
        const bool exact = session.file_sizes_bytes.count(filename) > 0;
        if (exact && isKbOf(it->second, was)) {
            c.legacy_kb = true;
            continue;
        }
        // A row in bytes, read through a source that lists only KB: that
        // source cannot see finer than a KB, so the KB it lists is the test.
        if (!exact && isKbOf(was, it->second)) continue;
        c.all_unchanged = false;
        break;
    }
    if (current.size() > stored.size()) {
        c.has_new_files = true;
        c.all_unchanged = false;
    }
    return c;
}

}  // namespace hms_cpap
