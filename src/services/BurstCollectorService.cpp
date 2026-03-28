#include "services/BurstCollectorService.h"
#include "services/InsightsEngine.h"
#include "utils/ConfigManager.h"
#include "utils/FileUtils.h"
#include <iostream>
#include <iomanip>
#include <sstream>
#include <fstream>
#include <filesystem>
#include <algorithm>
#include <set>

namespace hms_cpap {

BurstCollectorService::BurstCollectorService(int burst_interval_seconds)
    : burst_interval_seconds_(burst_interval_seconds),
      running_(false) {

    // Load configuration from environment
    device_id_ = ConfigManager::get("CPAP_DEVICE_ID", "cpap_resmed_23243570851");
    device_name_ = ConfigManager::get("CPAP_DEVICE_NAME", "ResMed AirSense 10");

    // Check for local source mode (CPAP_SOURCE=local + CPAP_LOCAL_DIR)
    std::string source = ConfigManager::get("CPAP_SOURCE", "ezshare");
    if (source == "local") {
        local_source_dir_ = ConfigManager::get("CPAP_LOCAL_DIR", "");
        if (local_source_dir_.empty()) {
            std::cerr << "CPAP_SOURCE=local but CPAP_LOCAL_DIR not set!" << std::endl;
            throw std::runtime_error("CPAP_LOCAL_DIR required when CPAP_SOURCE=local");
        }
        std::cout << "CPAP: Local source mode — reading from " << local_source_dir_ << std::endl;
    } else {
        // ez Share mode: initialize HTTP client and discovery service
        ezshare_client_ = std::make_unique<EzShareClient>();
        discovery_service_ = std::make_unique<SessionDiscoveryService>(*ezshare_client_);
    }

    // Initialize MQTT client
    std::string mqtt_broker = ConfigManager::get("MQTT_BROKER", "192.168.2.15");
    std::string mqtt_port = ConfigManager::get("MQTT_PORT", "1883");
    std::string mqtt_user = ConfigManager::get("MQTT_USER", "aamat");
    std::string mqtt_password = ConfigManager::get("MQTT_PASSWORD", "exploracion");
    std::string mqtt_client_id = ConfigManager::get("MQTT_CLIENT_ID", "hms_cpap_service");

    hms::MqttConfig mqtt_config;
    mqtt_config.broker = mqtt_broker;
    mqtt_config.port = std::stoi(mqtt_port);
    mqtt_config.username = mqtt_user;
    mqtt_config.password = mqtt_password;
    mqtt_config.client_id = mqtt_client_id;

    mqtt_client_ = std::make_shared<hms::MqttClient>(mqtt_config);
    std::string broker_address = "tcp://" + mqtt_broker + ":" + mqtt_port;

    if (mqtt_client_->connect()) {
        std::cout << "✅ MQTT: Connected to " << broker_address << std::endl;
    } else {
        std::cerr << "⚠️  MQTT: Connection failed (will retry)" << std::endl;
    }

    // Initialize database service
    std::string db_host = ConfigManager::get("DB_HOST", "localhost");
    std::string db_port = ConfigManager::get("DB_PORT", "5432");
    std::string db_name = ConfigManager::get("DB_NAME", "cpap_monitoring");
    std::string db_user = ConfigManager::get("DB_USER", "maestro");
    std::string db_password = ConfigManager::get("DB_PASSWORD", "maestro_postgres_2026_secure");

    std::string connection_string = "host=" + db_host + " port=" + db_port +
                                   " dbname=" + db_name + " user=" + db_user +
                                   " password=" + db_password;

    db_service_ = std::make_shared<DatabaseService>(connection_string);

    if (db_service_->connect()) {
        std::cout << "✅ DB: Connected to " << db_name << std::endl;
    } else {
        std::cerr << "⚠️  DB: Connection failed (will retry)" << std::endl;
    }

    // Initialize data publisher
    data_publisher_ = std::make_unique<DataPublisherService>(mqtt_client_, db_service_);
    data_publisher_->initialize();

    std::cout << "🚀 BurstCollectorService initialized" << std::endl;
    std::cout << "   Device: " << device_name_ << " (" << device_id_ << ")" << std::endl;
    std::cout << "   Burst interval: " << burst_interval_seconds_ << " seconds" << std::endl;
    if (local_source_dir_.empty()) {
        std::cout << "   Source: ez Share at " << ConfigManager::get("EZSHARE_BASE_URL", "http://192.168.4.1") << std::endl;
    } else {
        std::cout << "   Source: Local directory at " << local_source_dir_ << std::endl;
    }

    // Initialize LLM client (optional)
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
        llm_config.max_tokens = ConfigManager::getInt("LLM_MAX_TOKENS", 1024);
        llm_config.keep_alive_seconds = ConfigManager::getInt("LLM_KEEP_ALIVE", 0);

        llm_client_ = std::make_unique<hms::LLMClient>(llm_config);

        // Load prompt template
        std::string prompt_file = ConfigManager::get("LLM_PROMPT_FILE", "");
        if (!prompt_file.empty()) {
            llm_prompt_template_ = hms::LLMClient::loadPromptFile(prompt_file);
        }
        if (llm_prompt_template_.empty()) {
            llm_prompt_template_ =
                "Summarize this CPAP session in 3-5 sentences. "
                "Include AHI assessment, usage compliance, and any concerns.\n\n"
                "Session data:\n{metrics}";
        }

        std::cout << "LLM: Enabled (" << hms::LLMClient::providerName(llm_config.provider)
                  << " / " << llm_config.model << " at " << llm_config.endpoint << ")" << std::endl;
    }

    // Subscribe to summary regeneration command
    if (llm_enabled_ && mqtt_client_ && mqtt_client_->isConnected()) {
        std::string cmd_topic = "cpap/" + device_id_ + "/command/regenerate_summary";
        mqtt_client_->subscribe(cmd_topic,
            [this](const std::string& /*topic*/, const std::string& /*payload*/) {
                std::cout << "LLM: Regenerate summary requested via MQTT" << std::endl;
                auto last_start = db_service_->getLastSessionStart(device_id_);
                if (!last_start.has_value()) {
                    std::cerr << "LLM: No sessions found for summary regeneration" << std::endl;
                    return;
                }
                auto metrics = db_service_->getNightlyMetrics(device_id_, last_start.value());
                if (!metrics.has_value()) {
                    std::cerr << "LLM: No metrics found for latest session" << std::endl;
                    return;
                }
                generateAndPublishSummary(metrics.value());
            }, 1);
        std::cout << "LLM: Subscribed to " << cmd_topic << " for on-demand summary" << std::endl;

        // On-demand weekly/monthly summary commands.
        // Payload is optional JSON: {"days": 14} overrides the default range.
        // Empty payload or no "days" key → default (7 for weekly, 30 for monthly).
        for (auto [cmd, period] : std::vector<std::pair<std::string, SummaryPeriod>>{
                 {"generate_weekly_summary",  SummaryPeriod::WEEKLY},
                 {"generate_monthly_summary", SummaryPeriod::MONTHLY}}) {
            std::string topic = "cpap/" + device_id_ + "/command/" + cmd;
            mqtt_client_->subscribe(topic,
                [this, period](const std::string& /*topic*/, const std::string& payload) {
                    int days_override = 0;
                    if (!payload.empty()) {
                        Json::Value json;
                        Json::CharReaderBuilder rb;
                        std::istringstream ss(payload);
                        if (Json::parseFromStream(rb, ss, &json, nullptr)
                            && json.isMember("days") && json["days"].isInt()) {
                            days_override = json["days"].asInt();
                        }
                    }
                    generateRangeSummary(period, days_override);
                }, 1);
            std::cout << "LLM: Subscribed to " << topic << std::endl;
        }
    }

