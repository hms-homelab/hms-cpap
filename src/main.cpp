#include "services/BurstCollectorService.h"
#include "services/SessionDiscoveryService.h"
#include "parsers/EDFParser.h"
#include "database/DatabaseService.h"
#include "utils/ConfigManager.h"
#include <iostream>
#include <iomanip>
#include <csignal>
#include <atomic>
#include <memory>
#include <string>
#include <cstring>
#include <filesystem>
#include <sstream>

std::atomic<bool> shutdown_requested(false);
std::unique_ptr<hms_cpap::BurstCollectorService> burst_service;

/**
 * Signal handler for graceful shutdown
 */
void signalHandler(int signal) {
    std::cout << "\n⚠️  Received signal " << signal << ", shutting down gracefully..." << std::endl;
    shutdown_requested = true;

    if (burst_service) {
        burst_service->stop();
    }
}

/**
 * Print banner
 */
void printBanner() {
    std::cout << R"(
╔═══════════════════════════════════════════════════════════╗
║                                                           ║
║      HMS-CPAP - CPAP Data Collection Service             ║
║                                                           ║
║      ResMed AirSense 10 via ez Share                     ║
║      Configurable via EZSHARE_BASE_URL                   ║
║                                                           ║
║      Version: 1.3.0+20260306 - Reparse CLI               ║
║      Platform: Linux                                      ║
║                                                           ║
╚═══════════════════════════════════════════════════════════╝
)" << std::endl;
}

/**
 * Print configuration
 */
void printConfiguration() {
    std::cout << "Configuration:" << std::endl;
    std::cout << "  Device ID:          " << hms_cpap::ConfigManager::get("CPAP_DEVICE_ID", "cpap_resmed_23243570851") << std::endl;
    std::cout << "  Device Name:        " << hms_cpap::ConfigManager::get("CPAP_DEVICE_NAME", "ResMed AirSense 10") << std::endl;
    std::cout << "  ez Share URL:       " << hms_cpap::ConfigManager::get("EZSHARE_BASE_URL", "http://192.168.4.1") << std::endl;
    std::cout << "  Burst Interval:     " << hms_cpap::ConfigManager::getInt("BURST_INTERVAL", 120) << " seconds" << std::endl;
    std::cout << "  Health Check Port:  " << hms_cpap::ConfigManager::getInt("HEALTH_CHECK_PORT", 8893) << std::endl;
    std::cout << std::endl;
}

/**
 * Run STR.edf backfill: parse file and save ALL records to DB.
 *
 * Usage: hms_cpap --backfill /path/to/str.edf
 */
int runBackfill(const std::string& filepath) {
    std::string device_id = hms_cpap::ConfigManager::get("CPAP_DEVICE_ID", "cpap_resmed_23243570851");

    std::cout << "STR Backfill: Parsing " << filepath << std::endl;
    auto records = hms_cpap::EDFParser::parseSTRFile(filepath, device_id);
    if (records.empty()) {
        std::cerr << "STR Backfill: No therapy days found in " << filepath << std::endl;
        return 1;
    }
    std::cout << "STR Backfill: Found " << records.size() << " therapy days" << std::endl;

    // Connect to DB
    std::string db_host = hms_cpap::ConfigManager::get("DB_HOST", "localhost");
    std::string db_port = hms_cpap::ConfigManager::get("DB_PORT", "5432");
    std::string db_name = hms_cpap::ConfigManager::get("DB_NAME", "cpap_monitoring");
    std::string db_user = hms_cpap::ConfigManager::get("DB_USER", "maestro");
    std::string db_password = hms_cpap::ConfigManager::get("DB_PASSWORD", "maestro_postgres_2026_secure");

    std::string conn_str = "host=" + db_host + " port=" + db_port +
                           " dbname=" + db_name + " user=" + db_user +
                           " password=" + db_password;

    hms_cpap::DatabaseService db(conn_str);
    if (!db.connect()) {
        std::cerr << "STR Backfill: DB connection failed" << std::endl;
        return 1;
    }

    if (!db.saveSTRDailyRecords(records)) {
        std::cerr << "STR Backfill: Failed to save records" << std::endl;
        return 1;
    }

    // Print summary
    const auto& first = records.front();
    const auto& last = records.back();
    auto first_t = std::chrono::system_clock::to_time_t(first.record_date);
    auto last_t = std::chrono::system_clock::to_time_t(last.record_date);
    std::cout << "STR Backfill: Saved " << records.size() << " days ("
              << std::put_time(std::localtime(&first_t), "%Y-%m-%d") << " to "
              << std::put_time(std::localtime(&last_t), "%Y-%m-%d") << ")"
              << std::endl;

    return 0;
}

