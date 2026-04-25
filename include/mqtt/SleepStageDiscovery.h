#pragma once

#include "mqtt_client.h"
#include <json/json.h>
#include <memory>
#include <string>

namespace hms_cpap {

/**
 * SleepStageDiscovery - Publishes HA MQTT auto-discovery configs for
 * sleep stage sensors. Follows the same pattern as DiscoveryPublisher.
 *
 * Usage:
 *   SleepStageDiscovery disco(mqtt, device_id, device_name, "ResMed", "AirSense 10");
 *   disco.publishAll();
 */
class SleepStageDiscovery {
public:
    SleepStageDiscovery(std::shared_ptr<hms::MqttClient> mqtt,
                        const std::string& device_id,
                        const std::string& device_name,
                        const std::string& manufacturer = "ResMed",
                        const std::string& model = "AirSense 10")
        : mqtt_(std::move(mqtt)),
          device_id_(device_id),
          device_name_(device_name),
          manufacturer_(manufacturer),
          model_(model) {}

    /**
     * Publish HA discovery configs for all sleep stage sensors.
     * Should be called once on startup (after MQTT connect).
     */
    bool publishAll() {
        bool ok = true;

        // Current sleep stage (Wake/Light/Deep/REM)
        ok &= publishSensor("sleep_stage_current", "Sleep Stage",
                             "", "", "measurement", "mdi:sleep");

        // Elapsed stage rollup (JSON with accumulated minutes)
        ok &= publishSensor("sleep_stage_elapsed", "Sleep Stage Elapsed",
                             "", "", "measurement", "mdi:chart-bar");

        // Final session summary (JSON with full breakdown)
        ok &= publishSensor("sleep_stage_summary", "Sleep Stage Summary",
                             "", "", "measurement", "mdi:chart-pie");

        return ok;
    }

    /**
     * Remove sleep stage sensors from HA (publish empty retained messages).
     */
    bool removeAll() {
        bool ok = true;
        for (const char* id : {"sleep_stage_current",
                                "sleep_stage_elapsed",
                                "sleep_stage_summary"}) {
            std::string topic = "homeassistant/sensor/" + device_id_ + "/" +
                                std::string(id) + "/config";
            ok &= mqtt_->publish(topic, "", 1, true);
        }
        return ok;
    }

private:
    bool publishSensor(const std::string& sensor_id,
                       const std::string& name,
                       const std::string& unit,
                       const std::string& device_class,
                       const std::string& state_class,
                       const std::string& icon) {
        std::string topic = "homeassistant/sensor/" + device_id_ + "/" +
                            sensor_id + "/config";

        Json::Value config;
        config["name"] = name;
        config["unique_id"] = device_id_ + "_" + sensor_id;
        config["state_topic"] = "homeassistant/sensor/" + device_id_ + "/" +
                                sensor_id + "/state";

        // Device info (same structure as DiscoveryPublisher::buildDeviceInfo)
        Json::Value device;
        device["identifiers"].append(device_id_);
        device["name"] = device_name_;
        device["manufacturer"] = manufacturer_;
        device["model"] = model_;
        config["device"] = device;

        if (!unit.empty())         config["unit_of_measurement"] = unit;
        if (!device_class.empty()) config["device_class"] = device_class;
        if (!state_class.empty())  config["state_class"] = state_class;
        if (!icon.empty())         config["icon"] = icon;

        Json::StreamWriterBuilder wb;
        wb["indentation"] = "";
        std::string payload = Json::writeString(wb, config);

        return mqtt_->publish(topic, payload, 1, true);
    }

    std::shared_ptr<hms::MqttClient> mqtt_;
    std::string device_id_;
    std::string device_name_;
    std::string manufacturer_;
    std::string model_;
};

}  // namespace hms_cpap
