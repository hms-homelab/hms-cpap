#pragma once
//
// SDD-037 D2 (ticket 129): the end of a night, as the DATA knows it.
//
// markSessionCompleted() stamps the clock. For a live night that is the right
// answer -- the moment the files stopped growing IS the estimate of mask-off.
// For an import it is the moment the import ran, so twenty nights read in one
// pass all carry one timestamp, which is what a user found on a first local
// import. Every path that stores a FINISHED night passes this instead.
//
// CpapdashBridge, not models/CPAPModels.h: the session types come from the
// shared parser library and the local header's definitions collide with its
// aliases (every other consumer includes the bridge for the same reason).
#include "database/IDatabase.h"
#include "parsers/CpapdashBridge.h"

#include <chrono>
#include <optional>
#include <string>

namespace hms_cpap {

/// The parser's own end for [s], else its start plus the duration it measured.
/// Empty when the parse knows neither, in which case the caller leaves the
/// session open rather than inventing a time.
inline std::optional<std::chrono::system_clock::time_point> dataEndOf(const CPAPSession& s) {
    if (s.session_end.has_value()) return s.session_end;
    if (s.session_start.has_value() && s.duration_seconds.has_value() &&
        s.duration_seconds.value() > 0) {
        return s.session_start.value() + std::chrono::seconds(s.duration_seconds.value());
    }
    return std::nullopt;
}

/// SDD-037 D1: is a local night old enough that storing it should also close
/// it? Inside the window the burst's checkpoint path still owns it, because a
/// local folder can be a mounted card, or the target of a sync that writes on a
/// lag, and calling that Done mid-night is the worse error.
inline bool localNightHasSettled(const std::chrono::system_clock::time_point& session_start,
                                 const std::chrono::system_clock::time_point& now,
                                 const std::chrono::seconds& window) {
    return session_start <= now - window;
}

/// Close [start] with the end [parsed] carries, falling back to the clock when
/// the parse knows no end. One line at each import site.
inline bool closeWithDataEnd(IDatabase& db, const std::string& device_id,
                             const std::chrono::system_clock::time_point& start,
                             const CPAPSession& parsed) {
    const auto end = dataEndOf(parsed);
    return end.has_value() ? db.markSessionCompletedAt(device_id, start, end.value())
                           : db.markSessionCompleted(device_id, start);
}

}  // namespace hms_cpap