    // Subscribe to insights regeneration command
    if (mqtt_client_ && mqtt_client_->isConnected()) {
        std::string insights_topic = "cpap/" + device_id_ + "/command/regenerate_insights";
        mqtt_client_->subscribe(insights_topic,
            [this](const std::string& /*topic*/, const std::string& /*payload*/) {
                std::cout << "Insights: Regenerate requested via MQTT" << std::endl;
                if (last_str_records_.empty()) {
                    // Cache is empty (e.g. after restart) -- download and parse STR first
                    std::cout << "Insights: Cache empty, running processSTRFile..." << std::endl;
                    processSTRFile();
                }
                if (last_str_records_.empty()) {
                    std::cerr << "Insights: Still no STR records after download" << std::endl;
                    return;
                }
                auto insights = InsightsEngine::analyze(last_str_records_);
                if (data_publisher_) {
                    data_publisher_->publishInsights(insights);
                }
            }, 1);
        std::cout << "Insights: Subscribed to " << insights_topic << " for on-demand regeneration" << std::endl;
    }

    // Subscribe to force_complete command (manual override for stuck sessions)
    if (mqtt_client_ && mqtt_client_->isConnected()) {
        std::string complete_topic = "cpap/" + device_id_ + "/command/force_complete";
        mqtt_client_->subscribe(complete_topic,
            [this](const std::string& /*topic*/, const std::string& /*payload*/) {
                std::cout << "Force complete: requested via MQTT" << std::endl;
                auto last_start = db_service_->getLastSessionStart(device_id_);
                if (!last_start.has_value()) {
                    std::cerr << "Force complete: no session found in DB" << std::endl;
                    return;
                }
                db_service_->markSessionCompleted(device_id_, last_start.value());
                db_service_->setForceCompleted(device_id_, last_start.value());
                if (data_publisher_) {
                    data_publisher_->publishSessionCompleted();
                    processSTRFile();
                    auto metrics = db_service_->getNightlyMetrics(device_id_, last_start.value());
                    if (metrics.has_value()) {
                        data_publisher_->publishHistoricalState(metrics.value());
                        if (llm_enabled_ && llm_client_) {
                            const STRDailyRecord* str_rec = !last_str_records_.empty()
                                ? &last_str_records_.back() : nullptr;
                            generateAndPublishSummary(metrics.value(), str_rec);
                        }
                    }
                }
            }, 1);
        std::cout << "Commands: Subscribed to " << complete_topic << std::endl;
    }

    // STARTUP CLEANUP: Clear any stale session_active state from previous run
    // This prevents stuck "session active" in HA after service restart or crash
    if (data_publisher_ && mqtt_client_ && mqtt_client_->isConnected()) {
        std::cout << "Startup: Clearing stale session_active state..." << std::endl;
        data_publisher_->publishSessionCompleted();
        session_active_cleared_ = true;
    }

}

BurstCollectorService::~BurstCollectorService() {
    stop();
}

void BurstCollectorService::start() {
    if (running_) {
        std::cout << "⚠️  BurstCollectorService already running" << std::endl;
        return;
    }

    running_ = true;
    worker_thread_ = std::thread(&BurstCollectorService::runLoop, this);
    std::cout << "✅ BurstCollectorService started" << std::endl;
}

void BurstCollectorService::stop() {
    if (!running_) {
        return;
    }

    std::cout << "🛑 Stopping BurstCollectorService..." << std::endl;
    running_ = false;

    if (worker_thread_.joinable()) {
        worker_thread_.join();
    }

    std::cout << "✅ BurstCollectorService stopped" << std::endl;
}

bool BurstCollectorService::isRunning() const {
    return running_;
}

std::chrono::system_clock::time_point BurstCollectorService::getLastBurstTime() const {
    return last_burst_time_;
}

