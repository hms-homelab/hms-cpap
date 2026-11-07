#include "services/Ds2Schedule.h"

#include <algorithm>
#include <cctype>

namespace hms_cpap::ds2sched {

namespace {
constexpr long long kRetryS = 3600;
}

std::optional<int> parseTimeOfDay(const std::string& s) {
    if (s.size() != 5 || s[2] != ':') return std::nullopt;
    for (int i : {0, 1, 3, 4})
        if (!std::isdigit(static_cast<unsigned char>(s[static_cast<size_t>(i)]))) return std::nullopt;
    const int h = (s[0] - '0') * 10 + (s[1] - '0');
    const int m = (s[3] - '0') * 10 + (s[4] - '0');
    if (h > 23 || m > 59) return std::nullopt;
    return h * 60 + m;
}

long long todayAt(long long now, int minutes) {
    std::time_t t = static_cast<std::time_t>(now);
    std::tm tm{};
#ifdef _WIN32
    localtime_s(&tm, &t);
#else
    localtime_r(&t, &tm);
#endif
    tm.tm_hour = minutes / 60;
    tm.tm_min = minutes % 60;
    tm.tm_sec = 0;
    tm.tm_isdst = -1;
    return static_cast<long long>(std::mktime(&tm));
}

Decision decide(const Input& in) {
    Decision d;
    const int minutes = parseTimeOfDay(in.read_at).value_or(10 * 60);
    const long long today = todayAt(in.now, minutes);
    // The slot this read belongs to: today's once it has come, else yesterday's.
    const long long slot = in.now >= today ? today : todayAt(in.now - 86400, minutes);
    const long long next_slot = in.now >= today ? todayAt(in.now + 86400, minutes) : today;

    if (in.last_read_at >= slot) {
        d.next_at = next_slot;
        d.why = "already read since " + in.read_at;
        return d;
    }
    if (in.last_attempt_at >= slot && in.now < in.last_attempt_at + kRetryS) {
        d.next_at = in.last_attempt_at + kRetryS;
        d.why = "the last try did not finish; trying again an hour after it";
        return d;
    }
    // Only in the hours after read_at: hms-cpap started, or the machine
    // paired, late in the evening must not read into the night.
    if (in.now >= slot + kWindowS) {
        d.next_at = next_slot;
        d.why = "outside the " + std::to_string(kWindowS / 3600) + " hours after " + in.read_at;
        return d;
    }
    if (in.mask_on) {
        d.next_at = std::min(in.now + kRetryS, next_slot);
        d.why = "a mask is on: never during therapy";
        return d;
    }
    d.due = true;
    d.next_at = in.now;
    return d;
}

}  // namespace hms_cpap::ds2sched
