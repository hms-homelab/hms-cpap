#include "services/BurstCollectorService.h"
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
║      Version: 1.2.0+20260306 - STR Daily Summaries       ║
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
 * Main entry point
 */
int main(int argc, char** argv) {
    // Handle --backfill mode
    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "--backfill") == 0) {
            if (i + 1 >= argc) {
                std::cerr << "Usage: hms_cpap --backfill <path/to/str.edf>" << std::endl;
                return 1;
            }
            return runBackfill(argv[i + 1]);
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
