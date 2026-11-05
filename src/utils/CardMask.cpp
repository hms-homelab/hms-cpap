#include "utils/CardMask.h"

#include <algorithm>

namespace hms_cpap {

std::optional<CardMask::Change> CardMask::grew(const std::string& session, long long start_s,
                                               long long end_s, long long now_s) {
    // A file that could not be read still grew now; how old its night is, its
    // start says (a session runs a night at most).
    const long long last = end_s > 0 ? end_s : now_s;
    if (last < now_s - kRecentS) return std::nullopt;
    if (end_s <= 0 && start_s < now_s - 12 * 3600) return std::nullopt;

    // Stems are YYYYMMDD_HHMMSS: they sort in time.
    if (!session_.empty() && session < session_) return std::nullopt;   // an older session, re-read

    if (session == session_) {
        const long long was_end = end_s_;
        end_s_   = std::max(end_s_, last);
        grew_at_ = now_s;
        if (on_) return std::nullopt;
        // Closed for a quiet that was only a missed burst: it grew again, so
        // the mask was on through it.
        if (last <= was_end) return std::nullopt;
        on_ = true;
        return Change{true, session, was_end > 0 ? was_end : start_s};
    }

    // A new session: the mask went on at its start (a later one while on is
    // the mask off and on again between two bursts).
    on_      = true;
    session_ = session;
    start_s_ = start_s;
    end_s_   = last;
    grew_at_ = now_s;
    return Change{true, session, start_s};
}

std::optional<CardMask::Change> CardMask::tick(long long now_s) {
    if (!on_ || now_s - grew_at_ < kMaskOffAfterS) return std::nullopt;
    on_ = false;
    return Change{false, session_, end_s_ > 0 ? end_s_ : grew_at_};
}

}  // namespace hms_cpap
