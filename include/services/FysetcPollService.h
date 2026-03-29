#pragma once

#include "clients/FysetcHttpClient.h"
#include "database/DatabaseService.h"
#include "services/DataPublisherService.h"
#include "mqtt_client.h"
#include "llm_client.h"
#include "models/CPAPModels.h"

#include <memory>
#include <string>
#include <map>
#include <thread>
#include <atomic>
#include <mutex>
#include <condition_variable>
#include <optional>

namespace hms_cpap {

/**
 * FysetcPollService - HTTP poll client for FYSETC SD WiFi Pro
 *
 * Replaces the MQTT-based FysetcReceiverService with a diff/ack HTTP protocol
 * that avoids SD bus corruption caused by WiFi radio interference.
 *
 * Lifecycle:
 *   1. Starts worker thread, blocks waiting for announce
 *   2. On announce (POST /fysetc/announce from device): records IP, signals thread
 *   3. POST /init to seed known files from local DB
 *   4. Periodic GET /poll -> GET /file -> POST /ack loop
 *   5. Session completion: 2 consecutive stable polls -> parse + publish
 *
 * Selected when config source = "fysetc_poll".
 */
class FysetcPollService {
public:
    FysetcPollService();
    ~FysetcPollService();

    FysetcPollService(const FysetcPollService&) = delete;
    FysetcPollService& operator=(const FysetcPollService&) = delete;

    void start();
    void stop();
    bool isRunning() const { return running_; }

    /**
     * Called from CpapController::fysetcAnnounce when device POSTs its IP.
     * Thread-safe: signals the worker to start/resume polling.
     */
    void onAnnounce(const std::string& ip, const std::string& device_id,
                    int poll_interval_sec, const std::string& fw_version);

private:
    enum class State { WAITING_FOR_ANNOUNCE, INITIALIZING, POLLING, ERROR_BACKOFF };

    // Worker thread
    std::thread worker_thread_;
    std::atomic<bool> running_{false};

    // Announce synchronization
    std::mutex announce_mutex_;
    std::condition_variable announce_cv_;
    std::string fysetc_ip_;
    std::string fysetc_device_id_;
    std::string fysetc_fw_;
    int poll_interval_sec_ = 65;
    bool announce_received_ = false;

    // Clients and services
    std::unique_ptr<FysetcHttpClient> http_client_;
    std::shared_ptr<DatabaseService> db_;
    std::shared_ptr<hms::MqttClient> mqtt_;
    std::shared_ptr<DataPublisherService> publisher_;

    // Config
    std::string device_id_;
    std::string device_name_;
    std::string data_dir_;
    std::string archive_dir_;

    // Session completion tracking: date_folder -> consecutive stable polls
    std::map<std::string, int> stable_poll_count_;
    // Set of date folders already processed (don't re-process)
    std::map<std::string, bool> processed_folders_;
    static constexpr int STABLE_POLLS_FOR_COMPLETION = 2;

    // LLM summary (same pattern as BurstCollectorService)
    std::unique_ptr<hms::LLMClient> llm_client_;
    bool llm_enabled_ = false;
    std::string llm_prompt_template_;

    // Consecutive error counter
    int consecutive_errors_ = 0;
    static constexpr int MAX_ERRORS_BEFORE_BACKOFF = 3;

    // ── Worker methods ──────────────────────────────────────────────

    void runLoop();
    bool doInit();
    bool doPollCycle();

    /**
     * Fetch a file from the device with retry on 500 (SD busy).
     * Writes bytes to local file at data_dir_/date_folder/filename.
     *
     * @return Confirmed byte offset after write, or -1 on failure
     */
    int64_t fetchAndSaveFile(const std::string& path, int64_t offset);

    /**
     * Extract date folder from a DATALOG path.
     * "DATALOG/20260328/BRP.EDF" -> "20260328"
     * "DATALOG/STR.EDF" -> "" (root level)
     */
    static std::string extractDateFolder(const std::string& path);

    /**
     * Extract filename from a DATALOG path.
     * "DATALOG/20260328/BRP.EDF" -> "BRP.EDF"
     */
    static std::string extractFilename(const std::string& path);

    // ── Session processing (reuses existing pipeline) ───────────────

    void processCompletedDateFolder(const std::string& date_folder);
    std::optional<STRDailyRecord> processSTRFile();
    void generateAndPublishSummary(const SessionMetrics& metrics,
                                    const STRDailyRecord* str_record = nullptr);
    std::string buildMetricsString(const SessionMetrics& metrics,
                                    const STRDailyRecord* str_record = nullptr) const;

    // ── Archival ────────────────────────────────────────────────────

    void archiveFiles(const std::string& date_folder);

    // ── Sleep helper ────────────────────────────────────────────────

    /** Sleep in 1-second increments for clean shutdown */
    void sleepSeconds(int seconds);
};

} // namespace hms_cpap
