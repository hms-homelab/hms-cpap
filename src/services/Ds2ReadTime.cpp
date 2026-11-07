#include "services/Ds2ReadTime.h"

#include <algorithm>
#include <cctype>
#include <ctime>

namespace hms_cpap::ds2time {

std::optional<int> parseTimeOfDay(const std::string& s) {
    if (s.size() != 5 || s[2] != ':') return std::nullopt;
    for (size_t i : {0u, 1u, 3u, 4u})
        if (!std::isdigit(static_cast<unsigned char>(s[i]))) return std::nullopt;
    const int h = (s[0] - '0') * 10 + (s[1] - '0');
    const int m = (s[3] - '0') * 10 + (s[4] - '0');
    if (h > 23 || m > 59) return std::nullopt;
    return h * 60 + m;
}

std::string normaliseMode(const std::string& mode) { return mode == "manual" ? "manual" : "daily"; }

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
    if (normaliseMode(in.read_mode) == "manual") {
        d.why = "only when asked";
        return d;
    }
    const int minutes = parseTimeOfDay(in.read_at).value_or(*parseTimeOfDay(kDefaultReadAt));
    const long long today = todayAt(in.now, minutes);
    // Tomorrow's slot: today's moved a day on through the local clock (a DST
    // change keeps it at the chosen time of day).
    const long long tomorrow = todayAt(today + 36 * 3600, minutes);
    if (in.now < today) {
        d.next_at = today;
        d.why = "before " + in.read_at;
        return d;
    }
    if (std::max(in.last_attempt_at, in.paired_at) >= today) {
        d.next_at = tomorrow;
        d.why = "already read (or paired) since " + in.read_at + " today";
        return d;
    }
    if (in.mask_on) {
        d.next_at = in.now;
        d.why = "a mask is on: the read waits until it is off";
        return d;
    }
    d.due = true;
    d.next_at = in.now;
    return d;
}

}  // namespace hms_cpap::ds2time
