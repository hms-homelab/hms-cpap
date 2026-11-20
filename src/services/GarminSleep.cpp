#include "services/GarminSleep.h"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <vector>

namespace hms_cpap {

using json = nlohmann::json;

namespace {

/// Days from 1970-01-01 to y-m-d in the proleptic Gregorian calendar.
long long daysFromCivil(long long y, unsigned m, unsigned d) {
    y -= m <= 2;
    const long long era = (y >= 0 ? y : y - 399) / 400;
    const unsigned yoe = static_cast<unsigned>(y - era * 400);
    const unsigned doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
    const unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return era * 146097 + static_cast<long long>(doe) - 719468;
}

bool digits(const std::string& s, size_t at, size_t n, int& out) {
    if (at + n > s.size()) return false;
    int v = 0;
    for (size_t i = at; i < at + n; ++i) {
        if (!std::isdigit(static_cast<unsigned char>(s[i]))) return false;
        v = v * 10 + (s[i] - '0');
    }
    out = v;
    return true;
}

/// A number, or a string that is one (not a bool): Garmin's numbers come both ways.
std::optional<double> numberOf(const json& v) {
    if (v.is_number()) return v.get<double>();
    if (v.is_string()) {
        const std::string s = v.get<std::string>();
        if (s.empty()) return std::nullopt;
        char* end = nullptr;
        const double d = std::strtod(s.c_str(), &end);
        if (end == s.c_str()) return std::nullopt;
        while (*end == ' ') ++end;
        if (*end != '\0') return std::nullopt;
        return d;
    }
    return std::nullopt;
}

const json& listOf(const json& sleep, const char* key) {
    static const json empty = json::array();
    if (!sleep.is_object() || !sleep.contains(key) || !sleep[key].is_array()) return empty;
    return sleep[key];
}

struct Hr { long long ms; double v; };
struct Move { long long start, end; double level; };

}  // namespace

std::string garminSessionFilename(long long sleep_start_ms) {
    return "garmin-" + std::to_string(sleep_start_ms);
}

std::optional<long long> parseGarminGmt(const json& v) {
    if (!v.is_string()) return std::nullopt;
    std::string s = v.get<std::string>();
    while (!s.empty() && std::isspace(static_cast<unsigned char>(s.front()))) s.erase(s.begin());
    while (!s.empty() && std::isspace(static_cast<unsigned char>(s.back()))) s.pop_back();
    // YYYY-MM-DD[T ]HH:MM:SS
    int y, mo, d, h, mi, se;
    if (s.size() < 19 || s[4] != '-' || s[7] != '-' || (s[10] != 'T' && s[10] != ' ') ||
        s[13] != ':' || s[16] != ':')
        return std::nullopt;
    if (!digits(s, 0, 4, y) || !digits(s, 5, 2, mo) || !digits(s, 8, 2, d) || !digits(s, 11, 2, h) ||
        !digits(s, 14, 2, mi) || !digits(s, 17, 2, se))
        return std::nullopt;
    if (mo < 1 || mo > 12 || d < 1 || d > 31 || h > 23 || mi > 59 || se > 60) return std::nullopt;
    size_t i = 19;
    long long ms = 0;
    if (i < s.size() && s[i] == '.') {
        ++i;
        const size_t from = i;
        long long frac = 0;
        int n = 0;
        while (i < s.size() && std::isdigit(static_cast<unsigned char>(s[i]))) {
            if (n < 3) { frac = frac * 10 + (s[i] - '0'); ++n; }
            ++i;
        }
        if (i == from) return std::nullopt;
        while (n < 3) { frac *= 10; ++n; }
        ms = frac;
    }
    long long offset_s = 0;   // none: UTC, as the reference reads Garmin's GMT strings
    if (i < s.size()) {
        const char c = s[i];
        if ((c == 'Z' || c == 'z') && i + 1 == s.size()) {
            // UTC
        } else if (c == '+' || c == '-') {
            int oh = 0, om = 0;
            const std::string rest = s.substr(i + 1);
            if (rest.size() == 5 && rest[2] == ':' && digits(rest, 0, 2, oh) && digits(rest, 3, 2, om)) {
            } else if (rest.size() == 4 && digits(rest, 0, 2, oh) && digits(rest, 2, 2, om)) {
            } else if (rest.size() == 2 && digits(rest, 0, 2, oh)) {
            } else {
                return std::nullopt;
            }
            offset_s = (c == '+' ? 1 : -1) * (oh * 3600LL + om * 60LL);
        } else {
            return std::nullopt;
        }
    }
    const long long secs = daysFromCivil(y, static_cast<unsigned>(mo), static_cast<unsigned>(d)) * 86400LL +
                           h * 3600LL + mi * 60LL + se - offset_s;
    return secs * 1000 + ms;
}

std::optional<long long> parseGarminEpochMs(const json& v) {
    const auto d = numberOf(v);
    if (!d || *d < 0 || *d > 1e15) return std::nullopt;
    return static_cast<long long>(*d);
}

GarminNight convertGarminSleep(const json& sleep, long long now_utc,
                               const std::function<long long(long long)>& wall_offset) {
    GarminNight n;
    if (!sleep.is_object()) {
        n.reason = "Garmin returned no sleep data";
        return n;
    }

    // Is Garmin's sleep period over?
    const json& dto = sleep.contains("dailySleepDTO") && sleep["dailySleepDTO"].is_object()
                          ? sleep["dailySleepDTO"] : json::object();
    const auto start = dto.contains("sleepStartTimestampGMT")
                           ? parseGarminEpochMs(dto["sleepStartTimestampGMT"]) : std::nullopt;
    const auto end = dto.contains("sleepEndTimestampGMT")
                         ? parseGarminEpochMs(dto["sleepEndTimestampGMT"]) : std::nullopt;
    if (!start || !end) {
        n.reason = "Garmin has no sleep period for this day yet";
        return n;
    }
    n.sleep_start_ms = *start;
    n.sleep_end_ms   = *end;
    if (*end <= *start || *end > now_utc * 1000 || *end - *start < 3600LL * 1000) {
        n.reason = "Garmin's sleep period is not complete yet";
        return n;
    }

    // SpO2, 50 to 100, each with a readable time.
    std::vector<std::pair<long long, double>> spo2;
    for (const auto& x : listOf(sleep, "wellnessEpochSPO2DataDTOList")) {
        if (!x.is_object() || !x.contains("spo2Reading") || !x["spo2Reading"].is_number()) continue;
        const double v = x["spo2Reading"].get<double>();
        if (v < 50 || v > 100) continue;
        const auto t = x.contains("epochTimestamp") ? parseGarminGmt(x["epochTimestamp"]) : std::nullopt;
        if (!t) continue;
        spo2.emplace_back(*t, v);
    }
    std::stable_sort(spo2.begin(), spo2.end(),
                     [](const auto& a, const auto& b) { return a.first < b.first; });
    n.valid_spo2 = static_cast<int>(spo2.size());
    if (n.valid_spo2 < kGarminMinSpo2) {
        n.verdict = GarminNight::Verdict::TooFewSamples;
        n.reason = "Garmin has " + std::to_string(n.valid_spo2) + " SpO2 samples for this night, fewer than " +
                   std::to_string(kGarminMinSpo2);
        return n;
    }

    // Heart rate, 30 to 220, at epoch milliseconds.
    std::vector<Hr> hr;
    for (const auto& x : listOf(sleep, "sleepHeartRate")) {
        if (!x.is_object() || !x.contains("value") || !x["value"].is_number()) continue;
        const double v = x["value"].get<double>();
        if (v < 30 || v > 220) continue;
        const auto t = x.contains("startGMT") ? parseGarminEpochMs(x["startGMT"]) : std::nullopt;
        if (!t) continue;
        hr.push_back({*t, v});
    }
    std::stable_sort(hr.begin(), hr.end(), [](const Hr& a, const Hr& b) { return a.ms < b.ms; });

    // Movement, by interval.
    std::vector<Move> moves;
    for (const auto& x : listOf(sleep, "sleepMovement")) {
        if (!x.is_object()) continue;
        const auto a = x.contains("startGMT") ? parseGarminGmt(x["startGMT"]) : std::nullopt;
        const auto b = x.contains("endGMT") ? parseGarminGmt(x["endGMT"]) : std::nullopt;
        const auto level = x.contains("activityLevel") ? numberOf(x["activityLevel"]) : std::nullopt;
        if (!a || !b || !level || *a >= *b || !(*level > -1e9 && *level < 1e9)) continue;
        moves.push_back({*a, *b, *level});
    }
    std::stable_sort(moves.begin(), moves.end(), [](const Move& a, const Move& b) { return a.start < b.start; });

    const auto wall = [&wall_offset](long long utc_ms) {
        const long long utc_s = utc_ms / 1000;
        const long long off = wall_offset ? wall_offset(utc_s) : 0;
        return std::chrono::system_clock::time_point(std::chrono::milliseconds(utc_ms + off * 1000));
    };

    auto& s = n.session;
    s.filename = garminSessionFilename(*start);
    for (const auto& [t, v] : spo2) {
        cpapdash::parser::OximetrySample smp{};
        smp.timestamp    = wall(t);
        smp.spo2         = static_cast<uint8_t>(static_cast<int>(v));
        // No pulse within 2 minutes: none, not a guess. Validity is per field
        // (decided 2026-11-19, SDD-078): the SpO2 reading counts on its own,
        // so the pulse is left blank as 0 (the CSV writer's "no reading"),
        // never 0xFF, which OximetrySample::valid() reads as the whole
        // sample being invalid.
        smp.heart_rate   = kGarminNoHeartRate;
        smp.invalid_flag = 0;
        smp.motion       = 0;
        smp.vibration    = 0;
        // The nearest heart rate, within 2 minutes.
        if (!hr.empty()) {
            const auto it = std::lower_bound(hr.begin(), hr.end(), t,
                                             [](const Hr& h, long long ms) { return h.ms < ms; });
            const Hr* best = nullptr;
            if (it != hr.begin()) best = &*(it - 1);
            if (it != hr.end() && (!best || std::llabs(it->ms - t) < std::llabs(best->ms - t))) best = &*it;
            if (best && std::llabs(best->ms - t) <= kGarminHrPairSeconds * 1000LL) {
                smp.heart_rate = static_cast<uint8_t>(static_cast<int>(best->v));
                ++n.hr_paired;
            }
        }
        for (const auto& m : moves) {
            if (m.start <= t && t < m.end) {
                const long long lv = static_cast<long long>(m.level);   // as the CSV path reads it
                smp.motion = static_cast<uint8_t>(std::clamp<long long>(lv, 0, 255));
                ++n.motion_paired;
                break;
            }
        }
        s.samples.push_back(smp);
        n.sample_utc_s.push_back(t / 1000);   // SDD-079 D4: the instant, for hms-json clock "utc"
    }

    // Garmin's SpO2 epoch is a minute; the samples say what this night's is.
    double interval = 60;
    if (spo2.size() > 1) {
        std::vector<long long> gaps;
        for (size_t i = 1; i < spo2.size(); ++i)
            if (spo2[i].first > spo2[i - 1].first) gaps.push_back(spo2[i].first - spo2[i - 1].first);
        if (!gaps.empty()) {
            std::nth_element(gaps.begin(), gaps.begin() + static_cast<long>(gaps.size() / 2), gaps.end());
            interval = static_cast<double>(gaps[gaps.size() / 2]) / 1000.0;
        }
    }
    s.start_time       = wall(*start);
    s.end_time         = wall(*end);
    s.duration_seconds = static_cast<int>((*end - *start) / 1000);
    s.sample_interval  = interval;
    s.metrics          = cpapdash::parser::VLDParser::calculateMetrics(s.samples, interval);
    // Per field: the pulse figures are of the samples that have a pulse, so
    // a blank one never pulls the average or the minimum down to 0.
    {
        long long sum = 0;
        int count = 0, lo = 300, hi = 0;
        for (const auto& smp : s.samples) {
            if (smp.heart_rate == kGarminNoHeartRate || smp.heart_rate == 0xFF) continue;
            sum += smp.heart_rate;
            ++count;
            lo = std::min(lo, static_cast<int>(smp.heart_rate));
            hi = std::max(hi, static_cast<int>(smp.heart_rate));
        }
        s.metrics.avg_hr = count ? static_cast<double>(sum) / count : 0;
        s.metrics.min_hr = count ? lo : 300;
        s.metrics.max_hr = hi;
    }
    n.verdict = GarminNight::Verdict::Ready;
    n.reason  = "ready";
    return n;
}

const char* garminVerdictName(GarminNight::Verdict v) {
    switch (v) {
        case GarminNight::Verdict::Ready:         return "ready";
        case GarminNight::Verdict::Incomplete:    return "incomplete";
        case GarminNight::Verdict::TooFewSamples: return "too_few_samples";
    }
    return "incomplete";
}

}  // namespace hms_cpap
