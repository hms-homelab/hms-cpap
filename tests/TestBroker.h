// TestBroker.h
//
// The MQTT broker the live-broker tests talk to, named by HMS_TEST_MQTT
// ("host:port" or "host"). They publish RETAINED Home Assistant discovery
// configs and homeassistant/status, so they must never reach a real home
// broker, and localhost:1883 on a dev box usually is one: on the hub it is the
// broker Home Assistant and every device in the house use, and runs there left
// a retained "offline" on homeassistant/status and test devices in discovery.
//
// Unset, the config points at port 1 on the loopback, which refuses at once,
// so every live-broker test takes its existing "broker not available" skip.
// A gate run starts a throwaway broker and names it:
//     mosquitto -p 18830 &   HMS_TEST_MQTT=localhost:18830 ./tests/run_tests
#pragma once

#include "mqtt_config.h"

#include <cstdlib>
#include <string>

inline hms::MqttConfig testMqttConfig(const std::string& client_id) {
    hms::MqttConfig cfg;
    cfg.client_id = client_id;
    cfg.broker = "127.0.0.1";
    cfg.port = 1;
    if (const char* v = std::getenv("HMS_TEST_MQTT"); v && *v) {
        const std::string s(v);
        const auto colon = s.rfind(':');
        cfg.broker = s.substr(0, colon);
        cfg.port = colon == std::string::npos ? 1883 : std::stoi(s.substr(colon + 1));
    }
    return cfg;
}
