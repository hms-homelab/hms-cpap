#include "utils/DeviceIdMigration.h"
#include "mqtt_client.h"

#include <chrono>
#include <cstdio>
#include <iostream>
#include <mutex>
#include <random>
#include <set>
#include <thread>
#include <vector>

namespace hms_cpap {

std::string generateDeviceId() {
    std::random_device rd;
    std::uniform_int_distribution<unsigned> dist(0, 0xFFFFFFFFu);
    char hex[9];
    std::snprintf(hex, sizeof(hex), "%08x", dist(rd));
    return std::string("cpapdash_") + hex;
}

bool isOldDefaultDeviceId(const std::string& id) {
    return id == AppConfig::kLegacyDeviceId || id == AppConfig::kDefaultDeviceId;
}

bool beginDeviceIdBackfill(AppConfig& config) {
    if (!config.device_id_previous.empty()) return true;   // an earlier start's, unfinished
    if (config.device_id_pinned || !isOldDefaultDeviceId(config.device_id)) return false;
    config.device_id_previous = config.device_id;
    config.device_id = generateDeviceId();
    return true;
}

bool clearRetainedDevice(const hms::MqttConfig& mqtt, const std::string& old_id) {
    if (old_id.empty()) return true;
    hms::MqttConfig cfg = mqtt;
    // Its own client id: the service's client connects right after, and a
    // broker evicts a second connection under the same id.
    cfg.client_id = (mqtt.client_id.empty() ? std::string("hms_cpap") : mqtt.client_id) +
                    "_sdd051";
    cfg.topic_prefix.clear();   // no status/LWT topic for a one-shot client
    hms::MqttClient client(cfg);
    if (!client.connect()) return false;

    std::mutex mu;
    std::set<std::string> retained;
    auto collect = [&](const std::string& topic, const std::string& payload) {
        if (payload.empty()) return;
        std::lock_guard<std::mutex> lk(mu);
        retained.insert(topic);
    };
    // ONE subscribe for both filters. hms::MqttClient holds its own lock across
    // the paho subscribe, while paho's receive thread holds paho's lock to
    // deliver a message into that same client lock: a second subscribe made
    // while the first one's retained messages are still arriving deadlocks.
    client.subscribe(std::vector<std::string>{"homeassistant/+/" + old_id + "/#",
                                              "cpap/" + old_id + "/#"},
                     collect, 1);
    // Retained messages arrive straight after the subscribe; nothing publishes
    // under the old id any more, so whatever comes is what is left behind.
    std::this_thread::sleep_for(std::chrono::seconds(3));

    std::set<std::string> topics;
    {
        std::lock_guard<std::mutex> lk(mu);
        topics = retained;
    }
    for (const auto& t : topics) client.publish(t, "", 1, true);
    std::this_thread::sleep_for(std::chrono::milliseconds(500));
    client.disconnect();
    std::cout << "MQTT: removed the old device " << old_id << " (" << topics.size()
              << " retained topics)" << std::endl;
    return true;
}

}  // namespace hms_cpap
