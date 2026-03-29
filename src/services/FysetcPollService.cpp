#include "services/FysetcPollService.h"
#include "services/SessionDiscoveryService.h"
#include "parsers/EDFParser.h"
#include "utils/ConfigManager.h"

#include <json/json.h>
#include <iostream>
#include <fstream>
#include <filesystem>
#include <chrono>
#include <ctime>
#include <iomanip>
#include <sstream>

namespace hms_cpap {

FysetcPollService::FysetcPollService() {
    device_id_ = ConfigManager::get("CPAP_DEVICE_ID", "cpap_resmed_23243570851");
    device_name_ = ConfigManager::get("CPAP_DEVICE_NAME", "ResMed AirSense 10");
    data_dir_ = ConfigManager::get("CPAP_TEMP_DIR", "/tmp/cpap_data");
    archive_dir_ = ConfigManager::get("CPAP_ARCHIVE_DIR", "/mnt/public/cpap_data/DATALOG");

    http_client_ = std::make_unique<FysetcHttpClient>();

    // MQTT
    std::string mqtt_broker = ConfigManager::get("MQTT_BROKER", "192.168.2.15");
    int mqtt_port = ConfigManager::getInt("MQTT_PORT", 1883);
    std::string mqtt_user = ConfigManager::get("MQTT_USER", "aamat");
    std::string mqtt_pass = ConfigManager::get("MQTT_PASSWORD", "exploracion");

    hms::MqttConfig mqtt_cfg;
    mqtt_cfg.broker = mqtt_broker;
    mqtt_cfg.port = mqtt_port;
    mqtt_cfg.username = mqtt_user;
    mqtt_cfg.password = mqtt_pass;
    mqtt_cfg.client_id = "hms_cpap_fysetc_poll";

    mqtt_ = std::make_shared<hms::MqttClient>(mqtt_cfg);
    if (!mqtt_->connect()) {
        std::cerr << "FysetcPoll: MQTT connection failed (will retry)" << std::endl;
    }

    // Database
    std::string db_host = ConfigManager::get("DB_HOST", "localhost");
    std::string db_port = ConfigManager::get("DB_PORT", "5432");
    std::string db_name = ConfigManager::get("DB_NAME", "cpap_monitoring");
    std::string db_user = ConfigManager::get("DB_USER", "maestro");
    std::string db_pass = ConfigManager::get("DB_PASSWORD", "maestro_postgres_2026_secure");

    std::string conn_str = "host=" + db_host + " port=" + db_port +
                           " dbname=" + db_name + " user=" + db_user +
                           " password=" + db_pass;

    db_ = std::make_shared<DatabaseService>(conn_str);
    if (!db_->connect()) {
        std::cerr << "FysetcPoll: DB connection failed (will retry)" << std::endl;
    }

    // DataPublisher
    publisher_ = std::make_shared<DataPublisherService>(mqtt_, db_);
    publisher_->initialize();

    // LLM (optional)
    std::string llm_enabled_str = ConfigManager::get("LLM_ENABLED", "false");
    llm_enabled_ = (llm_enabled_str == "true" || llm_enabled_str == "1");

    if (llm_enabled_) {
        hms::LLMConfig llm_config;
        llm_config.enabled = true;
        llm_config.provider = hms::LLMClient::parseProvider(
            ConfigManager::get("LLM_PROVIDER", "ollama"));
        llm_config.endpoint = ConfigManager::get("LLM_ENDPOINT", "http://192.168.2.5:11434");
        llm_config.model = ConfigManager::get("LLM_MODEL", "llama3.1:8b-instruct-q4_K_M");
        llm_config.api_key = ConfigManager::get("LLM_API_KEY", "");
        llm_config.keep_alive_seconds = ConfigManager::getInt("LLM_KEEP_ALIVE", 0);

        llm_client_ = std::make_unique<hms::LLMClient>(llm_config);

        std::string prompt_file = ConfigManager::get("LLM_PROMPT_FILE", "");
        if (!prompt_file.empty()) {
            llm_prompt_template_ = hms::LLMClient::loadPromptFile(prompt_file);
        }
    }
}

FysetcPollService::~FysetcPollService() {
    stop();
}

void FysetcPollService::start() {
    if (running_) return;
    running_ = true;

    // Publish session_active OFF on startup (clear stale state)
    if (mqtt_) {
        mqtt_->publish("cpap/" + device_id_ + "/realtime/session_active", "OFF", true);
    }

    worker_thread_ = std::thread(&FysetcPollService::runLoop, this);
    std::cout << "FysetcPoll: Worker thread started (waiting for announce)" << std::endl;
}

void FysetcPollService::stop() {
    if (!running_) return;
    running_ = false;
    announce_cv_.notify_all();

    if (worker_thread_.joinable()) {
        worker_thread_.join();
    }
    std::cout << "FysetcPoll: Worker thread stopped" << std::endl;
}

void FysetcPollService::onAnnounce(const std::string& ip,
                                    const std::string& device_id,
                                    int poll_interval_sec,
                                    const std::string& fw_version) {
    std::lock_guard<std::mutex> lock(announce_mutex_);
    fysetc_ip_ = ip;
    fysetc_device_id_ = device_id;
    fysetc_fw_ = fw_version;
    poll_interval_sec_ = poll_interval_sec;
    announce_received_ = true;

    std::cout << "FysetcPoll: Announce received - device=" << device_id
              << " ip=" << ip << " poll=" << poll_interval_sec << "s"
              << " fw=" << fw_version << std::endl;

    announce_cv_.notify_one();
}

// ── Worker thread ───────────────────────────────────────────────────────────

void FysetcPollService::runLoop() {
    State state = State::WAITING_FOR_ANNOUNCE;
    int backoff_sec = 5;

    while (running_) {
        switch (state) {

        case State::WAITING_FOR_ANNOUNCE: {
            std::unique_lock<std::mutex> lock(announce_mutex_);
            announce_cv_.wait(lock, [this] { return announce_received_ || !running_; });
            if (!running_) return;

            http_client_->setBaseURL("http://" + fysetc_ip_);
            announce_received_ = false;
            state = State::INITIALIZING;
            backoff_sec = 5;
            consecutive_errors_ = 0;
            break;
        }

        case State::INITIALIZING: {
            if (doInit()) {
                state = State::POLLING;
                consecutive_errors_ = 0;
                std::cout << "FysetcPoll: Init complete, entering poll loop ("
                          << poll_interval_sec_ << "s interval)" << std::endl;
            } else {
                consecutive_errors_++;
                if (consecutive_errors_ >= MAX_ERRORS_BEFORE_BACKOFF) {
                    state = State::ERROR_BACKOFF;
                } else {
                    sleepSeconds(5);
                }
            }
            break;
        }

        case State::POLLING: {
            sleepSeconds(poll_interval_sec_);
            if (!running_) return;

            // Check for IP change (re-announce while polling)
            {
                std::lock_guard<std::mutex> lock(announce_mutex_);
                if (announce_received_) {
                    http_client_->setBaseURL("http://" + fysetc_ip_);
                    announce_received_ = false;
                    state = State::INITIALIZING;
                    std::cout << "FysetcPoll: IP changed, re-initializing" << std::endl;
                    break;
                }
            }

            if (doPollCycle()) {
                consecutive_errors_ = 0;
            } else {
                consecutive_errors_++;
                if (consecutive_errors_ >= MAX_ERRORS_BEFORE_BACKOFF) {
                    state = State::ERROR_BACKOFF;
                    std::cerr << "FysetcPoll: " << consecutive_errors_
                              << " consecutive errors, backing off" << std::endl;
                }
            }
            break;
        }

        case State::ERROR_BACKOFF: {
            std::cerr << "FysetcPoll: Error backoff " << backoff_sec << "s" << std::endl;
            sleepSeconds(backoff_sec);
            if (!running_) return;

            // Try health check
            if (http_client_->healthCheck()) {
                state = State::INITIALIZING;
                backoff_sec = 5;
                consecutive_errors_ = 0;
                std::cout << "FysetcPoll: Device back online, re-initializing" << std::endl;
            } else {
                backoff_sec = std::min(backoff_sec * 2, 300); // cap at 5 min
            }

            // Check for new announce
            {
                std::lock_guard<std::mutex> lock(announce_mutex_);
                if (announce_received_) {
                    http_client_->setBaseURL("http://" + fysetc_ip_);
                    announce_received_ = false;
                    state = State::INITIALIZING;
                    backoff_sec = 5;
                }
            }
            break;
        }

        } // switch
    }
}

bool FysetcPollService::doInit() {
    auto known_files = db_->getFysetcFileOffsets(device_id_);
    std::cout << "FysetcPoll: /init with " << known_files.size() << " known file(s)" << std::endl;

    int negotiated_interval = poll_interval_sec_;
    if (!http_client_->init(poll_interval_sec_, known_files, negotiated_interval)) {
        std::cerr << "FysetcPoll: /init failed" << std::endl;
        return false;
    }

    poll_interval_sec_ = negotiated_interval;
    return true;
}

bool FysetcPollService::doPollCycle() {
    auto poll_result = http_client_->poll();
    if (!poll_result) {
        std::cerr << "FysetcPoll: /poll failed" << std::endl;
        return false;
    }

    const auto& resp = *poll_result;

    if (resp.status == "pending") {
        std::cout << "FysetcPoll: Scan pending, retry in " << resp.next_poll_sec << "s" << std::endl;
        return true; // not an error
    }

    if (resp.status == "no_change") {
        // Increment stable count for all tracked date folders
        for (auto& [folder, count] : stable_poll_count_) {
            if (!processed_folders_[folder]) {
                count++;
                if (count >= STABLE_POLLS_FOR_COMPLETION) {
                    std::cout << "FysetcPoll: Date folder " << folder
                              << " stable for " << count << " polls, processing" << std::endl;
                    processCompletedDateFolder(folder);
                    processed_folders_[folder] = true;
                }
            }
        }
        return true;
    }

    if (resp.status != "ok") {
        std::cerr << "FysetcPoll: Unexpected status: " << resp.status << std::endl;
        return false;
    }

    // status == "ok": we have data
    std::cout << "FysetcPoll: Cycle " << resp.cycle
              << " - new_dirs=" << resp.new_dirs.size()
              << " new_files=" << resp.new_files.size()
              << " deltas=" << resp.deltas.size() << std::endl;

    // Create directories
    for (const auto& dir : resp.new_dirs) {
        std::string date_folder = dir.name;
        std::string local_dir = data_dir_ + "/" + date_folder;
        std::filesystem::create_directories(local_dir);

        // Initialize stability tracking
        if (stable_poll_count_.find(date_folder) == stable_poll_count_.end()) {
            stable_poll_count_[date_folder] = 0;
            processed_folders_[date_folder] = false;
        }
    }

    // Fetch new files and deltas, collect confirmed offsets for /ack
    std::vector<std::pair<std::string, int64_t>> confirmed;

    for (const auto& file : resp.new_files) {
        std::string date_folder = extractDateFolder(file.path);
        if (!date_folder.empty()) {
            std::filesystem::create_directories(data_dir_ + "/" + date_folder);
            if (stable_poll_count_.find(date_folder) == stable_poll_count_.end()) {
                stable_poll_count_[date_folder] = 0;
                processed_folders_[date_folder] = false;
            }
        }

        int64_t offset = fetchAndSaveFile(file.path, 0);
        if (offset >= 0) {
            confirmed.emplace_back(file.path, offset);
        }

        // Reset stability for this folder
        if (!date_folder.empty()) {
            stable_poll_count_[date_folder] = 0;
            processed_folders_[date_folder] = false;
        }
    }

    for (const auto& delta : resp.deltas) {
        std::string date_folder = extractDateFolder(delta.path);

        int64_t offset = fetchAndSaveFile(delta.path, delta.from);
        if (offset >= 0) {
            confirmed.emplace_back(delta.path, offset);
        }

        // Reset stability for this folder
        if (!date_folder.empty()) {
            stable_poll_count_[date_folder] = 0;
            processed_folders_[date_folder] = false;
        }
    }

    // POST /ack
    if (!confirmed.empty()) {
        auto ack_resp = http_client_->ack(confirmed);
        if (ack_resp) {
            std::cout << "FysetcPoll: Acked " << ack_resp->n_acked << " file(s)" << std::endl;
        } else {
            std::cerr << "FysetcPoll: /ack failed" << std::endl;
        }

        // Save offsets to DB
        db_->saveFysetcFileOffsets(device_id_, confirmed);
    }

    return true;
}

// ── File fetch ──────────────────────────────────────────────────────────────

int64_t FysetcPollService::fetchAndSaveFile(const std::string& path, int64_t offset) {
    std::string date_folder = extractDateFolder(path);
    std::string filename = extractFilename(path);

    // Determine local file path
    std::string local_path;
    if (date_folder.empty()) {
        // Root level file (e.g., DATALOG/STR.EDF)
        local_path = data_dir_ + "/" + filename;
    } else {
        std::string dir = data_dir_ + "/" + date_folder;
        std::filesystem::create_directories(dir);
        local_path = dir + "/" + filename;
    }

    // Retry up to 3 times on 500 (SD busy)
    for (int attempt = 0; attempt < 3; attempt++) {
        int http_status = 0;
        auto data = http_client_->fetchFile(path, offset, http_status);

        if (http_status == 200 && !data.empty()) {
            // Write to local file
            std::ios_base::openmode mode = (offset > 0)
                ? (std::ios::binary | std::ios::in | std::ios::out)
                : (std::ios::binary | std::ios::trunc | std::ios::out);

            std::ofstream ofs(local_path, mode);
            if (!ofs) {
                // File may not exist for in/out mode, try create
                ofs.open(local_path, std::ios::binary | std::ios::out);
            }
            if (!ofs) {
                std::cerr << "FysetcPoll: Cannot open " << local_path << " for writing" << std::endl;
                return -1;
            }

            if (offset > 0) {
                ofs.seekp(offset);
            }
            ofs.write(reinterpret_cast<const char*>(data.data()),
                      static_cast<std::streamsize>(data.size()));
            ofs.close();

            int64_t confirmed_offset = offset + static_cast<int64_t>(data.size());
            std::cout << "FysetcPoll: Saved " << path << " [" << offset
                      << ".." << confirmed_offset << "] (" << data.size() << " bytes)" << std::endl;
            return confirmed_offset;
        }

        if (http_status == 500) {
            // SD busy (CPAP owns bus), wait and retry
            std::cout << "FysetcPoll: SD busy for " << path
                      << ", retry " << (attempt + 1) << "/3 in 10s" << std::endl;
            sleepSeconds(10);
            if (!running_) return -1;
            continue;
        }

        if (http_status == 404) {
            std::cerr << "FysetcPoll: File not found: " << path << std::endl;
            return -1;
        }

        // Other error (timeout, connection refused, etc.)
        std::cerr << "FysetcPoll: Fetch error for " << path
                  << " (status=" << http_status << ")" << std::endl;
        return -1;
    }

    std::cerr << "FysetcPoll: Gave up on " << path << " after 3 SD-busy retries" << std::endl;
    return -1;
}

// ── Path helpers ────────────────────────────────────────────────────────────

std::string FysetcPollService::extractDateFolder(const std::string& path) {
    // "DATALOG/20260328/BRP.EDF" -> "20260328"
    // "DATALOG/STR.EDF" -> ""
    auto parts = std::filesystem::path(path);
    std::vector<std::string> components;
    for (const auto& part : parts) {
        components.push_back(part.string());
    }

    // Expect: ["DATALOG", "20260328", "BRP.EDF"] for date-folder files
    if (components.size() >= 3) {
        const auto& candidate = components[1];
        // Verify it's a date folder (8 digits)
        if (candidate.size() == 8 &&
            std::all_of(candidate.begin(), candidate.end(), ::isdigit)) {
            return candidate;
        }
    }
    return "";
}

std::string FysetcPollService::extractFilename(const std::string& path) {
    return std::filesystem::path(path).filename().string();
}

// ── Session processing ──────────────────────────────────────────────────────

void FysetcPollService::processCompletedDateFolder(const std::string& date_folder) {
    std::string session_dir = data_dir_ + "/" + date_folder;
    if (!std::filesystem::exists(session_dir)) {
        std::cerr << "FysetcPoll: No data directory at " << session_dir << std::endl;
        return;
    }

    std::cout << "FysetcPoll: Processing completed folder " << date_folder << std::endl;

    // Group files into sessions using existing logic
    auto sessions = SessionDiscoveryService::groupLocalFolder(session_dir, date_folder);
    if (sessions.empty()) {
        std::cerr << "FysetcPoll: No sessions found in " << session_dir << std::endl;
        return;
    }

    std::cout << "FysetcPoll: Found " << sessions.size()
              << " session group(s) in " << date_folder << std::endl;

    for (const auto& session : sessions) {
        // Check if already in DB
        if (db_->sessionExists(device_id_, session.session_start)) {
            continue;
        }

        // Create temp directory with only this session's files (symlinks)
        std::string temp_dir = "/tmp/cpap_fysetc_poll/" + date_folder + "_" + session.session_prefix;
        std::filesystem::create_directories(temp_dir);

        // Clean existing symlinks
        for (const auto& entry : std::filesystem::directory_iterator(temp_dir)) {
            std::filesystem::remove(entry.path());
        }

        auto symlinkFile = [&](const std::string& filename) {
            std::filesystem::path src = std::filesystem::path(session_dir) / filename;
            std::filesystem::path dst = std::filesystem::path(temp_dir) / filename;
            if (std::filesystem::exists(src)) {
                std::error_code ec;
                std::filesystem::create_symlink(src, dst, ec);
            }
        };

        for (const auto& f : session.brp_files) symlinkFile(f);
        for (const auto& f : session.pld_files) symlinkFile(f);
        for (const auto& f : session.sad_files) symlinkFile(f);
        if (!session.csl_file.empty()) symlinkFile(session.csl_file);
        if (!session.eve_file.empty()) symlinkFile(session.eve_file);

        // Parse EDF files
        auto parsed = EDFParser::parseSession(
            temp_dir, device_id_, device_name_, session.session_start);

        if (!parsed) {
            std::cerr << "FysetcPoll: Failed to parse session "
                      << session.session_prefix << std::endl;
            std::filesystem::remove_all(temp_dir);
            continue;
        }

        // Set relative file paths
        std::string relative_base = "DATALOG/" + date_folder + "/";
        if (!session.brp_files.empty()) parsed->brp_file_path = relative_base + session.brp_files[0];
        if (!session.eve_file.empty()) parsed->eve_file_path = relative_base + session.eve_file;
        if (!session.sad_files.empty()) parsed->sad_file_path = relative_base + session.sad_files[0];
        if (!session.pld_files.empty()) parsed->pld_file_path = relative_base + session.pld_files[0];
        if (!session.csl_file.empty()) parsed->csl_file_path = relative_base + session.csl_file;

        // Save + publish
        if (publisher_->publishSession(*parsed)) {
            double hours = parsed->duration_seconds.value_or(0) / 3600.0;
            double ahi = parsed->metrics.has_value() ? parsed->metrics->ahi : 0.0;
            std::cout << "FysetcPoll: Published session " << session.session_prefix
                      << " (" << std::fixed << std::setprecision(1) << hours << "h"
                      << ", AHI=" << std::setprecision(2) << ahi << ")" << std::endl;

            // Mark completed + publish nightly metrics
            if (parsed->session_start.has_value()) {
                db_->markSessionCompleted(device_id_, *parsed->session_start);
                auto nightly = db_->getNightlyMetrics(device_id_, *parsed->session_start);
                if (nightly.has_value()) {
                    publisher_->publishHistoricalState(nightly.value());
                }
                publisher_->publishSessionCompleted();
            }
        }

        std::filesystem::remove_all(temp_dir);
    }

    // Process STR.edf
    auto str_record = processSTRFile();

    // Generate LLM summary (non-fatal)
    if (llm_enabled_ && llm_client_) {
        auto last_start = db_->getLastSessionStart(device_id_);
        if (last_start.has_value()) {
            auto nightly = db_->getNightlyMetrics(device_id_, *last_start);
            if (nightly.has_value()) {
                generateAndPublishSummary(nightly.value(),
                    str_record.has_value() ? &str_record.value() : nullptr);
            }
        }
    }

    // Archive to permanent storage
    archiveFiles(date_folder);

    // Clean temp
    std::filesystem::remove_all("/tmp/cpap_fysetc_poll");
}

std::optional<STRDailyRecord> FysetcPollService::processSTRFile() {
    std::vector<std::string> candidates = {
        data_dir_ + "/STR.edf",
        data_dir_ + "/STR.EDF",
        archive_dir_ + "/STR.edf",
        archive_dir_ + "/STR.EDF",
    };

    std::string str_path;
    for (const auto& path : candidates) {
        if (std::filesystem::exists(path)) {
            str_path = path;
            break;
        }
    }

    if (str_path.empty()) {
        std::cout << "FysetcPoll: STR.edf not available yet" << std::endl;
        return std::nullopt;
    }

    auto records = EDFParser::parseSTRFile(str_path, device_id_);
    if (records.empty()) {
        std::cerr << "FysetcPoll: No therapy days found in STR.edf" << std::endl;
        return std::nullopt;
    }

    size_t save_count = std::min(records.size(), static_cast<size_t>(7));
    std::vector<STRDailyRecord> recent(records.end() - save_count, records.end());

    if (db_->saveSTRDailyRecords(recent)) {
        std::cout << "FysetcPoll: Saved " << save_count << " STR daily record(s)" << std::endl;

        const auto& latest = records.back();
        if (latest.hasTherapy()) {
            double nightly_ahi = 0;
            auto last_start = db_->getLastSessionStart(device_id_);
            if (last_start.has_value()) {
                auto nightly = db_->getNightlyMetrics(device_id_, *last_start);
                if (nightly.has_value()) {
                    nightly_ahi = nightly->ahi;
                }
            }
            publisher_->publishSTRState(latest, nightly_ahi);
        }

        return records.back();
    }

    return std::nullopt;
}

// ── LLM summary ────────────────────────────────────────────────────────────

void FysetcPollService::generateAndPublishSummary(const SessionMetrics& metrics,
                                                   const STRDailyRecord* str_record) {
    std::cout << "FysetcPoll LLM: Generating session summary..." << std::endl;

    std::string metrics_str = buildMetricsString(metrics, str_record);
    std::string prompt = hms::LLMClient::substituteTemplate(
        llm_prompt_template_, {{"metrics", metrics_str}});

    auto summary = llm_client_->generate(prompt);
    if (!summary) {
        std::cerr << "FysetcPoll LLM: Summary generation failed (non-fatal)" << std::endl;
        return;
    }

    std::cout << "FysetcPoll LLM: Summary generated (" << summary->size() << " chars)" << std::endl;

    if (publisher_) {
        publisher_->publishSessionSummary(summary.value());
    }
}

std::string FysetcPollService::buildMetricsString(const SessionMetrics& metrics,
                                                   const STRDailyRecord* str_record) const {
    std::ostringstream oss;
    oss << std::fixed << std::setprecision(2);

    oss << "Usage: " << metrics.usage_hours.value_or(0.0) << " hours"
        << " (" << metrics.usage_percent.value_or(0.0) << "% of 8h target)\n";
    oss << "AHI: " << metrics.ahi << " events/hour\n";
    oss << "Total events: " << metrics.total_events
        << " (obstructive=" << metrics.obstructive_apneas
        << ", central=" << metrics.central_apneas
        << ", hypopnea=" << metrics.hypopneas
        << ", RERA=" << metrics.reras << ")\n";

    if (metrics.avg_pressure.has_value()) {
        oss << "Pressure: avg=" << metrics.avg_pressure.value() << " cmH2O";
        if (metrics.pressure_p95.has_value()) oss << ", 95th=" << metrics.pressure_p95.value();
        oss << "\n";
    }

    if (metrics.avg_leak_rate.has_value()) {
        oss << "Leak: avg=" << metrics.avg_leak_rate.value() << " L/min";
        if (metrics.max_leak_rate.has_value()) oss << ", max=" << metrics.max_leak_rate.value();
        oss << "\n";
    }

    if (str_record && str_record->hasTherapy()) {
        oss << "\nSTR daily summary:"
            << " AHI=" << str_record->ahi
            << " usage=" << (str_record->duration_minutes / 60.0) << "h"
            << " leak_95=" << str_record->leak_95 << " L/min"
            << " mask_press_95=" << str_record->mask_press_95 << " cmH2O\n";
    }

    return oss.str();
}

// ── Archival ────────────────────────────────────────────────────────────────

void FysetcPollService::archiveFiles(const std::string& date_folder) {
    std::string src_dir = data_dir_ + "/" + date_folder;
    std::string dest_dir = archive_dir_ + "/" + date_folder;

    if (!std::filesystem::exists(src_dir)) return;

    std::filesystem::create_directories(dest_dir);

    for (const auto& entry : std::filesystem::directory_iterator(src_dir)) {
        if (entry.is_regular_file()) {
            std::filesystem::path dest = std::filesystem::path(dest_dir) / entry.path().filename();
            std::error_code ec;
            std::filesystem::copy_file(entry.path(), dest,
                                       std::filesystem::copy_options::overwrite_existing, ec);
            if (ec) {
                std::cerr << "FysetcPoll: Failed to archive "
                          << entry.path().filename() << ": " << ec.message() << std::endl;
            }
        }
    }

    std::cout << "FysetcPoll: Archived " << date_folder << " to " << dest_dir << std::endl;
}

// ── Sleep helper ────────────────────────────────────────────────────────────

void FysetcPollService::sleepSeconds(int seconds) {
    for (int i = 0; i < seconds && running_; i++) {
        std::this_thread::sleep_for(std::chrono::seconds(1));
    }
}

} // namespace hms_cpap
