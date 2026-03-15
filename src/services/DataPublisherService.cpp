#include "services/DataPublisherService.h"
#include "utils/ConfigManager.h"
#include <json/json.h>
#include <iostream>
#include <iomanip>
#include <sstream>

namespace hms_cpap {

DataPublisherService::DataPublisherService(std::shared_ptr<hms::MqttClient> mqtt_client,
                                           std::shared_ptr<DatabaseService> db_service)
    : mqtt_client_(mqtt_client),
      db_service_(db_service) {

    device_id_ = ConfigManager::get("CPAP_DEVICE_ID", "cpap_resmed_23243570851");
    device_name_ = ConfigManager::get("CPAP_DEVICE_NAME", "ResMed AirSense 10");
    serial_number_ = "23243570851";

    discovery_publisher_ = std::make_unique<DiscoveryPublisher>(
        mqtt_client_,
        device_id_,
        device_name_,
        "ResMed",
        "AirSense 10"
    );
}

DataPublisherService::~DataPublisherService() {
}

bool DataPublisherService::initialize() {
    // Subscribe to Home Assistant status to republish discovery on HA restart
    if (mqtt_client_ && mqtt_client_->isConnected()) {
        mqtt_client_->subscribe("homeassistant/status",
            [this](const std::string& topic, const std::string& payload) {
                if (payload == "online") {
                    std::cout << "🏠 Home Assistant restarted, republishing discovery..." << std::endl;
                    publishDiscovery();
                }
            }, 1);
        std::cout << "✅ Subscribed to homeassistant/status" << std::endl;
    }

    // Publish discovery on startup
    publishDiscovery();

    std::cout << "DataPublisherService initialized" << std::endl;
    return true;
}

std::string DataPublisherService::createDeviceJson() const {
    Json::Value device;
    device["identifiers"].append(device_id_);
    device["name"] = device_name_;
    device["model"] = "AirSense 10";
    device["manufacturer"] = "ResMed";
    device["sw_version"] = "MID=36 VID=39";

    Json::StreamWriterBuilder builder;
    builder["indentation"] = "";
    return Json::writeString(builder, device);
}

bool DataPublisherService::publishDiscovery() {
    std::cout << "📡 MQTT: Publishing discovery messages..." << std::endl;

    bool rt_success = publishRealtimeDiscovery();
    bool hist_success = publishHistoricalDiscovery();
    bool str_success = publishSTRDiscovery();
    bool insights_success = publishInsightsDiscovery();

    if (rt_success && hist_success && str_success && insights_success) {
        std::cout << "MQTT: Discovery published (8 realtime + 25 historical + 14 daily + 1 insights = 48 sensors)" << std::endl;
        return true;
    }

    return false;
}

bool DataPublisherService::publishRealtimeDiscovery() {
    std::cout << "  📊 Real-time sensors (8)..." << std::endl;

    std::string device_json = createDeviceJson();

    struct SensorDef {
        std::string name;
        std::string unit;
        std::string device_class;
        std::string icon;
    };

    // 8 REAL-TIME METRICS (updated while mask ON)
    std::vector<SensorDef> realtime_sensors = {
        // Session info
        {"session_status", "", "", "mdi:sleep"},
        {"session_duration", "h", "duration", "mdi:clock-outline"},
        {"last_session_time", "", "timestamp", "mdi:clock"},

        // Current pressure
        {"current_pressure", "cmH2O", "", "mdi:gauge"},
        {"min_pressure", "cmH2O", "", "mdi:gauge-low"},
        {"max_pressure", "cmH2O", "", "mdi:gauge-full"},

        // Current flow
        {"current_flow_rate", "L/min", "", "mdi:air-filter"},
        {"max_flow_rate", "L/min", "", "mdi:wind-turbine"}
    };

    for (const auto& sensor : realtime_sensors) {
        std::string object_id = device_id_ + "_rt_" + sensor.name;
        std::string state_topic = "cpap/" + device_id_ + "/realtime/" + sensor.name;

        Json::Value config;
        config["name"] = "RT " + sensor.name;  // Prefix with "RT" for Home Assistant
        config["unique_id"] = object_id;
        config["state_topic"] = state_topic;

        Json::CharReaderBuilder reader_builder;
        std::istringstream device_stream(device_json);
        Json::parseFromStream(reader_builder, device_stream, &config["device"], nullptr);

        if (!sensor.unit.empty()) {
            config["unit_of_measurement"] = sensor.unit;
        }
        if (!sensor.device_class.empty()) {
            config["device_class"] = sensor.device_class;
        }
        if (!sensor.icon.empty()) {
            config["icon"] = sensor.icon;
        }

        std::string discovery_topic = "homeassistant/sensor/" + device_id_ + "/rt_" + sensor.name + "/config";

        Json::StreamWriterBuilder builder;
        builder["indentation"] = "";
        std::string config_json = Json::writeString(builder, config);

        if (!mqtt_client_->publish(discovery_topic, config_json, 1, true)) {
            std::cerr << "❌ MQTT: Failed to publish realtime discovery for " << sensor.name << std::endl;
            return false;
        }
    }

    // Binary sensor: session_active
    {
        std::string object_id = device_id_ + "_rt_session_active";
        std::string state_topic = "cpap/" + device_id_ + "/realtime/session_active";

        Json::Value config;
        config["name"] = "RT session_active";
        config["unique_id"] = object_id;
        config["state_topic"] = state_topic;
        config["device_class"] = "running";
        config["icon"] = "mdi:sleep";

        Json::CharReaderBuilder reader_builder;
        std::istringstream device_stream(device_json);
        Json::parseFromStream(reader_builder, device_stream, &config["device"], nullptr);

        std::string discovery_topic = "homeassistant/binary_sensor/" + device_id_ + "/rt_session_active/config";

        Json::StreamWriterBuilder builder;
        builder["indentation"] = "";
        std::string config_json = Json::writeString(builder, config);

        mqtt_client_->publish(discovery_topic, config_json, 1, true);
    }

    std::cout << "    ✓ 8 realtime sensors + 1 binary" << std::endl;
    return true;
}

bool DataPublisherService::publishHistoricalDiscovery() {
    std::cout << "  📈 Historical sensors (25)..." << std::endl;

    std::string device_json = createDeviceJson();

    struct SensorDef {
        std::string name;
        std::string unit;
        std::string device_class;
        std::string icon;
    };

    // 25 HISTORICAL METRICS (calculated after session completes)
    std::vector<SensorDef> historical_sensors = {
        // Usage (2)
        {"usage_hours", "h", "duration", "mdi:clock-check"},
        {"usage_percent", "%", "", "mdi:percent"},

        // Events (10)
        {"ahi", "events/h", "", "mdi:alert-circle-outline"},
        {"total_events", "", "", "mdi:counter"},
        {"obstructive_apneas", "", "", "mdi:alert"},
        {"central_apneas", "", "", "mdi:alert-octagon"},
        {"hypopneas", "", "", "mdi:alert-circle"},
        {"reras", "", "", "mdi:alert-outline"},
        {"clear_airway_apneas", "", "", "mdi:alert-decagram"},
        {"avg_event_duration", "s", "duration", "mdi:timer-outline"},
        {"max_event_duration", "s", "duration", "mdi:timer"},
        {"time_in_apnea_percent", "%", "", "mdi:percent-outline"},

        // Pressure stats (3)
        {"avg_pressure", "cmH2O", "", "mdi:gauge"},
        {"pressure_p95", "cmH2O", "", "mdi:chart-bell-curve"},
        {"pressure_p50", "cmH2O", "", "mdi:chart-bell-curve-cumulative"},

        // Flow stats (2)
        {"avg_flow_rate", "L/min", "", "mdi:air-filter"},
        {"flow_p95", "L/min", "", "mdi:chart-line"},

        // Respiratory metrics (8)
        {"avg_respiratory_rate", "breaths/min", "", "mdi:lungs"},
        {"avg_tidal_volume", "mL", "", "mdi:water"},
        {"avg_minute_ventilation", "L/min", "", "mdi:air-filter"},
        {"avg_inspiratory_time", "s", "duration", "mdi:timer-sand"},
        {"avg_expiratory_time", "s", "duration", "mdi:timer-sand-empty"},
        {"avg_ie_ratio", "", "", "mdi:division"},
        {"avg_flow_limitation", "", "", "mdi:alert-circle-outline"},
        {"avg_leak_rate", "L/min", "", "mdi:leak"}
    };

    for (const auto& sensor : historical_sensors) {
        std::string object_id = device_id_ + "_hist_" + sensor.name;
        std::string state_topic = "cpap/" + device_id_ + "/historical/" + sensor.name;

        Json::Value config;
        config["name"] = "HIST " + sensor.name;  // Prefix with "HIST"
        config["unique_id"] = object_id;
        config["state_topic"] = state_topic;

        Json::CharReaderBuilder reader_builder;
        std::istringstream device_stream(device_json);
        Json::parseFromStream(reader_builder, device_stream, &config["device"], nullptr);

        if (!sensor.unit.empty()) {
            config["unit_of_measurement"] = sensor.unit;
        }
        if (!sensor.device_class.empty()) {
            config["device_class"] = sensor.device_class;
        }
        if (!sensor.icon.empty()) {
            config["icon"] = sensor.icon;
        }

        std::string discovery_topic = "homeassistant/sensor/" + device_id_ + "/hist_" + sensor.name + "/config";

        Json::StreamWriterBuilder builder;
        builder["indentation"] = "";
        std::string config_json = Json::writeString(builder, config);

        if (!mqtt_client_->publish(discovery_topic, config_json, 1, true)) {
            std::cerr << "❌ MQTT: Failed to publish historical discovery for " << sensor.name << std::endl;
            return false;
        }
    }

    std::cout << "    ✓ 25 historical sensors" << std::endl;
    return true;
}

void DataPublisherService::publishMqttState(const CPAPSession& session) {
    std::cout << "📡 MQTT: Publishing session state..." << std::endl;

    bool is_completed = (session.status == CPAPSession::Status::COMPLETED);

    // ===== REAL-TIME METRICS (always publish) =====
    publishRealtimeState(session);

    // ===== HISTORICAL METRICS (only if session completed) =====
    if (is_completed && session.metrics.has_value()) {
        publishHistoricalState(session);
    }
}

void DataPublisherService::publishRealtimeState(const CPAPSession& session) {
    bool is_completed = (session.status == CPAPSession::Status::COMPLETED);

    // Session status
    std::string status_str = is_completed ? "completed" : "in_progress";
    mqtt_client_->publish("cpap/" + device_id_ + "/realtime/session_status", status_str, 0, true);

    // Session active (binary)
    // If session status is IN_PROGRESS, it means files are still growing = active
    // Otherwise check if breathing data is recent (within 5 minutes)
    bool is_recently_active = !is_completed;  // IN_PROGRESS = active
    if (is_completed && !session.breathing_summary.empty()) {
        const auto& latest = session.breathing_summary.back();
        auto now = std::chrono::system_clock::now();
        auto seconds_since_last = std::chrono::duration_cast<std::chrono::seconds>(
            now - latest.timestamp).count();
        is_recently_active = (seconds_since_last < 300);
    }
    mqtt_client_->publish("cpap/" + device_id_ + "/realtime/session_active",
                         is_recently_active ? "ON" : "OFF", 0, true);

    // Session duration
    if (session.duration_seconds.has_value()) {
        double hours = session.duration_seconds.value() / 3600.0;
        mqtt_client_->publish("cpap/" + device_id_ + "/realtime/session_duration",
                             std::to_string(hours), 0, true);
    }

    // Last session time (ISO 8601 with timezone)
    if (session.session_start.has_value()) {
        auto time_t = std::chrono::system_clock::to_time_t(session.session_start.value());
        std::tm* tm = std::localtime(&time_t);
        std::ostringstream oss;
        oss << std::put_time(tm, "%Y-%m-%dT%H:%M:%S%z");
        // Insert colon in timezone: +0500 -> +05:00 (ISO 8601)
        std::string timestamp_str = oss.str();
        if (timestamp_str.length() >= 5) {
            timestamp_str.insert(timestamp_str.length() - 2, ":");
        }
        mqtt_client_->publish("cpap/" + device_id_ + "/realtime/last_session_time",
                             timestamp_str, 0, true);
    }

    // Current/live metrics (from latest breathing summary)
    if (!session.breathing_summary.empty()) {
        const auto& latest = session.breathing_summary.back();

        // Pressure
        mqtt_client_->publish("cpap/" + device_id_ + "/realtime/current_pressure",
                             std::to_string(latest.avg_pressure), 0, true);

        // Flow
        mqtt_client_->publish("cpap/" + device_id_ + "/realtime/current_flow_rate",
                             std::to_string(latest.avg_flow_rate), 0, true);

        // Leak
        if (latest.leak_rate.has_value()) {
        mqtt_client_->publish("cpap/" + device_id_ + "/realtime/current_leak_rate",
                             std::to_string(latest.leak_rate.value()), 0, true);
    }

        // Respiratory
        if (latest.respiratory_rate.has_value()) {
        mqtt_client_->publish("cpap/" + device_id_ + "/realtime/current_respiratory_rate",
                             std::to_string(latest.respiratory_rate.value()), 0, true);
    }
        if (latest.tidal_volume.has_value()) {
        mqtt_client_->publish("cpap/" + device_id_ + "/realtime/current_tidal_volume",
                             std::to_string(latest.tidal_volume.value()), 0, true);
    }
        if (latest.minute_ventilation.has_value()) {
        mqtt_client_->publish("cpap/" + device_id_ + "/realtime/current_minute_ventilation",
                             std::to_string(latest.minute_ventilation.value()), 0, true);
    }
        if (latest.flow_limitation.has_value()) {
        mqtt_client_->publish("cpap/" + device_id_ + "/realtime/current_flow_limitation",
                             std::to_string(latest.flow_limitation.value()), 0, true);
    }

        // Min/max pressure/flow (from session so far)
        if (session.metrics.has_value()) {
            if (session.metrics->min_pressure.has_value()) {
        mqtt_client_->publish("cpap/" + device_id_ + "/realtime/min_pressure",
                             std::to_string(session.metrics->min_pressure.value()), 0, true);
    }
            if (session.metrics->max_pressure.has_value()) {
        mqtt_client_->publish("cpap/" + device_id_ + "/realtime/max_pressure",
                             std::to_string(session.metrics->max_pressure.value()), 0, true);
    }
            if (session.metrics->max_flow_rate.has_value()) {
        mqtt_client_->publish("cpap/" + device_id_ + "/realtime/max_flow_rate",
                             std::to_string(session.metrics->max_flow_rate.value()), 0, true);
    }
        }
    }

    // Current vitals (from latest vitals)
    if (!session.vitals.empty()) {
        const auto& latest_vital = session.vitals.back();
        if (latest_vital.spo2.has_value()) {
        mqtt_client_->publish("cpap/" + device_id_ + "/realtime/current_spo2",
                             std::to_string(latest_vital.spo2.value()), 0, true);
    }

        if (latest_vital.heart_rate.has_value()) {
            mqtt_client_->publish("cpap/" + device_id_ + "/realtime/current_heart_rate",
                                 std::to_string(latest_vital.heart_rate.value()), 0, true);
        }
    }

    std::cout << "  ✓ Real-time state published" << std::endl;
}

void DataPublisherService::publishHistoricalState(const CPAPSession& session) {
    publishHistoricalState(session.metrics.value());
}

void DataPublisherService::publishHistoricalState(const SessionMetrics& m) {

    // USAGE
    if (m.usage_hours.has_value()) {
        mqtt_client_->publish("cpap/" + device_id_ + "/historical/usage_hours",
                             std::to_string(m.usage_hours.value()), 0, true);
    }
    if (m.usage_percent.has_value()) {
        mqtt_client_->publish("cpap/" + device_id_ + "/historical/usage_percent",
                             std::to_string(m.usage_percent.value()), 0, true);
    }

    // EVENTS
    mqtt_client_->publish("cpap/" + device_id_ + "/historical/ahi",
                         std::to_string(m.ahi), 0, true);
    mqtt_client_->publish("cpap/" + device_id_ + "/historical/total_events",
                         std::to_string(m.total_events), 0, true);
    mqtt_client_->publish("cpap/" + device_id_ + "/historical/obstructive_apneas",
                         std::to_string(m.obstructive_apneas), 0, true);
    mqtt_client_->publish("cpap/" + device_id_ + "/historical/central_apneas",
                         std::to_string(m.central_apneas), 0, true);
    mqtt_client_->publish("cpap/" + device_id_ + "/historical/hypopneas",
                         std::to_string(m.hypopneas), 0, true);
    mqtt_client_->publish("cpap/" + device_id_ + "/historical/reras",
                         std::to_string(m.reras), 0, true);
    mqtt_client_->publish("cpap/" + device_id_ + "/historical/clear_airway_apneas",
                         std::to_string(m.clear_airway_apneas), 0, true);

    if (m.avg_event_duration.has_value()) {
        mqtt_client_->publish("cpap/" + device_id_ + "/historical/avg_event_duration",
                             std::to_string(m.avg_event_duration.value()), 0, true);
    }
    if (m.max_event_duration.has_value()) {
        mqtt_client_->publish("cpap/" + device_id_ + "/historical/max_event_duration",
                             std::to_string(m.max_event_duration.value()), 0, true);
    }
    if (m.time_in_apnea_percent.has_value()) {
        mqtt_client_->publish("cpap/" + device_id_ + "/historical/time_in_apnea_percent",
                             std::to_string(m.time_in_apnea_percent.value()), 0, true);
    }

    // PRESSURE
    if (m.avg_pressure.has_value()) {
        mqtt_client_->publish("cpap/" + device_id_ + "/historical/avg_pressure",
                             std::to_string(m.avg_pressure.value()), 0, true);
    }
    if (m.pressure_p95.has_value()) {
        mqtt_client_->publish("cpap/" + device_id_ + "/historical/pressure_p95",
                             std::to_string(m.pressure_p95.value()), 0, true);
    }
    if (m.pressure_p50.has_value()) {
        mqtt_client_->publish("cpap/" + device_id_ + "/historical/pressure_p50",
                             std::to_string(m.pressure_p50.value()), 0, true);
    }

    // LEAK
    if (m.avg_leak_rate.has_value()) {
        mqtt_client_->publish("cpap/" + device_id_ + "/historical/avg_leak_rate",
                             std::to_string(m.avg_leak_rate.value()), 0, true);
    }
    if (m.max_leak_rate.has_value()) {
        mqtt_client_->publish("cpap/" + device_id_ + "/historical/max_leak_rate",
                             std::to_string(m.max_leak_rate.value()), 0, true);
    }
    if (m.leak_p95.has_value()) {
        mqtt_client_->publish("cpap/" + device_id_ + "/historical/leak_p95",
                             std::to_string(m.leak_p95.value()), 0, true);
    }

    // FLOW
    if (m.avg_flow_rate.has_value()) {
        mqtt_client_->publish("cpap/" + device_id_ + "/historical/avg_flow_rate",
                             std::to_string(m.avg_flow_rate.value()), 0, true);
    }
    if (m.flow_p95.has_value()) {
        mqtt_client_->publish("cpap/" + device_id_ + "/historical/flow_p95",
                             std::to_string(m.flow_p95.value()), 0, true);
    }

    // RESPIRATORY
    if (m.avg_respiratory_rate.has_value()) {
        mqtt_client_->publish("cpap/" + device_id_ + "/historical/avg_respiratory_rate",
                             std::to_string(m.avg_respiratory_rate.value()), 0, true);
    }
    if (m.avg_tidal_volume.has_value()) {
        mqtt_client_->publish("cpap/" + device_id_ + "/historical/avg_tidal_volume",
                             std::to_string(m.avg_tidal_volume.value()), 0, true);
    }
    if (m.avg_minute_ventilation.has_value()) {
        mqtt_client_->publish("cpap/" + device_id_ + "/historical/avg_minute_ventilation",
                             std::to_string(m.avg_minute_ventilation.value()), 0, true);
    }
    if (m.avg_inspiratory_time.has_value()) {
        mqtt_client_->publish("cpap/" + device_id_ + "/historical/avg_inspiratory_time",
                             std::to_string(m.avg_inspiratory_time.value()), 0, true);
    }
    if (m.avg_expiratory_time.has_value()) {
        mqtt_client_->publish("cpap/" + device_id_ + "/historical/avg_expiratory_time",
                             std::to_string(m.avg_expiratory_time.value()), 0, true);
    }
    if (m.avg_ie_ratio.has_value()) {
        mqtt_client_->publish("cpap/" + device_id_ + "/historical/avg_ie_ratio",
                             std::to_string(m.avg_ie_ratio.value()), 0, true);
    }
    if (m.avg_flow_limitation.has_value()) {
        mqtt_client_->publish("cpap/" + device_id_ + "/historical/avg_flow_limitation",
                             std::to_string(m.avg_flow_limitation.value()), 0, true);
    }

    // SPO2
    if (m.avg_spo2.has_value()) {
        mqtt_client_->publish("cpap/" + device_id_ + "/historical/avg_spo2",
                             std::to_string(m.avg_spo2.value()), 0, true);
    }
    if (m.min_spo2.has_value()) {
        mqtt_client_->publish("cpap/" + device_id_ + "/historical/min_spo2",
                             std::to_string(m.min_spo2.value()), 0, true);
    }
    if (m.max_spo2.has_value()) {
        mqtt_client_->publish("cpap/" + device_id_ + "/historical/max_spo2",
                             std::to_string(m.max_spo2.value()), 0, true);
    }
    if (m.spo2_p95.has_value()) {
        mqtt_client_->publish("cpap/" + device_id_ + "/historical/spo2_p95",
                             std::to_string(m.spo2_p95.value()), 0, true);
    }
    if (m.spo2_p50.has_value()) {
        mqtt_client_->publish("cpap/" + device_id_ + "/historical/spo2_p50",
                             std::to_string(m.spo2_p50.value()), 0, true);
    }

    if (m.spo2_drops.has_value()) {
        mqtt_client_->publish("cpap/" + device_id_ + "/historical/spo2_drops",
                             std::to_string(m.spo2_drops.value()), 0, true);
    }

    // HEART RATE
    if (m.avg_heart_rate.has_value()) {
        mqtt_client_->publish("cpap/" + device_id_ + "/historical/avg_heart_rate",
                             std::to_string(m.avg_heart_rate.value()), 0, true);
    }
    if (m.min_heart_rate.has_value()) {
        mqtt_client_->publish("cpap/" + device_id_ + "/historical/min_heart_rate",
                             std::to_string(m.min_heart_rate.value()), 0, true);
    }
    if (m.max_heart_rate.has_value()) {
        mqtt_client_->publish("cpap/" + device_id_ + "/historical/max_heart_rate",
                             std::to_string(m.max_heart_rate.value()), 0, true);
    }
    if (m.hr_p95.has_value()) {
        mqtt_client_->publish("cpap/" + device_id_ + "/historical/hr_p95",
                             std::to_string(m.hr_p95.value()), 0, true);
    }
    if (m.hr_p50.has_value()) {
        mqtt_client_->publish("cpap/" + device_id_ + "/historical/hr_p50",
                             std::to_string(m.hr_p50.value()), 0, true);
    }

    std::cout << "  ✓ Historical state published (39 metrics)" << std::endl;
}

bool DataPublisherService::publishSTRDiscovery() {
    std::cout << "  📊 STR daily sensors (13)..." << std::endl;

    std::string device_json = createDeviceJson();

    struct SensorDef {
        std::string name;
        std::string unit;
        std::string device_class;
        std::string icon;
    };

    std::vector<SensorDef> str_sensors = {
        {"str_ahi",           "events/h", "", "mdi:alert-circle-outline"},
        {"str_oai",           "events/h", "", "mdi:alert"},
        {"str_cai",           "events/h", "", "mdi:alert-octagon"},
        {"str_hi",            "events/h", "", "mdi:alert-circle"},
        {"str_rin",           "events/h", "", "mdi:alert-outline"},
        {"str_csr",           "min",      "", "mdi:chart-timeline"},
        {"str_usage_hours",   "h",        "duration", "mdi:clock-check"},
        {"str_mask_events",   "",         "", "mdi:face-mask"},
        {"str_leak_95",       "L/min",    "", "mdi:leak"},
        {"str_press_95",      "cmH2O",    "", "mdi:gauge"},
        {"str_spo2_50",       "%",        "", "mdi:heart-pulse"},
        {"str_patient_hours", "h",        "duration", "mdi:counter"},
        {"ahi_delta",         "events/h", "", "mdi:delta"},
        {"session_summary",   "",         "", "mdi:text-box-outline"},
    };

    for (const auto& sensor : str_sensors) {
        std::string object_id = device_id_ + "_daily_" + sensor.name;
        std::string state_topic = "cpap/" + device_id_ + "/daily/" + sensor.name;

        Json::Value config;
        config["name"] = "STR " + sensor.name;
        config["unique_id"] = object_id;
        config["state_topic"] = state_topic;

        Json::CharReaderBuilder reader_builder;
        std::istringstream device_stream(device_json);
        Json::parseFromStream(reader_builder, device_stream, &config["device"], nullptr);

        if (!sensor.unit.empty())
            config["unit_of_measurement"] = sensor.unit;
        if (!sensor.device_class.empty())
            config["device_class"] = sensor.device_class;
        if (!sensor.icon.empty())
            config["icon"] = sensor.icon;

        // Session summary is published as JSON {"summary": "..."}.
        // Use value_template for short state, json_attributes_topic for full text.
        if (sensor.name == "session_summary") {
            config["value_template"] = "{{ value_json.summary[:50] }}...";
            config["json_attributes_topic"] = state_topic;
        }

        std::string discovery_topic = "homeassistant/sensor/" + device_id_ + "/daily_" + sensor.name + "/config";

        Json::StreamWriterBuilder builder;
        builder["indentation"] = "";
        std::string config_json = Json::writeString(builder, config);

        if (!mqtt_client_->publish(discovery_topic, config_json, 1, true)) {
            std::cerr << "MQTT: Failed to publish STR discovery for " << sensor.name << std::endl;
            return false;
        }
    }

    std::cout << "    14 STR daily sensors" << std::endl;
    return true;
}

void DataPublisherService::publishSTRState(const STRDailyRecord& record, double nightly_ahi) {
    std::string prefix = "cpap/" + device_id_ + "/daily/";

    auto pub = [&](const std::string& name, double value) {
        std::ostringstream oss;
        oss << std::fixed << std::setprecision(2) << value;
        mqtt_client_->publish(prefix + name, oss.str(), 0, true);
    };

    pub("str_ahi", record.ahi);
    pub("str_oai", record.oai);
    pub("str_cai", record.cai);
    pub("str_hi", record.hi);
    pub("str_rin", record.rin);
    pub("str_csr", record.csr);
    pub("str_usage_hours", record.duration_minutes / 60.0);
    pub("str_mask_events", static_cast<double>(record.mask_events / 2));  // pairs
    pub("str_leak_95", record.leak_95);
    pub("str_press_95", record.mask_press_95);
    pub("str_spo2_50", record.spo2_50);
    pub("str_patient_hours", record.patient_hours);

    // AHI delta: str_ahi - our calculated ahi
    double delta = (nightly_ahi > 0) ? record.ahi - nightly_ahi : 0;
    pub("ahi_delta", delta);

    std::cout << "  STR daily state published (AHI=" << record.ahi
              << ", usage=" << record.duration_minutes / 60.0 << "h"
              << ", delta=" << delta << ")" << std::endl;
}

bool DataPublisherService::publishSessionCompleted() {
    std::cout << "📤 MQTT: Publishing session completed status..." << std::endl;

    // Publish session_status = completed
    mqtt_client_->publish("cpap/" + device_id_ + "/realtime/session_status", "completed", 0, true);

    // Publish session_active = OFF
    mqtt_client_->publish("cpap/" + device_id_ + "/realtime/session_active", "OFF", 0, true);

    std::cout << "  ✓ Session status: completed" << std::endl;
    std::cout << "  ✓ Session active: OFF" << std::endl;

    return true;
}

bool DataPublisherService::publishSession(const CPAPSession& session) {
    std::cout << "\n" << std::string(60, '=') << std::endl;
    std::cout << "📤 Publishing OSCAR-compatible session data..." << std::endl;
    std::cout << std::string(60, '=') << std::endl;

    bool db_success = true;
    bool mqtt_success = true;

    // 1. Save to database
    if (db_service_ && db_service_->isConnected()) {
        db_success = db_service_->saveSession(session);
    } else {
        std::cerr << "⚠️  DB: Not connected, skipping database save" << std::endl;
        db_success = false;
    }

    // 2. Publish to MQTT
    static bool discovery_published = false;
    static bool was_disconnected = false;

    std::cerr << "DEBUG: Checking MQTT connection (mqtt_client_=" << (mqtt_client_ ? "valid" : "null") << ")..." << std::endl;
    bool mqtt_connected = false;
    if (mqtt_client_) {
        std::cerr << "DEBUG: Calling mqtt_client_->isConnected()..." << std::endl;
        mqtt_connected = mqtt_client_->isConnected();
        std::cerr << "DEBUG: isConnected() returned " << mqtt_connected << std::endl;
    }
    std::cerr << "DEBUG: MQTT check done (" << (mqtt_connected ? "connected" : "disconnected") << ")" << std::endl;

    if (mqtt_connected) {
        // Publish discovery on first run or after reconnection
        // Discovery messages are retained, so safe to republish
        if (!discovery_published || was_disconnected) {
            publishDiscovery();
            discovery_published = true;
            was_disconnected = false;
        }

        // Publish state
        publishMqttState(session);
        mqtt_success = true;
    } else {
        std::cerr << "⚠️  MQTT: Not connected, skipping MQTT publish" << std::endl;
        mqtt_success = false;
        was_disconnected = true;  // Mark that we were disconnected
    }

    std::cout << std::string(60, '=') << std::endl;
    if (db_success && mqtt_success) {
        std::cout << "✅ OSCAR metrics published successfully (DB + MQTT)" << std::endl;
    } else if (db_success) {
        std::cout << "⚠️  Session published to DB only" << std::endl;
    } else if (mqtt_success) {
        std::cout << "⚠️  Session published to MQTT only" << std::endl;
    } else {
        std::cout << "❌ Session publish failed" << std::endl;
    }
    std::cout << std::string(60, '=') << std::endl << std::endl;

    return db_success || mqtt_success;
}

bool DataPublisherService::publishSessionSummary(const std::string& summary) {
    if (!mqtt_client_ || !mqtt_client_->isConnected()) {
        std::cerr << "MQTT: Not connected, skipping summary publish" << std::endl;
        return false;
    }

    // Publish summary as JSON (plain text exceeds HA's 255-char state limit)
    std::string topic = "cpap/" + device_id_ + "/daily/session_summary";
    Json::Value json;
    json["summary"] = summary;
    Json::StreamWriterBuilder writer;
    writer["indentation"] = "";
    std::string payload = Json::writeString(writer, json);
    mqtt_client_->publish(topic, payload, 0, true);

    std::cout << "  LLM summary published to " << topic << std::endl;
    return true;
}

bool DataPublisherService::publishInsightsDiscovery() {
    std::cout << "  Insights sensor (1)..." << std::endl;

    // Single sensor that holds JSON array of insights
    std::string topic = "homeassistant/sensor/" + device_id_ + "/therapy_insights/config";

    Json::Value config;
    config["name"] = "Therapy Insights";
    config["unique_id"] = device_id_ + "_therapy_insights";
    config["state_topic"] = "cpap/" + device_id_ + "/insights/state";
    config["icon"] = "mdi:lightbulb-on";
    config["value_template"] = "{{ value_json | length }}";
    config["json_attributes_topic"] = "cpap/" + device_id_ + "/insights/state";
    config["json_attributes_template"] = "{{ {'insights': value_json} | tojson }}";

    Json::Value device;
    device["identifiers"].append(device_id_);
    device["name"] = device_name_;
    device["manufacturer"] = "ResMed";
    device["model"] = "AirSense 10";
    config["device"] = device;

    Json::StreamWriterBuilder builder;
    builder["indentation"] = "";
    std::string payload = Json::writeString(builder, config);

    bool ok = mqtt_client_->publish(topic, payload, 1, true);
    if (ok) std::cout << "    1 insights sensor" << std::endl;
    return ok;
}

void DataPublisherService::publishInsights(const std::vector<Insight>& insights) {
    auto json = InsightsEngine::toJson(insights);

    Json::StreamWriterBuilder builder;
    builder["indentation"] = "";
    std::string payload = Json::writeString(builder, json);

    std::string topic = "cpap/" + device_id_ + "/insights/state";
    mqtt_client_->publish(topic, payload, 0, true);

    std::cout << "  Insights published (" << insights.size() << " insights)" << std::endl;
}

} // namespace hms_cpap
