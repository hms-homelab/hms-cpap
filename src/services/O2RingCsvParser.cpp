#include "services/O2RingCsvParser.h"

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

// Parse a Wellue time field into a UTC time_point. Handles both:
//   "06:53:07 Apr 12 2026"     (24-hour)
//   "11:20:29PM Jun 19, 2026"  (12-hour AM/PM, comma after the day)
// Returns false if it can't be parsed. We treat the wall clock as UTC; only
// deltas matter for interval/duration, so the zone is irrelevant as long as
// it's consistent.
bool parseTimestamp(std::string t, std::chrono::system_clock::time_point& out) {
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

    char mon[16] = {};
    int day = 0, year = 0;
    if (sscanf(rest.c_str(), "%15s %d %d", mon, &day, &year) < 3) return false;
    int month = 0;
    for (int i = 0; i < 12; ++i)
        if (strncmp(mon, months[i], 3) == 0) { month = i + 1; break; }
    if (month == 0 || day < 1 || year < 2000) return false;

    std::tm tm{};
    tm.tm_year = year - 1900;
    tm.tm_mon = month - 1;
    tm.tm_mday = day;
    tm.tm_hour = hh;
    tm.tm_min = mm;
    tm.tm_sec = ssec;
    out = std::chrono::system_clock::from_time_t(timegm(&tm));
    return true;
}

}  // namespace

OximetrySession O2RingCsvParser::parse(const std::string& content,
                                       const std::string& filename) {
    OximetrySession session;
    session.filename = filename;

    std::istringstream ss(content);
    std::string line;
    std::getline(ss, line);  // header

    while (std::getline(ss, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (trim(line).empty()) continue;
        auto cols = splitCsv(line);
        if (cols.size() < 3) continue;

        std::chrono::system_clock::time_point ts;
        if (!parseTimestamp(cols[0], ts)) continue;
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
