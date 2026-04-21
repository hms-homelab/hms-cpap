#pragma once

// WiFiSwitchClient no longer needed - ez Share accessed via dedicated interface
#include "clients/IDataSource.h"
#include "clients/EzShareClient.h"
#include "clients/FysetcTcpServer.h"
#include "clients/FysetcDataSource.h"
#include "llm_client.h"
#include "parsers/CpapdashBridge.h"
#include "services/DataPublisherService.h"
#include "services/OximetryService.h"
#include "services/SessionDiscoveryService.h"
#include "mqtt_client.h"
#include "database/IDatabase.h"
#include "utils/AppConfig.h"
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

    /// Set live config pointer for hot-reload (called once after construction)
    void setAppConfig(AppConfig* cfg);

    /// Signal that config changed (called from controller thread, safe)
    void markConfigDirty() { config_dirty_ = true; }

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

    // Data source (ezShare HTTP or Fysetc TCP — both implement IDataSource)
    std::unique_ptr<IDataSource> data_source_;
    std::unique_ptr<FysetcTcpServer> fysetc_server_;

    // Services
    std::unique_ptr<SessionDiscoveryService> discovery_service_;

    // Publishers
    std::shared_ptr<hms::MqttClient> mqtt_client_;
    std::shared_ptr<IDatabase> db_service_;
    std::unique_ptr<DataPublisherService> data_publisher_;

    // O2 Ring oximetry
    std::unique_ptr<OximetryService> oximetry_service_;

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

    // Pending range summary requests (set by MQTT callback, executed by worker thread)
    std::atomic<int> pending_weekly_days_{0};   // >0 = generate weekly with N days
    std::atomic<int> pending_monthly_days_{0};  // >0 = generate monthly with N days

    // Connection recovery tracking
    int consecutive_failures_ = 0;
    static constexpr int MAX_FAILURES_BEFORE_RESET = 3;  // Log recovery after 3 consecutive failures
    bool recovery_logged_ = false;  // Prevent repeated recovery log messages

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
     * Generate and publish a weekly or monthly LLM summary.
     *
     * Queries the DB for per-night metrics over the date range, formats them
     * into a multi-night report, sends to the LLM, and publishes to MQTT.
     * Called automatically on Sundays (weekly) and 1st of month (monthly),
     * or on-demand via MQTT command.
     *
     * @param period WEEKLY (7 days) or MONTHLY (30 days)
     * @param days_override Override the default range (0 = use default for period)
     */
    void generateRangeSummary(SummaryPeriod period, int days_override = 0);

    /**
     * Build a multi-night metrics string for the LLM (weekly/monthly).
     * Lists each night with key metrics, then appends period averages.
     */
    std::string buildRangeMetricsString(const std::vector<SessionMetrics>& nights,
                                        SummaryPeriod period) const;

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

    // ── Hot-reload ──────────────────────────────────────────────────────
    AppConfig* app_config_ = nullptr;
    std::atomic<bool> config_dirty_{false};

    struct ConfigSnapshot {
        std::string source, ezshare_url, local_dir;
        std::string db_type, db_host, db_name, db_user, db_password, sqlite_path;
        int db_port = 0;
        bool mqtt_enabled = false;
        std::string mqtt_broker, mqtt_user, mqtt_password, mqtt_client_id;
        int mqtt_port = 1883;
        bool llm_enabled = false;
        std::string llm_provider, llm_endpoint, llm_model, llm_api_key;
        std::string device_id, device_name;
        int burst_interval = 0;
        bool o2ring_enabled = false;
        std::string o2ring_mode, o2ring_mule_url;
    };
    ConfigSnapshot last_config_;

    /// Check dirty flag and reinitialize changed clients
    void reloadConfig();
    /// Copy live AppConfig into snapshot
    void snapshotConfig(ConfigSnapshot& snap);
    /// (Re)create MQTT subscriptions for commands
    void setupMqttSubscriptions();
};

} // namespace hms_cpap
