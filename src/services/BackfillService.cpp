#include "services/BackfillService.h"
#include "parsers/CpapdashBridge.h"

#include <spdlog/spdlog.h>
#include <algorithm>
#include <iomanip>
#include <regex>
#include <sstream>

namespace hms_cpap {

BackfillService::BackfillService(Config config, std::shared_ptr<IDatabase> db)
    : config_(std::move(config)), db_(std::move(db)) {}

BackfillService::~BackfillService() {
    stop();
}

void BackfillService::start() {
    if (running_) return;
    running_ = true;
    worker_thread_ = std::thread(&BackfillService::runLoop, this);
    spdlog::info("BackfillService: started");
}

void BackfillService::stop() {
    running_ = false;
    if (worker_thread_.joinable()) {
        worker_thread_.join();
    }
}

void BackfillService::trigger(const std::string& start_date,
                              const std::string& end_date,
                              const std::string& local_dir) {
    {
        std::lock_guard<std::mutex> lock(pending_mutex_);
        pending_start_ = start_date;
        pending_end_ = end_date;
        pending_local_dir_ = local_dir;
    }
    backfill_requested_ = true;
    spdlog::info("BackfillService: backfill triggered (start={}, end={})",
                 start_date.empty() ? "all" : start_date,
                 end_date.empty() ? "all" : end_date);
}

Json::Value BackfillService::getStatus() const {
    std::lock_guard<std::mutex> lock(progress_mutex_);
    Json::Value j;
    j["status"] = progress_.status;
    j["folders_total"] = progress_.folders_total;
    j["folders_done"] = progress_.folders_done;
    j["sessions_parsed"] = progress_.sessions_parsed;
    j["sessions_saved"] = progress_.sessions_saved;
    j["sessions_deleted"] = progress_.sessions_deleted;
    j["errors"] = progress_.errors;
    if (!progress_.error_message.empty())
        j["error_message"] = progress_.error_message;
    if (!progress_.started_at.empty())
        j["started_at"] = progress_.started_at;
    if (!progress_.completed_at.empty())
        j["completed_at"] = progress_.completed_at;
    if (!progress_.start_date.empty())
        j["start_date"] = progress_.start_date;
    if (!progress_.end_date.empty())
        j["end_date"] = progress_.end_date;
    return j;
}

void BackfillService::runLoop() {
    while (running_) {
        // Sleep in 1-second increments so we can exit quickly
        for (int i = 0; i < 2 && running_; ++i) {
            std::this_thread::sleep_for(std::chrono::seconds(1));
        }
        if (!running_) break;

        if (!backfill_requested_.exchange(false)) continue;

        std::string start_date, end_date, local_dir;
        {
            std::lock_guard<std::mutex> lock(pending_mutex_);
            start_date = pending_start_;
            end_date = pending_end_;
            local_dir = pending_local_dir_;
            pending_start_.clear();
            pending_end_.clear();
            pending_local_dir_.clear();
        }

        // Use override local_dir if provided, otherwise config
        if (!local_dir.empty()) {
            config_.local_dir = local_dir;
        }

        executeBackfill(start_date, end_date);
    }
}

void BackfillService::executeBackfill(const std::string& start_date,
                                      const std::string& end_date) {
    // Reset progress
    {
        std::lock_guard<std::mutex> lock(progress_mutex_);
        progress_ = Progress{};
        progress_.status = "running";
        progress_.started_at = currentTimestamp();
        progress_.start_date = start_date;
        progress_.end_date = end_date;
    }

    try {
        auto date_folders = listDateFolders(start_date, end_date);

        {
            std::lock_guard<std::mutex> lock(progress_mutex_);
            progress_.folders_total = static_cast<int>(date_folders.size());
        }

        spdlog::info("BackfillService: scanning {} date folder(s) in {}",
                     date_folders.size(), config_.local_dir);

        for (const auto& folder : date_folders) {
            if (!running_) break;

            std::string folder_path = config_.local_dir + "/" + folder;

            if (!std::filesystem::exists(folder_path)) {
                std::lock_guard<std::mutex> lock(progress_mutex_);
                progress_.folders_done++;
                continue;
            }

            // Group files into sessions
            auto sessions = SessionDiscoveryService::groupLocalFolder(folder_path, folder);

            if (sessions.empty()) {
                std::lock_guard<std::mutex> lock(progress_mutex_);
                progress_.folders_done++;
                continue;
            }

            // Delete existing sessions for this date folder
            int deleted = db_->deleteSessionsByDateFolder(config_.device_id, folder);
            {
                std::lock_guard<std::mutex> lock(progress_mutex_);
                progress_.sessions_deleted += deleted;
            }

            // Parse each session group
            for (const auto& session : sessions) {
                if (!running_) break;

                // Stage files to temp dir for isolated parsing
                std::string temp_dir = (std::filesystem::temp_directory_path() /
                    "cpap_backfill" / (folder + "_" + session.session_prefix)).string();
                std::filesystem::create_directories(temp_dir);

                // Clear any previous files
                for (const auto& entry : std::filesystem::directory_iterator(temp_dir)) {
                    std::filesystem::remove(entry.path());
                }

                // Copy session files
                auto stageFile = [&](const std::string& filename) {
                    auto src = std::filesystem::path(folder_path) / filename;
                    auto dst = std::filesystem::path(temp_dir) / filename;
                    if (std::filesystem::exists(src)) {
                        std::filesystem::copy_file(src, dst,
                            std::filesystem::copy_options::overwrite_existing);
                    }
                };

                for (const auto& f : session.brp_files) stageFile(f);
                for (const auto& f : session.pld_files) stageFile(f);
                for (const auto& f : session.sad_files) stageFile(f);
                if (!session.csl_file.empty()) stageFile(session.csl_file);
                if (!session.eve_file.empty()) stageFile(session.eve_file);

                {
                    std::lock_guard<std::mutex> lock(progress_mutex_);
                    progress_.sessions_parsed++;
                }

                // Parse
                auto parsed = EDFParser::parseSession(
                    temp_dir, config_.device_id, config_.device_name,
                    session.session_start);

                if (!parsed) {
                    spdlog::warn("BackfillService: failed to parse session {} in {}",
                                session.session_prefix, folder);
                    std::lock_guard<std::mutex> lock(progress_mutex_);
                    progress_.errors++;
                    std::filesystem::remove_all(temp_dir);
                    continue;
                }

                // Set relative file paths (same format as normal pipeline)
                std::string relative_base = "DATALOG/" + folder + "/";
                if (!session.brp_files.empty())
                    parsed->brp_file_path = relative_base + session.brp_files[0];
                if (!session.eve_file.empty())
                    parsed->eve_file_path = relative_base + session.eve_file;
                if (!session.sad_files.empty())
                    parsed->sad_file_path = relative_base + session.sad_files[0];
                if (!session.pld_files.empty())
                    parsed->pld_file_path = relative_base + session.pld_files[0];
                if (!session.csl_file.empty())
                    parsed->csl_file_path = relative_base + session.csl_file;

                // Save to DB
                if (db_->saveSession(*parsed)) {
                    std::lock_guard<std::mutex> lock(progress_mutex_);
                    progress_.sessions_saved++;

                    // Backfilled sessions are complete (files aren't growing).
                    // markSessionCompleted() sets session_end so they show as
                    // "Done" instead of "LIVE" in the sessions list.
                    db_->markSessionCompleted(config_.device_id, session.session_start);

                    // Store checkpoint file sizes
                    std::map<std::string, int> checkpoint_sizes;
                    for (const auto& [filename, size_kb] : session.file_sizes_kb) {
                        if (filename.find("_BRP.edf") != std::string::npos ||
                            filename.find("_PLD.edf") != std::string::npos ||
                            filename.find("_SAD.edf") != std::string::npos ||
                            filename.find("_SA2.edf") != std::string::npos) {
                            checkpoint_sizes[filename] = size_kb;
                        }
                    }
                    db_->updateCheckpointFileSizes(
                        config_.device_id, session.session_start, checkpoint_sizes);
                } else {
                    spdlog::warn("BackfillService: failed to save session {} in {}",
                                session.session_prefix, folder);
                    std::lock_guard<std::mutex> lock(progress_mutex_);
                    progress_.errors++;
                }

                // Cleanup temp dir
                std::filesystem::remove_all(temp_dir);
            }

            {
                std::lock_guard<std::mutex> lock(progress_mutex_);
                progress_.folders_done++;
            }
        }

        // Cleanup top-level temp
        std::filesystem::remove_all(
            std::filesystem::temp_directory_path() / "cpap_backfill");

        // Process STR.edf to populate cpap_daily_summary (feeds the dashboard).
        // STR.edf lives at the SD root, one level above DATALOG.
        processSTRFile();

        {
            std::lock_guard<std::mutex> lock(progress_mutex_);
            progress_.status = progress_.errors > 0 ? "complete" : "complete";
            progress_.completed_at = currentTimestamp();
        }

        spdlog::info("BackfillService: complete — parsed={}, saved={}, deleted={}, errors={}",
                     progress_.sessions_parsed, progress_.sessions_saved,
                     progress_.sessions_deleted, progress_.errors);

    } catch (const std::exception& e) {
        spdlog::error("BackfillService: error — {}", e.what());
        std::lock_guard<std::mutex> lock(progress_mutex_);
        progress_.status = "error";
        progress_.error_message = e.what();
        progress_.completed_at = currentTimestamp();
    }
}

std::vector<std::string> BackfillService::listDateFolders(
    const std::string& start_date, const std::string& end_date) const {

    std::vector<std::string> folders;
    std::regex date_re(R"(^\d{8}$)");

    if (!std::filesystem::exists(config_.local_dir)) {
        throw std::runtime_error("Local directory not found: " + config_.local_dir);
    }

    for (const auto& entry : std::filesystem::directory_iterator(config_.local_dir)) {
        if (!entry.is_directory()) continue;
        std::string name = entry.path().filename().string();
        if (!std::regex_match(name, date_re)) continue;
        folders.push_back(name);
    }

    std::sort(folders.begin(), folders.end());

    // Filter by date range if specified (dates are YYYY-MM-DD, folders are YYYYMMDD)
    if (!start_date.empty() || !end_date.empty()) {
        auto toFolderFormat = [](const std::string& date) -> std::string {
            // YYYY-MM-DD -> YYYYMMDD
            std::string result;
            for (char c : date) {
                if (c != '-') result += c;
            }
            return result;
        };

        std::string start_folder = start_date.empty() ? "" : toFolderFormat(start_date);
        std::string end_folder = end_date.empty() ? "" : toFolderFormat(end_date);

        folders.erase(
            std::remove_if(folders.begin(), folders.end(),
                [&](const std::string& f) {
                    if (!start_folder.empty() && f < start_folder) return true;
                    if (!end_folder.empty() && f > end_folder) return true;
                    return false;
                }),
            folders.end());
    }

    return folders;
}

std::string BackfillService::currentTimestamp() {
    auto now = std::chrono::system_clock::now();
    auto t = std::chrono::system_clock::to_time_t(now);
    std::tm tm;
#ifdef _WIN32
    localtime_s(&tm, &t);
#else
    localtime_r(&t, &tm);
#endif
    std::ostringstream oss;
    oss << std::put_time(&tm, "%Y-%m-%d %H:%M:%S");
    return oss.str();
}

void BackfillService::processSTRFile() {
    // STR.edf lives at the SD root, one level above DATALOG
    auto parent = std::filesystem::path(config_.local_dir).parent_path();
    std::string str_path;

    for (auto& name : {"STR.edf", "STR.EDF"}) {
        auto p = parent / name;
        if (std::filesystem::exists(p)) { str_path = p.string(); break; }
    }
    // Also check inside DATALOG as fallback
    if (str_path.empty()) {
        for (auto& name : {"STR.edf", "STR.EDF"}) {
            auto p = std::filesystem::path(config_.local_dir) / name;
            if (std::filesystem::exists(p)) { str_path = p.string(); break; }
        }
    }
    if (str_path.empty()) {
        spdlog::warn("BackfillService: STR.edf not found — dashboard will be empty");
        return;
    }

    try {
        auto records = EDFParser::parseSTRFile(str_path, config_.device_id);
        if (!records.empty()) {
            db_->saveSTRDailyRecords(records);
            spdlog::info("BackfillService: STR.edf processed — {} daily record(s)",
                         records.size());
        }
    } catch (const std::exception& e) {
        spdlog::warn("BackfillService: STR.edf parse error — {}", e.what());
    }
}

}  // namespace hms_cpap
