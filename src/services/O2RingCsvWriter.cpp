#include "services/O2RingCsvWriter.h"

#include <cstdio>
#include <ctime>
#include <sstream>

namespace hms_cpap {

using cpapdash::parser::OximetrySession;

namespace {

// "06:53:07 Apr 12 2026" — the Checkme dialect, rendered as UTC.
//
// UTC, not local, because O2RingCsvParser reads these back through timegm.
// A ring writes wall-clock with no zone and the parser picked a clock; the
// writer has to pick the same one or write-then-parse moves the night. This
// cost four hours of offset the first time round, caught by the round-trip
// test. Changing the PARSER instead would silently reinterpret every CSV a
// user has already uploaded, which is not a trade worth making for tidiness.
std::string stampUtc(std::chrono::system_clock::time_point tp) {
    static const char* kMon[] = {"Jan","Feb","Mar","Apr","May","Jun",
                                 "Jul","Aug","Sep","Oct","Nov","Dec"};
    std::time_t t = std::chrono::system_clock::to_time_t(tp);
    std::tm tm{};
#ifdef _WIN32
    ::gmtime_s(&tm, &t);
#else
    ::gmtime_r(&t, &tm);
#endif
    char buf[40];
    std::snprintf(buf, sizeof(buf), "%02d:%02d:%02d %s %d %d",
                  tm.tm_hour, tm.tm_min, tm.tm_sec,
                  kMon[tm.tm_mon < 0 || tm.tm_mon > 11 ? 0 : tm.tm_mon],
                  tm.tm_mday, tm.tm_year + 1900);
    return buf;
}

}  // namespace

std::string O2RingCsvWriter::write(const OximetrySession& session) {
    std::ostringstream out;

    // Header from a real Checkme O2 Max export. The parser ignores it and reads
    // positionally, but SleepHQ may not, so it is copied rather than invented.
    out << "Time,Oxygen Level,Pulse Rate,Motion,O2 Reminder,PR Reminder\r\n";

    for (const auto& s : session.samples) {
        // A sample the ring could not read is written back as the SENTINEL it
        // came in as, never skipped.
        //
        // Skipping would silently compress the night: twenty minutes with the
        // ring off the finger would come back as a shorter session rather than
        // a session with a gap, and the interval auto-detection on re-import
        // would then measure the wrong cadence off the healed-over timestamps.
        //
        // 255 for both, and 255 specifically: the parser's tests are
        // `spo2 > 0 && spo2 <= 100` and `hr > 0 && hr < 255`, so 255 fails both
        // and round trips back to the same 0xFF this sample already holds. (The
        // 16-bit 65535 a ring writes for heart rate cannot be represented here
        // and does not need to be — it fails the same test.)
        const bool sp_ok = s.spo2 != 0xFF && s.invalid_flag == 0;
        const bool hr_ok = s.heart_rate != 0xFF;

        out << stampUtc(s.timestamp) << ','
            << (sp_ok ? static_cast<int>(s.spo2) : 255) << ','
            << (hr_ok ? static_cast<int>(s.heart_rate) : 255) << ','
            << static_cast<int>(s.motion)
            << ",0,0\r\n";   // the reminder columns: always present, never ours
    }

    return out.str();
}

std::string O2RingCsvWriter::filenameFor(const OximetrySession& session,
                                         const std::string& device) {
    std::time_t t = std::chrono::system_clock::to_time_t(session.start_time);
    // Same clock as the rows, so the filename and the first timestamp agree.
    std::tm tm{};
#ifdef _WIN32
    ::gmtime_s(&tm, &t);
#else
    ::gmtime_r(&t, &tm);
#endif
    char stamp[24];
    std::snprintf(stamp, sizeof(stamp), "%04d%02d%02d%02d%02d%02d",
                  tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday,
                  tm.tm_hour, tm.tm_min, tm.tm_sec);

    std::string name = device.empty() ? "O2Ring" : device;
    // The name lands in a filename and in a multipart part name. A ring never
    // puts a separator in its model name, but the value can come from the
    // database, so it is not trusted to stay that way.
    for (char& c : name) {
        if (c == '/' || c == '\\' || c == ',' || c == '"' || c == '\n' || c == '\r') c = '_';
    }
    return name + "_" + stamp + ".csv";
}

}  // namespace hms_cpap
