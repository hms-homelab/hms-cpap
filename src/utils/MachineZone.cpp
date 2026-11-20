// SDD-074: each machine is read on its own clock (see include/utils/MachineZone.h).
#include "utils/MachineZone.h"

#include "mule/PosixTz.h"
#include "utils/TimeCompat.h"

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <map>
#include <mutex>
#include <set>
#include <sstream>
#include <version>

// D9: std::chrono's zone database where the standard library has it (MSVC's
// C++20 build), the OS zoneinfo files everywhere else. No new dependency.
#if defined(__cpp_lib_chrono) && __cpp_lib_chrono >= 201907L
#define HMS_CPAP_STD_TZDB 1
#else
#define HMS_CPAP_STD_TZDB 0
#endif

namespace hms_cpap {

// ── labels ───────────────────────────────────────────────────────────────────

namespace wall {

long long labelOf(const std::tm& t) {
    std::tm c = t;
    c.tm_isdst = 0;
    return static_cast<long long>(timegm_utc(&c));
}

std::tm fieldsOf(long long label) {
    const std::time_t t = static_cast<std::time_t>(label);
    std::tm out{};
    gmtime_r(&t, &out);
    return out;
}

std::string text(long long label) {
    const std::tm t = fieldsOf(label);
    char buf[32];
    std::snprintf(buf, sizeof(buf), "%04d-%02d-%02d %02d:%02d:%02d", t.tm_year + 1900, t.tm_mon + 1,
                  t.tm_mday, t.tm_hour, t.tm_min, t.tm_sec);
    return buf;
}

std::string text(std::chrono::system_clock::time_point label) { return text(seconds(label)); }

std::optional<long long> parseText(const std::string& s) {
    int y = 0, mo = 0, d = 0, h = 0, mi = 0, se = 0;
    if (s.size() < 10) return std::nullopt;
    const int n = std::sscanf(s.c_str(), "%d-%d-%d%*c%d:%d:%d", &y, &mo, &d, &h, &mi, &se);
    if (n != 3 && n != 6) return std::nullopt;
    if (mo < 1 || mo > 12 || d < 1 || d > 31) return std::nullopt;
    std::tm t{};
    t.tm_year = y - 1900;
    t.tm_mon  = mo - 1;
    t.tm_mday = d;
    t.tm_hour = h;
    t.tm_min  = mi;
    t.tm_sec  = se;
    return labelOf(t);
}

}  // namespace wall

// ── zone database ────────────────────────────────────────────────────────────

namespace {

bool safeZoneName(const std::string& name) {
    if (name.empty() || name.size() > 64 || name.front() == '/' || name.find("..") != std::string::npos)
        return false;
    for (char c : name)
        if (!(std::isalnum(static_cast<unsigned char>(c)) || c == '/' || c == '_' || c == '-' || c == '+'))
            return false;
    return true;
}

#if HMS_CPAP_STD_TZDB

class StdZone : public ZoneRules {
public:
    explicit StdZone(const std::chrono::time_zone* z) : z_(z) {}
    long long offsetAt(long long utc) const override {
        const auto info = z_->get_info(std::chrono::sys_seconds(std::chrono::seconds(utc)));
        return info.offset.count();
    }

private:
    const std::chrono::time_zone* z_;
};

#else

// A TZif file (RFC 8536): its transitions, and the POSIX TZ footer for every
// instant after the last one.
class TzifZone : public ZoneRules {
public:
    std::vector<int64_t> at;          // transition instants, ascending
    std::vector<int32_t> offset_at;   // the offset from each one on
    int32_t before{0};                // the offset before the first
    std::optional<mule::PosixTz> footer;

