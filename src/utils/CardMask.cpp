#include "utils/CardMask.h"

#include <algorithm>

namespace hms_cpap {

std::vector<CardMask::Change> CardMask::grew(const std::string& session, long long start_s,
                                             long long end_s, long long now_s, long long records) {
    // A file that could not be read still grew now; how old its night is, its
    // start says (a session runs a night at most).
    const long long last = end_s > 0 ? end_s : now_s;
    if (last < now_s - kRecentS) return {};
    if (end_s <= 0 && start_s < now_s - 12 * 3600) return {};

    // Stems are YYYYMMDD_HHMMSS: they sort in time.
    if (!session_.empty() && session < session_) return {};   // an older session, re-read

    if (session == session_) {
        const long long was_end = end_s_;
        end_s_   = std::max(end_s_, last);
        grew_at_ = now_s;
        if (on_) return {};
        // Off, and then it grew again: the mask was on through it.
        if (last <= was_end) return {};
        on_ = true;
        listed_sig_.clear();
        unchanged_ = 0;
        return {Change{true, session, was_end > 0 ? was_end : start_s}};
    }

    // D3: a BRP that is only a header has no record yet; not a mask-on.
    if (records == 0) return {};

    // A new session: the one before it ended (its off first, D4), and the
    // mask went on at the new one's start.
    std::vector<Change> out;
    if (on_) out.push_back(Change{false, session_, end_s_ > 0 ? end_s_ : grew_at_});
    on_      = true;
    session_ = session;
    start_s_ = start_s;
    end_s_   = last;
    grew_at_ = now_s;
    // The listing that showed it before this burst's download is its first.
    if (pending_session_ == session) {
        listed_sig_ = pending_sig_;
    } else {
        listed_sig_.clear();
    }
    unchanged_ = 0;
    pending_session_.clear();
    pending_sig_.clear();
    out.push_back(Change{true, session, start_s});
    return out;
}

std::optional<CardMask::Change> CardMask::listed(const std::string& session, bool at_eof,
                                                 const std::string& sig) {
    if (session != session_) {
        // A newer BRP, seen before it grew into the open session.
        if (session > session_ && session >= pending_session_) {
            pending_session_ = session;
            pending_sig_     = sig;
        }
        return std::nullopt;
    }
    if (!on_) return std::nullopt;

    // D1: at EOF and unchanged since the last listing. D2: a listing that
    // shows more than is stored starts the count again; it proves nothing.
    if (at_eof && !listed_sig_.empty() && sig == listed_sig_) {
        ++unchanged_;
    } else {
        unchanged_ = 0;
    }
    listed_sig_ = sig;
    if (unchanged_ < kUnchangedLists) return std::nullopt;

    on_        = false;
    unchanged_ = 0;
    return Change{false, session_, end_s_ > 0 ? end_s_ : grew_at_};
}

}  // namespace hms_cpap
