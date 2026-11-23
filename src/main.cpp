#ifdef BUILD_WITH_WEB
#include <drogon/drogon.h>
#include <fstream>
#include "controllers/CpapController.h"
#include "web/ClockApi.h"
#include "controllers/EquipmentController.h"
#include "controllers/CleaningController.h"
#include "controllers/SleepBridgeSocket.h"
#include "controllers/MuleSocket.h"
#include "controllers/OscarHelperSocket.h"
#include "services/OscarHelperMain.h"
#include "services/OscarHelperDiscovery.h"
#include "services/OscarHelperHost.h"
#include "controllers/MuleDs2Routes.h"
#include "controllers/DevicesRoutes.h"
#include "controllers/SetupWatch.h"
#include "controllers/LocalModelRoutes.h"
#include "model/LocalModelService.h"
#include "sleepbridge/ReleaseFeed.h"
#include "mule/LivePublisher.h"
#include "mqtt/LiveState.h"
#include "sleepbridge/LiveNight.h"
#include "services/CpapDashSyncService.h"
#include "services/CardFileFeed.h"
#include "services/NightLanded.h"
#include "services/CpapDashLink.h"
#include "services/CpapDashStream.h"
#include "services/CardUpload.h"
#include "services/GarminService.h"
#include "services/CpapCard.h"
#include "sleepbridge/LearnedCard.h"
#include "sleepbridge/CardReadCli.h"
#include "services/OximetryImport.h"
#include "services/OximetryOutbox.h"
#include "services/RemovedNights.h"
#include "services/SetupService.h"
#include "services/SupplyPublisher.h"
#include "services/MyAirService.h"
#include "web/IngressBase.h"
#include "web/DataMoveServer.h"
#include "web/QueryService.h"
#ifndef _WIN32
#include "services/ReportGeneratorService.h"
#endif
#endif
#include "utils/OximetryDevice.h"
#include "services/BurstCollectorService.h"
#include "services/SessionDiscoveryService.h"
#include "services/DataPublisherService.h"
#include "parsers/CpapdashBridge.h"
#include "database/IDatabase.h"
#include "database/SQLiteDatabase.h"
#include "database/DatabaseFactory.h"
#include "services/PreflightService.h"
#ifdef WITH_POSTGRESQL
#include "database/DatabaseService.h"
#include "database/PostgresDatabase.h"
#endif
#include "agent/AgentMqttConfig.h"
#include "agent/AgentService.h"
#include "services/MLTrainingService.h"
#include "services/BackfillService.h"
#include <cpapdash/parser/OximetryCsv.h>
#include "services/PrismaIngestion.h"
#include "agent/IAgentLLM.h"
#include "mqtt_client.h"
#include "llm_client.h"
#include "utils/ConfigManager.h"
#include "utils/AppConfig.h"
#include "utils/DeviceIdMigration.h"
#include "utils/MachineIdentity.h"
#include "utils/CardResidue.h"
#include "utils/CardImport.h"
#include "utils/SessionEnd.h"
#include "utils/MachineClocks.h"
#include "utils/SessionLabels.h"
#include <cpapdash/parser/WallClock.h>
#include "utils/CheckpointSizes.h"
#include "services/As11NightImport.h"
#include "utils/FileLogger.h"
#include "utils/ReExec.h"
#include "utils/CardLayout.h"
#include "utils/DbProbeCli.h"
#include "utils/ParentWatch.h"
#include "clients/ble/BleTransport.h"
#include "clients/AirSense11BleClient.h"
#include <algorithm>
#include <iostream>
#include <iomanip>
#include <map>
#include <mutex>
#include <csignal>
#include <atomic>
#include <memory>
#include <set>
#include <regex>
#include <string>
#include <cstring>
#include <cstdlib>
#include <ctime>
#include <filesystem>
#include <sstream>
#ifdef _WIN32
#include <windows.h>
#endif

std::atomic<bool> shutdown_requested(false);
/// What main returns once the shutdown finishes. SDD-041: 42 hands a staged
/// update to the supervisor. It has to be RETURNED, after an orderly shutdown:
/// std::exit from a worker thread ran static destructors under a live Drogon
/// loop, trantor aborted ("forbidden to run loop on threads other than
/// event-loop thread"), and the supervisor saw exit 1 instead of 42.
std::atomic<int> requested_exit_code(0);
std::unique_ptr<hms_cpap::BurstCollectorService> burst_service;
std::unique_ptr<hms_cpap::AgentService> agent_service;
std::unique_ptr<hms_cpap::MLTrainingService> ml_service;
std::unique_ptr<hms_cpap::BackfillService> backfill_service;
// SDD-081: the OSCAR helper's end here, and the _hms-cpap._tcp advertisement.
std::shared_ptr<hms_cpap::oscar::helper::Host> oscar_helper_host;
std::unique_ptr<hms_cpap::oscar::helper::Advertiser> mdns_advertiser;

/**
 * Graceful shutdown logic (shared by all platforms)
 */
void requestShutdown() {
    shutdown_requested = true;
    // SDD-081: a read waiting on the helper ends before the reader is joined.
    if (oscar_helper_host) oscar_helper_host->shutdown();
    if (burst_service) burst_service->stop();
    if (agent_service) agent_service->stop();
    if (ml_service) ml_service->stop();
    if (backfill_service) backfill_service->stop();
#ifdef BUILD_WITH_WEB
    drogon::app().quit();
#endif
}

void signalHandler(int signal) {
    std::cout << "\nReceived signal " << signal << ", shutting down gracefully..." << std::endl;
    requestShutdown();
}

#ifdef _WIN32
BOOL WINAPI consoleCtrlHandler(DWORD ctrlType) {
    if (ctrlType == CTRL_C_EVENT || ctrlType == CTRL_CLOSE_EVENT ||
        ctrlType == CTRL_BREAK_EVENT) {
        std::cout << "\nReceived console event, shutting down gracefully..." << std::endl;
        requestShutdown();
        return TRUE;
    }
    return FALSE;
}
#endif

/**
 * Print banner
 */
void printBanner() {
    // The version line is built at runtime from HMS_CPAP_VERSION (the VERSION
    // file, injected by CMake) and padded to the box width. It used to be the
    // hardcoded string "2.2.0", which had drifted years behind the real version
    // and made startup logs actively misleading about what was deployed.
    constexpr size_t kBoxInnerWidth = 59;
    std::string version_line = "      Version: " + std::string(HMS_CPAP_VERSION);
    if (version_line.size() < kBoxInnerWidth)
        version_line.append(kBoxInnerWidth - version_line.size(), ' ');

    std::cout << R"(
╔═══════════════════════════════════════════════════════════╗
║                                                           ║
║      HMS-CPAP - CPAP Data Collection Service             ║
║                                                           ║
║      ResMed AirSense 10 Data Collection                  ║
║      Sources: HTTP, local                                 ║
║                                                           ║)"
              << "\n║" << version_line << "║"
              << R"(
║      Platform: Cross-platform (Linux, Windows)            ║
║                                                           ║
╚═══════════════════════════════════════════════════════════╝
)" << std::endl;
}

// Portable setenv wrapper
inline void portableSetenv(const char* name, const char* value) {
#ifdef _WIN32
    _putenv_s(name, value);
#else
    setenv(name, value, 1);
#endif
}

// Portable temp directory
inline std::string tempDir() {
    return (std::filesystem::temp_directory_path()).string();
}

/**
 * Print configuration
 */
void printConfiguration() {
    std::cout << "Configuration:" << std::endl;
    std::string source = hms_cpap::ConfigManager::get("CPAP_SOURCE", "ezshare");
    std::cout << "  Device ID:          " << hms_cpap::ConfigManager::get("CPAP_DEVICE_ID", hms_cpap::AppConfig::kDefaultDeviceId) << std::endl;
    std::cout << "  Device Name:        " << hms_cpap::ConfigManager::get("CPAP_DEVICE_NAME", hms_cpap::AppConfig::kDefaultDeviceName) << std::endl;
    if (source == "local") {
        std::cout << "  Source:             Local directory" << std::endl;
        std::cout << "  Local Dir:          " << hms_cpap::ConfigManager::get("CPAP_LOCAL_DIR", "(not set)") << std::endl;
    } else if (source == "lowenstein") {
        std::cout << "  Source:             Löwenstein Prisma" << std::endl;
        std::cout << "  Data Dir:           " << hms_cpap::ConfigManager::get("CPAP_LOCAL_DIR", "(not set)") << std::endl;
    } else if (source == "sefam") {
        std::cout << "  Source:             Sefam S.Box" << std::endl;
        std::cout << "  Card Dir:           " << hms_cpap::ConfigManager::get("CPAP_LOCAL_DIR", "(not set)") << std::endl;
    } else if (source == "bmc") {
        std::cout << "  Source:             BMC / React Health Luna" << std::endl;
        std::cout << "  Card Dir:           " << hms_cpap::ConfigManager::get("CPAP_LOCAL_DIR", "(not set)") << std::endl;
    } else if (source == "philips") {
        std::cout << "  Source:             Philips Respironics (experimental)" << std::endl;
        std::cout << "  Card Dir:           " << hms_cpap::ConfigManager::get("CPAP_LOCAL_DIR", "(not set)") << std::endl;
    } else {
        std::cout << "  Source:             ez Share" << std::endl;
        std::cout << "  ez Share URL:       " << hms_cpap::ConfigManager::get("EZSHARE_BASE_URL", "http://192.168.4.1") << std::endl;
    }
    // SDD-024: report what actually runs. CPAP_TRANSPORT/CPAP_FORMAT are set
    // from config.json by main(), so reading them here shows the resolved pair
    // rather than re-deriving it from a legacy string that may be a fallback.
    std::cout << "  Transport:          "
              << hms_cpap::ConfigManager::get("CPAP_TRANSPORT",
                     hms_cpap::AppConfig::transportForSource(source)) << std::endl;
    std::cout << "  Format:             "
              << hms_cpap::ConfigManager::get("CPAP_FORMAT",
                     hms_cpap::AppConfig::formatForSource(source)) << std::endl;
    std::cout << "  Burst Interval:     " << hms_cpap::ConfigManager::getInt("BURST_INTERVAL", 120) << " seconds" << std::endl;
    std::cout << "  Session Gap:        " << hms_cpap::ConfigManager::getInt("SESSION_GAP_MINUTES", 60) << " minutes" << std::endl;
    std::cout << "  Health Check Port:  " << hms_cpap::ConfigManager::getInt("HEALTH_CHECK_PORT", 8893) << std::endl;
    // Issue #57: a CPAP names its files and stamps its records on its own
    // clock, which is local time with no zone. hms-cpap reads those times in
    // this process's zone, so a container left on UTC files a 1 AM night as
    // 1 AM UTC and publishes it with +00:00. Say which zone is in use, and
    // how to change it when it is UTC.
    {
        const std::time_t now_t = std::time(nullptr);
        std::tm local{};
#ifdef _WIN32
        localtime_s(&local, &now_t);
#else
        localtime_r(&now_t, &local);
#endif
        char zone[64];
        std::strftime(zone, sizeof(zone), "%Z (UTC%z)", &local);
        const char* tz = std::getenv("TZ");
        std::cout << "  Time Zone:          " << zone
                  << (tz && *tz ? std::string(", TZ=") + tz : std::string()) << std::endl;
        char offset[8];
        std::strftime(offset, sizeof(offset), "%z", &local);
        if (std::string(offset) == "+0000")
            std::cout << "  (the machine's clock is read in this zone; if yours is not UTC, set TZ, "
                         "e.g. TZ=America/New_York in Docker, or set the machine's clock on "
                         "its card on the Devices page)"
                      << std::endl;
    }
    std::cout << std::endl;
}

/**
 * SDD-013: run the local sleep model's nightly job for one night, now, and exit.
 *
 * Usage: hms_cpap --local-model-night YYYYMMDD
 *
 * The same events the service handles when a night closes (Startup, which
 * checks the installed model, then NightClosed), on the configured database,
 * with no collector, web server or MQTT. local_model must be on and the model
 * installed, as for the nightly job. The night goes without its STR row, which
 * only the collector holds.
 */
int runLocalModelNight(hms_cpap::AppConfig& config, const std::string& config_path,
                       const std::string& folder) {
    std::shared_ptr<hms_cpap::IDatabase> db = hms_cpap::makeDatabaseFromConfig();
    if (!db->connect()) {
        std::cerr << "Local model: database connection failed" << std::endl;
        return 1;
    }
    hms_cpap::model::LocalModelService::Deps d;
    d.db = db;
    d.config = &config;
    d.config_path = config_path;
    d.device_id = hms_cpap::ConfigManager::get("CPAP_DEVICE_ID", hms_cpap::AppConfig::kDefaultDeviceId);
    hms_cpap::model::LocalModelService svc(std::move(d));
    using Event = hms_cpap::model::LocalModelService::Event;
    svc.handle(Event::Startup);
    if (const auto s = svc.status(); s["state"].asString() != "ready") {
        std::cerr << "Local model: not ready (" << s["state"].asString() << "): " << s["message"].asString()
                  << std::endl;
        return 1;
    }
    const auto t0 = std::chrono::steady_clock::now();
    svc.handle(Event::NightClosed, folder);
    const double secs = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
    // D-701-3: a night whose summary was written from these same facts is done.
    if (const auto skipped = svc.status()["last_skipped"].asString(); !skipped.empty()) {
        std::cout << "Local model: night " << skipped << ", already summarized from these facts" << std::endl;
        return 0;
    }
    const auto last = svc.status()["last"];
    std::cout << "Local model: night " << last["night"].asString() << ", "
              << (last["how"].asString().empty() ? "nothing written" : last["how"].asString()) << ", "
              << std::fixed << std::setprecision(1) << secs << " s"
              << (last["why"].asString().empty() ? "" : ", " + last["why"].asString()) << std::endl;
    return last["how"].asString().empty() ? 1 : 0;
}

/**
 * Run STR.edf backfill: parse file and save ALL records to DB.
 *
 * Usage: hms_cpap --backfill /path/to/str.edf
 */
