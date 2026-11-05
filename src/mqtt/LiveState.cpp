#include "mqtt/LiveState.h"

#include <ctime>

namespace hms_cpap {

namespace {

std::string compact(const Json::Value& v) {
    Json::StreamWriterBuilder w;
    w["indentation"] = "";
    return Json::writeString(w, v);
}

Json::Int64 secondsBetween(LiveState::Clock::time_point from, LiveState::Clock::time_point to) {
    return static_cast<Json::Int64>(std::chrono::duration_cast<std::chrono::seconds>(to - from).count());
}

}  // namespace

LiveState::LiveState(Publish publish, std::string device_id, std::string device_name,
                     Clock::time_point started, std::chrono::seconds settle)
    : publish_(std::move(publish)),
      device_id_(std::move(device_id)),
      device_name_(std::move(device_name)),
      started_(started),
      settle_(settle) {}

std::string LiveState::isoUtc(Clock::time_point t) {
    const std::time_t s = Clock::to_time_t(t);
    std::tm tm{};
#ifdef _WIN32
    gmtime_s(&tm, &s);
#else
    gmtime_r(&s, &tm);
#endif
    char buf[32];
    std::strftime(buf, sizeof(buf), "%Y-%m-%dT%H:%M:%SZ", &tm);
    return buf;
}

std::string LiveState::topic(const std::string& leaf) const {
    return "cpap/" + device_id_ + "/realtime/" + leaf;
}

std::string LiveState::maskConfigTopic() const {
    return "homeassistant/binary_sensor/" + device_id_ + "/mask/config";
}

std::string LiveState::eventsConfigTopic() const {
    return "homeassistant/sensor/" + device_id_ + "/live_events/config";
}

bool LiveState::send(const std::string& leaf, const std::string& payload, bool retain, int qos) {
    return publish_ && publish_(topic(leaf), payload, qos, retain);
}

bool LiveState::announce() {
    if (!publish_) return false;
    Json::Value device(Json::objectValue);
    device["identifiers"].append(device_id_);
    device["name"] = device_name_;

    Json::Value m(Json::objectValue);
    m["name"]                  = "Mask";
    m["unique_id"]             = device_id_ + "_mask";
    m["state_topic"]           = topic("mask");
    m["payload_on"]            = "ON";
    m["payload_off"]           = "OFF";
    // "running": Home Assistant shows Running / Not running, as it does for
    // the session and the Mule & Miner's live therapy.
    m["device_class"]          = "running";
    m["icon"]                  = "mdi:face-mask";
    m["json_attributes_topic"] = topic("mask_changed");
    m["device"]                = device;

    Json::Value e(Json::objectValue);
    e["name"]                  = "Live events";
    e["unique_id"]             = device_id_ + "_live_events";
    e["state_topic"]           = topic("live_events");
    e["value_template"]        = "{{ value_json.total }}";
    e["json_attributes_topic"] = topic("live_events");
    e["state_class"]           = "measurement";
    e["icon"]                  = "mdi:counter";
    e["device"]                = device;

    const bool a = publish_(maskConfigTopic(), compact(m), 1, true);
    const bool b = publish_(eventsConfigTopic(), compact(e), 1, true);
    return a && b;
}

void LiveState::sourceSeen(const std::string& source_id) {
    {
        std::lock_guard<std::mutex> lk(mu_);
        if (!seen_.insert(source_id).second) return;
    }
    announce();
}

void LiveState::sendMask(bool on, const std::string& source, const std::string& source_id,
                         const std::string& session, Clock::time_point occurred,
                         Clock::time_point detected) {
    Json::Value a(Json::objectValue);
    a["state"]       = on ? "on" : "off";
    a["source"]      = source;
    a["source_id"]   = source_id;
    a["session"]     = session;
    a["occurred_at"] = isoUtc(occurred);
    a["detected_at"] = isoUtc(detected);
    a["delay_s"]     = secondsBetween(occurred, detected);
    // The attributes first, so Home Assistant has them when the state flips.
    send("mask_changed", compact(a), true);
    send("mask", on ? "ON" : "OFF", true);
}

void LiveState::sendCounts() {
    Json::Value v(Json::objectValue);
    v["total"] = total_;
    for (const auto& [type, n] : counts_) v[type] = n;
    send("live_events", compact(v), true);
}

void LiveState::mask(const MaskChange& c, Clock::time_point detected_at) {
    std::lock_guard<std::mutex> lk(mu_);
    if (c.on) {
        on_.insert(c.source_id);
        if (said_ == Said::On) {
            // Still on, on a new session (off and on again between two looks
            // at a card): the state stands, the change it carries is new.
            if (!c.session.empty() && c.session != session_) {
                session_ = c.session;
                sendMask(true, c.source, c.source_id, c.session, c.occurred_at, detected_at);
            }
            return;
        }
        // A mask going on starts the counts over, unless it is the session
        // said off a moment ago coming back: an off on quiet that a late
        // delivery proved wrong (issue #57), the same night going on.
        const bool resumed = said_ == Said::Off && !c.session.empty() && c.session == session_;
        said_    = Said::On;
        session_ = c.session;
        if (!resumed) {
            counts_.clear();
            total_ = 0;
        }
        sendMask(true, c.source, c.source_id, c.session, c.occurred_at, detected_at);
        sendCounts();
        return;
    }
    on_.erase(c.source_id);
    if (!on_.empty() || said_ == Said::Off) return;
    said_     = Said::Off;
    off_from_ = c.source;
    sendMask(false, c.source, c.source_id, c.session, c.occurred_at, detected_at);
}

void LiveState::event(Json::Value e, Clock::time_point occurred_at, Clock::time_point detected_at) {
    std::lock_guard<std::mutex> lk(mu_);
    e["occurred_at"] = isoUtc(occurred_at);
    e["detected_at"] = isoUtc(detected_at);
    e["delay_s"]     = secondsBetween(occurred_at, detected_at);
    const std::string type = e.get("type", "").asString();
    if (!type.empty()) {
        ++counts_[type];
        ++total_;
    }
    send("event", compact(e), false);
    sendCounts();
}

void LiveState::tick(Clock::time_point now) {
    std::lock_guard<std::mutex> lk(mu_);
    if (said_ != Said::Nothing || now - started_ < settle_) return;
    // Said only once the broker took it: a broker away at the time is told
    // at the next tick.
    Json::Value a(Json::objectValue);
    a["state"]       = "off";
    a["source"]      = "startup";
    a["source_id"]   = "";
    a["session"]     = "";
    a["occurred_at"] = isoUtc(now);
    a["detected_at"] = isoUtc(now);
    a["delay_s"]     = 0;
    if (!send("mask", "OFF", true)) return;
    send("mask_changed", compact(a), true);
    said_ = Said::Off;
}

bool LiveState::maskOn() const {
    std::lock_guard<std::mutex> lk(mu_);
    return said_ == Said::On;
}

bool LiveState::isStreamingSource(const std::string& source) {
    return source == kSleepBridge || source == kMuleBle || source == kAs11Ble;
}

bool LiveState::streamSaidOff() const {
    std::lock_guard<std::mutex> lk(mu_);
    return said_ == Said::Off && isStreamingSource(off_from_);
}

}  // namespace hms_cpap
