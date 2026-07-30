#include "services/SyncFolderState.h"

#include <ctime>

namespace hms_cpap {

std::string strDayForSessionStart(const std::chrono::system_clock::time_point& start) {
    const auto shifted = start - std::chrono::hours(12);
    std::time_t t = std::chrono::system_clock::to_time_t(shifted);
    std::tm tm{};
#ifdef _WIN32
    localtime_s(&tm, &t);
#else
    localtime_r(&t, &tm);
#endif
    char buf[9];
    std::strftime(buf, sizeof(buf), "%Y%m%d", &tm);
    return std::string(buf);
}


FolderTransition advanceFolder(const FolderLedger& prev, const FolderObservation& obs) {
    FolderTransition out;
    out.next = prev;
    out.next.files_listed = true;

    const bool first_sighting =
        prev.last_file_count < 0 || prev.last_total_size < 0;
    const bool signature_changed =
        first_sighting ||
        obs.file_count != prev.last_file_count ||
        obs.total_size != prev.last_total_size;

    // Record what we saw either way, so the next burst compares against reality
    // rather than against the last signature we happened to approve of.
    out.next.last_file_count = obs.file_count;
    out.next.last_total_size = obs.total_size;

    if (signature_changed) {
        // Real growth (or the first time we have seen this folder at all). The
        // night is alive, so any previous settling is undone: a folder must be
        // able to flip back to in-progress on its own, which is half of what
        // ticket #25 was about.
        out.next.stable   = false;
        out.next.complete = false;
        // A changed signature is real progress, so the re-arm budget resets.
        out.next.resync_size  = -1;
        out.next.resync_count = 0;
        return out;
    }

    out.next.stable = true;
    // Done requires BOTH: stopped growing, and every listed file actually
    // stored. Reporting done on stability alone is what left a night sitting at
    // a fraction after all its files had in fact arrived.
    out.next.complete = obs.all_files_stored;

    // The close transition. Only on the edge, so a folder that stays settled
    // across many bursts arms its debt once rather than every cycle.
    const bool was_closed = prev.stable && prev.complete;
    if (out.next.complete && !was_closed) {
        out.closed = true;

        const bool same_signature = (prev.resync_size == obs.total_size);
        const int  arms_so_far    = same_signature ? prev.resync_count : 0;

        if (arms_so_far < kMaxResyncArms) {
            out.next.str_due      = true;
            out.next.sidecars_due = true;
            out.next.resync_size  = obs.total_size;
            out.next.resync_count = arms_so_far + 1;
            out.armed_debt        = true;
        } else {
            // Already re-armed the maximum number of times at this exact
            // signature and still coming back. Stop rather than loop: the
            // night is reported as it stands and a human can look at it.
            out.resync_exhausted = true;
        }
    }

    return out;
}

FolderLedger clearStrDebt(const FolderLedger& in) {
    FolderLedger out = in;
    out.str_due = false;
    return out;
}

FolderLedger clearSidecarDebt(const FolderLedger& in) {
    FolderLedger out = in;
    out.sidecars_due = false;
    return out;
}

NightState nightState(const FolderLedger& l) {
    // Still moving, or never settled: the night is live and nothing is wrong.
    if (!l.complete) return NightState::Live;
    // Settled, but the machine's own daily record never arrived. The transfer
    // finished as far as it is going to and the night is short of its STR.
    if (l.str_due) return NightState::Partial;
    return NightState::Complete;
}

const char* nightStateString(NightState s) {
    switch (s) {
        case NightState::Live:     return "live";
        case NightState::Partial:  return "partial";
        case NightState::Complete:
        default:                   return "complete";
    }
}

}  // namespace hms_cpap