/**
 * Reparse sessions from local archive for a date range.
 *
 * Usage: hms_cpap --reparse /mnt/public/cpap_data/DATALOG 2025-08-18 [2025-09-09]
 *
 * Scans date folders in the given range, groups files into sessions using
 * the same 2-hour gap logic, deletes old DB records, and re-parses fresh.
 */
int runReparse(const std::string& archive_dir, const std::string& start_str, const std::string& end_str) {
    std::string device_id = hms_cpap::ConfigManager::get("CPAP_DEVICE_ID", "cpap_resmed_23243570851");
    std::string device_name = hms_cpap::ConfigManager::get("CPAP_DEVICE_NAME", "ResMed AirSense 10");

    // Parse dates
    auto parseDate = [](const std::string& s) -> std::tm {
        std::tm tm = {};
        tm.tm_year = std::stoi(s.substr(0, 4)) - 1900;
        tm.tm_mon  = std::stoi(s.substr(5, 2)) - 1;
        tm.tm_mday = std::stoi(s.substr(8, 2));
        tm.tm_isdst = -1;
        std::mktime(&tm);
        return tm;
    };

    std::tm start_tm, end_tm;
    try {
        start_tm = parseDate(start_str);
        end_tm = parseDate(end_str);
    } catch (...) {
        std::cerr << "Error: dates must be YYYY-MM-DD format" << std::endl;
        return 1;
    }

    // Generate YYYYMMDD folder names for the date range
    std::vector<std::string> date_folders;
    std::tm current = start_tm;
    time_t end_t = std::mktime(&end_tm);

    while (std::mktime(&current) <= end_t) {
        char buf[9];
        std::strftime(buf, sizeof(buf), "%Y%m%d", &current);
        date_folders.push_back(buf);
        current.tm_mday++;
        std::mktime(&current);  // normalize
    }

    std::cout << "Reparse: Scanning " << date_folders.size() << " date folder(s) in "
              << archive_dir << std::endl;

    // Connect to DB
    std::string db_host = hms_cpap::ConfigManager::get("DB_HOST", "localhost");
    std::string db_port = hms_cpap::ConfigManager::get("DB_PORT", "5432");
    std::string db_name = hms_cpap::ConfigManager::get("DB_NAME", "cpap_monitoring");
    std::string db_user = hms_cpap::ConfigManager::get("DB_USER", "maestro");
    std::string db_password = hms_cpap::ConfigManager::get("DB_PASSWORD", "maestro_postgres_2026_secure");

    std::string conn_str = "host=" + db_host + " port=" + db_port +
                           " dbname=" + db_name + " user=" + db_user +
                           " password=" + db_password;

    hms_cpap::DatabaseService db(conn_str);
    if (!db.connect()) {
        std::cerr << "Reparse: DB connection failed" << std::endl;
        return 1;
    }

    int total_deleted = 0;
    int total_parsed = 0;
    int total_saved = 0;

    for (const auto& folder : date_folders) {
        std::string folder_path = archive_dir + "/" + folder;

        if (!std::filesystem::exists(folder_path)) {
            continue;  // No data for this date
        }

        std::cout << "\n--- Folder: " << folder << " ---" << std::endl;

        // Group files into sessions
        auto sessions = hms_cpap::SessionDiscoveryService::groupLocalFolder(folder_path, folder);

        if (sessions.empty()) {
            std::cout << "  No sessions found" << std::endl;
            continue;
        }

        // Delete existing sessions for this date folder
        int deleted = db.deleteSessionsByDateFolder(device_id, folder);
        if (deleted > 0) {
            std::cout << "  Deleted " << deleted << " existing session(s) from DB" << std::endl;
            total_deleted += deleted;
        }

        // Parse each session group
        for (const auto& session : sessions) {
            // Create temp directory with only this session's files (symlinks)
            std::string temp_dir = "/tmp/cpap_reparse/" + folder + "_" + session.session_prefix;
            std::filesystem::create_directories(temp_dir);

            // Clear any previous symlinks
            for (const auto& entry : std::filesystem::directory_iterator(temp_dir)) {
                std::filesystem::remove(entry.path());
            }

            // Symlink all files belonging to this session
            auto symlinkFile = [&](const std::string& filename) {
                std::filesystem::path src = std::filesystem::path(folder_path) / filename;
                std::filesystem::path dst = std::filesystem::path(temp_dir) / filename;
                if (std::filesystem::exists(src)) {
                    std::filesystem::create_symlink(src, dst);
                }
            };

            for (const auto& f : session.brp_files) symlinkFile(f);
            for (const auto& f : session.pld_files) symlinkFile(f);
            for (const auto& f : session.sad_files) symlinkFile(f);
            if (!session.csl_file.empty()) symlinkFile(session.csl_file);
            if (!session.eve_file.empty()) symlinkFile(session.eve_file);

            // Parse
            total_parsed++;
            auto parsed = hms_cpap::EDFParser::parseSession(
                temp_dir, device_id, device_name, session.session_start);

            if (!parsed) {
                std::cerr << "  Failed to parse session " << session.session_prefix << std::endl;
                continue;
            }

            // Set relative file paths (same format as normal pipeline)
            std::string relative_base = "DATALOG/" + folder + "/";
            if (!session.brp_files.empty()) parsed->brp_file_path = relative_base + session.brp_files[0];
            if (!session.eve_file.empty()) parsed->eve_file_path = relative_base + session.eve_file;
            if (!session.sad_files.empty()) parsed->sad_file_path = relative_base + session.sad_files[0];
            if (!session.pld_files.empty()) parsed->pld_file_path = relative_base + session.pld_files[0];
            if (!session.csl_file.empty()) parsed->csl_file_path = relative_base + session.csl_file;

            // Save to DB
            if (db.saveSession(*parsed)) {
                total_saved++;
                double hours = parsed->duration_seconds.value_or(0) / 3600.0;
                double ahi = parsed->metrics.has_value() ? parsed->metrics->ahi : 0.0;
                std::cout << "  Saved: " << session.session_prefix
                          << " (" << std::fixed << std::setprecision(1) << hours << "h"
                          << ", AHI=" << std::setprecision(2) << ahi << ")" << std::endl;
            } else {
                std::cerr << "  Failed to save session " << session.session_prefix << std::endl;
            }

            // Cleanup temp dir
            std::filesystem::remove_all(temp_dir);
        }
    }

    // Cleanup
    std::filesystem::remove_all("/tmp/cpap_reparse");

    std::cout << "\nReparse complete:" << std::endl;
    std::cout << "  Deleted: " << total_deleted << " old session(s)" << std::endl;
    std::cout << "  Parsed:  " << total_parsed << " session(s)" << std::endl;
    std::cout << "  Saved:   " << total_saved << " session(s)" << std::endl;

    return (total_saved > 0) ? 0 : 1;
}

