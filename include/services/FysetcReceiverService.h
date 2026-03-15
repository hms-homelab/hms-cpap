#pragma once

#include "mqtt_client.h"
#include "llm_client.h"
#include "database/DatabaseService.h"
#include "services/DataPublisherService.h"
#include "models/CPAPModels.h"
#include <memory>
#include <string>
#include <map>
#include <mutex>
#include <optional>

namespace hms_cpap {

/**
 * FysetcReceiverService - Receives EDF data from FYSETC SD WiFi Pro via MQTT
 *
 * Manifest-driven protocol:
 * 1. Responds to sync/request with local file offsets (realtime delta sync)
 * 2. Receives base64-encoded EDF chunks and writes to disk
 * 3. On manifest: diffs against local files, sends cmd/fetch for missing data
 * 4. On session end: processes completed files via EDFParser + DataPublisherService
 *
 * All file management decisions (what to fetch, when to parse) are made here.
 * FYSETC is a dumb SD reader -- it only sends what we ask for.
 *
 * Purely event-driven via MQTT callbacks (no worker thread).
 * Started instead of BurstCollectorService when CPAP_SOURCE=fysetc.
 */
class FysetcReceiverService {
public:
    FysetcReceiverService(
        std::shared_ptr<hms::MqttClient> mqtt,
        std::shared_ptr<DatabaseService> db,
        std::shared_ptr<DataPublisherService> publisher);

    ~FysetcReceiverService();

    FysetcReceiverService(const FysetcReceiverService&) = delete;
    FysetcReceiverService& operator=(const FysetcReceiverService&) = delete;

    bool start();
    void stop();

private:
    void onSyncRequest(const std::string& topic, const std::string& payload);
    void onChunk(const std::string& topic, const std::string& payload);
    void onSessionActive(const std::string& topic, const std::string& payload);
    void onManifest(const std::string& topic, const std::string& payload);

    void requestMissingFiles(const std::string& date,
                             const std::map<std::string, uint64_t>& remote_files);
    void processCompletedSession();
    std::optional<STRDailyRecord> processSTRFile(const std::string& date_folder);
    void generateAndPublishSummary(const SessionMetrics& metrics,
                                    const STRDailyRecord* str_record = nullptr);
    std::string buildMetricsString(const SessionMetrics& metrics,
                                    const STRDailyRecord* str_record = nullptr) const;

    std::string data_dir_;
    std::string archive_dir_;
    std::string device_id_;
    std::string device_name_;
    std::string topic_prefix_;

    std::shared_ptr<hms::MqttClient> mqtt_;
    std::shared_ptr<DatabaseService> db_;
    std::shared_ptr<DataPublisherService> publisher_;

    // LLM summary
    std::unique_ptr<hms::LLMClient> llm_client_;
    bool llm_enabled_ = false;
    std::string llm_prompt_template_;

    std::mutex file_mutex_;

    // Track active session date folder
    std::string active_date_folder_;

    // Track manifest: remote file sizes from FYSETC (for transfer completion detection)
    std::map<std::string, uint64_t> manifest_sizes_;  // filename -> remote size
    std::string manifest_date_;
};

} // namespace hms_cpap
