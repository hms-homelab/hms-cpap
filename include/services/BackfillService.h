#pragma once

#include "database/IDatabase.h"
#include "services/SessionDiscoveryService.h"
#include "parsers/CpapdashBridge.h"
#include "utils/ConfigManager.h"

#include <json/json.h>
#include <atomic>
#include <chrono>
#include <filesystem>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace hms_cpap {

class BackfillService {
public:
    struct Config {
        std::string local_dir;     // DATALOG directory path
        std::string device_id;
        std::string device_name;
    };

    struct Progress {
        std::string status = "idle";  // idle, running, complete, error
        int folders_total = 0;
        int folders_done = 0;
        int sessions_parsed = 0;
        int sessions_saved = 0;
        int sessions_deleted = 0;
        int errors = 0;
        std::string error_message;
        std::string started_at;
        std::string completed_at;
        // Optional date range filter
        std::string start_date;
        std::string end_date;
    };

    BackfillService(Config config, std::shared_ptr<IDatabase> db);
    ~BackfillService();

    void start();
    void stop();

    /// Trigger a backfill. Empty dates = scan everything.
    /// local_dir overrides config if non-empty (for live config updates).
    void trigger(const std::string& start_date = "",
                 const std::string& end_date = "",
                 const std::string& local_dir = "");

    /// Thread-safe status for API polling.
    Json::Value getStatus() const;

private:
    Config config_;
    std::shared_ptr<IDatabase> db_;

    std::thread worker_thread_;
    std::atomic<bool> running_{false};
    std::atomic<bool> backfill_requested_{false};

    Progress progress_;
    mutable std::mutex progress_mutex_;

    // Pending trigger params (set by trigger(), consumed by worker)
    std::string pending_start_;
    std::string pending_end_;
    std::string pending_local_dir_;
    std::mutex pending_mutex_;

    void runLoop();
    void executeBackfill(const std::string& start_date,
                         const std::string& end_date);

    /// List all YYYYMMDD folders in local_dir, optionally filtered by date range.
    std::vector<std::string> listDateFolders(const std::string& start_date,
                                             const std::string& end_date) const;

    static std::string currentTimestamp();
};

}  // namespace hms_cpap
