#include "model/NightJson.h"

#include "model/NarrationService.h"

#include <functional>
#include <map>
#include <stdexcept>

namespace hms_cpap::model {

namespace {

using json = nlohmann::json;

double number(const json& v, const std::string& key) {
    if (!v.is_number()) throw std::invalid_argument(key + " is not a number");
    return v.get<double>();
}

int integer(const json& v, const std::string& key) {
    if (!v.is_number_integer()) throw std::invalid_argument(key + " is not an integer");
    return v.get<int>();
}

// Each known key and where it goes. Absent keys stay at the struct's default,
// which for every optional figure is "absent".
template <typename T>
using Setters = std::map<std::string, std::function<void(T&, const json&, const std::string&)>>;

template <typename T>
void readInto(T& out, const json& j, const Setters<T>& setters, const std::string& where) {
    if (!j.is_object()) throw std::invalid_argument(where + " is not an object");
    for (const auto& [key, value] : j.items()) {
        if (value.is_null()) continue;
        const auto it = setters.find(key);
        if (it == setters.end()) throw std::invalid_argument("unknown key " + where + "." + key);
        it->second(out, value, where + "." + key);
    }
}

#define F_NUM(field) {#field, [](auto& o, const json& v, const std::string& k) { o.field = number(v, k); }}
#define F_INT(field) {#field, [](auto& o, const json& v, const std::string& k) { o.field = integer(v, k); }}

const Setters<SessionMetrics>& metricSetters() {
    static const Setters<SessionMetrics> s = {
        {"sleep_day", [](auto& o, const json& v, const std::string& k) {
             if (!v.is_string() || !night_facts::isIsoDay(v.get<std::string>()))
                 throw std::invalid_argument(k + " is not YYYY-MM-DD");
             o.sleep_day = v.get<std::string>();
         }},
        {"index_kind", [](auto& o, const json& v, const std::string& k) {
             const auto kind = v.is_string() ? v.get<std::string>() : "";
             if (kind == "ahi") o.index_kind = SessionMetrics::IndexKind::AHI;
             else if (kind == "ungraded") o.index_kind = SessionMetrics::IndexKind::Ungraded;
             else throw std::invalid_argument(k + " is not \"ahi\" or \"ungraded\"");
         }},
        {"therapy_mode", [](auto& o, const json& v, const std::string& k) {
             o.therapy_mode = integer(v, k);
         }},
        F_INT(total_events), F_NUM(ahi),
        F_INT(obstructive_apneas), F_INT(central_apneas), F_INT(hypopneas), F_INT(reras),
        F_INT(clear_airway_apneas), F_INT(unclassified_apneas), F_INT(other_events),
        F_NUM(avg_event_duration), F_NUM(max_event_duration),
        F_NUM(avg_pressure), F_NUM(min_pressure), F_NUM(max_pressure), F_NUM(pressure_p95),
        F_NUM(avg_leak_rate), F_NUM(max_leak_rate), F_NUM(leak_p95), F_NUM(leak_p50),
        F_NUM(avg_respiratory_rate), F_NUM(avg_tidal_volume), F_NUM(avg_minute_ventilation),
        F_NUM(avg_flow_limitation), F_NUM(avg_therapy_pressure), F_NUM(avg_mask_pressure),
        F_NUM(avg_epr_pressure), F_NUM(avg_snore), F_NUM(avg_target_ventilation),
        F_NUM(usage_hours), F_NUM(usage_percent),
    };
    return s;
}

const Setters<STRDailyRecord>& strSetters() {
    static const Setters<STRDailyRecord> s = {
        {"family", [](auto& o, const json& v, const std::string& k) {
             static const std::map<std::string, STRDailyRecord::Family> kFamilies = {
                 {"unknown", STRDailyRecord::Family::Unknown},
                 {"cpap", STRDailyRecord::Family::Cpap},
                 {"autoset", STRDailyRecord::Family::AutoSet},
                 {"bilevel", STRDailyRecord::Family::BiLevel},
                 {"asv", STRDailyRecord::Family::Asv}};
             const auto it = kFamilies.find(v.is_string() ? v.get<std::string>() : "");
             if (it == kFamilies.end()) throw std::invalid_argument(k + " is not a family");
             o.family = it->second;
         }},
        F_INT(mode), F_INT(mask_events), F_NUM(ahi), F_NUM(leak_95), F_NUM(mask_press_95),
        F_NUM(asv_epap), F_NUM(asv_min_ps), F_NUM(asv_max_ps),
        F_NUM(tgt_ipap_50), F_NUM(tgt_epap_50), F_NUM(tgt_vent_50),
    };
    return s;
}

const Setters<NightOximetryFacts>& oximetrySetters() {
    static const Setters<NightOximetryFacts> s = {
        F_NUM(avg_spo2), F_NUM(min_spo2), F_NUM(spo2_baseline), F_NUM(odi_3pct),
        F_NUM(time_below_90), F_NUM(time_below_88), F_NUM(avg_hr),
        F_INT(min_hr), F_INT(max_hr),
    };
    return s;
}

#undef F_NUM
#undef F_INT

}  // namespace

NightFacts NightInput::facts() const {
    return modelNight(metrics, str ? &*str : nullptr, oximetry);
}

NightInput nightFromJson(const json& j) {
    if (!j.is_object()) throw std::invalid_argument("a night is not an object");
    NightInput in;
    for (const auto& [key, value] : j.items()) {
        if (key == "id") continue;
        if (key == "metrics") readInto(in.metrics, value, metricSetters(), "metrics");
        else if (key == "str") {
            if (value.is_null()) continue;
            STRDailyRecord r;
            readInto(r, value, strSetters(), "str");
            in.str = r;
        } else if (key == "oximetry") {
            if (value.is_null()) continue;
            NightOximetryFacts o;
            readInto(o, value, oximetrySetters(), "oximetry");
            in.oximetry = o;
        } else {
            throw std::invalid_argument("unknown key " + key);
        }
    }
    if (!j.contains("metrics")) throw std::invalid_argument("no metrics");
    return in;
}

}  // namespace hms_cpap::model
