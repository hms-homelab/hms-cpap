#pragma once

#include "database/IDatabase.h"
#include "services/CardUpload.h"
#include "services/SessionDiscoveryService.h"
#include "parsers/CpapdashBridge.h"
#include "utils/ConfigManager.h"

#include <json/json.h>
#include <atomic>
#include <chrono>
#include <filesystem>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace hms_cpap {

class BackfillService {
public:
    struct Config {
        /// SDD-010: the SD card ROOT, the folder holding BOTH STR.edf and
        /// DATALOG. NOT the DATALOG folder itself. Date folders are read from
        /// <local_dir>/DATALOG/<YYYYMMDD>/.
        std::string local_dir;
        std::string device_id;
        std::string device_name;
        // SleepHQ auto-export gate for local-mode/backfill ingests. Creds and the
        // enabled flag live on the global AppConfig (read live by the export
        // service); only the backfill-specific toggle is snapshotted here.
        struct {
            bool enabled = false;
            bool auto_on_backfill = true;
        } sleephq;
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

    /// SDD-031: import an uploaded Sefam or Löwenstein card kept at [root], on
    /// this worker, reporting through the same status the upload page polls.
    void triggerCardImport(UploadedCard kind, const std::string& root);

    /**
     * SDD-039 D2: run an uploaded zip's import HERE rather than on the request
     * thread.
     *
     * The handler used to extract, classify and mirror the whole card inline.
     * On a local disk that is a second; on a stalled network share it holds one
     * of the web server's threads for as long as the mount takes, and the app
     * stops answering while the user watches a spinner (CpapDash support 129).
     * The work is the same, the thread is the one that already owns long ingest
     * and already reports through /api/backfill/status.
     *
     * [job] is the existing importer, given the zip's path on disk; it is
     * responsible for its own cleanup, as it was when it ran inline.
     */
    void triggerZipImport(const std::string& zip_path,
                          std::function<void(const std::string&)> job);

    /// Thread-safe status for API polling.
    Json::Value getStatus() const;

private:
    std::atomic<bool> card_import_requested_{false};
    UploadedCard pending_card_kind_ = UploadedCard::Unknown;
    std::string pending_card_root_;
    void executeCardImport(UploadedCard kind, const std::string& root);

    /// SDD-039 D2: a queued zip import, run on this worker.
    std::atomic<bool> zip_import_requested_{false};
    std::string pending_zip_path_;
    std::function<void(const std::string&)> pending_zip_job_;

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

    /// Parse STR.edf and save daily summaries to populate the dashboard.
    /// @return true only if STR.edf parsed and its daily records were saved.
    ///         False leaves cpap_daily_summary unfilled, so the caller derives it
    ///         from the sessions instead rather than finishing with a blank
    ///         dashboard (issue #16).
    bool processSTRFile();
};

}  // namespace hms_cpap
