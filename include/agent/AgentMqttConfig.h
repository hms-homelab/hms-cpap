#pragma once
//
// The AI agent's own MQTT connection, from the same MQTT_* settings the
// collector reads. No user by default: an empty MQTT_USER connects
// anonymously (the client sets a user and password only when one is given),
// for brokers without auth; a configured user and password are used as they
// are.
//
#include "mqtt_config.h"
#include "utils/ConfigManager.h"

#include <string>

namespace hms_cpap {

inline hms::MqttConfig agentMqttConfig() {
    hms::MqttConfig c;
    c.broker = ConfigManager::get("MQTT_BROKER", "127.0.0.1");
    c.port = std::stoi(ConfigManager::get("MQTT_PORT", "1883"));
    c.username = ConfigManager::get("MQTT_USER", "");
    c.password = ConfigManager::get("MQTT_PASSWORD", "");
    c.client_id = ConfigManager::get("MQTT_CLIENT_ID", "hms_cpap") + "_agent";
    return c;
}

}  // namespace hms_cpap
