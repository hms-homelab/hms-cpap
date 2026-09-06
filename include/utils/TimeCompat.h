#pragma once
#include <ctime>
#include <sys/stat.h>
#ifdef _WIN32
#include <sys/utime.h>     // MSVC has no <utime.h>; the POSIX one lives here
#else
#include <utime.h>
#endif

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

// ── A file's size and modification time, portably ───────────────────────────
//
// The ezShare card-stamp change detector stores the card's own timestamp AS the
// local file's mtime, so these are on the hot path for every sidecar decision.
// They live here rather than in the service because MSVC has no <utime.h> and
// spells both the struct and the call with a leading underscore -- which is
// exactly the kind of split this header exists to absorb. A raw <utime.h> in a
// .cpp compiled fine on macOS and Linux and broke only the Windows release
// build, where nothing local could see it.

/// Read a file's mtime and size. False when it does not exist.
inline bool fileStat(const char* path, time_t& mtime, long long& size) {
#ifdef _WIN32
    struct _stat64 st{};
    if (_stat64(path, &st) != 0) return false;
#else
    struct stat st{};
    if (::stat(path, &st) != 0) return false;
#endif
    mtime = st.st_mtime;
    size  = static_cast<long long>(st.st_size);
    return true;
}

/// Stamp a file with a given modification time. Best effort; false on failure.
inline bool setFileMtime(const char* path, time_t stamp) {
#ifdef _WIN32
    struct _utimbuf times{};
    times.actime = stamp;
    times.modtime = stamp;
    return _utime(path, &times) == 0;
#else
    struct utimbuf times{};
    times.actime = stamp;
    times.modtime = stamp;
    return ::utime(path, &times) == 0;
#endif
}
