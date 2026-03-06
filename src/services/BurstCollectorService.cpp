#include "services/BurstCollectorService.h"
#include "utils/ConfigManager.h"
#include <iostream>
#include <iomanip>
#include <sstream>
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
    // ez Share accessed via dedicated wlan1 interface (192.168.4.1)

    // Initialize clients
    ezshare_client_ = std::make_unique<EzShareClient>();

    // Initialize discovery service
    discovery_service_ = std::make_unique<SessionDiscoveryService>(*ezshare_client_);

    // Initialize MQTT client
    std::string mqtt_broker = ConfigManager::get("MQTT_BROKER", "192.168.2.15");
    std::string mqtt_port = ConfigManager::get("MQTT_PORT", "1883");
    std::string mqtt_user = ConfigManager::get("MQTT_USER", "aamat");
    std::string mqtt_password = ConfigManager::get("MQTT_PASSWORD", "exploracion");
    std::string mqtt_client_id = ConfigManager::get("MQTT_CLIENT_ID", "hms_cpap_service");

    mqtt_client_ = std::make_shared<MqttClient>(mqtt_client_id);
    std::string broker_address = "tcp://" + mqtt_broker + ":" + mqtt_port;

    if (mqtt_client_->connect(broker_address, mqtt_user, mqtt_password)) {
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
    std::cout << "   ez Share: " << ConfigManager::get("EZSHARE_BASE_URL", "http://192.168.4.1") << std::endl;

    // STARTUP CLEANUP: Clear any stale session_active state from previous run
    // This prevents stuck "session active" in HA after service restart or crash
    if (data_publisher_ && mqtt_client_ && mqtt_client_->isConnected()) {
        std::cout << "🧹 Startup: Clearing stale session_active state..." << std::endl;
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

    // Step 2: Access ez Share via configured URL
    std::cout << "✅ CPAP: Accessing ez Share at " << ConfigManager::get("EZSHARE_BASE_URL", "http://192.168.4.1") << std::endl;

    // Step 3: Discover new sessions (delta logic)
    std::vector<SessionFileSet> new_sessions;

    try {
        new_sessions = discovery_service_->discoverNewSessions(last_session_start);
        // Reset failure counter on successful connection
        consecutive_failures_ = 0;
        session_active_cleared_ = false;  // Allow future cleanup if needed
    } catch (const std::exception& e) {
        std::cerr << "❌ CPAP: Session discovery failed: " << e.what() << std::endl;

        // CONNECTION RECOVERY: Track consecutive failures
        consecutive_failures_++;
        std::cerr << "   ⚠️  Consecutive failures: " << consecutive_failures_
                  << "/" << MAX_FAILURES_BEFORE_RESET << std::endl;

        // After MAX_FAILURES_BEFORE_RESET consecutive failures, clear session_active
        // This prevents stuck "session running" in HA when connection is lost
        if (consecutive_failures_ >= MAX_FAILURES_BEFORE_RESET && !session_active_cleared_) {
            std::cout << "🔄 RECOVERY: Clearing session_active after "
                      << consecutive_failures_ << " consecutive failures" << std::endl;

            if (data_publisher_) {
                data_publisher_->publishSessionCompleted();
                session_active_cleared_ = true;
                std::cout << "   ✅ Published session_active=OFF to MQTT" << std::endl;
            }
        }

        return false;
    }

    if (new_sessions.empty()) {
        std::cout << "ℹ️  CPAP: No new sessions to download" << std::endl;
        return true;  // Success - just nothing new
    }

    std::cout << "📦 CPAP: Found " << new_sessions.size() << " new session(s)" << std::endl;

    // Step 4: Download new session files
    auto download_start = std::chrono::steady_clock::now();
    std::string local_base_dir = ConfigManager::get("CPAP_TEMP_DIR", "/tmp/cpap_data");
    std::vector<std::pair<std::string, std::chrono::system_clock::time_point>> downloaded_sessions;

    for (const auto& session : new_sessions) {
        // Check if session exists in DB
        bool exists_in_db = db_service_->sessionExists(device_id_, session.session_start);

        if (!exists_in_db) {
            // New session - download all files
            std::cout << "📦 CPAP: New session " << session.session_prefix
                      << " (not in DB, " << session.total_size_kb << " KB)" << std::endl;

            if (downloadSessionFiles(session, local_base_dir)) {
                // Store checkpoint file sizes in KB (exact values from ez Share HTML)
                // This ensures comparison works: ez Share KB == DB KB
                std::map<std::string, int> checkpoint_sizes;
                for (const auto& [filename, size_kb] : session.file_sizes_kb) {
                    if (filename.find("_BRP.edf") != std::string::npos ||
                        filename.find("_PLD.edf") != std::string::npos ||
                        filename.find("_SAD.edf") != std::string::npos) {
                        checkpoint_sizes[filename] = size_kb;
                    }
                }

                // Store checkpoint file sizes (in KB, matching ez Share) in DB
                db_service_->updateCheckpointFileSizes(device_id_, session.session_start, checkpoint_sizes);

                std::string session_dir = local_base_dir + "/" + session.date_folder;

                // Store session dir + filename timestamp for parsing
                downloaded_sessions.push_back({session_dir, session.session_start});
            } else {
                std::cerr << "⚠️  CPAP: Failed to download session " << session.session_prefix << std::endl;
            }
            continue;
        }

        // Session exists - check if checkpoint files changed
        auto db_checkpoint_sizes = db_service_->getCheckpointFileSizes(device_id_, session.session_start);

        // Extract current checkpoint sizes from ez Share
        std::map<std::string, int> current_checkpoint_sizes;
        for (const auto& [filename, size_kb] : session.file_sizes_kb) {
            if (filename.find("_BRP.edf") != std::string::npos ||
                filename.find("_PLD.edf") != std::string::npos ||
                filename.find("_SAD.edf") != std::string::npos) {
                current_checkpoint_sizes[filename] = size_kb;
            }
        }

        // Detect session stop: ALL checkpoint files unchanged AND no new checkpoint files
        bool all_unchanged = true;
        bool has_new_files = false;

        // Check if any existing files grew
        for (const auto& [filename, db_size] : db_checkpoint_sizes) {
            auto it = current_checkpoint_sizes.find(filename);
            if (it == current_checkpoint_sizes.end()) {
                // File disappeared (unlikely, but handle it)
                all_unchanged = false;
                break;
            }
            if (it->second != db_size) {
                all_unchanged = false;
                break;
            }
        }

        // Check if there are new checkpoint files
        if (current_checkpoint_sizes.size() > db_checkpoint_sizes.size()) {
            has_new_files = true;
            all_unchanged = false;  // New files mean activity
        }

        if (all_unchanged && !has_new_files) {
            // Session stopped - mark as COMPLETED in DB
            std::cout << "🛑 CPAP: Session " << session.session_prefix
                      << " stopped (all checkpoint files unchanged, no new files)" << std::endl;

            if (db_service_->markSessionCompleted(device_id_, session.session_start)) {
                std::cout << "   ✅ Marked as COMPLETED in database" << std::endl;
            }

            // Only publish nightly metrics for sessions in the CURRENT sleep night.
            // Old sessions from previous nights are re-scanned every cycle but their
            // metrics must not overwrite the current night's historical MQTT topics.
            {
                auto now = std::chrono::system_clock::now();
                auto now_t = std::chrono::system_clock::to_time_t(now);
                auto sess_t = std::chrono::system_clock::to_time_t(session.session_start);
                // Sleep day = calendar date of (timestamp - 12h)
                auto now_day = (now_t - 12*3600) / 86400;
                auto sess_day = (sess_t - 12*3600) / 86400;

                if (sess_day == now_day && data_publisher_) {
                    auto metrics = db_service_->getNightlyMetrics(device_id_, session.session_start);
                    if (metrics.has_value()) {
                        data_publisher_->publishHistoricalState(metrics.value());
                        std::cout << "   ✅ Nightly metrics published ("
                                  << metrics.value().usage_hours.value_or(0.0) << "h, AHI "
                                  << metrics.value().ahi << ")" << std::endl;
                    }
                    data_publisher_->publishSessionCompleted();

                    // Download and publish STR daily summary on session completion
                    processSTRFile();
                }
            }

            // Don't download/parse - nothing changed
            std::cout << "   ⏭️  Skipping download (no changes)" << std::endl;
            continue;
        }

        // Checkpoint files changed - re-download and parse
        std::cout << "📈 CPAP: Re-downloading session " << session.session_prefix
                  << " (checkpoint files changed: " << db_checkpoint_sizes.size()
                  << " → " << current_checkpoint_sizes.size() << " files)" << std::endl;

        if (downloadSessionFiles(session, local_base_dir)) {
            // Store checkpoint file sizes in KB (exact values from ez Share HTML)
            std::map<std::string, int> checkpoint_sizes;
            for (const auto& [filename, size_kb] : session.file_sizes_kb) {
                if (filename.find("_BRP.edf") != std::string::npos ||
                    filename.find("_PLD.edf") != std::string::npos ||
                    filename.find("_SAD.edf") != std::string::npos) {
                    checkpoint_sizes[filename] = size_kb;
                }
            }

            // Update checkpoint file sizes (in KB, matching ez Share) in DB
            db_service_->updateCheckpointFileSizes(device_id_, session.session_start, checkpoint_sizes);

            // Store session dir + filename timestamp for parsing
            std::string session_dir = local_base_dir + "/" + session.date_folder;
            downloaded_sessions.push_back({session_dir, session.session_start});
        } else {
            std::cerr << "⚠️  CPAP: Failed to download session " << session.session_prefix << std::endl;
        }
    }

    // Step 5: No WiFi disconnect needed (using bridged ez Share)
    std::cout << "✅ CPAP: Session download complete (bridged connection)" << std::endl;

    if (downloaded_sessions.empty()) {
        std::cerr << "❌ CPAP: No sessions downloaded successfully" << std::endl;
        return false;
    }

    auto download_end = std::chrono::steady_clock::now();
    auto download_ms = std::chrono::duration_cast<std::chrono::milliseconds>(download_end - download_start).count();

    std::cout << "✅ CPAP: Downloaded " << downloaded_sessions.size()
              << " session(s) in " << download_ms << " ms" << std::endl;

    // Step 5a: Archive downloaded files to permanent storage
    std::cout << "📦 CPAP: Archiving files to permanent storage..." << std::endl;
    std::string permanent_archive = ConfigManager::get("CPAP_ARCHIVE_DIR", "/home/aamat/maestro_hub/cpap_data");

    // Get unique date folders from downloaded sessions
    std::set<std::string> date_folders;
    for (const auto& session : new_sessions) {
        date_folders.insert(session.date_folder);
    }

    for (const auto& date_folder : date_folders) {
        archiveSessionFiles(date_folder, local_base_dir, permanent_archive);
    }

    std::cout << "✅ CPAP: Files archived to " << permanent_archive << "/DATALOG/" << std::endl;

    // Step 6: Parse all downloaded sessions
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
                    } else if (filename.find("_SAD.edf") != std::string::npos && !parsed->sad_file_path.has_value()) {
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

            // Also publish nightly aggregated metrics while session is active
            // so historical/ MQTT topics stay current (not just on session stop)
            auto metrics = db_service_->getNightlyMetrics(device_id_, latest->session_start.value());
            if (metrics.has_value()) {
                data_publisher_->publishHistoricalState(metrics.value());
                std::cout << "   Nightly metrics updated ("
                          << metrics.value().usage_hours.value_or(0.0) << "h, AHI "
                          << metrics.value().ahi << ")" << std::endl;
            }
        }
    }

    // Step 9: Update device last_seen
    db_service_->updateDeviceLastSeen(device_id_);

    auto cycle_end = std::chrono::steady_clock::now();
    auto cycle_ms = std::chrono::duration_cast<std::chrono::milliseconds>(cycle_end - cycle_start).count();

    std::cout << "\n" << std::string(60, '=') << std::endl;
    std::cout << "✅ CPAP: Burst cycle completed successfully" << std::endl;
    std::cout << "   Downloaded: " << downloaded_sessions.size() << " sessions" << std::endl;
    std::cout << "   Parsed: " << parsed_sessions.size() << " sessions" << std::endl;
    std::cout << "   Saved to DB: " << saved_count << " sessions" << std::endl;
    std::cout << "   ⏱️  Timing: Download=" << download_ms << "ms, Parse=" << parse_ms << "ms, Total=" << cycle_ms << "ms" << std::endl;
    if (cycle_ms > 60000) {
        std::cout << "   ⚠️  WARNING: Cycle took >" << (cycle_ms/1000) << "s! Consider increasing BURST_INTERVAL" << std::endl;
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

} // namespace hms_cpap
