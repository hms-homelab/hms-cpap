#pragma once
//
// A POSIX TZ string ("EST5EDT,M3.2.0,M11.1.0", "CET-1CEST,M3.5.0,M10.5.0/3",
// "<+0530>-5:30") as a wall clock, with no process-wide setenv("TZ"): a
// Mule & Miner reports the owner's zone in this form, and the night's files
// are named on that clock, as the machine's card would name them.
//
// Supported: a standard name (letters, or <...>) and offset, an optional
// daylight name with an optional offset (default one hour ahead), and the
// rules Mm.w.d, Jn and n, each with an optional /time (default 02:00:00).
// Daylight with no rule takes the US rule, as POSIX leaves it to the system.
//
#include "services/As11NightWriter.h"

#include <cstdint>
#include <optional>
#include <string>

namespace hms_cpap::mule {

struct PosixTzRule {
    enum Kind { kMonthWeekDay, kJulianNoLeap, kJulianZero } kind{kMonthWeekDay};
    int month{0}, week{0}, wday{0};   // Mm.w.d
    int day{0};                       // Jn (1..365) or n (0..365)
    int time_s{2 * 3600};             // local time of the change, seconds
};

struct PosixTz {
    int  std_east_s{0};               // standard offset east of UTC, seconds
    bool has_dst{false};
    int  dst_east_s{0};
    PosixTzRule start, end;

    /// Offset east of UTC in seconds at a UTC instant.
    int offsetAt(int64_t utc_s) const;
};

/// nullopt when the string is not a POSIX TZ this reads.
std::optional<PosixTz> parsePosixTz(const std::string& tz);

/// The wall clock of a POSIX TZ; nullopt when it does not parse.
std::optional<WallClockFn> posixTzClock(const std::string& tz);

}  // namespace hms_cpap::mule
