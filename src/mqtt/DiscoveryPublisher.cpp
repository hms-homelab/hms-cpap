#include "mqtt/DiscoveryPublisher.h"
#include <iostream>

namespace hms_cpap {

DiscoveryPublisher::DiscoveryPublisher(std::shared_ptr<MqttClient> mqtt_client,
                                       const std::string& device_id,
                                       const std::string& device_name,
                                       const std::string& manufacturer,
                                       const std::string& model)
    : mqtt_client_(mqtt_client),
      device_id_(device_id),
      device_name_(device_name),
      manufacturer_(manufacturer),
      model_(model) {
}

Json::Value DiscoveryPublisher::buildDeviceInfo() const {
    Json::Value device;
    device["identifiers"].append(device_id_);
    device["name"] = device_name_;
    device["manufacturer"] = manufacturer_;
    device["model"] = model_;

    return device;
}

bool DiscoveryPublisher::publishSensorConfig(const std::string& sensor_id,
                                              const std::string& name,
                                              const std::string& unit_of_measurement,
                                              const std::string& device_class,
                                              const std::string& state_class,
                                              const std::string& icon) {
    // Build discovery topic
    std::string topic = "homeassistant/sensor/" + device_id_ + "/" + sensor_id + "/config";

    // Build config JSON
    Json::Value config;
    config["name"] = name;
    config["unique_id"] = device_id_ + "_" + sensor_id;
    config["state_topic"] = "homeassistant/sensor/" + device_id_ + "/" + sensor_id + "/state";
    config["device"] = buildDeviceInfo();

    if (!unit_of_measurement.empty()) {
        config["unit_of_measurement"] = unit_of_measurement;
    }

    if (!device_class.empty()) {
        config["device_class"] = device_class;
    }

    if (!state_class.empty()) {
        config["state_class"] = state_class;
    }

    if (!icon.empty()) {
        config["icon"] = icon;
    }

    // Serialize to JSON string
    Json::StreamWriterBuilder writer;
    writer["indentation"] = "";  // Compact JSON
    std::string payload = Json::writeString(writer, config);

    // Publish (retained, QoS 1)
    return mqtt_client_->publish(topic, payload, 1, true);
}

bool DiscoveryPublisher::publishBinarySensorConfig(const std::string& sensor_id,
                                                    const std::string& name,
                                                    const std::string& device_class,
                                                    const std::string& icon) {
    // Build discovery topic
    std::string topic = "homeassistant/binary_sensor/" + device_id_ + "/" + sensor_id + "/config";

    // Build config JSON
    Json::Value config;
    config["name"] = name;
    config["unique_id"] = device_id_ + "_" + sensor_id;
    config["state_topic"] = "homeassistant/sensor/" + device_id_ + "/" + sensor_id + "/state";
    config["payload_on"] = "1";
    config["payload_off"] = "0";
    config["device"] = buildDeviceInfo();

    if (!device_class.empty()) {
        config["device_class"] = device_class;
    }

    if (!icon.empty()) {
        config["icon"] = icon;
    }

    // Serialize to JSON string
    Json::StreamWriterBuilder writer;
    writer["indentation"] = "";
    std::string payload = Json::writeString(writer, config);

    // Publish (retained, QoS 1)
    return mqtt_client_->publish(topic, payload, 1, true);
}

bool DiscoveryPublisher::publishAll() {
    std::cout << "📡 Discovery: Publishing all sensor configs for " << device_name_ << std::endl;

    bool all_success = true;

    // Battery metrics
    all_success &= publishSensorConfig("battery_charge", "Battery Charge", "%", "battery", "measurement");
    all_success &= publishSensorConfig("battery_voltage", "Battery Voltage", "V", "voltage", "measurement");
    all_success &= publishSensorConfig("battery_runtime", "Battery Runtime", "min", "duration", "measurement", "mdi:timer-outline");
    all_success &= publishSensorConfig("battery_nominal_voltage", "Battery Nominal Voltage", "V", "voltage", "measurement");
    all_success &= publishSensorConfig("battery_low_charge_threshold", "Battery Low Charge Threshold", "%", "battery", "measurement");
    all_success &= publishSensorConfig("battery_warning_charge_threshold", "Battery Warning Charge Threshold", "%", "battery", "measurement");

    // Input metrics
    all_success &= publishSensorConfig("input_voltage", "Input Voltage", "V", "voltage", "measurement");
    all_success &= publishSensorConfig("input_nominal_voltage", "Input Nominal Voltage", "V", "voltage", "measurement");
    all_success &= publishSensorConfig("high_voltage_transfer", "High Voltage Transfer", "V", "voltage", "measurement");
    all_success &= publishSensorConfig("low_voltage_transfer", "Low Voltage Transfer", "V", "voltage", "measurement");
    all_success &= publishSensorConfig("input_sensitivity", "Input Sensitivity", "", "", "", "mdi:tune");
    all_success &= publishSensorConfig("last_transfer_reason", "Last Transfer Reason", "", "", "", "mdi:information-outline");

    // Load & status
    all_success &= publishSensorConfig("load_percentage", "Load", "%", "power_factor", "measurement", "mdi:gauge");
    all_success &= publishSensorConfig("load_watts", "Load Power", "W", "power", "measurement");
    all_success &= publishSensorConfig("ups_status", "UPS Status", "", "", "", "mdi:information");

    // Binary sensors
    all_success &= publishBinarySensorConfig("power_failure", "Power Failure", "power", "mdi:power-plug-off");

    // UPS info
    all_success &= publishSensorConfig("ups_nominal_power", "Nominal Power", "W", "power", "measurement");
    all_success &= publishSensorConfig("beeper_status", "Beeper Status", "", "", "", "mdi:volume-high");
    all_success &= publishSensorConfig("self_test_result", "Self Test Result", "", "", "", "mdi:clipboard-check");
    all_success &= publishSensorConfig("firmware_version", "Firmware Version", "", "", "", "mdi:chip");

    // Driver info
    all_success &= publishSensorConfig("driver_name", "Driver Name", "", "", "", "mdi:application");
    all_success &= publishSensorConfig("driver_version", "Driver Version", "", "", "", "mdi:tag");
    all_success &= publishSensorConfig("driver_state", "Driver State", "", "", "", "mdi:state-machine");

    // Temperature (if available)
    all_success &= publishSensorConfig("temperature", "Temperature", "°C", "temperature", "measurement");

    // Output voltage
    all_success &= publishSensorConfig("output_voltage", "Output Voltage", "V", "voltage", "measurement");
    all_success &= publishSensorConfig("output_nominal_voltage", "Output Nominal Voltage", "V", "voltage", "measurement");

    if (all_success) {
        std::cout << "✅ Discovery: All sensor configs published successfully" << std::endl;
    } else {
        std::cerr << "⚠️  Discovery: Some sensor configs failed to publish" << std::endl;
    }

    return all_success;
}

bool DiscoveryPublisher::removeDevice() {
    std::cout << "📡 Discovery: Removing device " << device_name_ << " from Home Assistant" << std::endl;

    bool all_success = true;

    // List of all sensor IDs
    std::vector<std::string> sensor_ids = {
        "battery_charge", "battery_voltage", "battery_runtime",
        "battery_nominal_voltage", "battery_low_charge_threshold", "battery_warning_charge_threshold",
        "input_voltage", "input_nominal_voltage", "high_voltage_transfer", "low_voltage_transfer",
        "input_sensitivity", "last_transfer_reason",
        "load_percentage", "load_watts", "ups_status",
        "ups_nominal_power", "beeper_status", "self_test_result", "firmware_version",
        "driver_name", "driver_version", "driver_state",
        "temperature", "output_voltage", "output_nominal_voltage"
    };

    // Regular sensors
    for (const auto& sensor_id : sensor_ids) {
        std::string topic = "homeassistant/sensor/" + device_id_ + "/" + sensor_id + "/config";
        all_success &= mqtt_client_->publish(topic, "", 1, true);  // Empty retained message
    }

    // Binary sensors
    std::string binary_topic = "homeassistant/binary_sensor/" + device_id_ + "/power_failure/config";
    all_success &= mqtt_client_->publish(binary_topic, "", 1, true);

    if (all_success) {
        std::cout << "✅ Discovery: Device removed from Home Assistant" << std::endl;
    } else {
        std::cerr << "⚠️  Discovery: Some removal operations failed" << std::endl;
    }

    return all_success;
}

}  // namespace hms_cpap
