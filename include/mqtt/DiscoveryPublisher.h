#pragma once

#include "mqtt_client.h"
#include <memory>
#include <string>
#include <json/json.h>

namespace hms_cpap {

/**
 * DiscoveryPublisher - Home Assistant MQTT Discovery publisher
 *
 * Publishes sensor discovery configurations for UPS devices
 * Follows Home Assistant MQTT Discovery protocol
 */
class DiscoveryPublisher {
public:
    /**
     * Constructor
     *
     * @param mqtt_client Shared MQTT client
     * @param device_id MQTT device ID (e.g., "apc_ups")
     * @param device_name Friendly device name (e.g., "Docker NUT UPS")
     * @param manufacturer Device manufacturer (e.g., "American Power Conversion")
     * @param model Device model (e.g., "Back-UPS XS 1000M")
     */
    DiscoveryPublisher(std::shared_ptr<hms::MqttClient> mqtt_client,
                       const std::string& device_id,
                       const std::string& device_name,
                       const std::string& manufacturer = "American Power Conversion",
                       const std::string& model = "Back-UPS XS 1000M");

    /**
     * Publish all sensor discovery configurations
     *
     * Should be called once on startup
     * All messages are retained for Home Assistant
     *
     * @return true if all configs published successfully
     */
    bool publishAll();

    /**
     * Remove device from Home Assistant (unpublish)
     *
     * Publishes empty retained messages to clear discovery
     *
     * @return true if all configs cleared successfully
     */
    bool removeDevice();

private:
    /**
     * Publish single sensor discovery config
     *
     * @param sensor_id Sensor identifier (e.g., "battery_charge")
     * @param name Friendly sensor name (e.g., "Battery Charge")
     * @param unit_of_measurement Unit string (e.g., "%", "V", "min")
     * @param device_class Device class (e.g., "battery", "voltage", "duration")
     * @param state_class State class ("measurement", "total", or empty)
     * @param icon Icon name (e.g., "mdi:battery", optional)
     * @return true if published successfully
     */
    bool publishSensorConfig(const std::string& sensor_id,
                             const std::string& name,
                             const std::string& unit_of_measurement,
                             const std::string& device_class,
                             const std::string& state_class = "measurement",
                             const std::string& icon = "");

    /**
     * Publish binary sensor discovery config
     *
     * @param sensor_id Sensor identifier (e.g., "power_failure")
     * @param name Friendly sensor name (e.g., "Power Failure")
     * @param device_class Device class (e.g., "power", "problem")
     * @param icon Icon name (optional)
     * @return true if published successfully
     */
    bool publishBinarySensorConfig(const std::string& sensor_id,
                                    const std::string& name,
                                    const std::string& device_class,
                                    const std::string& icon = "");

    /**
     * Build device info JSON
     *
     * @return Device info JSON object
     */
    Json::Value buildDeviceInfo() const;

    std::shared_ptr<hms::MqttClient> mqtt_client_;
    std::string device_id_;
    std::string device_name_;
    std::string manufacturer_;
    std::string model_;
};

}  // namespace hms_cpap
