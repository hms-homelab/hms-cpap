#include "services/reports/figures/ClinicianFigures.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <map>
#include <set>
#include <sstream>

namespace hms_cpap::clinician {

// ---- Calendar ----------------------------------------------------------------

namespace {

// Howard Hinnant's days_from_civil / civil_from_days: exact for the proleptic
// Gregorian calendar, no clock and no zone involved.
long daysFromCivil(int y, unsigned m, unsigned d) {
    y -= m <= 2;
    const long era = (y >= 0 ? y : y - 399) / 400;
    const unsigned yoe = static_cast<unsigned>(y - era * 400);
    const unsigned doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
    const unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return era * 146097 + static_cast<long>(doe) - 719468;
}

void civilFromDays(long z, int& y, unsigned& m, unsigned& d) {
    z += 719468;
    const long era = (z >= 0 ? z : z - 146096) / 146097;
    const unsigned doe = static_cast<unsigned>(z - era * 146097);
    const unsigned yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    const long yy = static_cast<long>(yoe) + era * 400;
    const unsigned doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    const unsigned mp = (5 * doy + 2) / 153;
    d = doy - (153 * mp + 2) / 5 + 1;
    m = mp < 10 ? mp + 3 : mp - 9;
    y = static_cast<int>(yy + (m <= 2));
}

bool allDigits(const std::string& s, std::size_t from, std::size_t n) {
    if (s.size() < from + n) return false;
    for (std::size_t i = from; i < from + n; ++i)
        if (s[i] < '0' || s[i] > '9') return false;
    return true;
}

unsigned lastDayOf(int y, unsigned m) {
    static const unsigned kDays[] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
    if (m == 2) {
        const bool leap = (y % 4 == 0 && y % 100 != 0) || y % 400 == 0;
        return leap ? 29 : 28;
    }
    return kDays[m - 1];
}

std::optional<double> numOf(const Json::Value& row, const char* key) {
    if (!row.isObject() || !row.isMember(key)) return std::nullopt;
    const Json::Value& v = row[key];
    if (v.isNull()) return std::nullopt;
    if (v.isNumeric()) {
        const double d = v.asDouble();
        return std::isfinite(d) ? std::optional<double>(d) : std::nullopt;
    }
    if (v.isString()) {
        const std::string s = v.asString();
        if (s.empty()) return std::nullopt;
        char* endp = nullptr;
        const double d = std::strtod(s.c_str(), &endp);
        if (endp == s.c_str() || !std::isfinite(d)) return std::nullopt;
        return d;
    }
    return std::nullopt;
}

/// The machine's pressure for one statistic (issue #56): the delivered
/// pressure when the night has it, else the mask's. A 0 is a machine without
/// the channel, not a pressure, so it is absent here (issue 15).
std::optional<double> machinePressureOf(const Json::Value& row, const std::string& stat) {
    const auto therapy = numOf(row, ("therapy_press_" + stat).c_str());
    if (therapy && *therapy > 0) return therapy;
    const auto mask = numOf(row, ("mask_press_" + stat).c_str());
    if (mask && *mask > 0) return mask;
    return std::nullopt;
}

/// An hours-weighted mean of `get` over the nights that carry it.
template <typename Get>
std::optional<double> weighted(const std::vector<const Night*>& ns, Get get) {
    double sum = 0, w = 0;
    for (const auto* n : ns) {
        const auto v = get(*n);
        if (!v || n->minutes <= 0) continue;
        sum += *v * n->minutes;
        w += n->minutes;
    }
    if (w <= 0) return std::nullopt;
    return sum / w;
}

template <typename Get>
std::optional<double> extreme(const std::vector<const Night*>& ns, Get get, bool max) {
    std::optional<double> out;
    for (const auto* n : ns) {
        const auto v = get(*n);
        if (!v) continue;
        if (!out || (max ? *v > *out : *v < *out)) out = v;
    }
    return out;
}

double median(std::vector<double> v) {
    if (v.empty()) return 0;
    std::sort(v.begin(), v.end());
    const std::size_t n = v.size();
    return n % 2 ? v[n / 2] : (v[n / 2 - 1] + v[n / 2]) / 2.0;
}

std::string fmtPressure(double p) {
    char buf[16];
    std::snprintf(buf, sizeof(buf), "%.1f", p);
    return buf;
}

}  // namespace

std::optional<long> dayNumber(const std::string& ymd) {
    if (ymd.size() < 10 || !allDigits(ymd, 0, 4) || ymd[4] != '-' || !allDigits(ymd, 5, 2) ||
        ymd[7] != '-' || !allDigits(ymd, 8, 2))
        return std::nullopt;
    const int y = std::atoi(ymd.substr(0, 4).c_str());
    const unsigned m = static_cast<unsigned>(std::atoi(ymd.substr(5, 2).c_str()));
    const unsigned d = static_cast<unsigned>(std::atoi(ymd.substr(8, 2).c_str()));
    if (m < 1 || m > 12 || d < 1 || d > lastDayOf(y, m)) return std::nullopt;
    return daysFromCivil(y, m, d);
}

std::string dateOf(long day) {
    int y;
    unsigned m, d;
    civilFromDays(day, y, m, d);
    char buf[16];
    std::snprintf(buf, sizeof(buf), "%04d-%02u-%02u", y, m, d);
    return buf;
}

std::string addMonths(const std::string& ymd, int months) {
    const auto day = dayNumber(ymd);
    if (!day) return ymd;
    int y;
    unsigned m, d;
    civilFromDays(*day, y, m, d);
    long total = static_cast<long>(y) * 12 + (m - 1) + months;
    const int ny = static_cast<int>(total >= 0 ? total / 12 : (total - 11) / 12);
    const unsigned nm = static_cast<unsigned>(total - static_cast<long>(ny) * 12) + 1;
    const unsigned nd = std::min(d, lastDayOf(ny, nm));
    return dateOf(daysFromCivil(ny, nm, nd));
}

// ---- One night ------------------------------------------------------------------

std::optional<Night> nightFrom(const Json::Value& row) {
    if (!row.isObject()) return std::nullopt;
    const std::string raw = row.get("record_date", "").asString();
    if (!dayNumber(raw)) return std::nullopt;
    Night n;
    n.date    = raw.substr(0, 10);
    n.minutes = numOf(row, "duration_minutes").value_or(0);
    n.ahi = numOf(row, "ahi");
    n.oai = numOf(row, "oai");
    n.hi  = numOf(row, "hi");
    n.uai = numOf(row, "uai");
    n.cai = numOf(row, "cai");
    n.rin = numOf(row, "rin");
    n.csr_minutes = numOf(row, "csr");
    n.leak_50  = numOf(row, "leak_50");
    n.leak_95  = numOf(row, "leak_95");
    n.leak_max = numOf(row, "leak_max");
    n.press_50  = machinePressureOf(row, "50");
    n.press_95  = machinePressureOf(row, "95");
    n.press_max = machinePressureOf(row, "max");
    n.press_min = numOf(row, "press_min");
    n.epap_min = numOf(row, "epap_min");
    n.epap_avg = numOf(row, "epap_avg");
    n.epap_max = numOf(row, "epap_max");
    n.flow_lim_95 = numOf(row, "flow_lim_95");
    n.large_leak_minutes   = numOf(row, "large_leak_minutes");
    n.leak_over_24_minutes = numOf(row, "leak_over_24_minutes");
    n.family = static_cast<int>(numOf(row, "family").value_or(0));
    if (const auto m = numOf(row, "mode")) n.mode = static_cast<int>(*m);
    n.epr_level        = numOf(row, "epr_level");
    n.pressure_setting = numOf(row, "pressure_setting");
    n.apap_min = numOf(row, "apap_min_pressure");
    n.apap_max = numOf(row, "apap_max_pressure");
    n.bl_min_epap = numOf(row, "bl_min_epap");
    n.bl_max_ipap = numOf(row, "bl_max_ipap");
    n.bl_ps   = numOf(row, "bl_ps");
    n.bl_ipap = numOf(row, "bl_ipap");
    n.bl_epap = numOf(row, "bl_epap");
    n.ungraded = row.isMember("index_kind") && row["index_kind"].isString() &&
                 row["index_kind"].asString() == "ungraded";
    return n;
}

std::vector<Night> nightsFrom(const Json::Value& rows) {
    std::map<std::string, Night> byDate;
    if (rows.isArray())
        for (const auto& r : rows)
            if (auto n = nightFrom(r)) byDate[n->date] = std::move(*n);
    std::vector<Night> out;
    out.reserve(byDate.size());
    for (auto& [d, n] : byDate) out.push_back(std::move(n));
    return out;
}

// ---- One window -----------------------------------------------------------------

WindowFigures figuresFor(const std::vector<Night>& nights, const Window& w,
                         double threshold_minutes) {
    WindowFigures f;
    const auto s = dayNumber(w.start);
    const auto e = dayNumber(w.end);
    if (!s || !e || *e < *s) return f;
    f.total_days = static_cast<int>(*e - *s + 1);

    std::vector<const Night*> used;
    for (const auto& n : nights) {
        const auto d = dayNumber(n.date);
        if (!d || *d < *s || *d > *e || n.minutes <= 0) continue;
        used.push_back(&n);
    }
    f.used_days = static_cast<int>(used.size());
    std::vector<double> hours, ahis;
    double total_hours = 0;
    for (const auto* n : used) {
        const double h = n->minutes / 60.0;
        hours.push_back(h);
        total_hours += h;
        if (n->minutes >= threshold_minutes) ++f.days_4h; else ++f.days_under_4h;
        if (n->ahi) ahis.push_back(*n->ahi);
    }
    if (f.total_days > 0) f.pct_total_4h = 100.0 * f.days_4h / f.total_days;
    if (f.used_days > 0) {
        f.pct_used_4h  = 100.0 * f.days_4h / f.used_days;
        f.avg_hours    = total_hours / f.used_days;
        f.median_hours = median(hours);
    }
    if (used.empty()) return f;

    f.ahi = weighted(used, [](const Night& n) { return n.ahi; });
    if (!ahis.empty()) f.ahi_median = median(ahis);
    f.oai = weighted(used, [](const Night& n) { return n.oai; });
    f.hi  = weighted(used, [](const Night& n) { return n.hi; });
    f.uai = weighted(used, [](const Night& n) { return n.uai; });
    f.cai = weighted(used, [](const Night& n) { return n.cai; });
    f.rin = weighted(used, [](const Night& n) { return n.rin; });
    f.flow_lim_95 = weighted(used, [](const Night& n) { return n.flow_lim_95; });

    double csr = 0, csr_minutes = 0;
    bool any_csr = false;
    for (const auto* n : used)
        if (n->csr_minutes) { any_csr = true; csr += *n->csr_minutes; csr_minutes += n->minutes; }
    if (any_csr && csr_minutes > 0) f.csr_pct = 100.0 * csr / csr_minutes;

    f.leak_median = weighted(used, [](const Night& n) { return n.leak_50; });
    f.leak_95     = weighted(used, [](const Night& n) { return n.leak_95; });

    // The machine's flags where it writes them, the 24 L/min line where it
    // does not. A night with neither does not count toward the share.
    double leak = 0, leak_minutes = 0;
    for (const auto* n : used) {
        if (n->large_leak_minutes) {
            leak += *n->large_leak_minutes;
            leak_minutes += n->minutes;
            f.large_leak_uses_flags = true;
        } else if (n->leak_over_24_minutes) {
            leak += *n->leak_over_24_minutes;
            leak_minutes += n->minutes;
            f.large_leak_uses_line = true;
        }
    }
    if (leak_minutes > 0) f.large_leak_pct = std::min(100.0, 100.0 * leak / leak_minutes);

    f.press_median = weighted(used, [](const Night& n) { return n.press_50; });
    f.press_95     = weighted(used, [](const Night& n) { return n.press_95; });
    f.press_max    = extreme(used, [](const Night& n) { return n.press_max; }, true);
    f.press_min    = extreme(used, [](const Night& n) { return n.press_min; }, false);
    f.epap_avg     = weighted(used, [](const Night& n) { return n.epap_avg; });
    f.epap_min     = extreme(used, [](const Night& n) { return n.epap_min; }, false);
    f.epap_max     = extreme(used, [](const Night& n) { return n.epap_max; }, true);
    return f;
}

std::optional<Night> lastNight(const std::vector<Night>& nights, const std::string& end) {
    const auto e = dayNumber(end);
    if (!e) return std::nullopt;
    std::optional<Night> out;
    for (const auto& n : nights) {
        const auto d = dayNumber(n.date);
        if (!d || *d > *e || n.minutes <= 0) continue;
        if (!out || n.date > out->date) out = n;
    }
    return out;
}

std::optional<Night> worstNight(const std::vector<Night>& nights, const std::string& start,
                                const std::string& end) {
    const auto s = dayNumber(start);
    const auto e = dayNumber(end);
    if (!s || !e) return std::nullopt;
    std::optional<Night> out;
    for (const auto& n : nights) {
        const auto d = dayNumber(n.date);
        if (!d || *d < *s || *d > *e || n.minutes <= 0 || !n.ahi) continue;
        if (!out || *n.ahi > *out->ahi || (*n.ahi == *out->ahi && n.date > out->date)) out = n;
    }
    return out;
}

std::vector<Window> standardWindows(const std::vector<Night>& nights, const std::string& start,
                                    const std::string& end, const std::string& first_night) {
    std::vector<Window> out;
    const auto e = dayNumber(end);
    if (!e) return out;
    const auto first = dayNumber(first_night);
    const auto keep = [&](const std::string& from) {
        const auto f = dayNumber(from);
        return f && (!first || *f >= *first);
    };
    if (const auto last = lastNight(nights, end))
        out.push_back({"Last night", last->date, last->date});
    const std::string d7  = dateOf(*e - 6);
    const std::string d30 = dateOf(*e - 29);
    const std::string m6  = dateOf(*dayNumber(addMonths(end, -6)) + 1);
    const std::string y1  = dateOf(*dayNumber(addMonths(end, -12)) + 1);
    if (keep(d7))  out.push_back({"Last 7 days", d7, end});
    if (keep(d30)) out.push_back({"Last 30 days", d30, end});
    if (keep(m6))  out.push_back({"Last 6 months", m6, end});
    if (keep(y1))  out.push_back({"Last year", y1, end});
    bool duplicate = false;
    for (const auto& w : out) duplicate |= (w.start == start && w.end == end);
    if (!duplicate && dayNumber(start) && start <= end) out.push_back({"Your range", start, end});
    return out;
}

std::vector<Window> monthlyWindows(const std::string& end, const std::string& first_night) {
    std::vector<Window> out;
    const auto e = dayNumber(end);
    if (!e) return out;
    const auto first = dayNumber(first_night);
    for (int i = 0; i < 12; ++i) {
        const std::string anchor = addMonths(end.substr(0, 8) + "01", -i);
        const auto s = dayNumber(anchor);
        int y;
        unsigned m, d;
        civilFromDays(*s, y, m, d);
        const long month_end = daysFromCivil(y, m, lastDayOf(y, m));
        const long last = std::min(month_end, *e);
        if (first && last < *first) break;
        out.push_back({anchor.substr(0, 7), anchor, dateOf(last)});
    }
    return out;
}

// ---- Settings periods ----------------------------------------------------------

std::string therapyLabel(int family, std::optional<int> mode) {
    if (!mode) return "";
    // family: 0 unknown 1 CPAP 2 AutoSet 3 bi-level 4 ASV. A name only where
    // the family and the mode agree; never from the number alone.
    static const std::map<int, std::map<int, std::string>> kLabels = {
        {1, {{0, "CPAP"}}},
        {2, {{0, "CPAP"}, {1, "APAP"}}},
        {4, {{7, "ASV"}, {8, "ASVAuto"}}},
        {3, {{6, "VAuto"}, {8, "VAuto"}}},
    };
    const auto f = kLabels.find(family);
    if (f == kLabels.end()) return "";
    const auto m = f->second.find(*mode);
    return m == f->second.end() ? "" : m->second;
}

namespace {

struct SettingsKey {
    std::string mode_label, pressure, relief;
    bool operator==(const SettingsKey& o) const {
        return mode_label == o.mode_label && pressure == o.pressure && relief == o.relief;
    }
};

SettingsKey keyOf(const Night& n) {
    SettingsKey k;
    k.mode_label = therapyLabel(n.family, n.mode);
    if (k.mode_label == "APAP" && n.apap_min && n.apap_max) {
        k.pressure = fmtPressure(*n.apap_min) + " - " + fmtPressure(*n.apap_max) + " cmH2O";
    } else if (k.mode_label == "CPAP" && n.pressure_setting && *n.pressure_setting > 0) {
        k.pressure = fmtPressure(*n.pressure_setting) + " cmH2O";
    } else if (n.family == 3 && n.bl_min_epap && n.bl_max_ipap) {
        k.pressure = "EPAP " + fmtPressure(*n.bl_min_epap) + ", IPAP " +
                     fmtPressure(*n.bl_max_ipap) + " cmH2O";
        if (n.bl_ps) k.pressure += ", PS " + fmtPressure(*n.bl_ps);
    } else if (n.apap_min && n.apap_max) {
        k.pressure = fmtPressure(*n.apap_min) + " - " + fmtPressure(*n.apap_max) + " cmH2O";
    } else if (n.pressure_setting && *n.pressure_setting > 0) {
        k.pressure = fmtPressure(*n.pressure_setting) + " cmH2O";
    }
    if (n.epr_level) {
        const int level = static_cast<int>(std::lround(*n.epr_level));
        k.relief = level > 0 ? "EPR " + std::to_string(level) : "EPR off";
    }
    return k;
}

}  // namespace

std::vector<SettingsPeriod> settingsPeriods(const std::vector<Night>& nights) {
    std::vector<const Night*> sorted;
    for (const auto& n : nights)
        if (n.minutes > 0) sorted.push_back(&n);
    std::sort(sorted.begin(), sorted.end(),
              [](const Night* a, const Night* b) { return a->date < b->date; });

    std::vector<SettingsPeriod> out;
    std::vector<const Night*> run;
    SettingsKey runKey;
    const auto flush = [&]() {
        if (run.empty()) return;
        SettingsPeriod p;
        p.first = run.front()->date;
        p.last  = run.back()->date;
        p.nights = static_cast<int>(run.size());
        p.mode_label = runKey.mode_label;
        p.pressure = runKey.pressure;
        p.relief = runKey.relief;
        p.ahi = weighted(run, [](const Night& n) { return n.ahi; });
        p.flow_lim_95 = weighted(run, [](const Night& n) { return n.flow_lim_95; });
        p.leak_95 = weighted(run, [](const Night& n) { return n.leak_95; });
        double h = 0;
        for (const auto* n : run) h += n->minutes / 60.0;
        p.avg_hours = h / run.size();
        out.push_back(p);
        run.clear();
    };
    for (const auto* n : sorted) {
        const SettingsKey k = keyOf(*n);
        // A night whose settings nobody recorded belongs to no period; counted
        // as a period of its own it would split the stretch it sits in.
        if (k.mode_label.empty() && k.pressure.empty() && k.relief.empty()) continue;
        if (!run.empty() && !(k == runKey)) flush();
        if (run.empty()) runKey = k;
        run.push_back(n);
    }
    flush();
    return out;
}

// ---- CMS adherence ---------------------------------------------------------------

CmsAdherence cmsAdherence(const std::vector<Night>& nights, const std::string& first_night,
                          const std::string& today) {
    CmsAdherence c;
    const auto f = dayNumber(first_night);
    const auto t = dayNumber(today);
    if (!f || !t) return c;
    c.known = true;
    c.first_night = dateOf(*f);
    c.day_90 = dateOf(*f + 89);
    c.complete = *f + 89 <= *t;

    std::set<long> good;   // days with >= 4 h
    for (const auto& n : nights) {
        const auto d = dayNumber(n.date);
        if (d && *d >= *f && *d <= *f + 89 && n.minutes >= 240) good.insert(*d);
    }
    // Every 30-day window inside the 90 that has begun; a window still running
    // counts the days so far against all 30.
    const long last_start = std::min(*f + 60, *t);
    int best = -1;
    long best_s = *f;
    for (long s = *f; s <= last_start; ++s) {
        int count = 0;
        for (long d = s; d < s + 30; ++d) count += good.count(d) ? 1 : 0;
        if (count > best) { best = count; best_s = s; }
    }
    if (best < 0) best = 0;
    c.best_start = dateOf(best_s);
    c.best_end = dateOf(best_s + 29);
    c.best_nights_4h = best;
    c.best_pct = 100.0 * best / 30.0;
    c.met = best >= 21;
    return c;
}

// ---- One night's page --------------------------------------------------------------

std::optional<long> epochOf(const std::string& ts) {
    // YYYY-MM-DD[ T]HH:MM:SS[.fff][+HH[:MM]|-HH[:MM]|Z]
    if (ts.size() < 19) return std::nullopt;
    const auto day = dayNumber(ts.substr(0, 10));
    if (!day || (ts[10] != ' ' && ts[10] != 'T') || !allDigits(ts, 11, 2) || ts[13] != ':' ||
        !allDigits(ts, 14, 2) || ts[16] != ':' || !allDigits(ts, 17, 2))
        return std::nullopt;
    long secs = *day * 86400L + std::atol(ts.substr(11, 2).c_str()) * 3600L +
                std::atol(ts.substr(14, 2).c_str()) * 60L + std::atol(ts.substr(17, 2).c_str());
    std::size_t i = 19;
    if (i < ts.size() && ts[i] == '.') {
        ++i;
        while (i < ts.size() && ts[i] >= '0' && ts[i] <= '9') ++i;
    }
    if (i >= ts.size() || ts[i] == 'Z') return secs;
    if (ts[i] != '+' && ts[i] != '-') return std::nullopt;
    const int sign = ts[i] == '+' ? 1 : -1;
    if (!allDigits(ts, i + 1, 2)) return std::nullopt;
    long off = std::atol(ts.substr(i + 1, 2).c_str()) * 3600L;
    std::size_t j = i + 3;
    if (j < ts.size() && ts[j] == ':') ++j;
    if (allDigits(ts, j, 2)) off += std::atol(ts.substr(j, 2).c_str()) * 60L;
    return secs - sign * off;
}

std::vector<Event> eventsFrom(const Json::Value& rows) {
    std::vector<Event> out;
    if (!rows.isArray()) return out;
    for (const auto& r : rows) {
        if (!r.isObject()) continue;
        const auto t = epochOf(r.get("event_timestamp", "").asString());
        if (!t) continue;
        Event e;
        e.type = r.get("event_type", "").asString();
        e.t = *t;
        e.duration = numOf(r, "duration_seconds").value_or(0);
        out.push_back(e);
    }
    std::sort(out.begin(), out.end(), [](const Event& a, const Event& b) { return a.t < b.t; });
    return out;
}

const std::vector<FlagRow>& flagRows() {
    // The machine's own names as stored: ResMed writes "Obstructive",
    // "Central", "Apnea"; other readers "Obstructive Apnea", "Clear Airway".
    static const std::vector<FlagRow> kRows = {
        {"CSR", {"CSR", "Cheyne Stokes", "Periodic Breathing"}},
        {"LL", {"Large Leak"}},
        {"CA", {"Central", "Central Apnea", "Clear Airway"}},
        {"OA", {"Obstructive", "Obstructive Apnea"}},
        {"UA", {"Apnea", "Unclassified Apnea"}},
        {"H", {"Hypopnea"}},
        {"RE", {"RERA"}},
        {"FL", {"Flow Limitation"}},
        {"VS", {"Vibratory Snore"}},
    };
    return kRows;
}

std::vector<int> flagCounts(const std::vector<Event>& events) {
    const auto& rows = flagRows();
    std::vector<int> counts(rows.size(), 0);
    for (const auto& e : events)
        for (std::size_t i = 0; i < rows.size(); ++i)
            if (std::find(rows[i].types.begin(), rows[i].types.end(), e.type) !=
                rows[i].types.end())
                ++counts[i];
    return counts;
}

std::vector<std::pair<long, double>> runningAhi(const std::vector<Event>& events, long t0) {
    static const std::set<std::string> kApneaHypopnea = {
        "Obstructive", "Obstructive Apnea", "Central", "Central Apnea", "Clear Airway",
        "Apnea", "Unclassified Apnea", "Hypopnea"};
    std::vector<std::pair<long, double>> out;
    int n = 0;
    for (const auto& e : events) {
        if (!kApneaHypopnea.count(e.type) || e.t <= t0) continue;
        ++n;
        // At least ten minutes in: three events in the first minute is not an
        // AHI of 180, it is a night that has not had time to mean anything.
        const double hours = std::max(1.0 / 6.0, (e.t - t0) / 3600.0);
        out.push_back({e.t, n / hours});
    }
    return out;
}

std::vector<std::pair<double, double>> timeAtPressure(const std::vector<double>& per_minute) {
    std::map<double, double> bins;
    for (const double p : per_minute) {
        if (!(p > 0) || !std::isfinite(p)) continue;
        bins[std::floor(p * 2.0) / 2.0] += 1.0;
    }
    return std::vector<std::pair<double, double>>(bins.begin(), bins.end());
}

std::optional<double> percentileCont(std::vector<double> values, double p) {
    values.erase(std::remove_if(values.begin(), values.end(),
                                [](double v) { return !std::isfinite(v); }),
                 values.end());
    if (values.empty()) return std::nullopt;
    std::sort(values.begin(), values.end());
    const double pos = p * static_cast<double>(values.size() - 1);
    const auto lo = static_cast<std::size_t>(std::floor(pos));
    const auto hi = static_cast<std::size_t>(std::ceil(pos));
    const double frac = pos - static_cast<double>(lo);
    return values[lo] + (values[hi] - values[lo]) * frac;
}

}  // namespace hms_cpap::clinician
