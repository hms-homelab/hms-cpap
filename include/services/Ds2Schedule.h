#pragma once

#include <ctime>
#include <optional>
#include <string>

namespace hms_cpap::ds2sched {

/**
 * SDD-064 D5: when the automatic DreamStation 2 read runs. Once a day at a
 * local time of day, after the night, and never while any source this
 * install has says a mask is on. A read that did not finish is tried again an
 * hour later, within kWindowS of read_at; after that it waits for the next
 * day's. Pure, so it is tested without a clock.
 */

constexpr long long kWindowS = 6 * 3600;

/// "HH:MM" (00:00 to 23:59) as minutes after midnight; nullopt when it is not one.
std::optional<int> parseTimeOfDay(const std::string& hhmm);

/// The epoch second of today's `minutes` after local midnight, for the local
/// day that `now` falls in.
long long todayAt(long long now, int minutes);

struct Input {
    long long now = 0;              // epoch s
    std::string read_at = "10:00";  // local time of day
    long long last_read_at = 0;     // epoch s, the last read that finished (0 = never)
    long long last_attempt_at = 0;  // epoch s, the last read tried (0 = never)
    bool mask_on = false;           // a source says therapy is running now
};

struct Decision {
    bool due = false;
    long long next_at = 0;          // epoch s of the next try, for the page
    std::string why;                // "", or why not now (for the log)
};

Decision decide(const Input& in);

}  // namespace hms_cpap::ds2sched