int runBackfill(const std::string& filepath) {
    std::string device_id = hms_cpap::ConfigManager::get("CPAP_DEVICE_ID", hms_cpap::AppConfig::kDefaultDeviceId);

    std::cout << "STR Backfill: Parsing " << filepath << std::endl;
    cpapdash::parser::WallClockLabels labels;   // SDD-074 D4
    auto records = hms_cpap::EDFParser::parseSTRFile(filepath, device_id);
    if (records.empty()) {
        std::cerr << "STR Backfill: No therapy days found in " << filepath << std::endl;
        return 1;
    }
    std::cout << "STR Backfill: Found " << records.size() << " therapy days" << std::endl;

    // Connect to the configured DB backend (sqlite/postgresql/mysql). Previously
    // this hardcoded a PostgreSQL DatabaseService, so --backfill failed for
    // SQLite users (issue #8). Use the shared factory so it matches the service.
    auto db = hms_cpap::makeDatabaseFromConfig();
    if (!db->connect()) {
        std::cerr << "STR Backfill: DB connection failed" << std::endl;
        return 1;
    }

    // SDD-029: never the nights an operator removed.
    if (!db->saveSTRDailyRecords(hms_cpap::withoutRemovedNights(
            records, hms_cpap::removedNightSet(*db, device_id)))) {
        std::cerr << "STR Backfill: Failed to save records" << std::endl;
        return 1;
    }

    // Print summary
    const auto& first = records.front();
    const auto& last = records.back();
    std::cout << "STR Backfill: Saved " << records.size() << " days ("
              << hms_cpap::wall::text(first.record_date).substr(0, 10) << " to "
              << hms_cpap::wall::text(last.record_date).substr(0, 10) << ")"
              << std::endl;

    return 0;
}

/**
 * Reparse sessions from local archive for a date range.
 *
 * Usage: hms_cpap --reparse /mnt/public/cpap_data 2025-08-18 [2025-09-09]
 *
 * SDD-010: card_root is the SD card ROOT, the folder holding both STR.edf and
 * DATALOG, NOT the DATALOG folder itself. One meaning of "the local dir"
 * across the whole binary.
 *
 * Scans date folders in the given range, groups files into sessions using
 * the same session gap logic (SESSION_GAP_MINUTES, default 60), deletes old DB records, and re-parses fresh.
 */
