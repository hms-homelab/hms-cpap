#pragma once
#include <ctime>

#ifdef _WIN32
inline struct tm* gmtime_r(const time_t* timer, struct tm* result) {
    return gmtime_s(result, timer) == 0 ? result : nullptr;
}
inline struct tm* localtime_r(const time_t* timer, struct tm* result) {
    return localtime_s(result, timer) == 0 ? result : nullptr;
}
#endif

// Portable "interpret struct tm as UTC -> time_t". Uniquely named to avoid
// colliding with libc's or Drogon's own timegm symbol on Windows (Drogon already
// exports timegm in drogon.lib, so defining our own caused LNK2005).
inline time_t timegm_utc(struct tm* tm) {
#ifdef _WIN32
    return _mkgmtime(tm);
#else
    return timegm(tm);
#endif
}
