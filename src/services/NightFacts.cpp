#include "services/NightFacts.h"

#include "services/BilevelSensors.h"
#include "utils/OximetryDevice.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <ctime>
#include <iomanip>
#include <sstream>

namespace hms_cpap {

namespace night_facts {

const char* const kEventLabelRule =
    "Report each event type only under the label it has here. An unclassified "
    "apnea is an apnea the machine did not classify: never describe it as "
    "obstructive, central or a hypopnea.";

const char* const kUngradedRule =
    "A night marked \"AHI: not available\" comes from a machine that does not "
    "detect hypopneas: no apnea-hypopnea index exists for it. Do not estimate "
    "one, do not describe its events as mild, normal or severe, and do not "
    "report any event type as zero.";

const char* const kNoGapRule =
    "Use only the figures given here. A figure that is not given was not "
    "recorded: say it is not available, never estimate or fill it in.";

}  // namespace night_facts

namespace {

double round2(double v) { return std::round(v * 100.0) / 100.0; }

// YYYY-MM-DD -> local noon of that day.
bool noonOf(const std::string& day, std::tm& out) {
    int y = 0, mo = 0, d = 0;
    if (!night_facts::isIsoDay(day) || std::sscanf(day.c_str(), "%4d-%2d-%2d", &y, &mo, &d) != 3)
        return false;
    out = std::tm{};
    out.tm_year = y - 1900;
    out.tm_mon = mo - 1;
    out.tm_mday = d;
    out.tm_hour = 12;
    out.tm_isdst = -1;
    std::mktime(&out);
    return true;
}

std::string compact(const std::tm& tm) {
    char buf[9];
    std::strftime(buf, sizeof(buf), "%Y%m%d", &tm);
    return buf;
}

std::string compactDay(std::string day) {
    day.erase(std::remove(day.begin(), day.end(), '-'), day.end());
    return day;
}

void putOpt(nlohmann::json& j, const char* key, const std::optional<double>& v) {
    if (v) j[key] = round2(*v);
}

}  // namespace

NightFacts NightFacts::from(const SessionMetrics& m, const STRDailyRecord* str,
                            const std::optional<NightOximetryFacts>& oximetry) {
    NightFacts f;
    f.sleep_day = m.sleep_day;
    f.graded = m.index_kind == SessionMetrics::IndexKind::AHI;
    f.index = m.ahi;
    f.total = m.total_events;
    if (f.graded) {
        f.obstructive = m.obstructive_apneas;
        f.central = m.central_apneas;
        f.clear_airway = m.clear_airway_apneas;
        f.unclassified = m.unclassified_apneas;
        f.hypopnea = m.hypopneas;
        f.reras = m.reras;
        // Issue #78: what total_events holds beyond the index and the RERAs,
        // so the parts the model is given add up to the total it is given.
        f.other = (std::max)(0, m.total_events - f.inIndex() - m.reras);
    }

    f.usage_hours = m.usage_hours;
    f.usage_percent = m.usage_percent;
    f.avg_event_duration = m.avg_event_duration;
    if (m.avg_event_duration) f.max_event_duration = m.max_event_duration;
    f.therapy_pressure = m.avg_therapy_pressure;
    f.pressure_avg = m.avg_pressure;
    f.pressure_min = m.min_pressure;
    f.pressure_max = m.max_pressure;
    f.pressure_p95 = m.pressure_p95;
    f.leak_avg = m.avg_leak_rate;
    f.leak_max = m.max_leak_rate;
    f.leak_p95 = m.leak_p95;
    f.leak_p50 = m.leak_p50;
    f.mask_pressure = m.avg_mask_pressure;
    f.epr_pressure = m.avg_epr_pressure;
    f.snore = m.avg_snore;
    if (m.avg_target_ventilation && *m.avg_target_ventilation > 0)
        f.target_ventilation = m.avg_target_ventilation;
    f.respiratory_rate = m.avg_respiratory_rate;
    f.tidal_volume = m.avg_tidal_volume;
    f.minute_ventilation = m.avg_minute_ventilation;
    f.flow_limitation = m.avg_flow_limitation;

    // SDD-030, the standing rule: the session's mode first, the STR's when
    // the session's is 0, named through the machine family.
    {
        const auto family = str ? str->family : MachineFamily::Unknown;
        const auto mode = therapyModeFor(
            m.therapy_mode, str ? std::optional<int>(str->mode) : std::nullopt);
        if (mode) f.therapy_mode = therapyModeName(*mode, family);
    }

    if (str) {
        NightStrFacts s;
        s.ahi = str->ahi;
        s.mask_on_off_pairs = str->mask_events / 2;
        s.leak_95 = str->leak_95;
        s.mask_press_95 = str->mask_press_95;
        if (str->asv_epap) {
            s.asv_epap = str->asv_epap;
            s.asv_min_ps = str->asv_min_ps.value_or(0);
            s.asv_max_ps = str->asv_max_ps.value_or(0);
        }
        if (str->tgt_ipap_50) {
            s.tgt_ipap_50 = str->tgt_ipap_50;
            s.tgt_epap_50 = str->tgt_epap_50.value_or(0);
            s.tgt_vent_50 = str->tgt_vent_50.value_or(0);
        }
        f.str = s;
    }
    f.oximetry = oximetry;
    return f;
}

namespace night_facts {

std::vector<std::string> rules(const std::vector<NightFacts>& nights) {
    bool any_graded = false, any_ungraded = false;
    for (const auto& n : nights) (n.graded ? any_graded : any_ungraded) = true;
    std::vector<std::string> out;
    if (any_graded) out.emplace_back(kEventLabelRule);
    if (any_ungraded) out.emplace_back(kUngradedRule);
    out.emplace_back(kNoGapRule);
    return out;
}

std::string renderNight(const NightFacts& n) {
    std::ostringstream oss;
    oss << std::fixed << std::setprecision(2);

    if (n.usage_hours) {
        oss << "Usage: " << *n.usage_hours << " hours";
        if (n.usage_percent) oss << " (" << *n.usage_percent << "% of 8h target)";
        oss << "\n";
    } else {
        oss << "Usage: not recorded\n";
    }

    // The index only goes in when it IS an AHI (SDD-024): an apnea count per
    // hour handed over as "AHI" comes back out as "your AHI is excellent".
    if (n.graded) {
        oss << "AHI: " << n.index << " events/hour\n";
        // Issue #78: every type the AHI counts is named, and what is not in
        // the AHI is its own line, so the parts add up to the total.
        oss << "Apneas and hypopneas (the events counted in the AHI): " << n.inIndex()
            << " (obstructive apnea=" << n.obstructive
            << ", central apnea=" << n.central
            << ", clear airway apnea=" << n.clear_airway
            << ", unclassified apnea=" << n.unclassified
            << ", hypopnea=" << n.hypopnea << ")\n";
        oss << "Other recorded events (not counted in the AHI): " << n.outsideIndex()
            << " (RERA=" << n.reras << ", other event types=" << n.other << ")\n";
        oss << "Total events recorded: " << n.total << "\n";
    } else {
        oss << "AHI: not available for this machine -- it does not detect "
               "hypopneas, so no apnea-hypopnea index can be formed. Do not "
               "estimate one, and do not describe the events below as mild, "
               "normal or severe.\n";
        // On a machine that does not classify, every per-type count is 0, and
        // "hypopnea=0" would be reported as "no hypopneas were recorded".
        oss << "Total events: " << n.total
            << " (all apneas; this machine does not classify them as "
               "obstructive or central and does not mark hypopneas, so no "
               "per-type breakdown exists. Do not report any type as zero.)\n";
    }

    if (n.avg_event_duration) {
        oss << "Avg event duration: " << *n.avg_event_duration << "s";
        if (n.max_event_duration) oss << ", max: " << *n.max_event_duration << "s";
        oss << "\n";
    }

    // Issue #56: the pressure the machine delivered, the number a user
    // compares with their prescription.
    if (n.therapy_pressure)
        oss << "Therapy pressure (delivered by the machine): avg=" << *n.therapy_pressure
            << " cmH2O\n";

    if (n.pressure_avg || n.pressure_p95) {
        oss << "Pressure:";
        if (n.pressure_avg) oss << " avg=" << *n.pressure_avg << " cmH2O";
        if (n.pressure_min) oss << ", min=" << *n.pressure_min << " cmH2O";
        if (n.pressure_max) oss << ", max=" << *n.pressure_max << " cmH2O";
        if (n.pressure_p95) oss << ", 95th=" << *n.pressure_p95 << " cmH2O";
        oss << "\n";
    }

    if (n.leak_avg || n.leak_max) {
        oss << "Leak:";
        if (n.leak_avg) oss << " avg=" << *n.leak_avg << " L/min";
        if (n.leak_max) oss << ", max=" << *n.leak_max << " L/min";
        if (n.leak_p95) oss << ", 95th=" << *n.leak_p95 << " L/min";
        if (n.leak_p50) oss << ", median=" << *n.leak_p50 << " L/min";
        oss << "\n";
    }

    if (n.mask_pressure) oss << "Mask pressure (actual): " << *n.mask_pressure << " cmH2O\n";
    if (n.epr_pressure) oss << "EPR/EPAP pressure: " << *n.epr_pressure << " cmH2O\n";
    if (n.snore) oss << "Snore index: " << *n.snore << " (0-5 scale)\n";
    if (n.target_ventilation)
        oss << "Target ventilation (ASV): " << *n.target_ventilation << " L/min\n";
    if (n.therapy_mode) oss << "Therapy mode: " << *n.therapy_mode << "\n";

    if (n.respiratory_rate) oss << "Respiratory rate: " << *n.respiratory_rate << " breaths/min\n";
    if (n.tidal_volume) oss << "Tidal volume: " << *n.tidal_volume << " mL\n";
    if (n.minute_ventilation) oss << "Minute ventilation: " << *n.minute_ventilation << " L/min\n";
    if (n.flow_limitation) oss << "Flow limitation: " << *n.flow_limitation << " (0-1 scale)\n";

    if (n.oximetry) {
        const auto& o = *n.oximetry;
        oss << "\nO2 Ring Oximetry (overnight):\n";
        oss << "  Avg SpO2: " << o.avg_spo2 << "%\n";
        oss << "  Min SpO2: " << o.min_spo2 << "%\n";
        oss << "  SpO2 baseline (P95): " << o.spo2_baseline << "%\n";
        oss << "  ODI (3%): " << o.odi_3pct << " events/hour\n";
        oss << "  Time below 90%: " << o.time_below_90 << "%\n";
        oss << "  Time below 88%: " << o.time_below_88 << "%\n";
        oss << "  Avg HR: " << o.avg_hr << " bpm\n";
        oss << "  HR range: " << o.min_hr << "-" << o.max_hr << " bpm\n";
    }

    if (n.str) {
        const auto& s = *n.str;
        oss << "\nResMed official daily summary:\n";
        // SDD-024: the STR's index of an Ungraded night is no AHI either.
        if (n.graded) oss << "  STR AHI: " << s.ahi << " events/hour\n";
        oss << "  Mask events: " << s.mask_on_off_pairs << " (on/off pairs)\n";
        oss << "  95th leak: " << s.leak_95 << " L/min\n";
        // The STR's MaskPress.95: measured at the mask, not the therapy pressure.
        oss << "  95th mask pressure: " << s.mask_press_95 << " cmH2O\n";
        if (s.asv_epap) {
            oss << "  ASV EPAP: " << *s.asv_epap << " cmH2O\n";
            oss << "  ASV Pressure Support: " << s.asv_min_ps.value_or(0) << "-"
                << s.asv_max_ps.value_or(0) << " cmH2O\n";
        }
        if (s.tgt_ipap_50) {
            oss << "  Target IPAP (median): " << *s.tgt_ipap_50 << " cmH2O\n";
            oss << "  Target EPAP (median): " << s.tgt_epap_50.value_or(0) << " cmH2O\n";
            oss << "  Target ventilation (median): " << s.tgt_vent_50.value_or(0) << " L/min\n";
        }
    }

    // D2: the rules ride with the facts.
    oss << "\n";
    for (const auto& r : rules({n})) oss << r << "\n";
    return oss.str();
}

std::string renderRange(const std::vector<NightFacts>& nights, const std::string& label,
                        const std::optional<RangeOximetryFacts>& oximetry) {
    std::ostringstream oss;
    oss << std::fixed << std::setprecision(2);

    oss << label << " CPAP report (" << nights.size() << " nights)\n";
    oss << "─────────────────────────────────────\n\n";

    oss << "Event codes: OA obstructive apnea, CA central apnea, CLR clear airway "
           "apnea, UA unclassified apnea, H hypopnea (these five make the AHI); "
           "RERA respiratory effort related arousal, other = every other "
           "recorded event type (not in the AHI).\n";
    for (const auto& r : rules(nights)) oss << r << "\n";
    oss << "\nNight-by-night:\n";

    double graded_index = 0, total_hours = 0, total_leak = 0;
    int graded = 0, leak_count = 0, usage_count = 0, compliant = 0;
    const NightFacts* best = nullptr;
    const NightFacts* worst = nullptr;

    for (const auto& n : nights) {
        oss << "  " << n.sleep_day;
        if (n.usage_hours) oss << " | " << *n.usage_hours << "h";
        else oss << " | usage not recorded";
        if (n.graded) {
            // Issue #78: the same split as the daily text (codes above).
            oss << " | AHI " << n.index << " | events " << n.total
                << " (in AHI " << n.inIndex() << ": OA=" << n.obstructive
                << " CA=" << n.central << " CLR=" << n.clear_airway
                << " UA=" << n.unclassified << " H=" << n.hypopnea
                << "; not in AHI " << n.outsideIndex() << ": RERA=" << n.reras
                << " other=" << n.other << ")";
        } else {
            oss << " | AHI not available | events " << n.total
                << " (all apneas, not classified)";
        }
        if (n.leak_avg) oss << " | leak avg " << *n.leak_avg << " L/min";
        if (n.pressure_avg) oss << " | press " << *n.pressure_avg << " cmH2O";
        oss << "\n";

        if (n.graded) {
            graded_index += n.index;
            ++graded;
            if (!best || n.index < best->index) best = &n;
            if (!worst || n.index > worst->index) worst = &n;
        }
        if (n.usage_hours) {
            total_hours += *n.usage_hours;
            ++usage_count;
            if (*n.usage_hours >= 4.0) ++compliant;
        }
        if (n.leak_avg) {
            total_leak += *n.leak_avg;
            ++leak_count;
        }
    }

    const int count = static_cast<int>(nights.size());
    oss << "\n" << label << " averages:\n";
    if (graded > 0) {
        oss << "  Avg AHI: " << (graded_index / graded) << " events/hour";
        if (graded < count) oss << " (over the " << graded << " nights that have an AHI)";
        oss << "\n";
    } else {
        oss << "  Avg AHI: not available (no night in this range has an AHI)\n";
    }
    if (usage_count > 0) {
        oss << "  Avg usage: " << (total_hours / usage_count) << " hours/night\n";
        oss << "  Total usage: " << total_hours << " hours\n";
    }
    if (leak_count > 0) oss << "  Avg leak: " << (total_leak / leak_count) << " L/min\n";
    if (count > 0)
        oss << "  Compliance (>=4h): " << compliant << "/" << count << " nights ("
            << (100.0 * compliant / count) << "%)\n";
    if (best) oss << "  Best AHI: " << best->index << " (" << best->sleep_day << ")\n";
    if (worst) oss << "  Worst AHI: " << worst->index << " (" << worst->sleep_day << ")\n";

    if (oximetry) {
        const auto& o = *oximetry;
        oss << "\nO2 Ring Oximetry (" << o.nights << " nights with data):\n";
        oss << "  Avg SpO2: " << o.avg_spo2 << "%\n";
        oss << "  Lowest SpO2: " << o.min_spo2 << "%\n";
        oss << "  Avg ODI (3%): " << o.avg_odi << " events/hour\n";
        oss << "  Avg time below 90%: " << o.avg_below_90 << "%\n";
        oss << "  Avg HR: " << o.avg_hr << " bpm\n";
    }
    return oss.str();
}

nlohmann::json toJson(const NightFacts& n) {
    nlohmann::json j;
    if (!n.sleep_day.empty()) j["sleep_day"] = n.sleep_day;
    if (n.graded) {
        j["ahi"] = round2(n.index);
        j["events_in_ahi"] = {{"total", n.inIndex()},
                              {"obstructive_apnea", n.obstructive},
                              {"central_apnea", n.central},
                              {"clear_airway_apnea", n.clear_airway},
                              {"unclassified_apnea", n.unclassified},
                              {"hypopnea", n.hypopnea}};
        j["events_not_in_ahi"] = {{"total", n.outsideIndex()},
                                  {"rera", n.reras},
                                  {"other_event_types", n.other}};
        j["total_events"] = n.total;
    } else {
        j["ahi"] = "not available: this machine does not detect hypopneas";
        j["total_events"] = n.total;
        j["events_note"] = "all apneas, not classified by type; no per-type breakdown exists";
    }
    putOpt(j, "usage_hours", n.usage_hours);
    putOpt(j, "usage_percent_of_8h", n.usage_percent);
    putOpt(j, "avg_event_duration_s", n.avg_event_duration);
    putOpt(j, "max_event_duration_s", n.max_event_duration);
    putOpt(j, "therapy_pressure_avg_cmh2o", n.therapy_pressure);
    putOpt(j, "pressure_avg_cmh2o", n.pressure_avg);
    putOpt(j, "pressure_min_cmh2o", n.pressure_min);
    putOpt(j, "pressure_max_cmh2o", n.pressure_max);
    putOpt(j, "pressure_p95_cmh2o", n.pressure_p95);
    putOpt(j, "leak_avg_l_min", n.leak_avg);
    putOpt(j, "leak_max_l_min", n.leak_max);
    putOpt(j, "leak_p95_l_min", n.leak_p95);
    putOpt(j, "leak_median_l_min", n.leak_p50);
    putOpt(j, "mask_pressure_cmh2o", n.mask_pressure);
    putOpt(j, "epr_pressure_cmh2o", n.epr_pressure);
    putOpt(j, "snore_index", n.snore);
    putOpt(j, "target_ventilation_l_min", n.target_ventilation);
    putOpt(j, "respiratory_rate_per_min", n.respiratory_rate);
    putOpt(j, "tidal_volume_ml", n.tidal_volume);
    putOpt(j, "minute_ventilation_l_min", n.minute_ventilation);
    putOpt(j, "flow_limitation", n.flow_limitation);
    if (n.therapy_mode) j["therapy_mode"] = *n.therapy_mode;
    if (n.oximetry) {
        const auto& o = *n.oximetry;
        j["oximetry"] = {{"avg_spo2_pct", round2(o.avg_spo2)},
                         {"min_spo2_pct", round2(o.min_spo2)},
                         {"spo2_baseline_p95_pct", round2(o.spo2_baseline)},
                         {"odi_3pct_per_hour", round2(o.odi_3pct)},
                         {"time_below_90_pct", round2(o.time_below_90)},
                         {"time_below_88_pct", round2(o.time_below_88)},
                         {"avg_hr_bpm", round2(o.avg_hr)},
                         {"min_hr_bpm", o.min_hr},
                         {"max_hr_bpm", o.max_hr}};
    }
    if (n.str) {
        const auto& s = *n.str;
        nlohmann::json sj;
        if (n.graded) sj["ahi"] = round2(s.ahi);
        sj["mask_on_off_pairs"] = s.mask_on_off_pairs;
        sj["leak_p95_l_min"] = round2(s.leak_95);
        sj["mask_pressure_p95_cmh2o"] = round2(s.mask_press_95);
        putOpt(sj, "asv_epap_cmh2o", s.asv_epap);
        putOpt(sj, "asv_min_ps_cmh2o", s.asv_min_ps);
        putOpt(sj, "asv_max_ps_cmh2o", s.asv_max_ps);
        putOpt(sj, "target_ipap_median_cmh2o", s.tgt_ipap_50);
        putOpt(sj, "target_epap_median_cmh2o", s.tgt_epap_50);
        putOpt(sj, "target_ventilation_median_l_min", s.tgt_vent_50);
        j["machine_daily_summary"] = sj;
    }
    return j;
}

std::optional<NightOximetryFacts> oximetryForNight(IDatabase& db, const std::string& sleep_day) {
    std::tm day{};
    if (!noonOf(sleep_day, day)) return std::nullopt;
    std::tm next = day;
    next.tm_mday += 1;
    std::mktime(&next);
    const auto oxi = db.getOximetrySummary(kOximetryDeviceId, compact(day), compact(next));
    if (!oxi.found) return std::nullopt;
    NightOximetryFacts o;
    o.avg_spo2 = oxi.avg_spo2;
    o.min_spo2 = oxi.min_spo2;
    o.spo2_baseline = oxi.spo2_baseline;
    o.odi_3pct = oxi.odi_3pct;
    o.time_below_90 = oxi.time_below_90;
    o.time_below_88 = oxi.time_below_88;
    o.avg_hr = oxi.avg_hr;
    o.min_hr = oxi.min_hr;
    o.max_hr = oxi.max_hr;
    return o;
}

std::optional<RangeOximetryFacts> oximetryForRange(IDatabase& db, const std::string& start_day,
                                                   const std::string& end_day) {
    const auto oxi = db.getOximetryRangeSummary(kOximetryDeviceId, compactDay(start_day),
                                                compactDay(end_day));
    if (!oxi.found) return std::nullopt;
    RangeOximetryFacts o;
    o.nights = oxi.nights;
    o.avg_spo2 = oxi.avg_spo2;
    o.min_spo2 = oxi.min_spo2;
    o.avg_odi = oxi.avg_odi;
    o.avg_below_90 = oxi.avg_below_90;
    o.avg_hr = oxi.avg_hr;
    return o;
}

std::vector<NightFacts> loadRange(IDatabase& db, const std::string& device_id,
                                  const std::string& start_day, const std::string& end_day) {
    std::tm start{};
    if (!noonOf(start_day, start) || !isIsoDay(end_day) || end_day < start_day) return {};
    // getMetricsForDateRange reads back from now; two days of margin cover a
    // night that began the evening before its sleep day's noon.
    const auto start_t = std::chrono::system_clock::from_time_t(std::mktime(&start));
    const long long back = static_cast<long long>(
                               std::chrono::duration_cast<std::chrono::hours>(
                                   std::chrono::system_clock::now() - start_t).count()) / 24 + 2;
    std::vector<NightFacts> out;
    for (const auto& m : db.getMetricsForDateRange(device_id, static_cast<int>((std::max)(back, 1LL)))) {
        if (m.sleep_day < start_day || m.sleep_day > end_day) continue;
        out.push_back(NightFacts::from(m, nullptr, oximetryForNight(db, m.sleep_day)));
    }
    return out;
}

std::optional<NightFacts> loadNight(IDatabase& db, const std::string& device_id,
                                    const std::string& sleep_day) {
    auto nights = loadRange(db, device_id, sleep_day, sleep_day);
    if (nights.empty()) return std::nullopt;
    return nights.front();
}

std::string sleepDayDaysAgo(int days) {
    const auto t = std::chrono::system_clock::to_time_t(
        std::chrono::system_clock::now() - std::chrono::hours(12) -
        std::chrono::hours(24LL * (std::max)(0, days)));
    std::tm tm{};
#ifdef _WIN32
    localtime_s(&tm, &t);
#else
    localtime_r(&t, &tm);
#endif
    char buf[11];
    std::strftime(buf, sizeof(buf), "%Y-%m-%d", &tm);
    return buf;
}

bool isIsoDay(const std::string& day) {
    if (day.size() != 10 || day[4] != '-' || day[7] != '-') return false;
    for (size_t i = 0; i < day.size(); ++i)
        if (i != 4 && i != 7 && (day[i] < '0' || day[i] > '9')) return false;
    return true;
}

}  // namespace night_facts
}  // namespace hms_cpap
