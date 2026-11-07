#pragma once

#include <optional>
#include <string>

namespace hms_cpap::ds2time {

/**
 * SDD-064 D5a: the DreamStation 2 says nothing about therapy over Bluetooth,
 * so its owner chooses when it is read: "Read automatically once a day at
 * [time]" (read_mode "daily", read_at "HH:MM" local, 11:00 by default) or
 * "Only when I ask" (read_mode "manual"). Another source saying the night
 * ended starts a read in both, and nothing reads while a mask is on.
 *
 * The daily read: once today's time has come, and no read was tried since
 * then (nor was the machine paired since then), it is due. A mask on at that
 * time delays it to the first check after the mask goes off. A day whose time
 * passed without a read (hms-cpap was off) is not read the next morning: the
 * next read is that day's own time. Pure, so it is tested without a clock.
 */

constexpr const char* kDefaultReadAt = "11:00";

/// "HH:MM" (00:00 to 23:59) as minutes after midnight; nullopt when it is not one.
std::optional<int> parseTimeOfDay(const std::string& hhmm);

/// "daily" or "manual"; anything else reads as "daily".
std::string normaliseMode(const std::string& mode);

/// The epoch second of `minutes` after local midnight on the local day `now` is in.
long long todayAt(long long now, int minutes);

struct Input {
    long long now = 0;               // epoch s
    std::string read_mode = "daily";
    std::string read_at = kDefaultReadAt;
    long long last_attempt_at = 0;   // epoch s, the last read tried for any reason (0 = never)
    long long paired_at = 0;         // epoch s, when the machine was paired
    bool mask_on = false;            // a source says a mask is on now
};

struct Decision {
    bool due = false;
    long long next_at = 0;   // epoch s of the next daily read, for the page; 0 with "manual"
    std::string why;         // why not now, for the log ("" when due)
};

Decision decide(const Input& in);

}  // namespace hms_cpap::ds2time
