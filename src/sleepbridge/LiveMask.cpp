#include "sleepbridge/LiveMask.h"

#include <json/json.h>

namespace hms_cpap::sleepbridge {

LiveMask::LiveMask(Publish publish, std::string device_id, std::string device_name,
                   Clock::time_point started, std::chrono::seconds settle)
    : publish_(std::move(publish)),
      device_id_(std::move(device_id)),
      device_name_(std::move(device_name)),
      started_(started),
      settle_(settle) {}

std::string LiveMask::stateTopic() const { return "cpap/" + device_id_ + "/realtime/mask"; }

std::string LiveMask::discoveryTopic() const {
    return "homeassistant/binary_sensor/" + device_id_ + "/mask/config";
}

bool LiveMask::send(const std::string& topic, const std::string& payload) {
    return publish_ && publish_(topic, payload, 1, true);
}

bool LiveMask::announce() {
    if (!publish_) return false;
    Json::Value device(Json::objectValue);
    device["identifiers"].append(device_id_);
    device["name"] = device_name_;
    Json::Value c(Json::objectValue);
    c["name"]         = "Mask";
    c["unique_id"]    = device_id_ + "_mask";
    c["state_topic"]  = stateTopic();
    c["payload_on"]   = "ON";
    c["payload_off"]  = "OFF";
    // "running": Home Assistant shows Running / Not running, as it does for
    // the session and the Mule & Miner's live therapy.
    c["device_class"] = "running";
    c["icon"]         = "mdi:face-mask";
    c["device"]       = device;
    Json::StreamWriterBuilder w;
    w["indentation"] = "";
    return send(discoveryTopic(), Json::writeString(w, c));
}

void LiveMask::sourceSeen(const std::string& sender) {
    {
        std::lock_guard<std::mutex> lk(mu_);
        if (!seen_.insert(sender).second) return;
    }
    announce();
}

void LiveMask::set(const std::string& sender, bool on) {
    (void)sender;   // one topic for the device, whichever bridge said it
    {
        std::lock_guard<std::mutex> lk(mu_);
        known_ = true;
    }
    send(stateTopic(), on ? "ON" : "OFF");
}

void LiveMask::tick(Clock::time_point now) {
    {
        std::lock_guard<std::mutex> lk(mu_);
        if (known_ || now - started_ < settle_) return;
    }
    // Known only once the broker took it: a broker away at the hour is told
    // at the next tick.
    if (send(stateTopic(), "OFF")) {
        std::lock_guard<std::mutex> lk(mu_);
        known_ = true;
    }
}

}  // namespace hms_cpap::sleepbridge