bool BurstCollectorService::downloadSessionFiles(
    const SessionFileSet& session,
    const std::string& local_base_dir) {

    std::cout << "📥 CPAP: Downloading session " << session.session_prefix
              << " (" << session.total_size_kb << " KB)" << std::endl;
    std::cout << "    Files: CSL=" << (session.csl_file.empty() ? "N" : "Y")
              << ", EVE=" << (session.eve_file.empty() ? "N" : "Y")
              << ", BRP=" << session.brp_files.size()
              << ", PLD=" << session.pld_files.size()
              << ", SAD=" << session.sad_files.size() << std::endl;

    // Keep original structure: all sessions from same date in same folder (like ez Share)
    std::string local_dir = local_base_dir + "/" + session.date_folder;
    std::filesystem::create_directories(local_dir);

    int downloaded = 0;
    int range_downloads = 0;
    int full_downloads = 0;

    // Helper lambda for smart download (Range if file exists, full otherwise)
    auto smartDownload = [&](const std::string& filename, const std::string& local_path) -> bool {
        // Check if file exists locally
        bool file_exists = std::filesystem::exists(local_path);
        size_t existing_size = file_exists ? std::filesystem::file_size(local_path) : 0;

        if (file_exists && existing_size > 0) {
            // Use Range download (incremental)
            size_t bytes_downloaded = 0;
            bool success = ezshare_client_->downloadFileRange(
                session.date_folder, filename, local_path, existing_size, bytes_downloaded
            );

            if (success) {
                if (bytes_downloaded > 0) {
                    range_downloads++;
                    std::cout << "  📥 Range: " << filename << " +" << bytes_downloaded
                              << " bytes (from byte " << existing_size << ")" << std::endl;
                } else {
                    std::cout << "  ✓ " << filename << " unchanged" << std::endl;
                }
                return true;
            }

            // Range failed, fallback to full download
            std::cerr << "⚠️  Range download failed for " << filename << ", trying full download..." << std::endl;
            std::filesystem::remove(local_path);  // Remove partial file
        }

        // Full download (new file or Range fallback)
        if (ezshare_client_->downloadFile(session.date_folder, filename, local_path)) {
            full_downloads++;
            return true;
        }

        return false;
    };

    // Download CSL (session summary) - always full download (small, doesn't grow)
    if (!session.csl_file.empty()) {
        std::string local_path = local_dir + "/" + session.csl_file;
        if (ezshare_client_->downloadFile(session.date_folder, session.csl_file, local_path)) {
            downloaded++;
            full_downloads++;
        } else {
            std::cerr << "⚠️  CPAP: Failed to download CSL: " << session.csl_file << std::endl;
        }
    }

    // Download EVE (events) - always full download (small, doesn't grow)
    if (!session.eve_file.empty()) {
        std::string local_path = local_dir + "/" + session.eve_file;
        if (ezshare_client_->downloadFile(session.date_folder, session.eve_file, local_path)) {
            downloaded++;
            full_downloads++;
        } else {
            std::cerr << "⚠️  CPAP: Failed to download EVE: " << session.eve_file << std::endl;
        }
    }

    // Download ALL BRP checkpoint files (use Range for growing files)
    for (const auto& filename : session.brp_files) {
        std::string local_path = local_dir + "/" + filename;
        if (smartDownload(filename, local_path)) {
            downloaded++;
        } else {
            std::cerr << "⚠️  CPAP: Failed to download BRP: " << filename << std::endl;
        }
    }

    // Download ALL PLD checkpoint files (use Range for growing files)
    for (const auto& filename : session.pld_files) {
        std::string local_path = local_dir + "/" + filename;
        if (smartDownload(filename, local_path)) {
            downloaded++;
        } else {
            std::cerr << "⚠️  CPAP: Failed to download PLD: " << filename << std::endl;
        }
    }

    // Download ALL SAD checkpoint files (use Range for growing files)
    for (const auto& filename : session.sad_files) {
        std::string local_path = local_dir + "/" + filename;
        if (smartDownload(filename, local_path)) {
            downloaded++;
        } else {
            std::cerr << "⚠️  CPAP: Failed to download SAD: " << filename << std::endl;
        }
    }

    // Accept ANY downloaded files (even partial/in-progress sessions)
    bool success = downloaded > 0;

    if (success) {
        std::cout << "✅ CPAP: Session " << session.session_prefix
                  << " downloaded (" << downloaded << " file(s)"
                  << " - " << range_downloads << " Range, " << full_downloads << " Full)"
                  << std::endl;
    } else {
        std::cerr << "❌ CPAP: Session " << session.session_prefix
                  << " - no files downloaded" << std::endl;
    }

    return success;
}

bool BurstCollectorService::archiveSessionFiles(
    const std::string& date_folder,
    const std::string& temp_base_dir,
    const std::string& archive_base_dir) {

    std::string temp_dir = temp_base_dir + "/" + date_folder;
    std::string archive_dir = archive_base_dir + "/DATALOG/" + date_folder;

    // Check if temp directory exists
    if (!std::filesystem::exists(temp_dir)) {
        std::cout << "📦 Archive: No temp files for " << date_folder << ", skipping" << std::endl;
        return true;  // Not an error, just nothing to archive
    }

    // Create archive directory
    std::filesystem::create_directories(archive_dir);

    int copied_count = 0;
    int skipped_count = 0;
    size_t total_bytes = 0;

    try {
        // Iterate through all files in temp directory
        for (const auto& entry : std::filesystem::directory_iterator(temp_dir)) {
            if (!entry.is_regular_file()) continue;

            std::string filename = entry.path().filename().string();
            std::string dest_path = archive_dir + "/" + filename;

            // Check if file already exists in archive
            if (std::filesystem::exists(dest_path)) {
                // Compare sizes - only skip if identical
                auto temp_size = std::filesystem::file_size(entry.path());
                auto archive_size = std::filesystem::file_size(dest_path);

                if (temp_size == archive_size) {
                    skipped_count++;
                    continue;
                }
            }

            // Copy file to archive
            std::filesystem::copy_file(
                entry.path(),
                dest_path,
                std::filesystem::copy_options::overwrite_existing
            );

            auto file_size = std::filesystem::file_size(dest_path);
            total_bytes += file_size;
            copied_count++;
        }

        if (copied_count > 0 || skipped_count > 0) {
            size_t total_kb = total_bytes / 1024;
            std::cout << "📦 Archive: " << date_folder
                      << " - Copied: " << copied_count
                      << ", Skipped: " << skipped_count
                      << " (" << total_kb << " KB)" << std::endl;
        }

        return true;

    } catch (const std::exception& e) {
        std::cerr << "❌ Archive: Failed to archive " << date_folder
                  << ": " << e.what() << std::endl;
        return false;
    }
}

void BurstCollectorService::processSTRFile() {
    try {
        std::string local_base = ConfigManager::get("CPAP_TEMP_DIR", "/tmp/cpap_data");
        std::string str_local_path = local_base + "/STR.EDF";

        // Download STR.EDF from SD card root
        if (!ezshare_client_->downloadRootFile("STR.EDF", str_local_path)) {
            std::cerr << "STR: Download failed (non-fatal)" << std::endl;
            return;
        }

        // Parse all daily records
        auto all_records = EDFParser::parseSTRFile(str_local_path, device_id_);
        if (all_records.empty()) {
            std::cerr << "STR: No therapy days found" << std::endl;
            return;
        }

        // Save last 7 days to DB (avoid rewriting full history every cycle)
        size_t save_count = std::min(all_records.size(), static_cast<size_t>(7));
        std::vector<STRDailyRecord> recent(
            all_records.end() - save_count, all_records.end());
        db_service_->saveSTRDailyRecords(recent);

        // Publish latest therapy day to MQTT
        const auto& latest = all_records.back();
        if (data_publisher_ && mqtt_client_ && mqtt_client_->isConnected()) {
            // Get our calculated nightly AHI for delta comparison
            double nightly_ahi = 0;
            auto nightly = db_service_->getNightlyMetrics(device_id_, latest.record_date);
            if (nightly.has_value()) {
                nightly_ahi = nightly->ahi;
            }
            data_publisher_->publishSTRState(latest, nightly_ahi);
        }

        // Cache for on-demand regeneration via MQTT command
        last_str_records_ = all_records;

        // Run insights engine on full STR history
        if (data_publisher_ && mqtt_client_ && mqtt_client_->isConnected()) {
            auto insights = InsightsEngine::analyze(all_records);
            data_publisher_->publishInsights(insights);
        }

        std::cout << "STR: Processed " << all_records.size() << " therapy days, saved "
                  << save_count << " recent to DB" << std::endl;

    } catch (const std::exception& e) {
        std::cerr << "STR: Processing failed (non-fatal): " << e.what() << std::endl;
    }
}

std::string BurstCollectorService::getCurrentDateString() const {
    auto now = std::chrono::system_clock::now();
    std::time_t now_time = std::chrono::system_clock::to_time_t(now);
    std::tm* now_tm = std::localtime(&now_time);

    std::ostringstream oss;
    oss << std::put_time(now_tm, "%Y%m%d");
    return oss.str();
}

