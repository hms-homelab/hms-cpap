#pragma once

#include "parsers/SleeplinkBridge.h"
#include "services/InsightsEngine.h"
#include "mqtt_client.h"
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
    DataPublisherService(std::shared_ptr<hms::MqttClient> mqtt_client,
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

    /**
     * Publish historical MQTT sensors from pre-loaded metrics
     *
     * Called after session completion to republish event/AHI data
     * that was not yet published when the session was still IN_PROGRESS.
     */
    void publishHistoricalState(const SessionMetrics& m);

    /**
     * Publish STR daily summary to MQTT (daily/ namespace).
     *
     * @param record Latest STR daily record
     * @param nightly_ahi Our calculated AHI for delta comparison (0 = unavailable)
     */
    void publishSTRState(const STRDailyRecord& record, double nightly_ahi = 0);

    /**
     * Publish therapy insights to MQTT.
     *
     * @param insights Vector of insights from InsightsEngine
     */
    void publishInsights(const std::vector<Insight>& insights);

    /**
     * Publish LLM-generated session summary to MQTT.
     *
     * @param summary Generated summary text
     * @return true if published successfully
     */
    bool publishSessionSummary(const std::string& summary);

    /**
     * Publish weekly or monthly LLM summary to MQTT.
     * Topic: cpap/{device_id}/weekly/summary or cpap/{device_id}/monthly/summary
     *
     * @param period WEEKLY or MONTHLY (DAILY goes through publishSessionSummary)
     * @param summary Generated summary text
     * @return true if published successfully
     */
    bool publishRangeSummary(SummaryPeriod period, const std::string& summary);

private:
    std::shared_ptr<hms::MqttClient> mqtt_client_;
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
     * Publish insights discovery (1 sensor)
     *
     * @return true if published successfully
     */
    bool publishInsightsDiscovery();

    /**
     * Publish STR daily discovery (13 sensors)
     *
     * @return true if published successfully
     */
    bool publishSTRDiscovery();

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