/**
 * Main entry point
 */
int main(int argc, char** argv) {
    // Handle CLI modes
    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "--backfill") == 0) {
            if (i + 1 >= argc) {
                std::cerr << "Usage: hms_cpap --backfill <path/to/str.edf>" << std::endl;
                return 1;
            }
            return runBackfill(argv[i + 1]);
        }
        if (std::strcmp(argv[i], "--reparse") == 0) {
            if (i + 1 >= argc) {
                std::cerr << "Usage: hms_cpap --reparse <archive_dir> <start_date> [end_date]" << std::endl;
                std::cerr << "  archive_dir: e.g., /mnt/public/cpap_data/DATALOG" << std::endl;
                std::cerr << "  dates: YYYY-MM-DD format" << std::endl;
                return 1;
            }
            std::string archive_dir = argv[i + 1];
            if (i + 2 >= argc) {
                std::cerr << "Usage: hms_cpap --reparse <archive_dir> <start_date> [end_date]" << std::endl;
                return 1;
            }
            std::string start_date = argv[i + 2];
            std::string end_date = (i + 3 < argc) ? argv[i + 3] : start_date;
            return runReparse(archive_dir, start_date, end_date);
        }
    }

    // Print banner
    printBanner();

    // Print configuration
    printConfiguration();

    // Register signal handlers
    std::signal(SIGINT, signalHandler);
    std::signal(SIGTERM, signalHandler);

    try {
        // Burst interval: default 2 minutes (120 seconds)
        int burst_interval = hms_cpap::ConfigManager::getInt("BURST_INTERVAL", 120);

        burst_service = std::make_unique<hms_cpap::BurstCollectorService>(burst_interval);
        burst_service->start();

        std::cout << "🚀 HMS-CPAP service is running..." << std::endl;
        std::cout << "   Accessing ez Share at " << hms_cpap::ConfigManager::get("EZSHARE_BASE_URL", "http://192.168.4.1") << std::endl;
        std::cout << "   Burst interval: " << burst_interval << " seconds" << std::endl;
        std::cout << "   Press Ctrl+C to stop" << std::endl << std::endl;

        // Main loop - wait for shutdown signal
        while (!shutdown_requested) {
            std::this_thread::sleep_for(std::chrono::seconds(1));
        }

        // Cleanup
        burst_service->stop();
        burst_service.reset();

        std::cout << "✅ HMS-CPAP service stopped cleanly" << std::endl;
        return 0;

    } catch (const std::exception& e) {
        std::cerr << "❌ Fatal error: " << e.what() << std::endl;
        return 1;
    }
}