bool BurstCollectorService::executeBurstCycle() {
    auto cycle_start = std::chrono::steady_clock::now();

    std::cout << "\n" << std::string(60, '=') << std::endl;
    std::cout << "🔄 CPAP: Starting burst cycle..." << std::endl;
    std::cout << std::string(60, '=') << std::endl;

    // Step 1: Query DB for last stored session (delta collection)
    auto last_session_start = db_service_->getLastSessionStart(device_id_);

    if (last_session_start.has_value()) {
        auto last_time = std::chrono::system_clock::to_time_t(last_session_start.value());
        std::cout << "📊 CPAP: Last stored session: "
                  << std::put_time(std::localtime(&last_time), "%Y-%m-%d %H:%M:%S")
                  << std::endl;
    } else {
        std::cout << "📊 CPAP: No previous sessions in DB (first run)" << std::endl;
    }

    // Step 2-5: Discover sessions and prepare for parsing
    // Two modes: ezShare (HTTP) or local filesystem
    std::vector<SessionFileSet> new_sessions;
    std::vector<std::pair<std::string, std::chrono::system_clock::time_point>> downloaded_sessions;
    auto download_start = std::chrono::steady_clock::now();

    if (!local_source_dir_.empty()) {
        // ===== LOCAL SOURCE MODE =====
        std::cout << "CPAP: Scanning local directory " << local_source_dir_ << std::endl;

        new_sessions = SessionDiscoveryService::discoverLocalSessions(
            local_source_dir_, last_session_start);

        if (new_sessions.empty()) {
            std::cout << "CPAP: No new sessions found locally" << std::endl;
            return true;
        }

        std::cout << "CPAP: Found " << new_sessions.size() << " session(s) to process" << std::endl;

        // For each session, create a temp directory with symlinks for session isolation
        // (parseSession reads ALL files in a dir, so we isolate each session's files)
        std::string temp_base = "/tmp/cpap_local";
        std::filesystem::create_directories(temp_base);

        for (const auto& session : new_sessions) {
            // Skip sessions that were force-completed (manual override)
            if (db_service_->isForceCompleted(device_id_, session.session_start)) {
                std::cout << "CPAP: Session " << session.session_prefix
                          << " force_completed, skipping" << std::endl;
                continue;
            }

            bool exists_in_db = db_service_->sessionExists(device_id_, session.session_start);

            if (exists_in_db) {
                // Check if checkpoint files changed (using file sizes from filesystem)
                auto db_checkpoint_sizes = db_service_->getCheckpointFileSizes(device_id_, session.session_start);

                std::map<std::string, int> current_checkpoint_sizes;
                for (const auto& [filename, size_kb] : session.file_sizes_kb) {
                    if (filename.find("_BRP.edf") != std::string::npos ||
                        filename.find("_PLD.edf") != std::string::npos ||
                        isOximetryFile(filename)) {
                        current_checkpoint_sizes[filename] = size_kb;
                    }
                }

                bool all_unchanged = true;
                for (const auto& [filename, db_size] : db_checkpoint_sizes) {
                    auto it = current_checkpoint_sizes.find(filename);
                    if (it == current_checkpoint_sizes.end() || it->second != db_size) {
                        all_unchanged = false;
                        break;
                    }
                }
                if (current_checkpoint_sizes.size() > db_checkpoint_sizes.size()) {
                    all_unchanged = false;
                }

                if (all_unchanged) {
                    std::cout << "CPAP: Session " << session.session_prefix
                              << " stopped (all checkpoint files unchanged)" << std::endl;

                    bool newly_completed = db_service_->markSessionCompleted(device_id_, session.session_start);

                    if (newly_completed && data_publisher_) {
                        auto metrics = db_service_->getNightlyMetrics(device_id_, session.session_start);
                        if (metrics.has_value()) {
                            data_publisher_->publishHistoricalState(metrics.value());
                            std::cout << "   Nightly metrics published ("
                                      << metrics.value().usage_hours.value_or(0.0) << "h, AHI "
                                      << metrics.value().ahi << ")" << std::endl;
                        }
                        data_publisher_->publishSessionCompleted();

                        // Generate LLM summary (non-fatal)
                        if (llm_enabled_ && llm_client_ && metrics.has_value()) {
                            generateAndPublishSummary(metrics.value());
                        }
                    }
                    continue;
                }

                std::cout << "CPAP: Session " << session.session_prefix
                          << " changed, re-parsing" << std::endl;
            } else {
                std::cout << "CPAP: New session " << session.session_prefix
                          << " (" << session.total_size_kb << " KB)" << std::endl;
            }

            // Create temp dir with symlinks for this session's files only
            std::string temp_dir = temp_base + "/" + session.date_folder + "_" + session.session_prefix;
            std::filesystem::create_directories(temp_dir);

            // Clear previous symlinks
            for (const auto& entry : std::filesystem::directory_iterator(temp_dir)) {
                std::filesystem::remove(entry.path());
            }

            std::string src_dir = local_source_dir_ + "/" + session.date_folder;
            auto symlinkFile = [&](const std::string& filename) {
                auto src = std::filesystem::path(src_dir) / filename;
                auto dst = std::filesystem::path(temp_dir) / filename;
                if (std::filesystem::exists(src)) {
                    std::filesystem::create_symlink(src, dst);
                }
            };

            for (const auto& f : session.brp_files) symlinkFile(f);
            for (const auto& f : session.pld_files) symlinkFile(f);
            for (const auto& f : session.sad_files) symlinkFile(f);
            if (!session.csl_file.empty()) symlinkFile(session.csl_file);
            if (!session.eve_file.empty()) symlinkFile(session.eve_file);

            // Store checkpoint sizes for change detection on next cycle
            std::map<std::string, int> checkpoint_sizes;
            for (const auto& [filename, size_kb] : session.file_sizes_kb) {
                if (filename.find("_BRP.edf") != std::string::npos ||
                    filename.find("_PLD.edf") != std::string::npos ||
                    isOximetryFile(filename)) {
                    checkpoint_sizes[filename] = size_kb;
                }
            }
            db_service_->updateCheckpointFileSizes(device_id_, session.session_start, checkpoint_sizes);

            downloaded_sessions.push_back({temp_dir, session.session_start});
        }

        if (downloaded_sessions.empty()) {
            std::cout << "CPAP: No sessions need processing" << std::endl;
            return true;
        }

    } else {
        // ===== EZSHARE MODE (original) =====
        std::cout << "CPAP: Accessing ez Share at " << ConfigManager::get("EZSHARE_BASE_URL", "http://192.168.4.1") << std::endl;

        try {
            new_sessions = discovery_service_->discoverNewSessions(last_session_start);
            consecutive_failures_ = 0;
            session_active_cleared_ = false;
        } catch (const std::exception& e) {
            std::cerr << "CPAP: Session discovery failed: " << e.what() << std::endl;

            consecutive_failures_++;
            std::cerr << "   Consecutive failures: " << consecutive_failures_
                      << "/" << MAX_FAILURES_BEFORE_RESET << std::endl;

            if (consecutive_failures_ >= MAX_FAILURES_BEFORE_RESET && !session_active_cleared_) {
                std::cout << "RECOVERY: Clearing session_active after "
                          << consecutive_failures_ << " consecutive failures" << std::endl;
                if (data_publisher_) {
                    data_publisher_->publishSessionCompleted();
                    session_active_cleared_ = true;
                }
            }

            return false;
        }

        if (new_sessions.empty()) {
            std::cout << "CPAP: No new sessions to download" << std::endl;
            return true;
        }

        std::cout << "CPAP: Found " << new_sessions.size() << " new session(s)" << std::endl;

        std::string local_base_dir = ConfigManager::get("CPAP_TEMP_DIR", "/tmp/cpap_data");

        for (const auto& session : new_sessions) {
            // Skip sessions that were force-completed (manual override)
            if (db_service_->isForceCompleted(device_id_, session.session_start)) {
                std::cout << "CPAP: Session " << session.session_prefix
                          << " force_completed, skipping" << std::endl;
                continue;
            }

            bool exists_in_db = db_service_->sessionExists(device_id_, session.session_start);

            if (!exists_in_db) {
                std::cout << "CPAP: New session " << session.session_prefix
                          << " (not in DB, " << session.total_size_kb << " KB)" << std::endl;

                if (downloadSessionFiles(session, local_base_dir)) {
                    std::map<std::string, int> checkpoint_sizes;
                    for (const auto& [filename, size_kb] : session.file_sizes_kb) {
                        if (filename.find("_BRP.edf") != std::string::npos ||
                            filename.find("_PLD.edf") != std::string::npos ||
                            isOximetryFile(filename)) {
                            checkpoint_sizes[filename] = size_kb;
                        }
                    }
                    db_service_->updateCheckpointFileSizes(device_id_, session.session_start, checkpoint_sizes);
                    std::string session_dir = local_base_dir + "/" + session.date_folder;
                    downloaded_sessions.push_back({session_dir, session.session_start});
                } else {
                    std::cerr << "CPAP: Failed to download session " << session.session_prefix << std::endl;
                }
                continue;
            }

            auto db_checkpoint_sizes = db_service_->getCheckpointFileSizes(device_id_, session.session_start);

            std::map<std::string, int> current_checkpoint_sizes;
            for (const auto& [filename, size_kb] : session.file_sizes_kb) {
                if (filename.find("_BRP.edf") != std::string::npos ||
                    filename.find("_PLD.edf") != std::string::npos ||
                    isOximetryFile(filename)) {
                    current_checkpoint_sizes[filename] = size_kb;
                }
            }

            bool all_unchanged = true;
            bool has_new_files = false;

            for (const auto& [filename, db_size] : db_checkpoint_sizes) {
                auto it = current_checkpoint_sizes.find(filename);
                if (it == current_checkpoint_sizes.end()) {
                    all_unchanged = false;
                    break;
                }
                if (it->second != db_size) {
                    all_unchanged = false;
                    break;
                }
            }

            if (current_checkpoint_sizes.size() > db_checkpoint_sizes.size()) {
                has_new_files = true;
                all_unchanged = false;
            }

            if (all_unchanged && !has_new_files) {
                std::cout << "CPAP: Session " << session.session_prefix
                          << " stopped (all checkpoint files unchanged)" << std::endl;

                // markSessionCompleted returns true only the FIRST time
                // (when session_end was not yet set). Returns false if already completed.
                bool newly_completed = db_service_->markSessionCompleted(device_id_, session.session_start);

                // Only trigger completion actions once, and only for the most recent
                // session by timestamp (not list position, which depends on scan order).
                // newly_completed ensures this fires exactly once (DB dedup).
                auto most_recent_start = std::max_element(
                    new_sessions.begin(), new_sessions.end(),
                    [](const SessionFileSet& a, const SessionFileSet& b) {
                        return a.session_start < b.session_start;
                    })->session_start;
                bool is_most_recent = (session.session_start == most_recent_start);

                if (newly_completed && is_most_recent && data_publisher_) {
                    auto metrics = db_service_->getNightlyMetrics(device_id_, session.session_start);
                    if (metrics.has_value()) {
                        data_publisher_->publishHistoricalState(metrics.value());
                        std::cout << "   Nightly metrics published ("
                                  << metrics.value().usage_hours.value_or(0.0) << "h, AHI "
                                  << metrics.value().ahi << ")" << std::endl;
                    }
                    data_publisher_->publishSessionCompleted();
                    processSTRFile();

                    // Generate LLM summary with STR data if available (non-fatal)
                    if (llm_enabled_ && llm_client_ && metrics.has_value()) {
                        const STRDailyRecord* str_rec = !last_str_records_.empty()
                            ? &last_str_records_.back() : nullptr;
                        generateAndPublishSummary(metrics.value(), str_rec);

                        // Auto-trigger weekly/monthly summaries based on config.
                        // WEEKLY_SUMMARY_DAY: 0=Sun..6=Sat (default 0=Sunday)
                        // MONTHLY_SUMMARY_DAY: day of month (default 1)
                        auto now = std::chrono::system_clock::now();
                        auto now_t = std::chrono::system_clock::to_time_t(now);
                        std::tm* tm = std::localtime(&now_t);
                        int weekly_day = ConfigManager::getInt("WEEKLY_SUMMARY_DAY", 0);
                        int monthly_day = ConfigManager::getInt("MONTHLY_SUMMARY_DAY", 1);
                        if (tm->tm_wday == weekly_day) {
                            generateRangeSummary(SummaryPeriod::WEEKLY);
                        }
                        if (tm->tm_mday == monthly_day) {
                            generateRangeSummary(SummaryPeriod::MONTHLY);
                        }
                    }
                } else if (!newly_completed && is_most_recent && data_publisher_) {
                    // Session was already completed (session_end set by prior cycle),
                    // but session_active may still be ON if publishSessionCompleted()
                    // never fired. Ensure it's cleared.
                    data_publisher_->publishSessionCompleted();
                }

                std::cout << "   Skipping download (no changes)" << std::endl;
                continue;
            }

            std::cout << "CPAP: Re-downloading session " << session.session_prefix
                      << " (checkpoint files changed)" << std::endl;

            if (downloadSessionFiles(session, local_base_dir)) {
                std::map<std::string, int> checkpoint_sizes;
                for (const auto& [filename, size_kb] : session.file_sizes_kb) {
                    if (filename.find("_BRP.edf") != std::string::npos ||
                        filename.find("_PLD.edf") != std::string::npos ||
                        isOximetryFile(filename)) {
                        checkpoint_sizes[filename] = size_kb;
                    }
                }
                db_service_->updateCheckpointFileSizes(device_id_, session.session_start, checkpoint_sizes);

                // Session resumed (mask put back on) — clear session_end so
                // markSessionCompleted() can fire again when it truly stops.
                db_service_->reopenSession(device_id_, session.session_start);

                std::string session_dir = local_base_dir + "/" + session.date_folder;
                downloaded_sessions.push_back({session_dir, session.session_start});
            } else {
                std::cerr << "CPAP: Failed to download session " << session.session_prefix << std::endl;
            }
        }

        if (downloaded_sessions.empty()) {
            std::cerr << "CPAP: No sessions downloaded successfully" << std::endl;
            return false;
        }

        auto download_end = std::chrono::steady_clock::now();
        auto download_ms = std::chrono::duration_cast<std::chrono::milliseconds>(download_end - download_start).count();
        std::cout << "CPAP: Downloaded " << downloaded_sessions.size()
                  << " session(s) in " << download_ms << " ms" << std::endl;

        // Archive downloaded files to permanent storage
        std::string permanent_archive = ConfigManager::get("CPAP_ARCHIVE_DIR", "/home/aamat/maestro_hub/cpap_data");
        std::set<std::string> date_folders;
        for (const auto& session : new_sessions) {
            date_folders.insert(session.date_folder);
        }
        for (const auto& date_folder : date_folders) {
            archiveSessionFiles(date_folder, local_base_dir, permanent_archive);
        }
    }

    // Step 6: Parse all sessions (same for both modes)
    auto parse_start = std::chrono::steady_clock::now();
    std::vector<CPAPSession> parsed_sessions;

    for (const auto& [session_dir, session_start] : downloaded_sessions) {
        std::cout << "📊 CPAP: Parsing session from " << session_dir << "..." << std::endl;

        // Pass filename timestamp to parser (DB lookup key)
        auto parsed = EDFParser::parseSession(session_dir, device_id_, device_name_, session_start);

        if (parsed) {
            // Set file path references (pointing to permanent archive)
            auto start_time_t = std::chrono::system_clock::to_time_t(session_start);
            std::tm* start_tm = std::localtime(&start_time_t);
            std::ostringstream date_oss;
            date_oss << std::put_time(start_tm, "%Y%m%d");
            std::string date_folder = date_oss.str();

            std::ostringstream prefix_oss;
            prefix_oss << std::put_time(start_tm, "%Y%m%d_%H%M%S");
            std::string session_prefix = prefix_oss.str();

            // Store RELATIVE paths in database (DATALOG/20250721/filename.edf)
            // This makes paths portable between different storage locations
            std::string relative_path_base = "DATALOG/" + date_folder + "/";

            // Set file paths (use first file of each type found in directory)
            if (std::filesystem::exists(session_dir)) {
                for (const auto& entry : std::filesystem::directory_iterator(session_dir)) {
                    std::string filename = entry.path().filename().string();

                    if (filename.find("_BRP.edf") != std::string::npos && !parsed->brp_file_path.has_value()) {
                        parsed->brp_file_path = relative_path_base + filename;
                    } else if (filename.find("_EVE.edf") != std::string::npos && !parsed->eve_file_path.has_value()) {
                        parsed->eve_file_path = relative_path_base + filename;
                    } else if (isOximetryFile(filename) && !parsed->sad_file_path.has_value()) {
                        parsed->sad_file_path = relative_path_base + filename;
                    } else if (filename.find("_PLD.edf") != std::string::npos && !parsed->pld_file_path.has_value()) {
                        parsed->pld_file_path = relative_path_base + filename;
                    } else if (filename.find("_CSL.edf") != std::string::npos && !parsed->csl_file_path.has_value()) {
                        parsed->csl_file_path = relative_path_base + filename;
                    }
                }
            }

            parsed_sessions.push_back(*parsed);  // Dereference unique_ptr and copy
            std::cout << "✅ CPAP: Parsed session successfully" << std::endl;
        } else {
            std::cerr << "⚠️  CPAP: Failed to parse session from " << session_dir << std::endl;
        }
    }

    if (parsed_sessions.empty()) {
        std::cerr << "❌ CPAP: No sessions parsed successfully" << std::endl;
        return false;
    }

    auto parse_end = std::chrono::steady_clock::now();
    auto parse_ms = std::chrono::duration_cast<std::chrono::milliseconds>(parse_end - parse_start).count();

    std::cout << "✅ CPAP: Parsed " << parsed_sessions.size() << " session(s) in " << parse_ms << " ms" << std::endl;

    // Step 7: Save ALL sessions to database
    int saved_count = 0;
    for (const auto& session : parsed_sessions) {
        if (db_service_->saveSession(session)) {
            saved_count++;
        } else {
            std::cerr << "⚠️  CPAP: Failed to save session to DB" << std::endl;
        }
    }

    std::cout << "💾 CPAP: Saved " << saved_count << "/" << parsed_sessions.size()
              << " session(s) to database" << std::endl;

    // Step 8: Publish LATEST session to MQTT (most recent by session_start)
    if (!parsed_sessions.empty() && data_publisher_) {
        // Find latest session
        auto latest = std::max_element(
            parsed_sessions.begin(),
            parsed_sessions.end(),
            [](const CPAPSession& a, const CPAPSession& b) {
                if (!a.session_start.has_value()) return true;
                if (!b.session_start.has_value()) return false;
                return a.session_start.value() < b.session_start.value();
            }
        );

        if (latest->session_start.has_value()) {
            std::cout << "📡 CPAP: Publishing latest session to MQTT..." << std::endl;

            // Display session summary
            std::cout << "\n" << std::string(60, '-') << std::endl;
            std::cout << "📋 Latest Session Summary:" << std::endl;
            std::cout << std::string(60, '-') << std::endl;
            std::cout << latest->toString() << std::endl;
            std::cout << std::string(60, '-') << std::endl;

            data_publisher_->publishSession(*latest);

            // Also publish nightly aggregated metrics
            auto metrics = db_service_->getNightlyMetrics(device_id_, latest->session_start.value());
            if (metrics.has_value()) {
                data_publisher_->publishHistoricalState(metrics.value());
                std::cout << "   Nightly metrics updated ("
                          << metrics.value().usage_hours.value_or(0.0) << "h, AHI "
                          << metrics.value().ahi << ")" << std::endl;
            }

            // Session is always IN_PROGRESS during parsing.
            // Completion (metrics, STR, summary) fires from the checkpoint
            // path when file sizes stop changing between cycles.
            std::cout << "  Session status: in_progress (files still growing)" << std::endl;
        }
    }

    // Step 9: Update device last_seen
    db_service_->updateDeviceLastSeen(device_id_);

    // Cleanup temp symlink dirs (local mode only)
    if (!local_source_dir_.empty()) {
        std::filesystem::remove_all("/tmp/cpap_local");
    }

    auto cycle_end = std::chrono::steady_clock::now();
    auto cycle_ms = std::chrono::duration_cast<std::chrono::milliseconds>(cycle_end - cycle_start).count();

    std::cout << "\n" << std::string(60, '=') << std::endl;
    std::cout << "CPAP: Burst cycle completed successfully" << std::endl;
    std::cout << "   Processed: " << downloaded_sessions.size() << " sessions" << std::endl;
    std::cout << "   Parsed: " << parsed_sessions.size() << " sessions" << std::endl;
    std::cout << "   Saved to DB: " << saved_count << " sessions" << std::endl;
    std::cout << "   Parse=" << parse_ms << "ms, Total=" << cycle_ms << "ms" << std::endl;
    if (cycle_ms > 60000) {
        std::cout << "   WARNING: Cycle took >" << (cycle_ms/1000) << "s! Consider increasing BURST_INTERVAL" << std::endl;
    }
    std::cout << std::string(60, '=') << std::endl << std::endl;

    return true;
}

