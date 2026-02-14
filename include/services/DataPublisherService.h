#pragma once

#include "models/CPAPModels.h"
#include "mqtt/MqttClient.h"
#include "mqtt/DiscoveryPublisher.h"
#include "database/DatabaseService.h"
#include <memory>
#include <string>

namespace hms_cpap {

/**
 * DataPublisherService - Publishes CPAP data to MQTT and PostgreSQL
 *
 * Coordinates data publishing to:
 * - MQTT (Home Assistant sensors via Discovery)
 * - PostgreSQL (historical data storage)
 *
 * Features:
 * - Non-blocking MQTT operations
 * - Graceful reconnection with exponential backoff
 * - Republishes discovery on MQTT reconnect
 * - Thread-safe operations
 */
class DataPublisherService {
public:
    /**
     * Constructor
     *
     * @param mqtt_client Shared MQTT client
     * @param db_service Database service
     */
    DataPublisherService(std::shared_ptr<MqttClient> mqtt_client,
                         std::shared_ptr<DatabaseService> db_service);

    /**
     * Destructor
     */
    ~DataPublisherService();

    /**
     * Initialize service (setup MQTT callbacks)
     *
     * @return true if initialized successfully
     */
    bool initialize();

    /**
     * Publish complete CPAP session
     *
     * - Saves to database
     * - Publishes to MQTT (sensors + discovery)
     *
     * @param session CPAPSession object
     * @return true if published successfully
     */
    bool publishSession(const CPAPSession& session);

    /**
     * Publish discovery messages to Home Assistant
     *
     * Called automatically on MQTT reconnect.
     *
     * @return true if published successfully
     */
    bool publishDiscovery();

    /**
     * Publish session completed status to MQTT
     *
     * Called when session is detected as stopped (checkpoint files unchanged).
     * Updates session_active=OFF and session_status=completed without full session data.
     *
     * @return true if published successfully
     */
    bool publishSessionCompleted();

private:
    std::shared_ptr<MqttClient> mqtt_client_;
    std::shared_ptr<DatabaseService> db_service_;
    std::unique_ptr<DiscoveryPublisher> discovery_publisher_;

    std::string device_id_;
    std::string device_name_;
    std::string serial_number_;

    /**
     * MQTT connection callback
     *
     * Called when MQTT reconnects - republishes discovery
     */
    void onMqttConnected();

    /**
     * Publish realtime discovery (15 sensors)
     *
     * @return true if published successfully
     */
    bool publishRealtimeDiscovery();

    /**
     * Publish historical discovery (39 sensors)
     *
     * @return true if published successfully
     */
    bool publishHistoricalDiscovery();

    /**
     * Publish session state to MQTT (realtime + historical)
     *
     * @param session Session data
     */
    void publishMqttState(const CPAPSession& session);

    /**
     * Publish realtime state (current/live metrics)
     *
     * @param session Session data
     */
    void publishRealtimeState(const CPAPSession& session);

    /**
     * Publish historical state (aggregated metrics)
     *
     * @param session Session data
     */
    void publishHistoricalState(const CPAPSession& session);

    /**
     * Create device info JSON for HA
     *
     * @return Device JSON object
     */
    std::string createDeviceJson() const;
};

} // namespace hms_cpap
