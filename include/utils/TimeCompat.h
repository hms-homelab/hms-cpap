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