void BurstCollectorService::runLoop() {
    std::cout << "🔁 BurstCollectorService worker thread started" << std::endl;

    while (running_) {
        // Execute burst cycle
        bool success = executeBurstCycle();

        if (success) {
            last_burst_time_ = std::chrono::system_clock::now();
        }

        // Wait for next cycle
        auto next_cycle = std::chrono::system_clock::now() + std::chrono::seconds(burst_interval_seconds_);
        std::time_t next_time = std::chrono::system_clock::to_time_t(next_cycle);

        std::cout << "⏰ CPAP: Next burst cycle at "
                  << std::put_time(std::localtime(&next_time), "%Y-%m-%d %H:%M:%S")
                  << std::endl;

        // Sleep in small intervals to allow clean shutdown
        auto sleep_start = std::chrono::system_clock::now();
        while (running_) {
            auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(
                std::chrono::system_clock::now() - sleep_start
            ).count();

            if (elapsed >= burst_interval_seconds_) {
                break;
            }

            std::this_thread::sleep_for(std::chrono::seconds(1));
        }
    }

    std::cout << "🔁 BurstCollectorService worker thread stopped" << std::endl;
}

// ─── LLM Summary ────────────────────────────────────────────────────────────

// --- Range summaries (weekly / monthly) ---
//
// Flow:
//   1. Query DB for per-night metrics over the last N days
//   2. Format each night into a concise line + compute period averages
//   3. Build a period-specific LLM prompt
//   4. Call LLM and publish result to cpap/{device}/weekly|monthly/summary

