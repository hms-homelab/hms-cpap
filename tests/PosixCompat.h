// The handful of POSIX calls the tests use, on MSVC as well.
//
// run_tests builds on Linux and macOS, and on Windows for the native BLE work
// (the vcpkg x64-windows-static-md build). On Windows there is no unistd.h and
// no setenv/unsetenv; these map them onto the CRT's own. Time functions
// (localtime_r, gmtime_r, timegm_utc) are in include/utils/TimeCompat.h.
#pragma once

#ifdef _WIN32
#include <process.h>   // getpid
#include <cstdlib>     // _putenv_s, std::getenv

// _putenv_s with an empty value removes the variable, which is unsetenv.
inline int setenv(const char* name, const char* value, int overwrite) {
    if (!overwrite && std::getenv(name)) return 0;
    return _putenv_s(name, value ? value : "");
}
inline int unsetenv(const char* name) { return _putenv_s(name, ""); }
#else
#include <unistd.h>
#endif
