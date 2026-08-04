#include "services/O2RingCsvParser.h"
#include "utils/TimeCompat.h"  // portable timegm() on MSVC (_mkgmtime)

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <sstream>
#include <vector>

namespace hms_cpap {

namespace {

using cpapdash::parser::OximetrySample;
using cpapdash::parser::OximetrySession;

// Split a CSV line honoring double-quoted fields, so a quoted timestamp that
// contains a comma ("11:20:29PM Jun 19, 2026") stays a single field. Quotes
// are stripped from the output.
std::vector<std::string> splitCsv(const std::string& line) {
    std::vector<std::string> out;
    std::string cur;
    bool inq = false;
    for (char c : line) {
        if (c == '"') inq = !inq;
        else if (c == ',' && !inq) { out.push_back(cur); cur.clear(); }
        else cur += c;
    }
    out.push_back(cur);
    return out;
}

std::string trim(const std::string& s) {
    size_t a = s.find_first_not_of(" \t\r\n");
    if (a == std::string::npos) return "";
    size_t b = s.find_last_not_of(" \t\r\n");
    return s.substr(a, b - a + 1);
}

// Which way round a purely numeric date field is written. The Wellue app
// follows the phone's locale, so the same ring exports "02/08/2026" as either
// the 2nd of August or the 8th of February (issue #17).
enum class DateOrder { DayFirst, MonthFirst };

// Split "02/08/2026" (or 02-08-2026) into its three numbers. Returns false for
// anything that is not exactly three numeric components, which is how a
// month-name date ("Apr 12 2026") declines this path.
bool splitNumericDate(const std::string& s, int& a, int& b, int& year) {
    int n = 0;
    char sep1 = 0, sep2 = 0, extra = 0;
    n = std::sscanf(s.c_str(), "%d%c%d%c%d%c", &a, &sep1, &b, &sep2, &year, &extra);
    if (n != 5) return false;                       // 6 means trailing junk
    if ((sep1 != '/' && sep1 != '-') || sep1 != sep2) return false;
    return true;
}

// Pull a YYYYMMDD out of the Wellue export filename, e.g.
// "O2Ring 0651_20260802215634.csv" -> 2026, 8, 2. Returns false if there is no
// digit run long enough to be a date stamp.
bool filenameDateStamp(const std::string& filename, int& year, int& month, int& day) {
    for (size_t i = 0; i < filename.size();) {
        if (!std::isdigit((unsigned char)filename[i])) { ++i; continue; }
        size_t j = i;
        while (j < filename.size() && std::isdigit((unsigned char)filename[j])) ++j;
        size_t len = j - i;
        if (len == 8 || len == 14) {
            std::string d = filename.substr(i, 8);
            int y = std::atoi(d.substr(0, 4).c_str());
            int m = std::atoi(d.substr(4, 2).c_str());
            int dd = std::atoi(d.substr(6, 2).c_str());
            if (y >= 2000 && m >= 1 && m <= 12 && dd >= 1 && dd <= 31) {
                year = y; month = m; day = dd;
                return true;
            }
        }
        i = j;
    }
    return false;
}

// Parse a Wellue time field into a UTC time_point. Handles:
//   "06:53:07 Apr 12 2026"     (24-hour, month name)
//   "11:20:29PM Jun 19, 2026"  (12-hour AM/PM, comma after the day)
//   "21:56:34 02/08/2026"      (numeric, order decided by the caller)
// Returns false if it can't be parsed. We treat the wall clock as UTC; only
// deltas matter for interval/duration, so the zone is irrelevant as long as
// it's consistent.
bool parseTimestamp(std::string t, DateOrder order,
                    std::chrono::system_clock::time_point& out) {
    static const char* months[] = {"Jan", "Feb", "Mar", "Apr", "May", "Jun",
                                    "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"};
    t = trim(t);
    for (auto& c : t) if (c == ',') c = ' ';  // drop the "Jun 19," comma

    size_t sp = t.find(' ');
    if (sp == std::string::npos) return false;
    std::string clock = t.substr(0, sp);
    std::string rest = trim(t.substr(sp + 1));

    int ampm = 0;  // 0 none, 1 AM, 2 PM
    if (clock.size() >= 2) {
        std::string suf = clock.substr(clock.size() - 2);
        for (auto& c : suf) c = (char)std::toupper((unsigned char)c);
        if (suf == "AM") { ampm = 1; clock = clock.substr(0, clock.size() - 2); }
        else if (suf == "PM") { ampm = 2; clock = clock.substr(0, clock.size() - 2); }
    }

    int hh = 0, mm = 0, ssec = 0;
    if (sscanf(clock.c_str(), "%d:%d:%d", &hh, &mm, &ssec) < 3) return false;
    if (ampm == 2 && hh < 12) hh += 12;   // PM
    if (ampm == 1 && hh == 12) hh = 0;    // 12 AM -> 00

    int day = 0, year = 0, month = 0;

    int na = 0, nb = 0, nyear = 0;
    if (splitNumericDate(rest, na, nb, nyear)) {
        // A component above 12 can only be the day, whatever the locale says.
        if (na > 12)      { day = na; month = nb; }
        else if (nb > 12) { month = na; day = nb; }
        else if (order == DateOrder::DayFirst) { day = na; month = nb; }
        else                                   { month = na; day = nb; }
        year = nyear;
    } else {
        char mon[16] = {};
        if (sscanf(rest.c_str(), "%15s %d %d", mon, &day, &year) < 3) return false;
        for (int i = 0; i < 12; ++i)
            if (strncmp(mon, months[i], 3) == 0) { month = i + 1; break; }
    }

    if (month < 1 || month > 12 || day < 1 || day > 31 || year < 2000) return false;

    std::tm tm{};
    tm.tm_year = year - 1900;
    tm.tm_mon = month - 1;
    tm.tm_mday = day;
    tm.tm_hour = hh;
    tm.tm_min = mm;
    tm.tm_sec = ssec;
    out = std::chrono::system_clock::from_time_t(timegm_utc(&tm));
    return true;
}

// Decide how to read ambiguous numeric dates, in order of confidence:
//   1. Any row where a component exceeds 12 settles it for the whole file.
//   2. A midnight crossing inside the file: the recording rolls to the next
//      calendar day, so the component that ticked up by one is the day.
//   3. The export filename's own YYYYMMDD stamp, checked against the first
//      row: Wellue names the file after the first sample, so whichever
//      reading reproduces that date is the right one.
//   4. A 12-hour AM/PM clock: the phone is set to a US-style locale, and
//      those are the locales that write month-first.
//   5. Otherwise day-first, which is what every locale that writes dates
//      numerically with slashes uses except the US.
DateOrder detectDateOrder(const std::string& content, const std::string& filename) {
    std::istringstream ss(content);
    std::string line;
    std::getline(ss, line);  // header

    bool have_first = false, have_prev = false, ampm_clock = false;
    int first_a = 0, first_b = 0, first_year = 0;
    int prev_a = 0, prev_b = 0;

    while (std::getline(ss, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (trim(line).empty()) continue;
        auto cols = splitCsv(line);
        if (cols.empty()) continue;

        std::string t = trim(cols[0]);
        size_t sp = t.find(' ');
        if (sp == std::string::npos) continue;
        std::string clock = t.substr(0, sp);
        std::string rest = trim(t.substr(sp + 1));

        int a = 0, b = 0, y = 0;
        if (!splitNumericDate(rest, a, b, y)) return DateOrder::DayFirst;  // month-name file, unused
        if (a > 12) return DateOrder::DayFirst;
        if (b > 12) return DateOrder::MonthFirst;

        if (clock.size() >= 2) {
            char c1 = (char)std::toupper((unsigned char)clock[clock.size() - 2]);
            char c2 = (char)std::toupper((unsigned char)clock.back());
            if (c2 == 'M' && (c1 == 'A' || c1 == 'P')) ampm_clock = true;
        }

        if (!have_first) { first_a = a; first_b = b; first_year = y; have_first = true; }
        if (have_prev && (a != prev_a || b != prev_b)) {
            if (b == prev_b && a == prev_a + 1) return DateOrder::DayFirst;
            if (a == prev_a && b == prev_b + 1) return DateOrder::MonthFirst;
        }
        prev_a = a; prev_b = b; have_prev = true;
    }

    int fy = 0, fm = 0, fd = 0;
    if (have_first && filenameDateStamp(filename, fy, fm, fd) && fy == first_year) {
        if (fd == first_a && fm == first_b) return DateOrder::DayFirst;
        if (fm == first_a && fd == first_b) return DateOrder::MonthFirst;
    }
    if (ampm_clock) return DateOrder::MonthFirst;
    return DateOrder::DayFirst;
}

}  // namespace

OximetrySession O2RingCsvParser::parse(const std::string& content,
                                       const std::string& filename) {
    OximetrySession session;
    session.filename = filename;

    const DateOrder order = detectDateOrder(content, filename);

    std::istringstream ss(content);
    std::string line;
    std::getline(ss, line);  // header

    while (std::getline(ss, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (trim(line).empty()) continue;
        auto cols = splitCsv(line);
        if (cols.size() < 3) continue;

        std::chrono::system_clock::time_point ts;
        if (!parseTimestamp(cols[0], order, ts)) continue;
        int s = std::atoi(trim(cols[1]).c_str());
        int h = std::atoi(trim(cols[2]).c_str());
        int mo = cols.size() > 3 ? std::atoi(trim(cols[3]).c_str()) : 0;

        // Sentinel readings (SpO2 255, HR 65535) / out-of-range are invalid.
        // Map them to 0xFF so OximetrySample::valid() excludes them (and so a
        // 16-bit HR sentinel can't overflow the uint8_t field).
        bool sp_ok = s > 0 && s <= 100;
        bool hr_ok = h > 0 && h < 255;

        OximetrySample sample;
        sample.timestamp = ts;
        sample.spo2 = sp_ok ? (uint8_t)s : 0xFF;
        sample.heart_rate = hr_ok ? (uint8_t)h : 0xFF;
        sample.invalid_flag = sp_ok ? 0 : 1;
        sample.motion = (mo >= 0 && mo <= 255) ? (uint8_t)mo : 0;
        sample.vibration = 0;
        session.samples.push_back(sample);
    }

    if (session.samples.empty()) return session;

    // Detect the base sample interval = smallest positive gap between samples.
    long long interval = 0;
    for (size_t i = 1; i < session.samples.size(); ++i) {
        long long d = std::chrono::duration_cast<std::chrono::seconds>(
                          session.samples[i].timestamp -
                          session.samples[i - 1].timestamp).count();
        if (d > 0 && (interval == 0 || d < interval)) interval = d;
    }
    session.sample_interval = interval > 0 ? (double)interval : 1.0;

    session.start_time = session.samples.front().timestamp;
    session.end_time = session.samples.back().timestamp;
    session.duration_seconds =
        (int)(session.samples.size() * session.sample_interval);

    session.metrics = cpapdash::parser::VLDParser::calculateMetrics(
        session.samples, session.sample_interval);

    return session;
}

}  // namespace hms_cpap