void BurstCollectorService::generateRangeSummary(SummaryPeriod period, int days_override) {
    int days_back = days_override > 0 ? days_override
                  : (period == SummaryPeriod::WEEKLY) ? 7 : 30;
    std::string period_str = (period == SummaryPeriod::WEEKLY) ? "weekly" : "monthly";

    std::cout << "LLM: Generating " << period_str << " summary ("
              << days_back << " days)..." << std::endl;

    auto nights = db_service_->getMetricsForDateRange(device_id_, days_back);
    if (nights.empty()) {
        std::cerr << "LLM: No data for " << period_str << " summary" << std::endl;
        return;
    }

    std::string metrics_str = buildRangeMetricsString(nights, period);

    // Period-specific prompt — the LLM sees all the per-night data and averages
    std::string prompt;
    if (period == SummaryPeriod::WEEKLY) {
        prompt = "You are a CPAP therapy analyst. Summarize this week of CPAP data "
                 "in 4-6 sentences. Highlight trends (improving/worsening AHI, "
                 "usage consistency), flag any concerning nights, and give one "
                 "actionable suggestion.\n\n" + metrics_str;
    } else {
        prompt = "You are a CPAP therapy analyst. Summarize this month of CPAP data "
                 "in 5-8 sentences. Identify overall trends in AHI, usage compliance, "
                 "and leak control. Compare the first half vs second half of the month. "
                 "Note any patterns (weekday vs weekend, etc.) and provide "
                 "recommendations.\n\n" + metrics_str;
    }

    auto summary = llm_client_->generate(prompt);
    if (!summary) {
        std::cerr << "LLM: " << period_str << " summary generation failed" << std::endl;
        return;
    }

    std::cout << "LLM: " << period_str << " summary generated ("
              << summary->size() << " chars)" << std::endl;

    if (data_publisher_) {
        data_publisher_->publishRangeSummary(period, summary.value());
    }
}