int runReparse(const std::string& card_root, const std::string& start_str, const std::string& end_str) {
    cpapdash::parser::WallClockLabels labels;   // SDD-074 D4
    const auto layout = hms_cpap::classifyLocalDir(card_root);
    if (layout != hms_cpap::LocalDirLayout::Root) {
        std::cerr << "hms_cpap --reparse: "
                  << hms_cpap::localDirProblem(layout, card_root) << "." << std::endl
                  << "  " << hms_cpap::localDirRemedy(layout, card_root) << std::endl;
        return 1;
    }
    const std::string archive_dir = hms_cpap::datalogDirFor(card_root);
    std::string device_id = hms_cpap::ConfigManager::get("CPAP_DEVICE_ID", hms_cpap::AppConfig::kDefaultDeviceId);
    std::string device_name = hms_cpap::ConfigManager::get("CPAP_DEVICE_NAME", hms_cpap::AppConfig::kDefaultDeviceName);

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

    // Connect to the configured DB backend via the shared factory (sqlite/
    // postgresql/mysql) so CLI tools and the service never disagree.
    auto db = hms_cpap::makeDatabaseFromConfig();
    if (!db->connect()) {
        std::cerr << "Reparse: DB connection failed ("
                  << hms_cpap::ConfigManager::get("DB_TYPE", "sqlite") << ")" << std::endl;
        return 1;
    }
    hms_cpap::machine_clocks::setPrimaryDevice(device_id);   // SDD-074
    hms_cpap::machine_clocks::loadAll(*db);

    int total_deleted = 0;
    int total_parsed = 0;
    int total_saved = 0;
    const auto removed = hms_cpap::removedNightSet(*db, device_id);

    for (const auto& folder : date_folders) {
        std::string folder_path = archive_dir + "/" + folder;

        if (!std::filesystem::exists(folder_path)) {
            continue;  // No data for this date
        }

        std::cout << "\n--- Folder: " << folder << " ---" << std::endl;

        // SDD-029 D3: reparsing a night is asking for it back.
        if (removed.count(folder) && db->restoreNight(device_id, folder)) {
            std::cout << "  Restored removed night " << folder << std::endl;
        }

        // Group files into sessions
        auto sessions = hms_cpap::SessionDiscoveryService::groupLocalFolder(folder_path, folder);

        if (sessions.empty()) {
            std::cout << "  No sessions found" << std::endl;
            continue;
        }

        // Parse every session of the night first, then write the night whole:
        // replaceNight() deletes the folder's stored sessions and saves these
        // in one transaction, so a kill at any point leaves the old night or
        // the new one, never neither.
        std::vector<hms_cpap::IDatabase::NightSessionWrite> night;
        std::vector<std::string> prefixes;

        for (const auto& session : sessions) {
            // Create temp directory with only this session's files (symlinks)
            std::string temp_dir = (std::filesystem::temp_directory_path() / "cpap_reparse" / (folder + "_" + session.session_prefix)).string();
            std::filesystem::create_directories(temp_dir);

            // Clear any previous symlinks
            for (const auto& entry : std::filesystem::directory_iterator(temp_dir)) {
                std::filesystem::remove(entry.path());
            }

            // Copy session files into temp dir for isolated parsing
            auto stageFile = [&](const std::string& filename) {
                std::filesystem::path src = std::filesystem::path(folder_path) / filename;
                std::filesystem::path dst = std::filesystem::path(temp_dir) / filename;
                if (std::filesystem::exists(src)) {
                    std::filesystem::copy_file(src, dst, std::filesystem::copy_options::overwrite_existing);
                }
            };

            for (const auto& f : session.brp_files) stageFile(f);
            for (const auto& f : session.pld_files) stageFile(f);
            for (const auto& f : session.sad_files) stageFile(f);
            for (const auto& f : session.csl_files) stageFile(f);
            for (const auto& f : session.eve_files) stageFile(f);

            // Parse
            total_parsed++;
            auto parsed = hms_cpap::EDFParser::parseSession(
                temp_dir, device_id, device_name, session.session_start);

            if (!parsed) {
                std::cerr << "  Failed to parse session " << session.session_prefix << std::endl;
                continue;
            }

            // SDD-047: the day's EVEs were staged; keep this stretch's events.
            hms_cpap::keepOwnEvents(*parsed, session);

            // Set relative file paths (same format as normal pipeline)
            hms_cpap::applySessionFilePaths(*parsed, session, folder);

            // Saved with the night below, which also closes it at the end the
            // data carries (SDD-037 D2), records the files it is made of
            // (SDD-014) and its checkpoint sizes in the burst cycle's own
            // shape (SDD-077 D3).
            hms_cpap::IDatabase::NightSessionWrite w;
            w.session = std::move(*parsed);
            w.start = session.session_start;
            w.files = hms_cpap::sessionFileRefs(session, folder);
            w.checkpoint_sizes = hms_cpap::checkpointSizes(session);
            night.push_back(std::move(w));
            prefixes.push_back(session.session_prefix);

            // Cleanup temp dir
            std::filesystem::remove_all(temp_dir);
        }

        const auto written = db->replaceNight(device_id, folder, night);
        if (!written.ok) {
            std::cerr << "  Failed to save night " << folder << ", kept as it was" << std::endl;
            continue;
        }
        if (written.deleted > 0) {
            std::cout << "  Deleted " << written.deleted << " existing session(s) from DB" << std::endl;
            total_deleted += written.deleted;
        }
        total_saved += written.saved;
        for (std::size_t i = 0; i < night.size(); ++i) {
            const auto& w = night[i];
            // SDD-077 D2: the card's copy replaces a Bluetooth copy of the
            // same stretch, after the card's row is committed.
            hms_cpap::As11NightImport::replaceConstructedFiles(
                *db, device_id, w.start, hms_cpap::dataEndOf(w.session).value_or(w.start));

            double hours = w.session.duration_seconds.value_or(0) / 3600.0;
            double ahi = w.session.metrics.has_value() ? w.session.metrics->ahi : 0.0;
            std::cout << "  Saved: " << prefixes[i]
                      << " (" << std::fixed << std::setprecision(1) << hours << "h"
                      << ", AHI=" << std::setprecision(2) << ahi << ")" << std::endl;
        }
    }

    // Cleanup
    std::filesystem::remove_all(std::filesystem::temp_directory_path() / "cpap_reparse");

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
    // A Windows in-place restart starts us while the process we replace is
    // still ending and still holds the web port: wait for its exit before
    // anything checks or binds (utils/ReExec.h). Nothing on POSIX.
    hms_cpap::reexec::waitForReplacedProcess();

    // SDD-081 D1: the OSCAR helper role. `--oscar-helper`, or an install whose
    // config.json says "role": "oscar_helper" (one role per install, F9): no
    // web server, no collector, no database.
    {
        bool helper_flag = false;
        bool plain_start = true;   // no other mode asked for (--preflight, --read-card, ...)
        std::string helper_config;
        for (int i = 1; i < argc; ++i) {
            if (std::strcmp(argv[i], "--oscar-helper") == 0) helper_flag = true;
            if (std::strcmp(argv[i], "--config") == 0 && i + 1 < argc) helper_config = argv[++i];
            else if (std::strcmp(argv[i], "--no-browser") != 0 && std::strcmp(argv[i], "--exit-with-parent") != 0)
                plain_start = false;
        }
        const std::string helper_dir = hms_cpap::AppConfig::dataDir();
        if (helper_config.empty()) helper_config = helper_dir + "/config.json";
        if (helper_flag ||
            (plain_start && hms_cpap::oscar::helper::configRole(helper_config) == "oscar_helper"))
            return hms_cpap::oscar::helper::runHelperMain(std::vector<std::string>(argv + 1, argv + argc),
                                                          helper_dir, helper_config, HMS_CPAP_VERSION);
    }

    // `--ble-scan [seconds]`: list what advertises over this computer's
    // Bluetooth and exit, before any config, log or database is touched. A
    // diagnostic: it never connects to anything.
    for (int i = 1; i < argc; ++i) {
        // SDD-073 D1: `--read-card <device> --card-root <folder> --out <file>`:
        // the installer's one elevated step, a read-only read of a card into
        // a .cpapcard. Nothing else runs (sleepbridge/CardReadCli.h).
        if (std::strcmp(argv[i], "--read-card") == 0)
            return hms_cpap::sleepbridge::runReadCard(std::vector<std::string>(argv + 1, argv + argc));
        if (std::strcmp(argv[i], "--ble-scan") == 0) {
            int seconds = 10;
            if (i + 1 < argc) {
                try { seconds = (std::max)(1, (std::min)(120, std::stoi(argv[i + 1]))); } catch (...) {}
            }
            return hms_cpap::ble::runScan(seconds);
        }
        // `--as11-spool-probe <data_dir> <YYYY-MM-DD> [spool...]`: a bench
        // probe. Opens a session with <data_dir>'s pairing, pulls each spool
        // on its own link, writes it to <data_dir>/spool-probe/<name>.bin and
        // prints one ### line per spool. Nothing else runs.
        if (std::strcmp(argv[i], "--as11-spool-probe") == 0) {
            if (i + 2 >= argc) {
                std::cerr << "usage: --as11-spool-probe <data_dir> <YYYY-MM-DD> [spool...]" << std::endl;
                return 2;
            }
            const std::string dir = argv[i + 1], from = argv[i + 2];
            std::vector<std::string> names;
            for (int j = i + 3; j < argc; ++j) names.emplace_back(argv[j]);
            if (names.empty())
                names = {"Summary", "TherapyEvents-RespiratoryEvents", "RespiratoryFlow6p25Hz",
                         "MaskPressure6p25Hz", "Leak0p5Hz", "InspiratoryPressure0p5Hz"};
            hms_cpap::AirSense11BleClient client(dir);
            const std::filesystem::path out_dir = std::filesystem::path(dir) / "spool-probe";
            std::error_code ec;
            std::filesystem::create_directories(out_dir, ec);
            int ok = 0;
            for (const auto& n : names) {
                const auto p = client.probeSpool(n, from);
                if (p.result == hms_cpap::as11::Result::Ok) {
                    ++ok;
                    std::ofstream f(out_dir / (n + ".bin"), std::ios::binary);
                    f.write(reinterpret_cast<const char*>(p.bytes.data()),
                            static_cast<std::streamsize>(p.bytes.size()));
                }
                std::cout << "### SPOOL " << n << " result=" << hms_cpap::as11::resultText(p.result)
                          << " bytes=" << p.bytes.size() << " seconds=" << p.seconds
                          << " status=" << (p.status.empty() ? "-" : p.status)
                          << " error=" << (p.error.empty() ? "-" : p.error) << std::endl;
            }
            return ok > 0 ? 0 : 1;
        }
    }

    // ── Support log ─────────────────────────────────────────────────
    // Started before anything else can fail, and before the config is read.
    // A user asked to "send the log" has to be able to produce one even when
    // the failure was in loading the config, which is exactly the case where
    // it is most often needed. Config can move it, resize it or switch it off
    // further down, once we know what config says.
    //
    // EXCEPT for --preflight, which must stay a plain short-lived process.
    // The Windows installer runs it as its last step and BLOCKS on it
    // (installer.iss [Code], Exec(..., ewWaitUntilTerminated)), so anything that
    // can delay our exit hangs Setup instead of failing it. Teeing buys nothing
    // there either: the caller already redirects our output to a file it reads
    // back. Scanned before start() rather than after, because by the time the
    // normal argument loop runs the pipe and its thread already exist.
    // --test-db and --create-db join --preflight here for the same reason: they
    // are short-lived probes a GUI blocks on, so nothing may hold their exit
    // open, and their whole output is the report the caller reads back.
    // SDD-067: --move-data too. The support log lives in the data directory,
    // which is the thing being moved.
    bool preflight_only = false;
    bool db_probe_mode  = false;
    bool db_provision   = false;
    int  move_data_at   = 0;
    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "--preflight") == 0) preflight_only = true;
        if (std::strcmp(argv[i], "--test-db") == 0)   db_probe_mode  = true;
        if (std::strcmp(argv[i], "--create-db") == 0) { db_probe_mode = true; db_provision = true; }
        if (std::strcmp(argv[i], "--move-data") == 0) move_data_at = i;
    }
    if (!preflight_only && !db_probe_mode && move_data_at == 0) {
        hms_cpap::FileLogger::start(hms_cpap::FileLogger::defaultPath(),
                                    5 * 1024 * 1024, 3);
        std::atexit([] { hms_cpap::FileLogger::stop(); });
    }

    // SDD-016: exit when the supervisor that started us dies.
    //
    // Opt-in, and it has to be: under systemd and Docker stdin is /dev/null and
    // reads EOF immediately, so watching it unconditionally would exit the
    // instant the service started. Only a parent that deliberately holds the
    // write end passes this flag.
    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "--exit-with-parent") == 0) {
            hms_cpap::watchParentPipe(0, [] {
                std::cerr << "Supervisor exited; shutting down." << std::endl;
                // _exit, not exit: the supervisor is already gone, nobody is
                // reading our output, and running static destructors here races
                // the very threads we are trying to stop. The service is
                // crash-safe by design -- it re-reads the card on the next
                // start -- so a fast exit costs nothing and hangs nothing.
                std::cout.flush();
                std::cerr.flush();
                std::_Exit(0);
            });
            break;
        }
    }

    // SDD-016: answer "can I open this database?" without an HTTP server.
    //
    // Deliberately BEFORE the config is loaded. The caller is asking about a
    // candidate database it is still deciding on, so the answer must not depend
    // on -- or disturb -- whatever is currently in config.json. It also means a
    // machine with no config at all can still be asked, which is exactly the
    // first-run case the supervisor's configurator has to handle.
    if (db_probe_mode) {
        std::string request((std::istreambuf_iterator<char>(std::cin)),
                            std::istreambuf_iterator<char>());
        const auto result = hms_cpap::runDbProbeCli(request, db_provision);
        std::cout << result.json;
        return result.exit_code;
    }

    // SDD-067 D18: `--move-data <from> <to> [--port N]`. The Home Assistant
    // add-on's run.sh calls it when the data directory has to move, before
    // the normal start; like --test-db it runs before the configuration and
    // the database are touched. It moves, serves only the progress page on
    // the web port, and exits 0 when the move is complete.
    if (move_data_at > 0) {
        if (move_data_at + 2 >= argc) {
            std::cerr << "usage: --move-data <from> <to> [--port N]" << std::endl;
            return 2;
        }
        int port = 8893;
        for (int i = 1; i + 1 < argc; ++i) {
            if (std::strcmp(argv[i], "--port") == 0) {
                try { port = std::stoi(argv[i + 1]); } catch (...) {}
            }
        }
        return hms_cpap::datamove::runMoveDataMode(argv[move_data_at + 1], argv[move_data_at + 2], port);
    }

    // ── Load AppConfig ──────────────────────────────────────────────
    std::string data_dir = hms_cpap::AppConfig::dataDir();
    if (hms_cpap::AppConfig::migrateLegacyDataDir(data_dir)) {
        std::cout << "Config: migrated from " << hms_cpap::AppConfig::legacyDataDir()
                  << " to " << data_dir << std::endl;
    }
    std::string config_path = data_dir + "/config.json";

    // Allow --config <path> override (scan before CLI modes)
    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "--config") == 0 && i + 1 < argc) {
            config_path = argv[i + 1];
            break;
        }
    }

    // SDD-006: suppress the first-run browser. For scripted installs and for
    // anyone who simply does not want a window appearing.
    // preflight_only is already resolved above, because the support log has to
    // know before it starts. SDD-005/006: --preflight validates the
    // configuration and exits. Lets an installer check its own work, lets
    // systemd use it as ExecStartPre, and lets the desktop shell find out WHY
    // it cannot start instead of inferring it from how fast the child died.
    bool no_browser = false;
    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "--no-browser") == 0) no_browser = true;
    }

    hms_cpap::AppConfig config;
    std::string config_error;
    const auto load_status =
        hms_cpap::AppConfig::loadFile(config_path, config, &config_error);

    // An unreadable config is NOT a first run, and treating it as one is how a
    // single mistyped character used to destroy a working setup: load() failed,
    // config_existed went false, and the "only create on first run" save below
    // wrote defaults straight over the user's file. Stop here instead, name the
    // parser's complaint, and leave the file exactly as they left it.
    if (load_status == hms_cpap::AppConfig::LoadStatus::Invalid) {
        std::cerr << "Refusing to start. The configuration file is not valid JSON:\n"
                  << "  " << config_path << "\n"
                  << "  " << config_error << "\n"
                  << "Your file has NOT been changed. Fix the character at the offset "
                     "above, or delete the file to start over from the setup wizard.\n";
        return 1;
    }

    const bool config_existed = (load_status == hms_cpap::AppConfig::LoadStatus::Ok);

    // Env vars fill any empty fields (fallback for systemd Environment= lines)
    config.applyEnvFallbacks();

    // Now that config is known, honour what it says about the support log.
    // Anything logged before this point is already in the default file.
    //
    // Skipped entirely for --preflight, for the same reason start() was: that
    // path must stay a plain process the installer can block on. Guarding only
    // the first start() was not enough, because this block would happily start
    // one anyway, and with no atexit handler registered the still-joinable pump
    // thread then hit ~thread() and aborted the process.
    if (!preflight_only) {
        const std::string want = config.logging.file.empty()
                                     ? hms_cpap::FileLogger::defaultPath()
                                     : config.logging.file;
        if (!config.logging.enabled) {
            hms_cpap::FileLogger::stop();
        } else if (want != hms_cpap::FileLogger::activePath()) {
            hms_cpap::FileLogger::stop();
            hms_cpap::FileLogger::start(
                want,
                static_cast<std::size_t>(config.logging.max_mb) * 1024 * 1024,
                config.logging.keep);
        }
        if (const auto p = hms_cpap::FileLogger::activePath(); !p.empty()) {
            std::cout << "Log: " << p << std::endl;
        }
    }

    // Checked BEFORE anything is opened or bound. Configuration errors are
    // deterministic: a busy port, a wrong password and an unwritable folder do
    // not become correct on a second attempt, so discovering them through a
    // restart loop hides the cause and tells the user a guess. Fail here, name
    // the problem, and say what to change.
    {
        // --local-model-night serves no web page and runs beside the service
        // that holds web_port: that port being in use is no problem for it.
        bool serves_web = true;
        for (int a = 1; a < argc; ++a)
            if (std::strcmp(argv[a], "--local-model-night") == 0) serves_web = false;
        const auto report = hms_cpap::PreflightService::run(config, serves_web);
        if (preflight_only) {
            std::cout << "Preflight checks:\n" << report.render();
            std::cout << (report.ok() ? "OK: configuration looks usable.\n"
                                      : "FAILED: fix the items marked FAIL above.\n");
            return report.ok() ? 0 : 1;
        }
        if (!report.ok()) {
            std::cerr << "Refusing to start. Configuration problems found:\n"
                      << report.render()
                      << "Config file: " << config_path << "\n";
            return 1;
        }
    }

    // Default sqlite_path if empty
    if (config.database.sqlite_path.empty()) {
        config.database.sqlite_path = data_dir + "/cpap.db";
    }

    // SDD-051: every install its own device id. A first run generates one; an
    // install still on an old fixed default gets one now, written down BEFORE
    // anything reads the id (the env bridge below) and before its rows move
    // (after the database connects), so a crash in between resumes the same
    // move. A pinned id is never touched.
    if (!config_existed && config.device_id == hms_cpap::AppConfig::kDefaultDeviceId) {
        config.device_id = hms_cpap::generateDeviceId();
    } else if (config_existed) {
        const std::string before = config.device_id;
        if (hms_cpap::beginDeviceIdBackfill(config) && config.device_id != before) {
            std::cout << "Config: device id " << before << " was a shared default; this install is "
                      << config.device_id << " from now on (SDD-051)" << std::endl;
            if (!config.save(config_path)) {
                std::cerr << "Config: could not save the new device id; keeping " << before
                          << std::endl;
                config.device_id = before;
                config.device_id_previous.clear();
            }
        }
    }

    // Only create config file on first run -- don't overwrite user's config
    if (!config_existed) {
        config.save(config_path);
    }

    // ── Bridge: set env vars from merged config so BurstCollectorService works ──
    // Config.json is the single source of truth; env vars are set for
    // legacy code that reads them via ConfigManager.
    portableSetenv("CPAP_DEVICE_ID", config.device_id.c_str());
    portableSetenv("CPAP_DEVICE_NAME", config.device_name.c_str());
    // SDD-022/024. config.json is read by AppConfig, but BurstCollectorService
    // and the controllers read ConfigManager, which is env-backed. This is the
    // bridge between the two. Exporting `source` alone left transport and
    // format at their defaults, so a config.json saying format=sefam ran the
    // ResMed collector -- the setting appeared to take and silently did not.
    portableSetenv("CPAP_SOURCE", config.legacySource().c_str());
    portableSetenv("CPAP_TRANSPORT", config.transport.c_str());
    portableSetenv("CPAP_FORMAT", config.format.c_str());
    portableSetenv("EZSHARE_BASE_URL", config.ezshare_url.c_str());
    portableSetenv("FLASHAIR_BASE_URL", config.flashair_url.c_str());
    // Store numeric conversions in locals to avoid dangling pointer from temporary
    std::string burst_str = std::to_string(config.burst_interval);
    std::string port_str  = std::to_string(config.web_port);
    portableSetenv("BURST_INTERVAL", burst_str.c_str());
    portableSetenv("HEALTH_CHECK_PORT", port_str.c_str());
    // SDD-001: every engine's saveSession reads it at use time (utils/Desaturation.h).
    std::string desat_str = std::to_string(config.desat_drop_pct);
    portableSetenv("DESAT_DROP_PCT", desat_str.c_str());
    if (!config.local_dir.empty()) portableSetenv("CPAP_LOCAL_DIR", config.local_dir.c_str());
    // SDD-006: the burst collector and the archive path both read this through
    // ConfigManager, so bridging it here is what makes a wizard-set archive
    // folder actually take effect.
    if (!config.archive_dir.empty()) portableSetenv("CPAP_ARCHIVE_DIR", config.archive_dir.c_str());

    // Database — always set DB_TYPE so BurstCollectorService picks the right backend
    portableSetenv("DB_TYPE", config.database.type.c_str());
    if (config.database.type == "sqlite") {
        portableSetenv("SQLITE_PATH", config.database.sqlite_path.c_str());
    } else if (config.database.type == "postgresql" || config.database.type == "mysql") {
        portableSetenv("DB_HOST", config.database.host.c_str());
        std::string db_port_str = std::to_string(config.database.port);
        portableSetenv("DB_PORT", db_port_str.c_str());
        portableSetenv("DB_NAME", config.database.name.c_str());
        portableSetenv("DB_USER", config.database.user.c_str());
        portableSetenv("DB_PASSWORD", config.database.password.c_str());
    }

    portableSetenv("MQTT_ENABLED", config.mqtt.enabled ? "true" : "false");
    if (config.mqtt.enabled) {
        portableSetenv("MQTT_BROKER", config.mqtt.broker.c_str());
        std::string mqtt_port_str = std::to_string(config.mqtt.port);
        portableSetenv("MQTT_PORT", mqtt_port_str.c_str());
        portableSetenv("MQTT_USER", config.mqtt.username.c_str());
        portableSetenv("MQTT_PASSWORD", config.mqtt.password.c_str());
        portableSetenv("MQTT_CLIENT_ID", config.mqtt.client_id.c_str());
    }

    if (config.llm.enabled) {
        portableSetenv("LLM_ENABLED", "true");
        portableSetenv("LLM_PROVIDER", config.llm.provider.c_str());
        portableSetenv("LLM_ENDPOINT", config.llm.endpoint.c_str());
        portableSetenv("LLM_MODEL", config.llm.model.c_str());
        portableSetenv("LLM_API_KEY", config.llm.api_key.c_str());
        std::string llm_tokens_str = std::to_string(config.llm.max_tokens);
        portableSetenv("LLM_MAX_TOKENS", llm_tokens_str.c_str());
    }

    if (config.agent.enabled) {
        portableSetenv("AGENT_ENABLED", "true");
        portableSetenv("AGENT_LLM_PROVIDER", config.llm.provider.c_str());
        portableSetenv("AGENT_LLM_ENDPOINT", config.llm.endpoint.c_str());
        portableSetenv("AGENT_LLM_MODEL", config.llm.model.c_str());
        portableSetenv("AGENT_LLM_API_KEY", config.llm.api_key.c_str());
        portableSetenv("AGENT_EMBED_MODEL", config.agent.embed_model.c_str());
    }

    // ── Create IDatabase from config ────────────────────────────────
    // Backend selection goes through makeDatabaseFromConfig() so the service
    // path, BurstCollectorService and the --backfill/--reparse CLI tools can
    // never disagree on which DB to use. Hand-rolling it here is what left
    // database.type="mysql" writing to MySQL from the collector while every
    // reader fell through to SQLite. The env bridge above already published
    // DB_TYPE and the DB_*/SQLITE_PATH vars the factory reads.
    std::shared_ptr<hms_cpap::IDatabase> db = hms_cpap::makeDatabaseFromConfig();

    // The factory downgrades to SQLite when a requested backend was not
    // compiled in. Refuse to start instead: silently logging therapy data to an
    // unexpected SQLite file the user never opens is worse than not starting.
    if (config.database.type != "sqlite" && db->dbType() == hms_cpap::DbType::SQLITE) {
        std::cerr << "Database backend '" << config.database.type
                  << "' is not compiled into this build" << std::endl;
        return 1;
    }
    db->connect();

    // SDD-074: each machine's clock, before anything reads a night. D11: said
    // in the startup block's words.
    hms_cpap::machine_clocks::setPrimaryDevice(config.device_id);
    hms_cpap::machine_clocks::loadAll(*db);
    hms_cpap::machine_clocks::setDisplayHost(config.display_clock == "host");
    {
        const auto clocks = hms_cpap::machine_clocks::all();
        if (clocks.empty())
            std::cout << "  Machine clock:      same as this computer (zone database: "
                      << hms_cpap::zonedb::source() << ")" << std::endl;
        for (const auto& [device, setting] : clocks)
            std::cout << "  Machine clock:      " << device << " on " << setting << std::endl;
    }

    // SDD-034: a cpap_sessions created before the backends declared
    // UNIQUE (device_id, session_start) has no such key, and saveSession's
    // upsert needs it: without it a growing night is inserted again every
    // burst instead of being updated. Repair it here, beside the migrations,
    // so an install gains the key by being updated and restarted. It runs once:
    // every later start finds the key and returns without touching a row.
    if (const auto key = db->inspectSessionKey(); !key.key_present) {
        std::cerr << "cpap_sessions has no unique key on (device_id, session_start): "
                  << key.duplicate_groups << " night(s) stored twice or more, "
                  << key.duplicate_rows << " row(s) in them. Repairing (SDD-034)..."
                  << std::endl;
        const auto fixed = db->repairSessionKey();
        if (fixed.ok && fixed.key_added) {
            std::cerr << "cpap_sessions: collapsed " << fixed.groups << " night(s), removed "
                      << fixed.rows_deleted
                      << " duplicate row(s), unique key added. The removed copies were the "
                         "shorter ones; a Reparse rebuilds anything from the card files."
                      << std::endl;
        } else {
            std::cerr << "cpap_sessions: SDD-034 repair did not finish (removed "
                      << fixed.rows_deleted
                      << " row(s)); the duplicates stay and so does the missing key."
                      << std::endl;
        }
    }

    // SDD-051 steps 2 to 4: the rows, then Home Assistant's old device, then
    // the marker. Before anything else opens the database or the broker. Every
    // step is safe to repeat, so an interrupted move simply finishes next time.
    if (!config.device_id_previous.empty()) {
        const std::string old_id = config.device_id_previous;
        const int moved = db->moveDeviceId(old_id, config.device_id);
        if (moved < 0) {
            // Nothing moved. Go on under the old id rather than show an empty
            // dashboard over a full database; a later start tries again.
            std::cerr << "Config: could not move the nights from " << old_id << " to "
                      << config.device_id << "; staying on " << old_id << " for now" << std::endl;
            config.device_id = old_id;
            config.device_id_previous.clear();
            config.save(config_path);
            portableSetenv("CPAP_DEVICE_ID", config.device_id.c_str());
        } else {
            std::cout << "Config: device id " << old_id << " -> " << config.device_id << ", "
                      << moved << " session(s) moved" << std::endl;
            if (hms_cpap::noteDeviceIdMoved(config, moved)) config.save(config_path);
            bool cleared = true;
            if (!config.device_id_previous_published) {
                std::cout << "MQTT: this install never published as " << old_id
                          << "; its Home Assistant topics are left to whoever did" << std::endl;
            } else if (config.mqtt.enabled && !config.mqtt.broker.empty()) {
                hms::MqttConfig mc;
                mc.broker = config.mqtt.broker;
                mc.port = config.mqtt.port;
                mc.username = config.mqtt.username;
                mc.password = config.mqtt.password;
                mc.client_id = config.mqtt.client_id;
                cleared = hms_cpap::clearRetainedDevice(mc, old_id);
                if (!cleared)
                    std::cerr << "MQTT: broker not reachable; the old device " << old_id
                              << " is removed on a later start" << std::endl;
            }
            if (cleared) {
                config.device_id_previous.clear();
                config.save(config_path);
            }
        }
    }

    // Print config source
    std::cout << "Config: " << config_path << (config_existed ? "" : " (created)") << std::endl;
    std::cout << "Database: " << config.database.type;
    if (config.database.type == "sqlite") std::cout << " (" << config.database.sqlite_path << ")";
    else std::cout << " (" << config.database.host << "/" << config.database.name << ")";
    std::cout << std::endl;

    // ── Handle CLI modes ────────────────────────────────────────────
    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "--backfill") == 0) {
            if (i + 1 >= argc) {
                std::cerr << "Usage: hms_cpap --backfill <path/to/str.edf>" << std::endl;
                return 1;
            }
            return runBackfill(argv[i + 1]);
        }
        if (std::strcmp(argv[i], "--local-model-night") == 0) {
            if (i + 1 >= argc) {
                std::cerr << "Usage: hms_cpap --local-model-night <YYYYMMDD>" << std::endl;
                return 1;
            }
            return runLocalModelNight(config, config_path, argv[i + 1]);
        }
        if (std::strcmp(argv[i], "--reparse") == 0) {
            if (i + 1 >= argc) {
                std::cerr << "Usage: hms_cpap --reparse <card_root> <start_date> [end_date]" << std::endl;
                std::cerr << "  card_root: the SD card ROOT, the folder holding BOTH"
                          << " STR.edf and DATALOG, e.g. /mnt/public/cpap_data" << std::endl;
                std::cerr << "  dates: YYYY-MM-DD format" << std::endl;
                return 1;
            }
            std::string card_root = argv[i + 1];
            if (i + 2 >= argc) {
                std::cerr << "Usage: hms_cpap --reparse <card_root> <start_date> [end_date]" << std::endl;
                return 1;
            }
            std::string start_date = argv[i + 2];
            std::string end_date = (i + 3 < argc) ? argv[i + 3] : start_date;
            return runReparse(card_root, start_date, end_date);
        }
    }

    // Print banner
    printBanner();

    // Print configuration
    printConfiguration();

    // Register signal handlers
    std::signal(SIGINT, signalHandler);
