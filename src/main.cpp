#include "services/BurstCollectorService.h"
#include "utils/ConfigManager.h"
#include <iostream>
#include <iomanip>
#include <csignal>
#include <atomic>
#include <memory>

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
║      Version: 1.1.3+20260219 - KB Fix + Recovery         ║
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
 * Main entry point
 */
int main(int argc, char** argv) {
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
