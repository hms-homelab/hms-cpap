#pragma once

// WiFiSwitchClient no longer needed - ez Share accessed via dedicated interface
#include "clients/EzShareClient.h"
#include "llm_client.h"
#include "parsers/EDFParser.h"
#include "models/CPAPModels.h"
#include "services/DataPublisherService.h"
#include "services/SessionDiscoveryService.h"
#include "mqtt_client.h"
#include "database/DatabaseService.h"
#include <memory>
#include <thread>
#include <atomic>
#include <chrono>
#include <mutex>

namespace hms_cpap {

/**
 * BurstCollectorService - Main service for CPAP data collection
 *
 * Implements periodic collection pattern:
 * 1. Wait for burst interval
 * 2. Access ez Share via EZSHARE_BASE_URL
 * 3. Download session EDF files
 * 4. Parse EDF files
 * 5. Publish to MQTT / save to database
 *
 * Runs continuously in background thread.
 * Note: No WiFi switching needed - ez Share is bridged to local network!
 */
class BurstCollectorService {
public:
    /**
     * Constructor
     *
     * @param burst_interval_seconds Interval between burst cycles (default: 300s = 5 min)
     */
    explicit BurstCollectorService(int burst_interval_seconds = 300);

    /**
     * Destructor - cleanup and stop service
     */
    ~BurstCollectorService();

    // Disable copy
    BurstCollectorService(const BurstCollectorService&) = delete;
    BurstCollectorService& operator=(const BurstCollectorService&) = delete;

    /**
     * Start service (spawns worker thread)
     */
    void start();

    /**
     * Stop service (joins worker thread)
     */
    void stop();

    /**
     * Check if service is running
     *
     * @return true if worker thread is active
     */
    bool isRunning() const;

    /**
     * Get last burst execution time
     *
     * @return Timestamp of last burst cycle
     */
    std::chrono::system_clock::time_point getLastBurstTime() const;

private:
    // Configuration
    int burst_interval_seconds_;
    std::string device_id_;
    std::string device_name_;
    std::string local_source_dir_;  // Empty = ezShare mode, set = local filesystem mode

    // Clients
    std::unique_ptr<EzShareClient> ezshare_client_;

    // Services
    std::unique_ptr<SessionDiscoveryService> discovery_service_;

    // Publishers
    std::shared_ptr<hms::MqttClient> mqtt_client_;
    std::shared_ptr<DatabaseService> db_service_;
    std::unique_ptr<DataPublisherService> data_publisher_;

    // Worker thread
    std::thread worker_thread_;
    std::atomic<bool> running_;
    std::mutex mutex_;

    // State
    std::chrono::system_clock::time_point last_burst_time_;

    // LLM summary
    std::unique_ptr<hms::LLMClient> llm_client_;
    bool llm_enabled_ = false;
    std::string llm_prompt_template_;

    // Cached STR records for on-demand insights regeneration
    std::vector<STRDailyRecord> last_str_records_;

    // Connection recovery tracking
    int consecutive_failures_ = 0;
    static constexpr int MAX_FAILURES_BEFORE_RESET = 3;  // Reset session_active after 3 consecutive failures
    bool session_active_cleared_ = false;  // Track if we've already cleared stale session

    /**
     * Worker thread main loop
     */
    void runLoop();

    /**
     * Execute single burst cycle
     *
     * @return true if cycle completed successfully
     */
    bool executeBurstCycle();

    /**
     * Download a single session file set
     *
     * @param session Session file set to download
     * @param local_base_dir Local base directory (/tmp/cpap_data)
     * @return true if at least required files downloaded
     */
    bool downloadSessionFiles(const SessionFileSet& session,
                             const std::string& local_base_dir);

    /**
     * Archive downloaded files to permanent storage
     *
     * Copies files from /tmp/cpap_data/ to permanent DATALOG archive,
     * preserving original SD card structure.
     *
     * @param date_folder Date folder (YYYYMMDD format)
     * @param temp_base_dir Temporary download location (default: /tmp/cpap_data)
     * @param archive_base_dir Permanent archive location
     * @return true if files archived successfully
     */
    bool archiveSessionFiles(const std::string& date_folder,
                            const std::string& temp_base_dir,
                            const std::string& archive_base_dir);

    /**
     * Process STR.edf: download, parse, save to DB, publish to MQTT.
     * Non-fatal: failure does not affect the session cycle.
     */
    void processSTRFile();

    /**
     * Generate LLM summary of session metrics and publish to MQTT.
     * Non-fatal: failure does not affect the session cycle.
     *
     * @param metrics Aggregated session metrics
     * @param str_record Optional STR daily record for additional context
     */
    void generateAndPublishSummary(const SessionMetrics& metrics,
                                   const STRDailyRecord* str_record = nullptr);

    /**
     * Build metrics string from session data for LLM prompt substitution.
     */
    std::string buildMetricsString(const SessionMetrics& metrics,
                                   const STRDailyRecord* str_record = nullptr) const;

    /**
     * Load prompt template from file.
     *
     * @param filepath Path to prompt template file
     * @return Prompt template string, or empty on failure
     */
    static std::string loadPromptFile(const std::string& filepath);

    /**
     * Get current date string (YYYYMMDD format)
     *
     * @return Date string for today
     */
    std::string getCurrentDateString() const;
};

} // namespace hms_cpap