std::string BurstCollectorService::buildRangeMetricsString(
    const std::vector<SessionMetrics>& nights, SummaryPeriod period) const {

    std::ostringstream oss;
    oss << std::fixed << std::setprecision(2);

    std::string period_str = (period == SummaryPeriod::WEEKLY) ? "Weekly" : "Monthly";
    oss << period_str << " CPAP report (" << nights.size() << " nights)\n";
    oss << "─────────────────────────────────────\n\n";

    // Per-night summary line
    oss << "Night-by-night:\n";
    double total_ahi = 0, total_hours = 0, total_leak = 0;
    int leak_count = 0;

    for (const auto& n : nights) {
        double hours = n.usage_hours.value_or(0.0);
        oss << "  " << n.sleep_day
            << " | " << hours << "h"
            << " | AHI " << n.ahi
            << " | events " << n.total_events
            << " (OA=" << n.obstructive_apneas
            << " CA=" << n.central_apneas
            << " H=" << n.hypopneas
            << " R=" << n.reras << ")";
        if (n.avg_leak_rate.has_value())
            oss << " | leak avg " << n.avg_leak_rate.value() << " L/min";
        if (n.avg_pressure.has_value())
            oss << " | press " << n.avg_pressure.value() << " cmH2O";
        oss << "\n";

        total_ahi += n.ahi;
        total_hours += hours;
        if (n.avg_leak_rate.has_value()) {
            total_leak += n.avg_leak_rate.value();
            leak_count++;
        }
    }

    // Period averages
    int count = static_cast<int>(nights.size());
    oss << "\n" << period_str << " averages:\n";
    oss << "  Avg AHI: " << (total_ahi / count) << " events/hour\n";
    oss << "  Avg usage: " << (total_hours / count) << " hours/night\n";
    oss << "  Total usage: " << total_hours << " hours\n";
    if (leak_count > 0)
        oss << "  Avg leak: " << (total_leak / leak_count) << " L/min\n";

    // Compliance: nights >= 4h
    int compliant = 0;
    for (const auto& n : nights)
        if (n.usage_hours.value_or(0.0) >= 4.0) compliant++;
    oss << "  Compliance (>=4h): " << compliant << "/" << count
        << " nights (" << (100.0 * compliant / count) << "%)\n";

    // Best and worst nights
    auto best = std::min_element(nights.begin(), nights.end(),
        [](const SessionMetrics& a, const SessionMetrics& b) { return a.ahi < b.ahi; });
    auto worst = std::max_element(nights.begin(), nights.end(),
        [](const SessionMetrics& a, const SessionMetrics& b) { return a.ahi < b.ahi; });
    oss << "  Best AHI: " << best->ahi << " (" << best->sleep_day << ")\n";
    oss << "  Worst AHI: " << worst->ahi << " (" << worst->sleep_day << ")\n";

    return oss.str();
}

