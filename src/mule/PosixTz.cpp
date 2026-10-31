#include "mule/PosixTz.h"

#include <cctype>

namespace hms_cpap::mule {

namespace {

int64_t floorDiv(int64_t a, int64_t b) { return a / b - ((a % b != 0) && ((a < 0) != (b < 0))); }

int64_t daysFromCivil(int y, int m, int d) {
    y -= m <= 2;
    const int64_t era = (y >= 0 ? y : y - 399) / 400;
    const int64_t yoe = y - era * 400;
    const int64_t doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
    const int64_t doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return era * 146097 + doe - 719468;
}

int yearOfDays(int64_t days) {
    const int64_t z = days + 719468;
    const int64_t era = (z >= 0 ? z : z - 146096) / 146097;
    const int64_t doe = z - era * 146097;
    const int64_t yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    const int64_t doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    const int64_t mp = (5 * doy + 2) / 153;
    const int month = static_cast<int>(mp < 10 ? mp + 3 : mp - 9);
    return static_cast<int>(yoe + era * 400 + (month <= 2));
}

bool leap(int y) { return (y % 4 == 0 && y % 100 != 0) || y % 400 == 0; }

int daysInMonth(int y, int m) {
    static const int k[] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
    return m == 2 && leap(y) ? 29 : k[m - 1];
}

// 0 = Sunday.
int weekday(int64_t days) {
    const int64_t w = (days + 4) % 7;   // 1970-01-01 was a Thursday
    return static_cast<int>(w < 0 ? w + 7 : w);
}

struct Cursor {
    const std::string& s;
    size_t at{0};
    bool done() const { return at >= s.size(); }
    char peek() const { return done() ? '\0' : s[at]; }
};

bool readName(Cursor& c) {
    if (c.peek() == '<') {
        const size_t close = c.s.find('>', c.at);
        if (close == std::string::npos || close == c.at + 1) return false;
        c.at = close + 1;
        return true;
    }
    const size_t from = c.at;
    while (!c.done() && std::isalpha(static_cast<unsigned char>(c.peek()))) c.at++;
    return c.at - from >= 3;
}

bool readNumber(Cursor& c, int* out, int max_digits) {
    const size_t from = c.at;
    int v = 0;
    while (!c.done() && std::isdigit(static_cast<unsigned char>(c.peek())) &&
           c.at - from < static_cast<size_t>(max_digits))
        v = v * 10 + (c.s[c.at++] - '0');
    if (c.at == from) return false;
    *out = v;
    return true;
}

// [+-]hh[:mm[:ss]], in seconds; sign as written.
bool readTime(Cursor& c, int* secs, bool signed_ok) {
    int sign = 1;
    if (signed_ok && (c.peek() == '+' || c.peek() == '-')) sign = c.s[c.at++] == '-' ? -1 : 1;
    int h = 0, m = 0, s = 0;
    if (!readNumber(c, &h, 3)) return false;
    if (c.peek() == ':') {
        c.at++;
        if (!readNumber(c, &m, 2)) return false;
        if (c.peek() == ':') {
            c.at++;
            if (!readNumber(c, &s, 2)) return false;
        }
    }
    if (h > 167 || m > 59 || s > 59) return false;
    *secs = sign * (h * 3600 + m * 60 + s);
    return true;
}

bool readRule(Cursor& c, PosixTzRule* r) {
    if (c.peek() == 'M') {
        c.at++;
        r->kind = PosixTzRule::kMonthWeekDay;
        if (!readNumber(c, &r->month, 2) || c.peek() != '.') return false;
        c.at++;
        if (!readNumber(c, &r->week, 1) || c.peek() != '.') return false;
        c.at++;
        if (!readNumber(c, &r->wday, 1)) return false;
        if (r->month < 1 || r->month > 12 || r->week < 1 || r->week > 5 || r->wday > 6) return false;
    } else if (c.peek() == 'J') {
        c.at++;
        r->kind = PosixTzRule::kJulianNoLeap;
        if (!readNumber(c, &r->day, 3) || r->day < 1 || r->day > 365) return false;
    } else {
        r->kind = PosixTzRule::kJulianZero;
        if (!readNumber(c, &r->day, 3) || r->day > 365) return false;
    }
    r->time_s = 2 * 3600;
    if (c.peek() == '/') {
        c.at++;
        if (!readTime(c, &r->time_s, true)) return false;
    }
    return true;
}

// The rule's day in year y, as days since 1970-01-01.
int64_t ruleDay(const PosixTzRule& r, int y) {
    const int64_t jan1 = daysFromCivil(y, 1, 1);
    switch (r.kind) {
        case PosixTzRule::kJulianNoLeap:
            return jan1 + (r.day - 1) + (leap(y) && r.day >= 60 ? 1 : 0);
        case PosixTzRule::kJulianZero:
            return jan1 + r.day;
        case PosixTzRule::kMonthWeekDay:
        default: {
            const int64_t first = daysFromCivil(y, r.month, 1);
            int d = 1 + (r.wday - weekday(first) + 7) % 7 + (r.week - 1) * 7;
            while (d > daysInMonth(y, r.month)) d -= 7;
            return first + d - 1;
        }
    }
}

}  // namespace

int PosixTz::offsetAt(int64_t utc_s) const {
    if (!has_dst) return std_east_s;
    const int y = yearOfDays(floorDiv(utc_s + std_east_s, 86400));
    // The change to daylight happens at a standard-time instant, the change
    // back at a daylight-time one.
    const int64_t on  = ruleDay(start, y) * 86400 + start.time_s - std_east_s;
    const int64_t off = ruleDay(end, y) * 86400 + end.time_s - dst_east_s;
    const bool dst = on < off ? (utc_s >= on && utc_s < off) : !(utc_s >= off && utc_s < on);
    return dst ? dst_east_s : std_east_s;
}

std::optional<PosixTz> parsePosixTz(const std::string& tz) {
    if (tz.empty() || tz.size() > 128) return std::nullopt;
    Cursor c{tz};
    PosixTz out;
    int west = 0;
    if (!readName(c) || !readTime(c, &west, true)) return std::nullopt;
    out.std_east_s = -west;
    if (c.done()) return out;

    if (!readName(c)) return std::nullopt;
    out.has_dst    = true;
    out.dst_east_s = out.std_east_s + 3600;
    if (!c.done() && c.peek() != ',') {
        int dw = 0;
        if (!readTime(c, &dw, true)) return std::nullopt;
        out.dst_east_s = -dw;
    }
    if (c.done()) {
        out.start = {PosixTzRule::kMonthWeekDay, 3, 2, 0, 0, 2 * 3600};
        out.end   = {PosixTzRule::kMonthWeekDay, 11, 1, 0, 0, 2 * 3600};
        return out;
    }
    if (c.peek() != ',') return std::nullopt;
    c.at++;
    if (!readRule(c, &out.start) || c.peek() != ',') return std::nullopt;
    c.at++;
    if (!readRule(c, &out.end) || !c.done()) return std::nullopt;
    return out;
}

std::optional<WallClockFn> posixTzClock(const std::string& tz) {
    const auto z = parsePosixTz(tz);
    if (!z) return std::nullopt;
    const PosixTz zone = *z;
    return WallClockFn([zone](int64_t utc_ms) {
        const int off = zone.offsetAt(floorDiv(utc_ms, 1000));
        return fixedOffsetClock(off / 60)(utc_ms);
    });
}

}  // namespace hms_cpap::mule