#ifndef _WIN32
    std::signal(SIGTERM, signalHandler);
#else
    SetConsoleCtrlHandler(consoleCtrlHandler, TRUE);
#endif

    try {
        std::string src = hms_cpap::ConfigManager::get("CPAP_SOURCE", "ezshare");

        std::cout << "HMS-CPAP service is running..." << std::endl;

        // Data source: HTTP polling (ezShare) or local directory
        int burst_interval = hms_cpap::ConfigManager::getInt("BURST_INTERVAL", 120);

        burst_service = std::make_unique<hms_cpap::BurstCollectorService>(burst_interval);
        // #108: the AirSense 11 the pairing belongs to is saved into this file.
        burst_service->setConfigPath(config_path);
        burst_service->initialize(&config);
        // SDD-081: a DreamStation 2 card read by an OSCAR helper on a home
        // PC. It connects to the Sleep Bridge's port, so it is there while
        // that stream is; built before the first burst so the reader the
        // first encrypted card builds knows it. Every helper hello is a read.
        if (config.sleep_bridge.enabled) {
            hms_cpap::oscar::helper::Host::Options ho;
            ho.state_dir  = (std::filesystem::path(data_dir) / "oscar-helper").string();
            ho.install_id = hms_cpap::CpapDashStreamer::loadOrCreateInstallId(data_dir);
            ho.version    = HMS_CPAP_VERSION;
            ho.name       = hms_cpap::oscar::helper::hostName();
            ho.on_hello   = [](const std::string& why) {
                if (burst_service) burst_service->requestOscarRead(why);
            };
            oscar_helper_host = std::make_shared<hms_cpap::oscar::helper::Host>(ho);
            burst_service->setOscarRemote(oscar_helper_host,
                                          (std::filesystem::path(ho.state_dir) / "slice").string());
            // D3.1: advertised where this process can reach the LAN's mDNS
            // (native, host network); inside a container's own network it
            // reaches nobody and the helper finds the host by its probe.
            mdns_advertiser = std::make_unique<hms_cpap::oscar::helper::Advertiser>(
                ho.name, config.sleep_bridge.port, ho.install_id, ho.version);
            mdns_advertiser->start();
        }
        burst_service->start();

        // The night, live, on the house's MQTT: the mask and the events, the
        // same topics and Home Assistant entities from every source (a Sleep
        // Bridge, a Mule & Miner, the AirSense 11 over Bluetooth, a card read
        // by the bursts). mqtt/LiveState.h. Its own client, as the Sleep
        // Bridge's was: MQTT on but the client not connected, the message is
        // dropped and the log says so.
        // #94 follow-up: the device's name in Home Assistant, by the rule the
        // data publisher announces it with, so the two never disagree.
        const std::string ha_device_name = hms_cpap::machineName(
            config.device_name,
            hms_cpap::installIdentity(config.legacySource() == "local" ? config.local_dir : "",
                                      config.archive_dir, hms_cpap::AppConfig::dataDir()));
        std::shared_ptr<hms_cpap::LiveState> live_state;
        if (config.mqtt.enabled) {
            hms::MqttConfig live_cfg;
            live_cfg.broker    = config.mqtt.broker;
            live_cfg.port      = config.mqtt.port;
            live_cfg.username  = config.mqtt.username;
            live_cfg.password  = config.mqtt.password;
            live_cfg.client_id = config.mqtt.client_id + "_live";
            auto live_mqtt = std::make_shared<hms::MqttClient>(live_cfg);
            live_mqtt->connect();
            live_state = std::make_shared<hms_cpap::LiveState>(
                [live_mqtt](const std::string& topic, const std::string& payload, int qos, bool retain) {
                    if (!live_mqtt->isConnected()) {
                        std::cout << "[live] MQTT not connected, dropped " << topic << " "
                                  << payload.substr(0, 120) << std::endl;
                        return false;
                    }
                    return live_mqtt->publish(topic, payload, qos, retain);
                },
                config.device_id, ha_device_name);
            live_state->announce();
            burst_service->setLiveState(live_state);
        }

        if (src == "local") {
            std::cout << "   Source: Local directory at " << hms_cpap::ConfigManager::get("CPAP_LOCAL_DIR", "") << std::endl;
        } else {
            std::cout << "   Source: HTTP at " << hms_cpap::ConfigManager::get("EZSHARE_BASE_URL", "http://192.168.4.1") << std::endl;
        }
        std::cout << "   Burst interval: " << burst_interval << " seconds" << std::endl;

        std::cout << "   Press Ctrl+C to stop" << std::endl << std::endl;

        // Start ML Training module if enabled
        if (config.ml_training.enabled) {
            hms_cpap::MLTrainingService::Config ml_cfg;
            ml_cfg.enabled = true;
            ml_cfg.schedule = config.ml_training.schedule;
            ml_cfg.min_days = config.ml_training.min_days;
            ml_cfg.max_training_days = config.ml_training.max_training_days;
            ml_cfg.device_id = config.device_id;

            // Determine model directory
            if (!config.ml_training.model_dir.empty()) {
                ml_cfg.model_dir = config.ml_training.model_dir;
            } else {
                std::string data_dir = std::getenv("HOME") ? std::string(std::getenv("HOME")) + "/.hms-cpap" : ".";
                ml_cfg.model_dir = data_dir + "/models";
            }

            // Separate DB connection for ML (client libs are not thread-safe)
            std::shared_ptr<hms_cpap::IDatabase> ml_db;
            if (config.database.type == "sqlite") {
                ml_db = db;
            } else {
                ml_db = hms_cpap::makeDatabaseFromConfig();
                ml_db->connect();
            }

            // Create ML MQTT client (separate from burst service)
            std::shared_ptr<hms::MqttClient> ml_mqtt;
            if (config.mqtt.enabled) {
                hms::MqttConfig ml_mqtt_cfg;
                ml_mqtt_cfg.broker = config.mqtt.broker;
                ml_mqtt_cfg.port = config.mqtt.port;
                ml_mqtt_cfg.username = config.mqtt.username;
                ml_mqtt_cfg.password = config.mqtt.password;
                ml_mqtt_cfg.client_id = config.mqtt.client_id + "_ml";
                ml_mqtt = std::make_shared<hms::MqttClient>(ml_mqtt_cfg);
                ml_mqtt->connect();
            }

            ml_service = std::make_unique<hms_cpap::MLTrainingService>(ml_cfg, ml_db, ml_mqtt);
            ml_service->start();

            std::cout << "ML Training: enabled (schedule: " << config.ml_training.schedule
                      << ", min_days: " << config.ml_training.min_days << ")" << std::endl;
        }

        // Start Agent module if enabled. SDD-072 D4: on every engine; its
        // tools read through IDatabase.
        std::string agent_enabled = hms_cpap::ConfigManager::get("AGENT_ENABLED", "false");
        if (agent_enabled == "true" || agent_enabled == "1") {
            std::string agent_provider = hms_cpap::ConfigManager::get("AGENT_LLM_PROVIDER", "ollama");
            std::string agent_endpoint = hms_cpap::ConfigManager::get("AGENT_LLM_ENDPOINT", "http://127.0.0.1:11434");
            std::string agent_model = hms_cpap::ConfigManager::get("AGENT_LLM_MODEL", "gpt-oss:120b-cloud");
            std::string agent_api_key = hms_cpap::ConfigManager::get("AGENT_LLM_API_KEY", "");
            std::string agent_embed = hms_cpap::ConfigManager::get("AGENT_EMBED_MODEL", "nomic-embed-text");
            double agent_temp = std::stod(hms_cpap::ConfigManager::get("AGENT_LLM_TEMPERATURE", "0.3"));
            int agent_max_tokens = hms_cpap::ConfigManager::getInt("AGENT_LLM_MAX_TOKENS", 2048);

            hms::LLMConfig llm_config;
            llm_config.enabled = true;
            llm_config.provider = hms::LLMClient::parseProvider(agent_provider);
            llm_config.endpoint = agent_endpoint;
            llm_config.model = agent_model;
            llm_config.api_key = agent_api_key;
            llm_config.temperature = agent_temp;
            llm_config.max_tokens = agent_max_tokens;

            auto llm_client = std::make_shared<hms::LLMClient>(llm_config);

            // Embeddings always go to Ollama (nomic-embed-text is local)
            std::shared_ptr<hms_cpap::AgentLLM> agent_llm;
            std::string embed_endpoint = hms_cpap::ConfigManager::get("AGENT_EMBED_ENDPOINT", "http://127.0.0.1:11434");
            if (llm_config.provider != hms::LLMProvider::OLLAMA) {
                hms::LLMConfig embed_config;
                embed_config.enabled = true;
                embed_config.provider = hms::LLMProvider::OLLAMA;
                embed_config.endpoint = embed_endpoint;
                embed_config.model = agent_embed;
                auto embed_client = std::make_shared<hms::LLMClient>(embed_config);
                agent_llm = std::make_shared<hms_cpap::AgentLLM>(llm_client, embed_client, agent_embed);
            } else {
                agent_llm = std::make_shared<hms_cpap::AgentLLM>(llm_client, agent_embed);
            }

            // The tools' own connection (client libraries are not thread-safe),
            // as the ML service does; SQLite shares the service's.
            std::shared_ptr<hms_cpap::IDatabase> agent_db;
            if (config.database.type == "sqlite") {
                agent_db = db;
            } else {
                agent_db = hms_cpap::makeDatabaseFromConfig();
                agent_db->connect();
            }

            // The pgvector memory's connection: PostgreSQL only.
            std::string a_conn_str;
            if (agent_db->dbType() == hms_cpap::DbType::POSTGRESQL) {
                a_conn_str = "host=" + hms_cpap::ConfigManager::get("DB_HOST", "localhost") +
                             " port=" + hms_cpap::ConfigManager::get("DB_PORT", "5432") +
                             " dbname=" + hms_cpap::ConfigManager::get("DB_NAME", "cpap_monitoring") +
                             " user=" + hms_cpap::ConfigManager::get("DB_USER", "maestro") +
                             " password=" + hms_cpap::ConfigManager::get("DB_PASSWORD", "");
            }

            std::string device_id = hms_cpap::ConfigManager::get("CPAP_DEVICE_ID", hms_cpap::AppConfig::kDefaultDeviceId);

            hms_cpap::AgentService::Config agent_cfg;
            agent_cfg.device_id = device_id;
            agent_cfg.db_connection_string = a_conn_str;
            agent_cfg.embed_model = agent_embed;
            agent_cfg.temperature = agent_temp;
            agent_cfg.max_iterations = hms_cpap::ConfigManager::getInt("AGENT_MAX_ITERATIONS", 5);
            agent_cfg.max_context = hms_cpap::ConfigManager::getInt("AGENT_MAX_CONTEXT", 20);
            agent_cfg.memory_limit = hms_cpap::ConfigManager::getInt("AGENT_MEMORY_LIMIT", 3);

            auto agent_mqtt = std::make_shared<hms::MqttClient>(hms_cpap::agentMqttConfig());
            agent_mqtt->connect();

            agent_service = std::make_unique<hms_cpap::AgentService>(agent_cfg, agent_mqtt,
                                                                     agent_llm, agent_db);
            agent_service->start();

            std::cout << "Agent: AI module enabled (model: " << agent_model << ")" << std::endl;
        }

#ifdef BUILD_WITH_WEB
        // Start web UI server (Drogon blocks until shutdown)
        {
            int web_port = config.web_port;
            // SDD-006: the shipped layout puts the binary and static/browser
            // side by side, but the historical default was CWD-relative, so
            // double-clicking the binary from anywhere else served no UI.
            std::string static_dir =
                hms_cpap::SetupService::resolveStaticDir(config.static_dir);
            if (static_dir != config.static_dir) {
                std::cout << "Web UI: serving from " << static_dir << std::endl;
            }

            // Separate DB connection for web queries (neither pqxx nor the
            // MySQL client is thread-safe). SQLite does its own locking, so it
            // shares the collector's handle.
            std::shared_ptr<hms_cpap::IDatabase> web_db;
            if (config.database.type == "sqlite") {
                web_db = db;
            } else {
                web_db = hms_cpap::makeDatabaseFromConfig();
                web_db->connect();
            }
            auto query_service = std::make_shared<hms_cpap::QueryService>(
                web_db ? web_db : db, config.device_id);
            hms_cpap::CpapController::setQueryService(query_service);
            hms_cpap::CpapController::setConfig(&config, config_path);
            hms_cpap::CpapController::setBurstService(burst_service.get());

            // SDD-004 equipment profiles + supplies. Uses the same connection the
            // web layer uses (web_db when the backend needs a separate one).
            hms_cpap::EquipmentController::setDatabase(web_db ? web_db : db);
            // SDD-007: cleaning schedules share the web connection for the same
            // thread-safety reason the equipment controller does.
            hms_cpap::CleaningController::setDatabase(web_db ? web_db : db);

            // Optional cloud mirror. Constructed even when disabled so the route can
            // answer 409 ("disabled") instead of 404 — a user pressing Sync deserves
            // to be told why nothing happened. enabled() gates all network work.
            {
                auto sync = std::make_shared<hms_cpap::CpapDashSyncService>();
                sync->setDatabase(web_db ? web_db : db);
                sync->setSettings(hms_cpap::CpapDashSyncService::Settings::from(
                    config.cpapdash.enabled, config.cpapdash.api_url,
                    config.cpapdash.token, config.cpapdash.auto_sync));
                hms_cpap::EquipmentController::setSyncService(sync);
                // SDD-006 section 5: PUT /api/config must re-apply these to the
                // LIVE service, not just to the file.
                hms_cpap::CpapController::setSyncService(sync);
                // SDD-007: cleaning edits make the mirror stale too.
                hms_cpap::CleaningController::setSyncService(sync);
                // Same instance drives the burst-loop sweep, so a UI edit marked
                // dirty and the periodic auto_sync share one cursor and one lock.
                if (burst_service) burst_service->setCpapDashSync(sync);
                if (config.cpapdash.enabled && config.cpapdash.token.empty())
                    std::cout << "CpapDash sync: enabled but no token set - staying local-only"
                              << std::endl;
            }

            // platform SDD-059: "Send my nights to CpapDash". One uploader, fed
            // by every collector through card_feed once a piece is on disk; it
            // talks to wss://api.cpapdash.com/socket/v1/hms only. Built even
            // with the switch off, so turning it on in Settings needs no
            // restart; off, it holds no connection and sends nothing.
            std::shared_ptr<hms_cpap::CpapDashStreamer> cpapdash_stream;
            {
                // Its own connection on a server database: it runs on its own
                // thread, as the web layer's does.
                std::shared_ptr<hms_cpap::IDatabase> stream_db = db;
                if (config.database.type != "sqlite") {
                    stream_db = hms_cpap::makeDatabaseFromConfig();
                    stream_db->connect();
                }
                const std::string data_dir = hms_cpap::AppConfig::dataDir();
                cpapdash_stream = std::make_shared<hms_cpap::CpapDashStreamer>(
                    stream_db,
                    [] { return hms_cpap::makeCpapDashLink(hms_cpap::CpapDashEndpoint{}); },
                    hms_cpap::CpapDashStreamer::loadOrCreateInstallId(data_dir), HMS_CPAP_VERSION);
                cpapdash_stream->setRootsProvider([data_dir] {
                    // Every card root this install holds (D8): the ez Share
                    // archive, a local folder read in place, and the Bluetooth
                    // and Sleep Bridge roots under the data folder.
                    const std::string archive = hms_cpap::ConfigManager::get(
                        "CPAP_ARCHIVE_DIR",
                        (std::filesystem::path(data_dir) / "cpap_data").string());
                    const std::string local = hms_cpap::ConfigManager::get("CPAP_LOCAL_DIR", "");
                    return hms_cpap::CpapDashStreamer::discoverRoots(data_dir, {archive, local});
                });
                // The one source behind this install, said on every hello:
                // ingest closes a night by it.
                cpapdash_stream->setSourceProvider([&config, data_dir] {
                    return hms_cpap::cdstream::sourceKind(
                        config.airsense11_ble.enabled || config.ds2_ble.enabled ||
                            config.airsense11.enabled,
                        config.sleep_bridge.enabled, data_dir,
                        config.transport == "local" ? config.local_dir : std::string());
                });
                // SDD-079 D4: the oximetry outbox, written at every oximetry
                // store; D7: recordings stored before 7.0.0 join it on every
                // walk.
                hms_cpap::oximetry_outbox::setRoot(hms_cpap::oximetry_outbox::rootFor(data_dir));
                cpapdash_stream->setOximetryBackfill([stream_db, data_dir] {
                    const std::string archive = hms_cpap::ConfigManager::get(
                        "CPAP_ARCHIVE_DIR",
                        (std::filesystem::path(data_dir) / "cpap_data").string());
                    hms_cpap::oximetry_outbox::backfill(*stream_db, archive);
                });
                cpapdash_stream->apply({config.cpapdash.stream, config.cpapdash.token});
                hms_cpap::card_feed::setSinks(
                    [s = cpapdash_stream](const std::string& root, const std::string& rel,
                                          const std::string& machine, const std::string& source) {
                        s->fileWritten(root, rel, machine, source);
                    },
                    [s = cpapdash_stream](const std::string& root, const std::string& machine,
                                          const std::string& source) {
                        s->rootWritten(root, machine, source);
                    },
                    [s = cpapdash_stream](const std::string& root) { s->burstEnded(root); });
                hms_cpap::CpapController::setStreamService(cpapdash_stream);
                // SDD-073 D6: "Connected to CpapDash" as it happens.
                cpapdash_stream->setStateListener([](const hms_cpap::CpapDashStreamer::Status& st) {
                    hms_cpap::SetupWatch::streamEvent(st.toJson());
                });
                hms_cpap::SetupWatch::setStreamSource(
                    [w = std::weak_ptr<hms_cpap::CpapDashStreamer>(cpapdash_stream)] {
                        if (auto s = w.lock()) return s->status().toJson();
                        Json::Value v(Json::objectValue);
                        v["state"] = "unavailable";
                        return v;
                    });
                cpapdash_stream->start();
                if (config.cpapdash.stream && config.cpapdash.token.empty())
                    std::cout << "CpapDash nights: switched on but no token set - nothing is sent"
                              << std::endl;
            }

            // SDD-078: Garmin SpO2 as an oximetry source. Built even when it is
            // off, so turning it on in Settings needs no restart. Asked only on
            // events (D2): a night's close, the dashboard or night list being
            // opened, the Devices page's button, the settings saved.
            std::shared_ptr<hms_cpap::GarminService> garmin;
            {
                std::shared_ptr<hms_cpap::IDatabase> garmin_db = db;
                if (config.database.type != "sqlite") {
                    garmin_db = hms_cpap::makeDatabaseFromConfig();
                    garmin_db->connect();
                }
                hms_cpap::GarminService::Deps gd;
                gd.db             = garmin_db;
                gd.config         = &config;
                gd.config_path    = config_path;
                gd.cpap_device_id = config.device_id;
                garmin = std::make_shared<hms_cpap::GarminService>(std::move(gd));
                garmin->start();
                hms_cpap::CpapController::setGarminService(garmin);
                if (burst_service)
                    burst_service->addNightClosedListener(
                        [g = garmin](const std::string& folder) { g->onNightClosed(folder); });
            }

            // Issue #96: an open dashboard hears a night go live (or settle)
            // from the burst that saw it, on the watch socket.
            if (burst_service)
                burst_service->addLiveNightListener([](bool live) { hms_cpap::SetupWatch::nightEvent(live); });

            // SDD-013: the local sleep model's nightly job. Built even when it
            // is off, so the Settings switch needs no restart. It runs only
            // when a night lands (D-701-3: the SDD-046 close, an upload, a
            // re-read, an import), never on a clock.
            std::shared_ptr<hms_cpap::model::LocalModelService> local_model;
            {
                std::shared_ptr<hms_cpap::IDatabase> lm_db = db;
                if (config.database.type != "sqlite") {
                    lm_db = hms_cpap::makeDatabaseFromConfig();
                    lm_db->connect();
                }
                hms_cpap::model::LocalModelService::Deps ld;
                ld.db          = lm_db;
                ld.config      = &config;
                ld.config_path = config_path;
                ld.device_id   = hms_cpap::ConfigManager::get(
                    "CPAP_DEVICE_ID", hms_cpap::AppConfig::kDefaultDeviceId);
                local_model = std::make_shared<hms_cpap::model::LocalModelService>(std::move(ld));
                // An open Settings page hears the model put in place (or
                // refused) on the watch socket.
                local_model->setStateListener(
                    [](const Json::Value& st) { hms_cpap::SetupWatch::localModelEvent(st); });
                local_model->start();
                hms_cpap::model::installRoutes(local_model.get());
                // Every path that stores a night says so (services/NightLanded.h),
                // with the night's STR row when it has one, so the model is
                // given the night the daily summary is.
                hms_cpap::night_landed::listen(
                    [m = local_model](const std::string& folder, const std::optional<hms_cpap::STRDailyRecord>& str) {
                        m->onNightClosed(folder, str);
                    });
            }

            // Issue #74: the configured LLM summarizes a summary-only night (no
            // session files; myAir's or the STR's daily record) when it lands:
            // myAir data stored, or an STR-only night closed. The listener only
            // queues; a night with sessions or no LLM is passed over at once,
            // and one written before from the same facts is skipped.
            auto daily_only_queue = std::make_shared<hms_cpap::daily_only::Queue>(
                [](const std::string& day) {
                    if (burst_service) burst_service->summarizeDailyOnlyNight(day);
                });
            daily_only_queue->start();
            hms_cpap::night_landed::listen(
                [q = daily_only_queue](const std::string& folder, const std::optional<hms_cpap::STRDailyRecord>&) {
                    q->enqueue(folder);
                });

            // SDD-041: check GitHub for a newer stable release, daily and on
            // demand. It only reports; the supervisor applies. Off by itself in a
            // container (D5), where the image is what updates.
            auto update_service = std::make_shared<hms_cpap::UpdateService>(HMS_CPAP_VERSION);
            update_service->setDataDir(hms_cpap::AppConfig::dataDir());
            {
                hms_cpap::UpdateService::Hooks hooks;
                // §3.5: a night still arriving means not now. Twenty minutes
                // covers a burst interval with room to spare.
                hooks.busy = [svc = burst_service.get()]() {
                    return svc && svc->dataArrivedWithin(std::chrono::minutes(20));
                };
                // D6: SQLite is copied before any update; a server database is
                // named in Settings instead and its rollback stays the user's.
                hooks.backup = [&config](const std::string& dir) -> std::string {
                    if (config.database.type != "sqlite") return "";
                    return hms_cpap::update::backupSqlite(
                        config.database.sqlite_path, dir + "/backup-" HMS_CPAP_VERSION ".db");
                };
                hooks.auto_update = [&config]() { return config.auto_update; };
                hooks.database = [&config]() { return config.database.type; };
                // Leave the way a signal does, and let main return the code.
                hooks.exit = [](int code) {
                    std::this_thread::sleep_for(std::chrono::milliseconds(750));
                    requested_exit_code = code;
                    requestShutdown();
                };
                update_service->setHooks(std::move(hooks));
            }
            hms_cpap::CpapController::setUpdateService(update_service);
            update_service->start();
            hms_cpap::CpapController::shutdown_with_code_ = [](int code) {
                requested_exit_code = code;
                requestShutdown();
            };

            // SDD-020: opt-in read-only pull of the same nights from ResMed's
            // myAir, so their score can sit next to ours on the dashboard.
            //
            // Constructed only when it is actually configured. Unlike the cloud
            // mirror above there is no route that needs to answer "disabled",
            // because the dashboard asks /api/capabilities first and simply does
            // not draw the panel.
            //
            // config_path is passed so the remembered-device token survives a
            // restart; without it a region with an email factor would ask for a
            // fresh code every start and a headless service cannot answer one.
            // Constructed even when disabled, for the same reason the cloud
            // mirror above is: the settings page has to be able to CONNECT an
            // account that is not configured yet, and a route that 404s cannot.
            // sweep() self-gates on enabled(), so an unconfigured instance costs
            // one null check per burst.
            {
                auto myair = std::make_shared<hms_cpap::MyAirService>(
                    web_db ? web_db : db, config, config_path);
                hms_cpap::CpapController::setMyAirService(myair);
                if (burst_service) burst_service->setMyAirService(myair);

                if (myair->enabled()) {
                    std::cout << "myAir: enabled (" << config.myair.region
                              << "), polling every " << config.myair.poll_minutes
                              << " minutes" << std::endl;
                } else if (config.myair.enabled) {
                    std::cout << "myAir: enabled but not connected - sign in from "
                                 "the settings page" << std::endl;
                }
            }

            // Wire report generator (PDF/gnuplot not available on Windows)
#ifndef _WIN32
            {
                std::string archive_dir = hms_cpap::ConfigManager::get("CPAP_ARCHIVE_DIR",
                    hms_cpap::AppConfig::dataDir());
                // Own connection for the same thread-safety reason as web_db.
                std::shared_ptr<hms_cpap::IDatabase> rpt_db;
                if (config.database.type == "sqlite") {
                    rpt_db = db;
                } else {
                    rpt_db = hms_cpap::makeDatabaseFromConfig();
                    rpt_db->connect();
                }

                std::shared_ptr<hms::MqttClient> rpt_mqtt;
                if (config.mqtt.enabled) {
                    hms::MqttConfig rpt_cfg;
                    rpt_cfg.broker   = config.mqtt.broker;
                    rpt_cfg.port     = config.mqtt.port;
                    rpt_cfg.username = config.mqtt.username;
                    rpt_cfg.password = config.mqtt.password;
                    rpt_cfg.client_id = config.mqtt.client_id + "_rpt";
                    rpt_mqtt = std::make_shared<hms::MqttClient>(rpt_cfg);
                    rpt_mqtt->connect();
                }

                // Resolved, not config.static_dir: the logo lives beside the
                // Angular bundle, so it has to follow the same relocation.
                std::string logo_path = static_dir + "/logo.png";
                if (!std::filesystem::exists(logo_path)) logo_path = "";

                auto report_svc = std::make_shared<hms_cpap::ReportGeneratorService>(
                    rpt_db, query_service, rpt_mqtt, config.device_id, archive_dir, logo_path);
                hms_cpap::CpapController::setReportService(report_svc);
            }
#endif // _WIN32

            // Wire ML service triggers to controller
            if (ml_service) {
                hms_cpap::CpapController::ml_train_trigger_ = [&]() {
                    ml_service->triggerTraining();
                };
                hms_cpap::CpapController::ml_status_getter_ = [&]() -> Json::Value {
                    return ml_service->getStatus();
                };
            }

            // Wire O2 Ring CSV upload: parse a Wellue export server-side and
            // store it under the same "o2ring" device the BLE path uses.
            {
                std::shared_ptr<hms_cpap::IDatabase> oxi_db = web_db ? web_db : db;
                hms_cpap::CpapController::oxi_csv_import_ =
                    [oxi_db](const std::string& content, const std::string& filename,
                             const std::string& source, const std::string& device_name) -> Json::Value {
                        // SDD-078 D3: stored with where it came from ("upload"
                        // unless the sender names a source).
                        return hms_cpap::importOximetryUpload(
                            *oxi_db, content, filename, hms_cpap::uploadOrigin(source, device_name));
                    };
            }

            // SDD-029 (#31): Remove night on the sessions page, and the Reparse
            // that restores it. The record they share is what every re-ingest
            // path consults, so a removed night stays removed.
            {
                std::shared_ptr<hms_cpap::IDatabase> night_db = web_db ? web_db : db;
                const std::string night_device = config.device_id;
                hms_cpap::CpapController::night_remove_ =
                    [night_db, night_device](const std::string& date) -> Json::Value {
                        Json::Value r;
                        const auto res = hms_cpap::removeNightByDate(*night_db, night_device, date);
                        if (!res.ok) {
                            r["error"] = "Failed to remove the night of " + date;
                            return r;
                        }
                        std::cout << "Sessions: removed night " << date << " (" << res.sessions
                                  << " session(s), " << res.oximetry << " oximetry)" << std::endl;
                        Json::Value& n = r["removed"];
                        n["sessions"]  = res.sessions;
                        n["daily"]     = res.daily;
                        n["oximetry"]  = res.oximetry;
                        n["ledger"]    = res.ledger;
                        n["summaries"] = res.summaries;
                        return r;
                    };
                hms_cpap::CpapController::night_restore_ =
                    [night_db, night_device](const std::string& date) {
                        if (hms_cpap::restoreNightByDate(*night_db, night_device, date))
                            std::cout << "Sessions: restored removed night " << date
                                      << " for reparse" << std::endl;
                    };
                // SDD-036: what the sessions page offers to restore.
                hms_cpap::CpapController::removed_nights_ = [night_db, night_device]() {
                    return hms_cpap::removedNightDates(*night_db, night_device);
                };
            }

            // Wire BackfillService whenever an archive/local DATALOG path is
            // known. It reparses from the permanent archive, so it must be
            // available in every source mode (ezshare/local): the
            // per-session UI reparse delegates to it. The card root is the
            // local folder in local mode and the archive the collector fills
            // in the others: keying this on local_dir alone left Reparse (and
            // SDD-029's restore) answering 503 on every ezShare install.
            const std::string card_root =
                !config.local_dir.empty() ? config.local_dir : config.archive_dir;
            if (!card_root.empty()) {
                hms_cpap::BackfillService::Config bf_cfg;
                bf_cfg.device_id = config.device_id;
                bf_cfg.device_name = config.device_name;
                bf_cfg.local_dir = card_root;
                bf_cfg.sleephq.enabled = config.sleephq.enabled;
                bf_cfg.sleephq.auto_on_backfill = config.sleephq.auto_on_backfill;

                // Separate DB connection (client libs not thread-safe)
                std::shared_ptr<hms_cpap::IDatabase> bf_db;
                if (config.database.type == "sqlite") {
                    bf_db = db;
                } else {
                    bf_db = hms_cpap::makeDatabaseFromConfig();
                    bf_db->connect();
                }

                backfill_service = std::make_unique<hms_cpap::BackfillService>(
                    bf_cfg, bf_db);
                // SDD-048: a re-parse reaches Home Assistant. The collector owns
                // the publisher; the backfill only raises its flag.
                backfill_service->setOnSessionsSaved([] {
                    if (burst_service) burst_service->requestRepublish();
                });
                backfill_service->start();

                hms_cpap::CpapController::backfill_trigger_ =
                    [&](const std::string& start, const std::string& end, const std::string& local_dir) {
                        backfill_service->trigger(start, end, local_dir);
                    };
                hms_cpap::CpapController::backfill_status_getter_ = [&]() -> Json::Value {
                    return backfill_service->getStatus();
                };

                // Wire CPAP zip upload: extract the SD card's DATALOG date
                // folders into the archive, then reparse them via backfill.
                // SDD-010: config.local_dir is the card ROOT, so extracted date
                // folders belong under its DATALOG, not directly inside it.
                std::string archive_dir = hms_cpap::datalogDirFor(card_root);
                std::string card_dir = card_root;   // the card ROOT (SDD-010)
                // SDD-036 D2: an uploaded card restores the removed nights it holds.
                std::shared_ptr<hms_cpap::IDatabase> upload_db = web_db ? web_db : db;
                const std::string upload_device = config.device_id;
                auto restoreUploaded = [upload_db, upload_device](const std::set<std::string>& nights,
                                                                  Json::Value& r) {
                    Json::Value arr(Json::arrayValue);
                    for (const auto& d : hms_cpap::restoreUploadedNights(*upload_db, upload_device, nights)) {
                        std::cout << "Upload: restored removed night " << d << std::endl;
                        arr.append(d);
                    }
                    r["restored_nights"] = arr;
                };
                // SDD-039 D2: the handler hands the zip here, and the worker
                // runs the importer wired just below.
                // The importer's reply is kept in /api/backfill/status under the
                // id the upload's reply carries (issue #55), so the page shows
                // what became of the zip instead of only the log saying it.
                hms_cpap::CpapController::cpap_zip_queue_ =
                    [](const std::string& zip_path) -> uint64_t {
                        if (!backfill_service) return 0;
                        return backfill_service->triggerZipImport(
                            zip_path, [](const std::string& path) {
                                Json::Value r = hms_cpap::CpapController::cpap_zip_import_
                                                    ? hms_cpap::CpapController::cpap_zip_import_(path)
                                                    : Json::Value();
                                if (r.isMember("error"))
                                    std::cerr << "Upload: " << r["error"].asString() << std::endl;
                                std::error_code ec;
                                std::filesystem::remove(path, ec);
                                return r;
                            });
                    };
                hms_cpap::CpapController::cpap_zip_import_ =
                    [archive_dir, card_dir, restoreUploaded](const std::string& zip_path) -> Json::Value {
                        namespace fs = std::filesystem;
                        Json::Value r;
                        std::error_code ec;
                        fs::path staging = fs::temp_directory_path() /
                            ("cpap_stage_" +
                             std::to_string(std::chrono::steady_clock::now()
                                                .time_since_epoch().count()));
                        fs::create_directories(staging, ec);
                        if (!hms_cpap::PrismaIngestion::extractZip(zip_path, staging.string())) {
                            fs::remove_all(staging, ec);
                            r["error"] = "Could not read zip archive";
                            return r;
                        }
                        // SDD-061: a .cpapcard's layout went to the Sleep Bridge
                        // when it was uploaded; it is not a file of the card.
                        hms_cpap::removeCardLayout(staging.string());
                        // SDD-083: a DreamStation 2 card whose files are
                        // encrypted is read by the OSCAR the user paired, the
                        // read its card from a Sleep Bridge Lite or a folder
                        // gets; with none paired it is refused and nothing is
                        // kept. hms-cpap never decrypts it.
                        if (const auto ds2 = hms_cpap::encryptedPhilipsCardIn(staging.string())) {
                            if (!burst_service || !burst_service->oscarSourcePaired()) {
                                fs::remove_all(staging, ec);
                                return hms_cpap::philipsUploadRefusal(
                                    hms_cpap::SetupService::isContainerised());
                            }
                            // Apart from a plain card's uploads/philips (D10).
                            const std::string store =
                                hms_cpap::AppConfig::dataDir() + "/uploads/philips-encrypted";
                            std::string keep_error;
                            const bool kept = hms_cpap::keepPhilipsUpload(*ds2, store, keep_error);
                            fs::remove_all(staging, ec);
                            if (!kept) {
                                r["error"] = keep_error;
                                return r;
                            }
                            std::cout << "Upload: an encrypted DreamStation 2 card, read through OSCAR"
                                      << std::endl;
                            const auto read = burst_service->readUploadedOscarCard(store);
                            if (!read) return hms_cpap::philipsUploadRefusal(
                                hms_cpap::SetupService::isContainerised());
                            return hms_cpap::philipsUploadReply(*read, burst_service->oscarHelperPaired());
                        }
                        // SDD-031 (#28): read the card it was given. A Sefam or
                        // Löwenstein card is kept under uploads/<format>/ (D2) and
                        // every session the database lacks is imported on the
                        // backfill worker (D3), which the upload page already polls.
                        // SDD-083 D10: a DreamStation card whose files are plain
                        // is read as a plain card in a folder is.
                        const auto plain_ds = hms_cpap::plainPhilipsCardIn(staging.string());
                        const auto kind = plain_ds ? hms_cpap::UploadedCard::Philips
                                                   : hms_cpap::classifyUploadedCard(staging.string());
                        if (kind == hms_cpap::UploadedCard::Sefam ||
                            kind == hms_cpap::UploadedCard::Lowenstein ||
                            kind == hms_cpap::UploadedCard::Bmc ||        // SDD-049
                            kind == hms_cpap::UploadedCard::Philips) {    // SDD-083 D10
                            const std::string fmt = hms_cpap::uploadedCardName(kind);
                            const std::string store =
                                hms_cpap::AppConfig::dataDir() + "/uploads/" + fmt;
                            // SDD-083 D11: the nights are read BEFORE anything is
                            // kept, so a refusal leaves the store as it was.
                            std::set<std::string> nights;
                            std::string merge_error;
                            fs::path ready = staging;   // what goes into the store
                            if (kind == hms_cpap::UploadedCard::Sefam ||
                                kind == hms_cpap::UploadedCard::Bmc) {
                                nights = hms_cpap::cardNights(kind, staging.string());
                            } else if (kind == hms_cpap::UploadedCard::Philips) {
                                nights = hms_cpap::cardNights(kind, *plain_ds);
                            } else {
                                // A .pdat is opened into its own folder, never kept
                                // as one (CardUpload.h says why); the nights are
                                // this upload's, not the whole store's. Opened
                                // beside the staging, merged only once it holds one.
                                ready = staging.string() + "_lowenstein";
                                const auto roots = hms_cpap::prepareLowensteinUpload(
                                    staging.string(), ready.string(), merge_error);
                                for (const auto& root : roots)
                                    for (const auto& n : hms_cpap::cardNights(kind, root))
                                        nights.insert(n);
                                if (roots.empty() && merge_error.empty())
                                    merge_error = "No Löwenstein sessions found in zip";
                            }
                            bool merged = false;
                            if (!nights.empty()) {
                                merged = kind == hms_cpap::UploadedCard::Philips
                                    ? hms_cpap::keepPhilipsUpload(*plain_ds, store, merge_error)
                                    : hms_cpap::mergeCardInto(ready.string(), store, merge_error);
                            }
                            fs::remove_all(staging, ec);
                            if (ready != staging) fs::remove_all(ready, ec);
                            if (nights.empty()) {
                                r["error"] = !merge_error.empty()
                                    ? merge_error
                                    : "The zip looks like a " + fmt +
                                          " card but holds no session this build can read";
                                return r;
                            }
                            if (!merged) {
                                r["error"] = merge_error;
                                return r;
                            }
                            restoreUploaded(nights, r);
                            backfill_service->triggerCardImport(kind, store);
                            Json::Value arr(Json::arrayValue);
                            for (const auto& d : nights) arr.append(d);
                            r["status"]         = "queued";
                            r["format"]         = fmt;
                            r["sessions_found"] = (int)nights.size();
                            r["dates"]          = arr;
                            r["message"]        = "Card kept; importing the nights not yet "
                                                  "stored. Poll /api/backfill/status";
                            return r;
                        }
                        // Mirror the card into the archive: date folders under
                        // DATALOG, everything else at its own path from the card
                        // root, filtered only by residualSkip. See SDD-014 and
                        // utils/CardImport.h for why this is not a slice.
                        auto imported = hms_cpap::mirrorCardInto(
                            staging.string(), card_dir, archive_dir);
                        const auto& dates = imported.dates;

                        fs::remove_all(staging, ec);
                        if (dates.empty()) {
                            r["error"] = "Not a card this build reads: no ResMed DATALOG "
                                         "date folders (YYYYMMDD), no Sefam S.Box sessions, "
                                         "no Löwenstein Prisma data, no BMC card and no "
                                         "DreamStation card in the zip";
                            return r;
                        }
                        auto dash = [](const std::string& d) {
                            return d.substr(0, 4) + "-" + d.substr(4, 2) + "-" + d.substr(6, 2);
                        };
                        // Before the backfill, or it skips the folder (ticket 128).
                        restoreUploaded(dates, r);
                        backfill_service->trigger(dash(*dates.begin()), dash(*dates.rbegin()), "");
                        Json::Value arr(Json::arrayValue);
                        for (auto& d : dates) arr.append(d);
                        r["status"]          = "queued";
                        r["sessions_found"]  = (int)dates.size();
                        r["dates"]           = arr;
                        // What actually landed. A silent success that dropped the
                        // payload is how this shipped in the first place.
                        r["files_copied"]    = imported.copied;
                        r["files_skipped"]   = imported.skipped;
                        r["str_found"]       = imported.saw_str;
                        r["message"]         = "Files added to archive; parsing. Poll /api/backfill/status";
                        return r;
                    };
            }

            drogon::app()
                .setLogLevel(trantor::Logger::kWarn)
                .addListener("0.0.0.0", web_port)
                // SDD-039 D3: two threads was never a decision, and two means
                // any two slow requests are an outage (support 129: a stalled
                // mount held one and the dashboard never answered). Follow the
                // cores, clamped: a Pi stays modest, a laptop is not one
                // blocked read away from an unresponsive app.
                .setThreadNum(std::min(8u, std::max(4u, std::thread::hardware_concurrency())))
                .setDocumentRoot(static_dir)
                // Drogon's own list of servable types (it replaces, not
                // adds) plus webp, which it 404s by default: the Sleep Bridge
                // setup's looping pictures are animated WebP (SDD-073).
                .setFileTypes({"html", "js", "css", "xml", "xsl", "txt", "svg", "ttf", "otf",
                               "woff2", "woff", "eot", "png", "jpg", "jpeg", "gif", "bmp",
                               "ico", "icns", "webp"})
                .setIdleConnectionTimeout(120)
                // Allow large CPAP zip / oximetry CSV uploads (default is 1 MB).
                .setClientMaxBodySize(512 * 1024 * 1024);

            // SPA fallback: read index.html from disk on each 404 so
            // frontend rebuilds take effect without restarting the service
            std::string spa_index_path = static_dir + "/index.html";
            if (std::filesystem::exists(spa_index_path)) {
                // SDD-021: one place that turns index.html into a response,
                // pointing <base href> at the Home Assistant Ingress prefix
                // when the proxy tells us there is one. Home Assistant serves
                // an add-on underneath /api/hassio_ingress/<token>/ and rewrites
                // nothing in the payload, so a page that assumes it lives at the
                // root loads and then 404s every asset and every API call.
                //
                // This is a function rather than post-handling advice because
                // advice DOES NOT RUN for either path that serves this file.
                // Measured, not assumed: with a debug line inside an advice,
                // a request to /api/capabilities logged and requests to / and
                // to /sessions did not. Drogon's static file router and its
                // custom error handler both sit outside the controller
                // pipeline that advice hangs off.
                //
                // No header means no rewrite, which is every other deployment:
                // plain Docker, the desktop app, the native service.
                auto serve_index =
                    [spa_index_path](const drogon::HttpRequestPtr& req)
                    -> drogon::HttpResponsePtr {
                    std::ifstream ifs(spa_index_path);
                    if (!ifs) return nullptr;
                    std::string html(std::istreambuf_iterator<char>(ifs), {});

                    const std::string& prefix = req->getHeader("x-ingress-path");
                    if (!prefix.empty()) {
                        hms_cpap::ingress::rewriteBaseHref(html, prefix);
                    }

                    auto resp = drogon::HttpResponse::newHttpResponse();
                    resp->setContentTypeCode(drogon::CT_TEXT_HTML);
                    resp->setBody(std::move(html));
                    return resp;
                };

                // Pre-routing, because it is the only hook that has the request
                // AND exists everywhere we build. The obvious alternatives both
                // failed on the way here:
                //
                //   post-handling advice never runs for either path that serves
                //   this file (Drogon's static file router and its custom error
                //   handler sit outside the controller pipeline), measured with
                //   a debug line: /api/capabilities logged, "/" did not;
                //
                //   setCustomErrorHandler has an overload taking the request in
                //   Drogon 1.9.13, which is what Homebrew ships, and NOT in
                //   1.9.0, which is what Debian trixie ships and therefore what
                //   the Docker image builds against. That one only showed up in
                //   the container build.
                //
                // The rule is the ordinary SPA one: a path with no file
                // extension that is not an API route is a client-side route, so
                // serve the shell and let Angular route it. Anything with an
                // extension falls through to the static file handler, so assets
                // are untouched.
                //
                // The web port only. Pre-routing advice runs for every
                // listener, and this one runs ahead of the Sleep Bridge's port
                // split, so without the check it answered the stream route
                // with this page (the bridge's upgrade got a 200 and never
                // connected) and served the UI on the stream port.
                drogon::app().registerPreRoutingAdvice(
                    [serve_index, web_port](const drogon::HttpRequestPtr& req,
                                            drogon::AdviceCallback&& stop,
                                            drogon::AdviceChainCallback&& carry_on) {
                        if (req->getLocalAddr().toPort() != web_port) {
                            carry_on();
                            return;
                        }
                        const std::string path = req->path();
                        const bool is_api =
                            path.rfind("/api/", 0) == 0 || path == "/health";
                        const size_t slash = path.find_last_of('/');
                        const bool has_extension =
                            path.find_last_of('.') != std::string::npos &&
                            path.find_last_of('.') > slash;

                        if (is_api || has_extension) {
                            carry_on();
                            return;
                        }
                        if (auto resp = serve_index(req)) {
                            stop(resp);
                            return;
                        }
                        carry_on();
                    });

                drogon::app().setCustomErrorHandler(
                    [](drogon::HttpStatusCode code) -> drogon::HttpResponsePtr {
                        Json::Value err;
                        err["error"] = static_cast<int>(code);
                        return drogon::HttpResponse::newHttpJsonResponse(err);
                    });
            }

            // SDD-062 D13: the public Sleep Bridge release feed, read on a
            // bridge's hello and when the Devices page loads, never on a
            // timer. Declared first: the services below use it until they go.
            hms_cpap::sleepbridge::FeedGet feed_get = [get = hms_cpap::mule::curlTransport().get](
                                                          const std::string& url, int timeout_s) {
                const auto r = get(url, timeout_s);
                hms_cpap::sleepbridge::FeedReply out;
                out.ok     = r.ok;
                out.status = r.status;
                out.body   = r.body;
                out.error  = r.error;
                return out;
            };
            std::unique_ptr<hms_cpap::sleepbridge::ReleaseFeed> release_feed;
            if (config.sleep_bridge.enabled)
                release_feed = std::make_unique<hms_cpap::sleepbridge::ReleaseFeed>(
                    config.sleep_bridge.release_feed, feed_get);
            // SDD-062 D12: the local names and the forgotten devices.
            hms_cpap::devices::DevicesStore devices_store(
                (std::filesystem::path(hms_cpap::AppConfig::dataDir()) / "devices.json").string());
            // The Sleep Bridge local stream, on a port of its own. The
            // service outlives app().run(); without the Sleep Bridge
            // component in this build it listens and refuses bridges.
            std::unique_ptr<hms_cpap::sleepbridge::SleepBridgeService> sleep_bridge;
            // The Mule & Miner local stream rides the same port, on its own
            // route, into the one AirSense 11 night writer (SDD-057's).
            std::unique_ptr<hms_cpap::mule::MuleService> mule_stream;
            // Its firmware updates, over the LAN, from the published manifest
            // or a file the owner has (docs/MULE_LOCAL_STREAM.md).
            std::unique_ptr<hms_cpap::mule::FirmwareUpdater> mule_updater;
            // SDD-065: a DreamStation 2 read through a unit in that mode.
            std::unique_ptr<hms_cpap::mule::MuleDs2> mule_ds2;
            // SDD-061: a .cpapcard's layout. With the Sleep Bridge off it is
            // checked and kept for when it is turned on; with it on (below) it
            // also goes to the bridges.
            hms_cpap::CpapController::card_layout_ =
                [](const std::string& cbl1, const std::string& file_name, const std::string&) -> Json::Value {
                    namespace sb = hms_cpap::sleepbridge;
                    Json::Value r;
                    std::string why;
                    const auto card = sb::decodeLearnedCard(cbl1, &why);
                    const std::string root =
                        (std::filesystem::path(hms_cpap::AppConfig::dataDir()) / "sleepbridge").string();
                    if (!card) {
                        r["error"] = why;
                    } else if (!sb::keepLearnedCard(root, *card, cbl1, file_name, &why)) {
                        r["error"] = "The card's layout could not be kept: " + why;
                    } else {
                        r = sb::learnedSummary(*card);
                        r["stored"]  = true;
                        r["bridges"] = Json::Value(Json::arrayValue);
                        r["message"] = "Kept for the Sleep Bridge, which is off in Settings.";
                    }
                    return r;
                };
            if (config.sleep_bridge.enabled) {
                if (config.sleep_bridge.port == web_port || config.sleep_bridge.port <= 0 ||
                    config.sleep_bridge.port > 65535) {
                    std::cerr << "Sleep Bridge: port " << config.sleep_bridge.port
                              << " is not usable (it must differ from the web port); the "
                                 "stream is off" << std::endl;
                } else {
                    if (config.sleep_bridge.port != hms_cpap::sleepbridge::kDefaultPort)
                        std::cout << "Sleep Bridge: the stream listens on port " << config.sleep_bridge.port
                                  << ", not the default 8894. A Sleep Bridge or a Mule & Miner reaches it "
                                     "only with " << config.sleep_bridge.port
                                  << " in its own Port field (its setup page, or the stream / config "
                                     "command)." << std::endl;
                    hms_cpap::sleepbridge::SleepBridgeService::Options sb;
                    sb.root = (std::filesystem::path(hms_cpap::AppConfig::dataDir()) / "sleepbridge")
                                  .string();
                    sb.port = config.sleep_bridge.port;
                    // SDD-062: each bridge's update job, kept across a restart;
                    // the release image's download (checked against the feed).
                    sb.jobs_dir = (std::filesystem::path(hms_cpap::AppConfig::dataDir()) / "ota" / "sleepbridge")
                                      .string();
                    sb.fetch = feed_get;
                    // SDD-073: the setup steps' waits, pushed as they happen.
                    sb.on_event = [](const std::string& event, const std::string& sender) {
                        hms_cpap::SetupWatch::bridgeEvent(event, sender);
                    };

                    // The live night. Each rebuild that wrote card files wakes
                    // the burst cycle, so a card folder source pointed at the
                    // bridge's root reads the night seconds after the bridge
                    // streamed it instead of at the next interval, and the
                    // files' new events and the mask's state go to the house's
                    // MQTT as they land, on the live topics every source shares
                    // (live_state, mqtt/LiveState.h). MQTT off: nothing to say.
                    using hms_cpap::sleepbridge::LiveNight;
                    auto lives    = std::make_shared<std::map<std::string, std::unique_ptr<LiveNight>>>();
                    auto lives_mu = std::make_shared<std::mutex>();
                    const std::string sb_root = sb.root;
                    auto liveFor = [=](const std::string& sender) -> LiveNight& {
                        auto& slot = (*lives)[sender];
                        if (slot) return *slot;
                        hms_cpap::sleepbridge::LiveSink sink;
                        sink.event = [=](const hms_cpap::sleepbridge::LiveEvent& e) {
                            const auto t = std::chrono::system_clock::to_time_t(e.at);
                            // SDD-074 D8: the machine's own offset at that
                            // instant (the host's when it has no setting).
                            const std::string at =
                                hms_cpap::machine_clocks::zoneFor(hms_cpap::machine_clocks::primaryDevice())
                                    .isoText(static_cast<long long>(t));
                            Json::Value v(Json::objectValue);
                            v["type"]        = e.type;
                            v["description"] = e.description;
                            v["at"]          = at;
                            v["epoch"]       = static_cast<Json::Int64>(t);
                            v["duration_s"]  = e.duration_s;
                            v["source"]      = hms_cpap::LiveState::kSleepBridge;
                            v["source_id"]   = sender;
                            v["bridge"]      = sender;
                            v["file"]        = e.file;
                            // SDD-058 D3: said only for an event found in the
                            // flow (never while D6's gate is closed).
                            if (e.computed) v["computed"] = true;
                            if (live_state) live_state->event(v, e.at);
                            std::cout << "[sleepbridge] " << sender << " event " << e.type << " ("
                                      << e.description << ") at " << at << std::endl;
                        };
                        sink.mask_change = [=](bool on, const std::string& session,
                                               std::chrono::system_clock::time_point occurred) {
                            if (live_state)
                                live_state->mask({hms_cpap::LiveState::kSleepBridge, sender, on,
                                                  occurred, session});
                            std::cout << "[sleepbridge] " << sender << " mask " << (on ? "on" : "off")
                                      << std::endl;
                        };
                        if (live_state) live_state->sourceSeen(sender);
                        slot = std::make_unique<LiveNight>(
                            (std::filesystem::path(sb_root) / sender).string(), std::move(sink));
                        return *slot;
                    };
                    sb.on_rebuilt = [=](const std::string& sender,
                                        const hms_cpap::sleepbridge::StepReport& step) {
                        {
                            std::lock_guard<std::mutex> lk(*lives_mu);
                            liveFor(sender).onWritten(step.written_paths,
                                                      std::chrono::system_clock::now(), step.catch_up);
                        }
                        // platform SDD-059 D3: the rebuilt files are on disk.
                        const std::string card_root = (std::filesystem::path(sb_root) / sender).string();
                        for (const auto& rel : step.written_paths)
                            hms_cpap::card_feed::fileWritten(card_root, rel, "",
                                                             hms_cpap::card_feed::kSourceCard);   // SDD-079 D1
                        // SDD-080 D2: a .B01 among them is a DreamStation 2
                        // closing a session: OSCAR reads the card.
                        if (burst_service) burst_service->oscarCardWritten(card_root, step.written_paths);
                        if (burst_service) burst_service->requestSyncNow();
                    };
                    // Issue #57: a batch landing is the stream alive, which
                    // is what lets five quiet minutes mean the mask is off
                    // rather than the bridge's link being down.
                    sb.on_banked = [=](const std::string& sender) {
                        std::lock_guard<std::mutex> lk(*lives_mu);
                        liveFor(sender).onDelivered(std::chrono::system_clock::now());
                    };
                    sb.on_tick = [=] {
                        std::lock_guard<std::mutex> lk(*lives_mu);
                        for (auto& [sender, live] : *lives) live->tick();
                        if (live_state) live_state->tick();
                    };

                    sleep_bridge = std::make_unique<hms_cpap::sleepbridge::SleepBridgeService>(
                        sb, [](const hms_cpap::sleepbridge::RebuildStart& start) {
                            return hms_cpap::sleepbridge::makeRebuilder(
                                start, [](const std::string& line) {
                                    std::cout << "[sleepbridge] " << line << std::endl;
                                });
                        });
                    sleep_bridge->setReleaseFeed(release_feed.get());
                    hms_cpap::sleepbridge::installRoutes(sleep_bridge.get(), release_feed.get());
                    sleep_bridge->start();
                    // SDD-073: "Use this bridge" and the setup watch read the
                    // bridges from here.
                    hms_cpap::CpapController::sleepbridge_status_ = [svc = sleep_bridge.get()] {
                        return svc->status();
                    };
                    hms_cpap::SetupWatch::setBridgeSource([svc = sleep_bridge.get()] { return svc->status(); });
                    hms_cpap::CpapController::card_layout_ =
                        [svc = sleep_bridge.get()](const std::string& cbl1, const std::string& file_name,
                                                   const std::string& zip_path) -> Json::Value {
                            // SDD-061 D9: the card's own files go with its layout.
                            std::string why;
                            const std::string files = hms_cpap::readCardFilesBlob(zip_path, &why);
                            auto a = svc->learnCard(cbl1, file_name, files);
                            if (!why.empty()) a.body["files_note"] = why;
                            return a.body;   // "error" set when it was not kept
                        };

                    hms_cpap::mule::MuleService::Options mo;
                    mo.data_dir = hms_cpap::AppConfig::dataDir();
                    mo.root     = (std::filesystem::path(mo.data_dir) / "mule").string();
                    mo.port     = config.sleep_bridge.port;
                    std::shared_ptr<hms_cpap::IAs11NightSink> night =
                        burst_service ? burst_service->getAs11NightSink()
                                      : std::make_shared<hms_cpap::As11NightWriter>(
                                            mo.data_dir, hms_cpap::hostLocalClock(),
                                            config.airsense11_ble.strict_stream_gaps);
                    mule_stream = std::make_unique<hms_cpap::mule::MuleService>(mo, night);
                    hms_cpap::mule::FirmwareUpdater::Options fo;
                    if (const char* m = std::getenv("HMS_CPAP_MULE_RELEASE_BASE"); m && *m) fo.release_base = m;
                    fo.jobs_dir     = (std::filesystem::path(mo.data_dir) / "ota" / "mule").string();
                    fo.pairing_file = (std::filesystem::path(mo.data_dir) / "ota" / "mule-pairing.json").string();
                    mule_updater = std::make_unique<hms_cpap::mule::FirmwareUpdater>(
                        *mule_stream, hms_cpap::mule::curlTransport(), fo);
                    hms_cpap::mule::installRoutes(mule_stream.get(), mule_updater.get());
                    // SDD-081 D2: the OSCAR helper's route, on the same port.
                    hms_cpap::oscar::helper::installRoutes(oscar_helper_host.get());
                    // SDD-065: its records land where the direct read's do
                    // and the same import turns them into nights; it is read
                    // on the owner's one choice for every DS2 (once a day at
                    // their time, or only when asked), and when another
                    // source says the night ended; never while a mask is on.
                    {
                        hms_cpap::mule::MuleDs2::Options d2;
                        d2.data_dir   = mo.data_dir;
                        d2.units_root = mo.root;
                        mule_ds2 = std::make_unique<hms_cpap::mule::MuleDs2>(d2, *mule_stream);
                        mule_stream->setAnswerHook(
                            [m = mule_ds2.get()](const hms_cpap::mule::MuleService::Answered& a) {
                                m->onAnswer(a);
                            });
                        hms_cpap::mule::installDs2Routes(mule_ds2.get());
                        if (auto* bs = burst_service.get()) {
                            mule_ds2->setImportDue([bs] { bs->ds2ImportDue(); });
                            mule_ds2->setNightSources([bs] { return bs->ds2NightSources(); });
                            // SDD-065 D4a: the owner's one read choice for every DS2.
                            mule_ds2->setReadSchedule([bs] { return bs->ds2ReadSchedule(); });
                            // SDD-065 D8: records and the card's own files, by default.
                            mule_ds2->setCardMode([&config] { return config.ds2_ble.card; });
                            bs->addDs2NightListener(
                                [m = mule_ds2.get()](const std::string& by) { m->nightEnded(by); });
                            bs->addDs2DailyListener(
                                [m = mule_ds2.get()](const hms_cpap::ds2time::Input& in) { m->dailyCheck(in); });
                        }
                    }
                    if (burst_service) {
                        // Live: every block and event wakes the sync, so the
                        // night is in the database and the UI within a block;
                        // the events and therapy on/off go to the broker as
                        // they arrive, for the house's automations: on the live
                        // topics every source shares, and on the unit's own
                        // live/* topics, kept for the automations built on them.
                        std::shared_ptr<hms_cpap::mule::LivePublisher> live;
                        if (auto mq = burst_service->getMqttClient()) {
                            live = std::make_shared<hms_cpap::mule::LivePublisher>(
                                [mq](const std::string& topic, const std::string& payload, int qos,
                                     bool retain) {
                                    return mq->isConnected() && mq->publish(topic, payload, qos, retain);
                                },
                                config.device_id, ha_device_name);
                            live->announce();
                        } else if (live_state) {
                            live = std::make_shared<hms_cpap::mule::LivePublisher>(
                                nullptr, config.device_id, ha_device_name);
                        }
                        if (live && live_state)
                            live->setState(live_state, hms_cpap::LiveState::kMuleBle);
                        auto* bs = burst_service.get();
                        mule_stream->setLive([bs] { bs->requestSyncNow(); }, live);
                    }
                }
            }

            // SDD-062: the Devices page's routes, over whatever is on.
            {
                hms_cpap::devices::RouteDeps dd;
                dd.config      = &config;
                dd.burst       = burst_service.get();
                dd.query       = query_service;
                dd.mule        = mule_stream.get();
                dd.updater     = mule_updater.get();
                dd.mule_ds2    = mule_ds2.get();
                dd.bridges     = sleep_bridge.get();
                dd.feed        = release_feed.get();
                dd.store       = &devices_store;
                dd.garmin      = garmin.get();
                dd.stream_port = config.sleep_bridge.port;
                dd.config_path = config_path;   // SDD-080
                dd.oscar_helper = oscar_helper_host.get();   // SDD-081
                // SDD-074: the clock setting, and "Re-read my nights": the
                // rows are backed up first, then the archive is read again by
                // the backfill worker, with the setting in force.
                dd.db = web_db ? web_db : db;
                {
                    std::shared_ptr<hms_cpap::IDatabase> rr_db = dd.db;
                    const std::string rr_root =
                        !config.local_dir.empty() ? config.local_dir : config.archive_dir;
                    dd.reread = [rr_db, rr_root](const std::string& device) -> Json::Value {
                        Json::Value r(Json::objectValue);
                        if (!backfill_service || rr_root.empty()) {
                            r["ok"]    = false;
                            r["error"] = "no archive of this machine's files to read again";
                            return r;
                        }
                        std::string why;
                        const auto made = hms_cpap::clockapi::backupRows(
                            *rr_db, device, static_cast<long long>(std::time(nullptr)), &why);
                        if (made.empty()) {
                            r["ok"]    = false;
                            r["error"] = why.empty() ? "the rows could not be backed up" : why;
                            return r;
                        }
                        Json::Value list(Json::arrayValue);
                        for (const auto& t : made) list.append(t);
                        std::cout << "Clock: " << device << "'s rows backed up (" << made.size()
                                  << " table(s)); reading every archived night again" << std::endl;
                        backfill_service->trigger("", "", rr_root);
                        r["ok"]     = true;
                        r["backup"] = list;
                        return r;
                    };
                }
                hms_cpap::devices::installRoutes(dd);
            }

            std::cout << "Web UI: http://0.0.0.0:" << web_port << std::endl;
            std::cout << "  /health         - Health check" << std::endl;
            std::cout << "  /api/dashboard  - Dashboard data" << std::endl;
            std::cout << "  /api/sessions   - Session list" << std::endl;
            std::cout << std::endl;

            // SDD-006: on a genuine first run, put the wizard in front of the
            // user instead of printing a URL to a terminal they may not be
            // looking at. Registered as beginning advice rather than called
            // before run(), because the listener is not accepting until the
            // event loop is up and a browser that arrives first sees a refused
            // connection.
            {
                hms_cpap::SetupService::LaunchContext lc;
                lc.setup_complete  = config.setup_complete;
                lc.no_browser_flag = no_browser;
                lc.supervised      = std::getenv("HMS_CPAP_SUPERVISED") != nullptr;
                lc.interactive     = hms_cpap::SetupService::isInteractiveSession();

                if (hms_cpap::SetupService::shouldOpenBrowser(lc)) {
                    const auto url = hms_cpap::SetupService::setupUrl(web_port);
                    drogon::app().registerBeginningAdvice([url]() {
                        std::cout << "Setup: opening " << url << std::endl;
                        if (!hms_cpap::SetupService::openInBrowser(url)) {
                            std::cout << "Setup: could not open a browser. "
                                         "Visit " << url << " to finish setup."
                                      << std::endl;
                        }
                    });
                } else if (!config.setup_complete) {
                    // Still tell them where to go: this is the path a headless
                    // or supervised first run takes.
                    std::cout << "Setup: not yet configured. Visit "
                              << hms_cpap::SetupService::setupUrl(web_port)
                              << " to finish setup." << std::endl;
                }
            }

            drogon::app().run();  // Blocks until quit()
            // The backfill worker first, while everything it saves through is
            // still alive. Drogon's own SIGTERM handler ends run() without
            // requestShutdown(), so nothing else stops it: it used to be
            // joined only by its static destructor during exit(), and a
            // re-parse caught mid-save then wrote after the database client's
            // statics were gone. On PostgreSQL the COMMIT went out empty, the
            // save printed success and rolled back, and the night it had
            // already deleted was missing from the database.
            if (backfill_service) backfill_service->stop();
            // The collector outlives this scope: it no longer calls into the
            // Mule & Miner's DreamStation 2 reads (SDD-065).
            if (burst_service) burst_service->clearDs2NightListeners();
            if (sleep_bridge) sleep_bridge->stop();
            update_service->stop();
            hms_cpap::CpapController::setUpdateService(nullptr);
            // The collectors stop telling it before it stops.
            hms_cpap::card_feed::setSinks({}, {});
            hms_cpap::CpapController::setStreamService(nullptr);
            cpapdash_stream->stop();
            if (burst_service) burst_service->clearNightClosedListeners();
            hms_cpap::night_landed::clear();
            daily_only_queue->stop();
            local_model->stop();
            hms_cpap::CpapController::setGarminService(nullptr);
            garmin->stop();
        }
#else
        // No web UI — simple sleep loop
        while (!shutdown_requested) {
            std::this_thread::sleep_for(std::chrono::seconds(1));
        }
#endif

        // Cleanup. The backfill before the collector: its saved-sessions
        // callback calls into burst_service.
        if (backfill_service) {
            backfill_service->stop();
            backfill_service.reset();
        }
        if (ml_service) {
            ml_service->stop();
            ml_service.reset();
        }
        if (agent_service) {
            agent_service->stop();
            agent_service.reset();
        }
        if (oscar_helper_host) oscar_helper_host->shutdown();
        if (mdns_advertiser) mdns_advertiser->stop();
        if (burst_service) {
            burst_service->stop();
            burst_service.reset();
        }

        std::cout << "HMS-CPAP service stopped cleanly" << std::endl;
        return requested_exit_code.load();

    } catch (const std::exception& e) {
        std::cerr << "Fatal error: " << e.what() << std::endl;
        return 1;
    }
}