    long long offsetAt(long long utc) const override {
        if (at.empty() || utc < at.front()) {
            if (at.empty() && footer) return footer->offsetAt(utc);
            return before;
        }
        if (utc >= at.back() && footer) return footer->offsetAt(utc);
        const auto it = std::upper_bound(at.begin(), at.end(), static_cast<int64_t>(utc));
        return offset_at[static_cast<size_t>(it - at.begin()) - 1];
    }
};

int64_t be(const unsigned char* p, int n) {
    uint64_t v = 0;
    for (int i = 0; i < n; ++i) v = (v << 8) | p[i];
    if (n == 4) return static_cast<int32_t>(static_cast<uint32_t>(v));
    return static_cast<int64_t>(v);
}

std::shared_ptr<TzifZone> parseTzif(const std::string& bytes) {
    const auto* b = reinterpret_cast<const unsigned char*>(bytes.data());
    const size_t n = bytes.size();
    if (n < 44 || std::memcmp(b, "TZif", 4) != 0) return nullptr;

    struct Counts { int64_t isut, isstd, leap, time, type, chars; };
    auto counts = [&](size_t h) {
        return Counts{be(b + h + 20, 4), be(b + h + 24, 4), be(b + h + 28, 4),
                      be(b + h + 32, 4), be(b + h + 36, 4), be(b + h + 40, 4)};
    };
    auto blockSize = [](const Counts& c, int tsize) {
        return static_cast<size_t>(c.time * tsize + c.time + c.type * 6 + c.chars + c.leap * (tsize + 4) +
                                   c.isstd + c.isut);
    };

    size_t h = 0;
    int tsize = 4;
    Counts c = counts(0);
    if (b[4] >= '2') {
        const size_t h2 = 44 + blockSize(c, 4);
        if (h2 + 44 > n || std::memcmp(b + h2, "TZif", 4) != 0) return nullptr;
        h = h2;
        tsize = 8;
        c = counts(h2);
    }
    const size_t data = h + 44;
    if (data + blockSize(c, tsize) > n || c.type < 1) return nullptr;

    auto z = std::make_shared<TzifZone>();
    const unsigned char* times = b + data;
    const unsigned char* idx = times + c.time * tsize;
    const unsigned char* types = idx + c.time;
    std::vector<int32_t> type_off(static_cast<size_t>(c.type));
    std::vector<bool> type_dst(static_cast<size_t>(c.type));
    for (int64_t i = 0; i < c.type; ++i) {
        type_off[i] = static_cast<int32_t>(be(types + i * 6, 4));
        type_dst[i] = types[i * 6 + 4] != 0;
    }
    z->before = type_off[0];
    for (int64_t i = 0; i < c.type; ++i)
        if (!type_dst[i]) { z->before = type_off[i]; break; }
    for (int64_t i = 0; i < c.time; ++i) {
        const unsigned t = idx[i];
        if (t >= type_off.size()) return nullptr;
        z->at.push_back(be(times + i * tsize, tsize));
        z->offset_at.push_back(type_off[t]);
    }
    if (tsize == 8) {
        const size_t f = data + blockSize(c, 8);
        if (f < n && b[f] == '\n') {
            const size_t e = bytes.find('\n', f + 1);
            if (e != std::string::npos && e > f + 1) z->footer = mule::parsePosixTz(bytes.substr(f + 1, e - f - 1));
        }
    }
    return z;
}

std::vector<std::string> zoneinfoDirs() {
    std::vector<std::string> out;
    if (const char* d = std::getenv("TZDIR"); d && *d) out.emplace_back(d);
    for (const char* d : {"/usr/share/zoneinfo", "/var/db/timezone/zoneinfo", "/usr/lib/zoneinfo",
                          "/usr/share/lib/zoneinfo"})
        out.emplace_back(d);
    return out;
}

std::string zoneinfoDir() {
    std::error_code ec;
    for (const auto& d : zoneinfoDirs())
        if (std::filesystem::is_directory(d, ec)) return d;
    return "";
}

std::string readFile(const std::filesystem::path& p) {
    std::ifstream in(p, std::ios::binary);
    if (!in) return "";
    std::ostringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

#endif

}  // namespace

namespace zonedb {

std::shared_ptr<const ZoneRules> load(const std::string& name) {
    if (!safeZoneName(name)) return nullptr;
    static std::mutex mu;
    static std::map<std::string, std::shared_ptr<const ZoneRules>> cache;
    std::lock_guard<std::mutex> lk(mu);
    if (auto it = cache.find(name); it != cache.end()) return it->second;
    std::shared_ptr<const ZoneRules> rules;
#if HMS_CPAP_STD_TZDB
    try {
        rules = std::make_shared<StdZone>(std::chrono::locate_zone(name));
    } catch (...) {
        rules = nullptr;
    }
#else
    const std::string dir = zoneinfoDir();
    if (!dir.empty()) {
        std::error_code ec;
        const auto p = std::filesystem::path(dir) / name;
        if (std::filesystem::is_regular_file(p, ec)) rules = parseTzif(readFile(p));
    }
#endif
    if (rules) cache[name] = rules;
    return rules;
}

std::vector<std::string> names() {
    std::set<std::string> out;
#if HMS_CPAP_STD_TZDB
    try {
        for (const auto& z : std::chrono::get_tzdb().zones) out.insert(std::string(z.name()));
    } catch (...) {
    }
#else
    const std::string dir = zoneinfoDir();
    if (!dir.empty()) {
        // The tables name every canonical zone; old aliases stay out of the list
        // (they still load when typed or stored).
        for (const char* tab : {"zone1970.tab", "zone.tab"}) {
            std::ifstream in(std::filesystem::path(dir) / tab);
            std::string line;
            while (std::getline(in, line)) {
                if (line.empty() || line[0] == '#') continue;
                std::istringstream ls(line);
                std::string cc, coord, zone;
                if (ls >> cc >> coord >> zone && safeZoneName(zone)) out.insert(zone);
            }
        }
        std::error_code ec;
        if (std::filesystem::is_regular_file(std::filesystem::path(dir) / "UTC", ec)) out.insert("UTC");
    }
#endif
    return {out.begin(), out.end()};
}

std::string source() {
#if HMS_CPAP_STD_TZDB
    return "std::chrono::tzdb";
#else
    const std::string dir = zoneinfoDir();
    return dir.empty() ? "none" : "zoneinfo:" + dir;
#endif
}

}  // namespace zonedb

// ── MachineZone ──────────────────────────────────────────────────────────────

namespace {

long long hostOffsetAt(long long utc) {
    const std::time_t t = static_cast<std::time_t>(utc);
    std::tm local{};
    if (!localtime_r(&t, &local)) return 0;
    return static_cast<long long>(timegm_utc(&local)) - utc;
}

/// "+10:00", "-05:30", "UTC+10:00", "UTC-5", "+1000": an offset in seconds.
std::optional<int> parseOffset(const std::string& s0, std::string* why) {
    std::string s = s0;
    if (s.rfind("UTC", 0) == 0 || s.rfind("GMT", 0) == 0) s = s.substr(3);
    if (s.size() < 2 || (s[0] != '+' && s[0] != '-')) return std::nullopt;
    const int sign = s[0] == '-' ? -1 : 1;
    std::string digits;
    for (size_t i = 1; i < s.size(); ++i) {
        if (s[i] == ':' && i == s.size() - 3) continue;
        if (!std::isdigit(static_cast<unsigned char>(s[i]))) return std::nullopt;
        digits += s[i];
    }
    int hh = 0, mm = 0;
    if (digits.size() <= 2) hh = std::atoi(digits.c_str());
    else if (digits.size() <= 4) {
        hh = std::atoi(digits.substr(0, digits.size() - 2).c_str());
        mm = std::atoi(digits.substr(digits.size() - 2).c_str());
    } else return std::nullopt;
    const int total = sign * (hh * 3600 + mm * 60);
    if (mm >= 60 || mm % 15 != 0) {
        if (why) *why = "An offset goes in 15 minute steps";
        return -999999;
    }
    if (total < -12 * 3600 || total > 14 * 3600) {
        if (why) *why = "An offset is between UTC-12:00 and UTC+14:00";
        return -999999;
    }
    return total;
}

}  // namespace

MachineZone MachineZone::fixed(int offset_s) {
    MachineZone z;
    z.kind_ = Kind::Fixed;
    z.fixed_s_ = offset_s;
    return z;
}

std::optional<MachineZone> MachineZone::parse(const std::string& setting0, std::string* why) {
    std::string setting = setting0;
    while (!setting.empty() && std::isspace(static_cast<unsigned char>(setting.back()))) setting.pop_back();
    while (!setting.empty() && std::isspace(static_cast<unsigned char>(setting.front()))) setting.erase(0, 1);
    if (setting.empty()) return host();
    if (auto off = parseOffset(setting, why)) {
        if (*off == -999999) return std::nullopt;
        return fixed(*off);
    }
    auto rules = zonedb::load(setting);
    if (!rules) {
        if (why) *why = "Not a time zone this computer knows: " + setting;
        return std::nullopt;
    }
    MachineZone z;
    z.kind_ = Kind::Named;
    z.name_ = setting;
    z.rules_ = std::move(rules);
    return z;
}

std::string MachineZone::offsetText(long long offset_s) {
    const char sign = offset_s < 0 ? '-' : '+';
    const long long a = offset_s < 0 ? -offset_s : offset_s;
    char buf[16];
    std::snprintf(buf, sizeof(buf), "%c%02lld:%02lld", sign, a / 3600, (a % 3600) / 60);
    return buf;
}

std::string MachineZone::isoText(long long utc) const {
    const long long off = offsetAt(utc);
    std::string t = wall::text(utc + off);
    t[10] = 'T';
    return t + offsetText(off);
}

std::string MachineZone::setting() const {
    switch (kind_) {
        case Kind::Host:  return "";
        case Kind::Fixed: return offsetText(fixed_s_);
        case Kind::Named: return name_;
    }
    return "";
}

long long MachineZone::offsetAt(long long utc) const {
    switch (kind_) {
        case Kind::Host:  return hostOffsetAt(utc);
        case Kind::Fixed: return fixed_s_;
        case Kind::Named: return rules_ ? rules_->offsetAt(utc) : 0;
    }
    return 0;
}

long long MachineZone::toInstant(long long wall_label, Resolution* how) const {
    if (how) *how = Resolution::Exact;
    if (kind_ == Kind::Fixed) return wall_label - fixed_s_;
    // The offsets in force half a day either side cover every zone's changes.
    const long long before = offsetAt(wall_label - 50400);
    const long long after = offsetAt(wall_label + 50400);
    std::set<long long> valid;
    for (long long o : {before, after})
        if (offsetAt(wall_label - o) == o) valid.insert(wall_label - o);
    if (how) *how = valid.empty() ? Resolution::Gap : valid.size() > 1 ? Resolution::Fold : Resolution::Exact;
    if (kind_ == Kind::Host) {
        // Exactly what the code did before this SDD.
        std::tm t = wall::fieldsOf(wall_label);
        t.tm_isdst = -1;
        return static_cast<long long>(std::mktime(&t));
    }
    if (!valid.empty()) return *valid.begin();   // a fold: the first occurrence
    // A gap: read with the offset before the jump, which lands after it.
    return wall_label - before;
}

std::string hostZoneName() {
#if HMS_CPAP_STD_TZDB
    try {
        if (const char* tz = std::getenv("TZ"); tz && *tz) return tz[0] == ':' ? tz + 1 : tz;
        return std::string(std::chrono::current_zone()->name());
    } catch (...) {
        return "";
    }
#else
    if (const char* tz = std::getenv("TZ"); tz && *tz) return tz[0] == ':' ? tz + 1 : tz;
    std::error_code ec;
    const auto target = std::filesystem::read_symlink("/etc/localtime", ec);
    if (!ec) {
        const std::string s = target.string();
        const auto at = s.find("zoneinfo/");
        if (at != std::string::npos) return s.substr(at + 9);
    }
    std::ifstream in("/etc/timezone");
    std::string line;
    if (std::getline(in, line)) return line;
    return "";
#endif
}

}  // namespace hms_cpap
