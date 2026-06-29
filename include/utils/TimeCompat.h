#pragma once
#include <ctime>

#ifdef _WIN32
inline struct tm* gmtime_r(const time_t* timer, struct tm* result) {
    return gmtime_s(result, timer) == 0 ? result : nullptr;
}
inline struct tm* localtime_r(const time_t* timer, struct tm* result) {
    return localtime_s(result, timer) == 0 ? result : nullptr;
}
// POSIX timegm() has no MSVC equivalent; _mkgmtime() is the Win32 counterpart
// (interprets the struct tm as UTC). Same signature/semantics.
inline time_t timegm(struct tm* tm) { return _mkgmtime(tm); }
#endif
