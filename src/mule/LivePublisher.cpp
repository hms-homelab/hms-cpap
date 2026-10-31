#include "mule/LivePublisher.h"

#include <json/json.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <ctime>

namespace hms_cpap::mule {

namespace {

std::string compact(const Json::Value& v) {
    Json::StreamWriterBuilder w;
    w["indentation"] = "";
    return Json::writeString(w, v);
}

std::string fixed(double v, int decimals) {
    char buf[32];
    std::snprintf(buf, sizeof(buf), "%.*f", decimals, v);
    return buf;
}

}  // namespace

std::string eventType(const std::string& e) {
    if (e == "ObstructiveApneaEnd") return "OA";
    if (e == "CentralApneaEnd") return "CA";
    if (e == "ApneaEnd") return "UA";
    if (e == "HypopneaEnd") return "H";
    if (e == "ReraEnd") return "RERA";
    if (e.rfind("Csr", 0) == 0 || e.rfind("CSR", 0) == 0) return "CSR";
    return e;
}

double LivePublisher::Night::hours() const {
    if (start_ms <= 0 || end_ms <= start_ms) return 0.0;
    return static_cast<double>(end_ms - start_ms) / 3600000.0;
}

double LivePublisher::Night::ahi() const {
    // Under five minutes masked on, the index would say little: it is
    // counted over five minutes at least.
    const double h = std::max(hours(), 5.0 / 60.0);
    return events() == 0 ? 0.0 : events() / h;
}

LivePublisher::LivePublisher(Publish publish, std::string device_id, std::string device_name)
    : publish_(std::move(publish)), device_id_(std::move(device_id)), device_name_(std::move(device_name)) {}

std::string LivePublisher::topic(const std::string& leaf) const {
    return "cpap/" + device_id_ + "/live/" + leaf;
}

void LivePublisher::send(const std::string& leaf, const std::string& payload, bool retain) {
    if (publish_) publish_(topic(leaf), payload, 0, retain);
}

bool LivePublisher::announce() {
    if (!publish_) return false;
    Json::Value device(Json::objectValue);
    device["identifiers"].append(device_id_);
    device["name"] = device_name_;
    struct Sensor {
        const char* kind;   // sensor or binary_sensor
        const char* id;
        const char* name;
        const char* leaf;
        const char* unit;
        const char* device_class;
        const char* icon;
    };
    const Sensor sensors[] = {
        {"sensor", "live_event_count", "Live event count", "event_count", "", "", "mdi:counter"},
        {"sensor", "live_ahi", "Live AHI", "ahi", "/h", "", "mdi:chart-line"},
        {"sensor", "live_last_event", "Live last event", "last_event", "", "", "mdi:lungs"},
        {"binary_sensor", "live_therapy", "Live therapy", "therapy", "", "running", "mdi:sleep"},
    };
    bool ok = true;
    for (const auto& s : sensors) {
        Json::Value c(Json::objectValue);
        c["name"]        = s.name;
        c["unique_id"]   = device_id_ + "_" + s.id;
        c["state_topic"] = topic(s.leaf);
        c["device"]      = device;
        c["icon"]        = s.icon;
        if (*s.unit) c["unit_of_measurement"] = s.unit;
        if (*s.device_class) c["device_class"] = s.device_class;
        if (std::string(s.kind) == "sensor" && std::string(s.leaf) != "last_event")
            c["state_class"] = "measurement";
        ok = publish_("homeassistant/" + std::string(s.kind) + "/" + device_id_ + "/" + s.id + "/config",
                      compact(c), 1, true) && ok;
    }
    return ok;
}

LivePublisher::Night LivePublisher::night(const std::string& unit) const {
    std::lock_guard<std::mutex> lk(mu_);
    const auto it = nights_.find(unit);
    return it == nights_.end() ? Night{} : it->second;
}

void LivePublisher::therapy(const std::string& unit, const std::string& machine, bool on) {
    Night n;
    {
        std::lock_guard<std::mutex> lk(mu_);
        Night& cur = nights_[unit];
        if (cur.on == on) return;
        if (on) cur = Night{};   // a new night: the counts start over
        cur.on = on;
        n      = cur;
    }
    Json::Value v(Json::objectValue);
    v["unit"]        = unit;
    v["machine"]     = machine;
    v["therapy"]     = on ? "on" : "off";
    v["at"]          = static_cast<Json::Int64>(std::time(nullptr));
    v["events"]      = n.events();
    v["hours"]       = fixed(n.hours(), 2);
    v["ahi"]         = fixed(n.ahi(), 1);
    send("therapy", on ? "ON" : "OFF", true);
    if (on) {
        send("event_count", "0", true);
        send("ahi", "0.0", true);
        send("last_event", "", true);
    }
    send("therapy_changed", compact(v), false);
}

void LivePublisher::block(const std::string& unit, int64_t start_ms, int64_t end_ms) {
    std::lock_guard<std::mutex> lk(mu_);
    Night& n = nights_[unit];
    if (n.start_ms == 0 || start_ms < n.start_ms) n.start_ms = start_ms;
    n.end_ms = std::max(n.end_ms, end_ms);
}

void LivePublisher::event(const std::string& unit, const std::string& machine, const as11::RespEvent& e) {
    const std::string type = eventType(e.event);
    Night n;
    {
        std::lock_guard<std::mutex> lk(mu_);
        Night& cur = nights_[unit];
        if (type == "OA" || type == "CA" || type == "UA") ++cur.apneas;
        else if (type == "H") ++cur.hypopneas;
        else if (type == "RERA") ++cur.reras;
        else if (type == "CSR") ++cur.csr;
        n = cur;
    }
    Json::Value v(Json::objectValue);
    v["type"]           = type;
    v["event"]          = e.event;
    v["unit"]           = unit;
    v["machine"]        = machine;
    v["time"]           = e.reportTime;   // the machine's clock, ISO UTC
    v["duration_s"]     = e.durationSeconds;
    v["backdate_s"]     = e.backdateSeconds;
    Json::Value night(Json::objectValue);
    night["events"]    = n.events();
    night["apneas"]    = n.apneas;
    night["hypopneas"] = n.hypopneas;
    night["reras"]     = n.reras;
    night["csr"]       = n.csr;
    night["hours"]     = fixed(n.hours(), 2);
    night["ahi"]       = fixed(n.ahi(), 1);
    v["night"]         = night;
    send("event", compact(v), false);
    send("last_event", type, true);
    send("event_count", std::to_string(n.events()), true);
    send("ahi", fixed(n.ahi(), 1), true);
}

}  // namespace hms_cpap::mule
