#include "utils/TimeCompat.h"
#include "services/BackfillService.h"
#include "services/RemovedNights.h"
#include "services/SleepHqExportService.h"
#include "parsers/CpapdashBridge.h"
#include "utils/CardLayout.h"
#include "utils/SessionEnd.h"

#include <spdlog/spdlog.h>
#include <algorithm>
#include <filesystem>
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

void BackfillService::triggerCardImport(UploadedCard kind, const std::string& root) {
    {
        std::lock_guard<std::mutex> lock(pending_mutex_);
        pending_card_kind_ = kind;
        pending_card_root_ = root;
    }
    card_import_requested_ = true;
    spdlog::info("BackfillService: {} card import triggered ({})", uploadedCardName(kind), root);
}

// SDD-039 D2: the upload's work, queued for this worker instead of held on a
// web thread.
void BackfillService::triggerZipImport(const std::string& zip_path,
                                       std::function<void(const std::string&)> job) {
    {
        std::lock_guard<std::mutex> lock(pending_mutex_);
        pending_zip_path_ = zip_path;
        pending_zip_job_ = std::move(job);
    }
    zip_import_requested_ = true;
    spdlog::info("BackfillService: zip import queued ({})", zip_path);
}

void BackfillService::executeCardImport(UploadedCard kind, const std::string& root) {
    {
        std::lock_guard<std::mutex> lock(progress_mutex_);
        progress_ = Progress{};
        progress_.status = "running";
        progress_.started_at = currentTimestamp();
    }
    try {
        // "folders" are sessions here: it is the unit the upload page counts.
        const auto counts = importCardSessions(
            *db_, kind, root, config_.device_id, config_.device_name,
            [this](int done, int total) {
                std::lock_guard<std::mutex> lock(progress_mutex_);
                progress_.folders_total = total;
                progress_.folders_done = done;
            });
        std::lock_guard<std::mutex> lock(progress_mutex_);
        progress_.folders_total = counts.found;
        progress_.folders_done = counts.found;
        progress_.sessions_parsed = counts.imported + counts.refused;
        progress_.sessions_saved = counts.imported;
        progress_.errors = counts.refused;
        if (!counts.nights.empty()) {
            progress_.start_date = *counts.nights.begin();
            progress_.end_date = *counts.nights.rbegin();
        }
        if (counts.found == 0) {
            progress_.status = "error";
            progress_.error_message = std::string("no ") + uploadedCardName(kind) +
                                      " sessions found in the uploaded card";
        } else {
            progress_.status = "complete";
        }
        progress_.completed_at = currentTimestamp();
    } catch (const std::exception& e) {
        std::lock_guard<std::mutex> lock(progress_mutex_);
        progress_.status = "error";
        progress_.error_message = e.what();
        progress_.completed_at = currentTimestamp();
        spdlog::error("BackfillService: card import failed: {}", e.what());
    }
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

        // SDD-031: an uploaded Sefam or Löwenstein card, on this same worker so
        // a card and a ResMed backfill never write at once.
        if (card_import_requested_.exchange(false)) {
            UploadedCard kind;
            std::string root;
            {
                std::lock_guard<std::mutex> lock(pending_mutex_);
                kind = pending_card_kind_;
                root = pending_card_root_;
            }
            executeCardImport(kind, root);
            continue;
        }

        // SDD-039 D2: an uploaded ResMed zip. Same worker, same reason: the
        // mirror is filesystem work that can block for as long as the
        // destination takes, and a web thread is the wrong place for it.
        if (zip_import_requested_.exchange(false)) {
            std::string path;
            std::function<void(const std::string&)> job;
            {
                std::lock_guard<std::mutex> lock(pending_mutex_);
                path = pending_zip_path_;
                job = pending_zip_job_;
                pending_zip_path_.clear();
                pending_zip_job_ = nullptr;
            }
            if (job) {
                try {
                    job(path);
                } catch (const std::exception& e) {
                    spdlog::error("BackfillService: zip import failed: {}", e.what());
                }
            }
            continue;
        }

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

        // SDD-010: local_dir is the card root; sessions live under its DATALOG.
        // listDateFolders() has already rejected a root that is not one, so this
        // is safe to derive here.
        const std::string datalog_dir = datalogDirFor(config_.local_dir);

        spdlog::info("BackfillService: scanning {} date folder(s) in {}",
                     date_folders.size(), datalog_dir);

        // SDD-029: a removed night stays out of a range backfill. A Reparse of
        // that one night clears the record before it gets here.
        const auto removed = removedNightSet(*db_, config_.device_id);

        for (const auto& folder : date_folders) {
            if (!running_) break;

            std::string folder_path = datalog_dir + "/" + folder;

            if (removed.count(folder)) {
                spdlog::info("BackfillService: {} is a removed night, skipped", folder);
                std::lock_guard<std::mutex> lock(progress_mutex_);
                progress_.folders_done++;
                continue;
            }

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
                for (const auto& f : session.csl_files) stageFile(f);
                for (const auto& f : session.eve_files) stageFile(f);

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
                applySessionFilePaths(*parsed, session, folder);

                // Save to DB
                if (db_->saveSession(*parsed)) {
                    std::lock_guard<std::mutex> lock(progress_mutex_);
                    progress_.sessions_saved++;

                    // Backfilled sessions are complete (files aren't growing),
                    // so session_end is set here and they show as "Done"
                    // instead of "LIVE". SDD-037 D2: with the end the parse
                    // measured, not the clock, or a whole history imported in
                    // one pass would carry the timestamp of the pass.
                    closeWithDataEnd(*db_, config_.device_id, session.session_start, *parsed);

                    // Record which files the night is actually made of (SDD-014)
                    db_->replaceSessionFiles(config_.device_id, session.session_start,
                                             sessionFileRefs(session, folder));

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

            // SleepHQ export (best-effort, async) — one import per backfilled
            // folder, from the local source dir (not the archive).
            //
            // root_dir must be the CARD ROOT: STR.edf and Identification.* are
            // siblings of DATALOG, not children of it. Getting this wrong made
            // exportFolder probe DATALOG/STR.edf, find nothing, and silently
            // skip every root file, producing machine-less SleepHQ imports
            // that process into nothing visible. Same failure hms-cpapdash-api
            // hit in 4d1fb05.
            //
            // SDD-010: config_.local_dir IS the card root now, so this is just
            // the value itself. It used to be parent_path() of it, back when
            // local_dir meant DATALOG.
            if (config_.sleephq.enabled && config_.sleephq.auto_on_backfill) {
                SleepHqExportService::getInstance().exportFolderAsync(
                    folder, folder_path, config_.local_dir);
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
        // STR.edf lives at the SD root, one level above DATALOG. Then derive
        // the summary from the sessions, ALWAYS, as the burst does (SDD-026):
        // the STR fills the _str columns and the nights we have no sessions
        // for, and the session writer re-asserts our numbers on every night
        // that has them. This used to run only when the STR was missing, which
        // left a backfilled history on the STR's numbers (the Pi, 2026-09-13:
        // 196 of 233 imported nights) and, with no STR, a blank dashboard
        // (issue #16).
        processSTRFile();
        db_->aggregateDailySummaryFromSessions(config_.device_id);

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

    // SDD-010: config_.local_dir is the card ROOT, so the date folders are one
    // level down, inside DATALOG.
    const auto layout = classifyLocalDir(config_.local_dir);
    if (layout != LocalDirLayout::Root) {
        throw std::runtime_error(localDirProblem(layout, config_.local_dir) + ". " +
                                 localDirRemedy(layout, config_.local_dir));
    }
    const std::string datalog_dir = datalogDirFor(config_.local_dir);

    for (const auto& entry : std::filesystem::directory_iterator(datalog_dir)) {
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

bool BackfillService::processSTRFile() {
    // SDD-010: STR.edf sits at the card ROOT, beside DATALOG, and
    // config_.local_dir IS that root. Looked for there and nowhere else.
    //
    // The old code reached up with parent_path() and then fell back to
    // searching inside DATALOG. ResMed never writes STR there, so the fallback
    // could not succeed; it only made a misconfigured path look like a missing
    // file. A wrong root is caught by classifyLocalDir() before we get here.
    std::string str_path;
    for (auto& name : {"STR.edf", "STR.EDF"}) {
        auto p = std::filesystem::path(config_.local_dir) / name;
        if (std::filesystem::exists(p)) { str_path = p.string(); break; }
    }
    if (str_path.empty()) {
        // Not an error: a card legitimately has no STR until the machine writes
        // one. The caller derives the daily summary from sessions instead.
        spdlog::warn("BackfillService: no STR.edf at {}; deriving the daily "
                     "summary from sessions instead.", config_.local_dir);
        return false;
    }

    try {
        auto records = EDFParser::parseSTRFile(str_path, config_.device_id);
        if (records.empty()) {
            spdlog::warn("BackfillService: STR.edf has no therapy days");
            return false;
        }
        // SDD-029: never the removed nights' summary rows.
        db_->saveSTRDailyRecords(
            withoutRemovedNights(records, removedNightSet(*db_, config_.device_id)));
        spdlog::info("BackfillService: STR.edf processed — {} daily record(s)",
                     records.size());
        return true;
    } catch (const std::exception& e) {
        spdlog::warn("BackfillService: STR.edf parse error — {}", e.what());
        return false;
    }
}

}  // namespace hms_cpap