// --- Daily summary (single night) ---

void BurstCollectorService::generateAndPublishSummary(const SessionMetrics& metrics,
                                                       const STRDailyRecord* str_record) {
    std::cout << "LLM: Generating session summary..." << std::endl;

    std::string metrics_str = buildMetricsString(metrics, str_record);
    std::string prompt = hms::LLMClient::substituteTemplate(
        llm_prompt_template_, {{"metrics", metrics_str}});

    auto summary = llm_client_->generate(prompt);
    if (!summary) {
        std::cerr << "LLM: Summary generation failed (non-fatal)" << std::endl;
        return;
    }

    std::cout << "LLM: Summary generated (" << summary->size() << " chars)" << std::endl;

    if (data_publisher_) {
        data_publisher_->publishSessionSummary(summary.value());
    }
}

std::string BurstCollectorService::buildMetricsString(const SessionMetrics& metrics,
                                                       const STRDailyRecord* str_record) const {
    std::ostringstream oss;
    oss << std::fixed << std::setprecision(2);

    // Usage
    oss << "Usage: " << metrics.usage_hours.value_or(0.0) << " hours"
        << " (" << metrics.usage_percent.value_or(0.0) << "% of 8h target)\n";

    // AHI and events
    oss << "AHI: " << metrics.ahi << " events/hour\n";
    oss << "Total events: " << metrics.total_events
        << " (obstructive=" << metrics.obstructive_apneas
        << ", central=" << metrics.central_apneas
        << ", hypopnea=" << metrics.hypopneas
        << ", RERA=" << metrics.reras << ")\n";

    if (metrics.avg_event_duration.has_value()) {
        oss << "Avg event duration: " << metrics.avg_event_duration.value() << "s";
        if (metrics.max_event_duration.has_value()) {
            oss << ", max: " << metrics.max_event_duration.value() << "s";
        }
        oss << "\n";
    }

    // Pressure (only include fields that have data)
    if (metrics.avg_pressure.has_value() || metrics.pressure_p95.has_value()) {
        oss << "Pressure:";
        if (metrics.avg_pressure.has_value()) {
            oss << " avg=" << metrics.avg_pressure.value() << " cmH2O";
        }
        if (metrics.min_pressure.has_value()) {
            oss << ", min=" << metrics.min_pressure.value() << " cmH2O";
        }
        if (metrics.max_pressure.has_value()) {
            oss << ", max=" << metrics.max_pressure.value() << " cmH2O";
        }
        if (metrics.pressure_p95.has_value()) {
            oss << ", 95th=" << metrics.pressure_p95.value() << " cmH2O";
        }
        oss << "\n";
    }

    // Leak (only include fields that have data)
    if (metrics.avg_leak_rate.has_value() || metrics.max_leak_rate.has_value()) {
        oss << "Leak:";
        if (metrics.avg_leak_rate.has_value()) {
            oss << " avg=" << metrics.avg_leak_rate.value() << " L/min";
        }
        if (metrics.max_leak_rate.has_value()) {
            oss << ", max=" << metrics.max_leak_rate.value() << " L/min";
        }
        if (metrics.leak_p95.has_value()) {
            oss << ", 95th=" << metrics.leak_p95.value() << " L/min";
        }
        if (metrics.leak_p50.has_value()) {
            oss << ", median=" << metrics.leak_p50.value() << " L/min";
        }
        oss << "\n";
    }

    // PLD-derived metrics (machine's own calculations)
    if (metrics.avg_mask_pressure.has_value()) {
        oss << "Mask pressure (actual): " << metrics.avg_mask_pressure.value() << " cmH2O\n";
    }
    if (metrics.avg_epr_pressure.has_value()) {
        oss << "EPR/EPAP pressure: " << metrics.avg_epr_pressure.value() << " cmH2O\n";
    }
    if (metrics.avg_snore.has_value()) {
        oss << "Snore index: " << metrics.avg_snore.value() << " (0-5 scale)\n";
    }

    // ASV-specific metrics
    if (metrics.avg_target_ventilation.has_value() && metrics.avg_target_ventilation.value() > 0) {
        oss << "Target ventilation (ASV): " << metrics.avg_target_ventilation.value() << " L/min\n";
    }
    if (metrics.therapy_mode.has_value()) {
        int mode = metrics.therapy_mode.value();
        std::string mode_name = "Unknown";
        if (mode == 0) mode_name = "CPAP";
        else if (mode == 1) mode_name = "APAP";
        else if (mode == 7) mode_name = "ASV (Fixed EPAP)";
        else if (mode == 8) mode_name = "ASV (Variable EPAP)";
        oss << "Therapy mode: " << mode_name << "\n";
    }

    // Respiratory
    if (metrics.avg_respiratory_rate.has_value()) {
        oss << "Respiratory rate: " << metrics.avg_respiratory_rate.value() << " breaths/min\n";
    }
    if (metrics.avg_tidal_volume.has_value()) {
        oss << "Tidal volume: " << metrics.avg_tidal_volume.value() << " mL\n";
    }
    if (metrics.avg_minute_ventilation.has_value()) {
        oss << "Minute ventilation: " << metrics.avg_minute_ventilation.value() << " L/min\n";
    }
    if (metrics.avg_flow_limitation.has_value()) {
        oss << "Flow limitation: " << metrics.avg_flow_limitation.value() << " (0-1 scale)\n";
    }

    // STR data (if available)
    if (str_record) {
        oss << "\nResMed official daily summary:\n";
        oss << "  STR AHI: " << str_record->ahi << " events/hour\n";
        oss << "  Mask events: " << (str_record->mask_events / 2) << " (on/off pairs)\n";
        oss << "  95th leak: " << str_record->leak_95 << " L/min\n";
        oss << "  95th pressure: " << str_record->mask_press_95 << " cmH2O\n";
        // ASV STR settings
        if (str_record->asv_epap.has_value()) {
            oss << "  ASV EPAP: " << str_record->asv_epap.value() << " cmH2O\n";
            oss << "  ASV Pressure Support: " << str_record->asv_min_ps.value_or(0) << "-"
                << str_record->asv_max_ps.value_or(0) << " cmH2O\n";
        }
        if (str_record->tgt_ipap_50.has_value()) {
            oss << "  Target IPAP (median): " << str_record->tgt_ipap_50.value() << " cmH2O\n";
            oss << "  Target EPAP (median): " << str_record->tgt_epap_50.value_or(0) << " cmH2O\n";
            oss << "  Target ventilation (median): " << str_record->tgt_vent_50.value_or(0) << " L/min\n";
        }
    }

    return oss.str();
}

std::string BurstCollectorService::loadPromptFile(const std::string& filepath) {
    return hms::LLMClient::loadPromptFile(filepath);
}

} // namespace hms_cpap
