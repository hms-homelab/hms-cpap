#ifdef BUILD_WITH_WEB
#include <drogon/drogon.h>
#include <fstream>
#include "controllers/CpapController.h"
#include "controllers/EquipmentController.h"
#include "controllers/CleaningController.h"
#include "services/CpapDashSyncService.h"
#include "services/SetupService.h"
#include "services/SupplyPublisher.h"
#include "services/MyAirService.h"
#include "web/IngressBase.h"
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
#ifdef WITH_POSTGRESQL
#include "agent/AgentService.h"
#endif
#include "services/MLTrainingService.h"
#include "services/BackfillService.h"
#include <cpapdash/parser/OximetryCsv.h>
#include "services/PrismaIngestion.h"
#include "agent/IAgentLLM.h"
#include "mqtt_client.h"
#include "llm_client.h"
#include "utils/ConfigManager.h"
#include "utils/AppConfig.h"
#include "utils/CardResidue.h"
#include "utils/CardImport.h"
#include "utils/FileLogger.h"
#include "utils/CardLayout.h"
#include "utils/DbProbeCli.h"
#include "utils/ParentWatch.h"
#include <iostream>
#include <iomanip>
#include <csignal>
#include <atomic>
#include <memory>
#include <set>
#include <regex>
#include <string>
#include <cstring>
#include <cstdlib>
#include <filesystem>
#include <sstream>
#ifdef _WIN32
#include <windows.h>
#endif

std::atomic<bool> shutdown_requested(false);
std::unique_ptr<hms_cpap::BurstCollectorService> burst_service;
#ifdef WITH_POSTGRESQL
std::unique_ptr<hms_cpap::AgentService> agent_service;
#endif
std::unique_ptr<hms_cpap::MLTrainingService> ml_service;
std::unique_ptr<hms_cpap::BackfillService> backfill_service;

/**
 * Graceful shutdown logic (shared by all platforms)
 */
void requestShutdown() {
    shutdown_requested = true;
    if (burst_service) burst_service->stop();
#ifdef WITH_POSTGRESQL
    if (agent_service) agent_service->stop();
#endif
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
    std::cout << "  Device ID:          " << hms_cpap::ConfigManager::get("CPAP_DEVICE_ID", "cpap_resmed_23243570851") << std::endl;
    std::cout << "  Device Name:        " << hms_cpap::ConfigManager::get("CPAP_DEVICE_NAME", "ResMed AirSense 10") << std::endl;
    if (source == "local") {
        std::cout << "  Source:             Local directory" << std::endl;
        std::cout << "  Local Dir:          " << hms_cpap::ConfigManager::get("CPAP_LOCAL_DIR", "(not set)") << std::endl;
    } else if (source == "fysetc") {
        std::cout << "  Source:             Fysetc" << std::endl;
        std::cout << "  Listen:             " << hms_cpap::ConfigManager::get("FYSETC_LISTEN_BIND", "0.0.0.0")
                  << ":" << hms_cpap::ConfigManager::getInt("FYSETC_LISTEN_PORT", 9000) << std::endl;
    } else {
        std::cout << "  Source:             ez Share" << std::endl;
        std::cout << "  ez Share URL:       " << hms_cpap::ConfigManager::get("EZSHARE_BASE_URL", "http://192.168.4.1") << std::endl;
    }
    std::cout << "  Burst Interval:     " << hms_cpap::ConfigManager::getInt("BURST_INTERVAL", 120) << " seconds" << std::endl;
    std::cout << "  Session Gap:        " << hms_cpap::ConfigManager::getInt("SESSION_GAP_MINUTES", 60) << " minutes" << std::endl;
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

    // Connect to the configured DB backend (sqlite/postgresql/mysql). Previously
    // this hardcoded a PostgreSQL DatabaseService, so --backfill failed for
    // SQLite users (issue #8). Use the shared factory so it matches the service.
    auto db = hms_cpap::makeDatabaseFromConfig();
    if (!db->connect()) {
        std::cerr << "STR Backfill: DB connection failed" << std::endl;
        return 1;
    }

    if (!db->saveSTRDailyRecords(records)) {
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
    const auto layout = hms_cpap::classifyLocalDir(card_root);
    if (layout != hms_cpap::LocalDirLayout::Root) {
        std::cerr << "hms_cpap --reparse: "
                  << hms_cpap::localDirProblem(layout, card_root) << "." << std::endl
                  << "  " << hms_cpap::localDirRemedy(layout, card_root) << std::endl;
        return 1;
    }
    const std::string archive_dir = hms_cpap::datalogDirFor(card_root);
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

    // Connect to the configured DB backend via the shared factory (sqlite/
    // postgresql/mysql) so CLI tools and the service never disagree.
    auto db = hms_cpap::makeDatabaseFromConfig();
    if (!db->connect()) {
        std::cerr << "Reparse: DB connection failed ("
                  << hms_cpap::ConfigManager::get("DB_TYPE", "sqlite") << ")" << std::endl;
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
        int deleted = db->deleteSessionsByDateFolder(device_id, folder);
        if (deleted > 0) {
            std::cout << "  Deleted " << deleted << " existing session(s) from DB" << std::endl;
            total_deleted += deleted;
        }

        // Parse each session group
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

            // Set relative file paths (same format as normal pipeline)
            hms_cpap::applySessionFilePaths(*parsed, session, folder);

            // Save to DB
            if (db->saveSession(*parsed)) {
                total_saved++;

                // Reparsed sessions are complete — set session_end
                db->markSessionCompleted(device_id, session.session_start);

                // Record which files the night is actually made of (SDD-014)
                db->replaceSessionFiles(device_id, session.session_start,
                                        hms_cpap::sessionFileRefs(session, folder));

                // Store checkpoint file sizes (same as burst cycle)
                std::map<std::string, int> checkpoint_sizes;
                for (const auto& [filename, size_kb] : session.file_sizes_kb) {
                    if (filename.find("_BRP.edf") != std::string::npos ||
                        filename.find("_PLD.edf") != std::string::npos ||
                        filename.find("_SAD.edf") != std::string::npos ||
                        filename.find("_SA2.edf") != std::string::npos) {
                        checkpoint_sizes[filename] = size_kb;
                    }
                }
                db->updateCheckpointFileSizes(device_id, session.session_start, checkpoint_sizes);

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
    bool preflight_only = false;
    bool db_probe_mode  = false;
    bool db_provision   = false;
    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "--preflight") == 0) preflight_only = true;
        if (std::strcmp(argv[i], "--test-db") == 0)   db_probe_mode  = true;
        if (std::strcmp(argv[i], "--create-db") == 0) { db_probe_mode = true; db_provision = true; }
    }
    if (!preflight_only && !db_probe_mode) {
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
        const auto report = hms_cpap::PreflightService::run(config);
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

    // Only create config file on first run -- don't overwrite user's config
    if (!config_existed) {
        config.save(config_path);
    }

    // ── Bridge: set env vars from merged config so BurstCollectorService works ──
    // Config.json is the single source of truth; env vars are set for
    // legacy code that reads them via ConfigManager.
    portableSetenv("CPAP_DEVICE_ID", config.device_id.c_str());
    portableSetenv("CPAP_DEVICE_NAME", config.device_name.c_str());
    portableSetenv("CPAP_SOURCE", config.source.c_str());
    portableSetenv("EZSHARE_BASE_URL", config.ezshare_url.c_str());
    // Store numeric conversions in locals to avoid dangling pointer from temporary
    std::string burst_str = std::to_string(config.burst_interval);
    std::string port_str  = std::to_string(config.web_port);
    portableSetenv("BURST_INTERVAL", burst_str.c_str());
    portableSetenv("HEALTH_CHECK_PORT", port_str.c_str());
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

        // Data source: HTTP polling (ezShare or Fysetc) or local directory
        int burst_interval = hms_cpap::ConfigManager::getInt("BURST_INTERVAL", 120);

        burst_service = std::make_unique<hms_cpap::BurstCollectorService>(burst_interval);
        burst_service->initialize(&config);
        burst_service->start();

        if (src == "local") {
            std::cout << "   Source: Local directory at " << hms_cpap::ConfigManager::get("CPAP_LOCAL_DIR", "") << std::endl;
        } else if (src == "fysetc") {
            std::cout << "   Source: Fysetc TCP on "
                      << hms_cpap::ConfigManager::get("FYSETC_LISTEN_BIND", "0.0.0.0") << ":"
                      << hms_cpap::ConfigManager::getInt("FYSETC_LISTEN_PORT", 9000) << std::endl;
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

#ifdef WITH_POSTGRESQL
        // Start Agent module if enabled
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

            // Build DB connection string for agent
            std::string a_db_host = hms_cpap::ConfigManager::get("DB_HOST", "localhost");
            std::string a_db_port = hms_cpap::ConfigManager::get("DB_PORT", "5432");
            std::string a_db_name = hms_cpap::ConfigManager::get("DB_NAME", "cpap_monitoring");
            std::string a_db_user = hms_cpap::ConfigManager::get("DB_USER", "maestro");
            std::string a_db_pass = hms_cpap::ConfigManager::get("DB_PASSWORD", "");
            std::string a_conn_str = "host=" + a_db_host + " port=" + a_db_port +
                                     " dbname=" + a_db_name + " user=" + a_db_user +
                                     " password=" + a_db_pass;

            std::string device_id = hms_cpap::ConfigManager::get("CPAP_DEVICE_ID", "cpap_resmed_23243570851");

            hms_cpap::AgentService::Config agent_cfg;
            agent_cfg.device_id = device_id;
            agent_cfg.db_connection_string = a_conn_str;
            agent_cfg.embed_model = agent_embed;
            agent_cfg.temperature = agent_temp;
            agent_cfg.max_iterations = hms_cpap::ConfigManager::getInt("AGENT_MAX_ITERATIONS", 5);
            agent_cfg.max_context = hms_cpap::ConfigManager::getInt("AGENT_MAX_CONTEXT", 20);
            agent_cfg.memory_limit = hms_cpap::ConfigManager::getInt("AGENT_MEMORY_LIMIT", 3);

            hms::MqttConfig amqtt_cfg;
            amqtt_cfg.broker = hms_cpap::ConfigManager::get("MQTT_BROKER", "127.0.0.1");
            amqtt_cfg.port = std::stoi(hms_cpap::ConfigManager::get("MQTT_PORT", "1883"));
            amqtt_cfg.username = hms_cpap::ConfigManager::get("MQTT_USER", "aamat");
            amqtt_cfg.password = hms_cpap::ConfigManager::get("MQTT_PASSWORD", "");
            amqtt_cfg.client_id = hms_cpap::ConfigManager::get("MQTT_CLIENT_ID", "hms_cpap") + "_agent";
            auto agent_mqtt = std::make_shared<hms::MqttClient>(amqtt_cfg);
            agent_mqtt->connect();

            agent_service = std::make_unique<hms_cpap::AgentService>(agent_cfg, agent_mqtt, agent_llm);
            agent_service->start();

            std::cout << "Agent: AI module enabled (model: " << agent_model << ")" << std::endl;
        }
#endif // WITH_POSTGRESQL

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
            if (config.myair.enabled && !config.myair.username.empty() &&
                (!config.myair.password.empty() || !config.myair.refresh_token.empty())) {
                auto myair = std::make_shared<hms_cpap::MyAirService>(
                    web_db ? web_db : db, config, config_path);
                if (burst_service) burst_service->setMyAirService(myair);
                std::cout << "myAir: enabled (" << config.myair.region
                          << "), polling every " << config.myair.poll_minutes
                          << " minutes" << std::endl;
            } else if (config.myair.enabled) {
                std::cout << "myAir: enabled but the username or password is empty "
                             "- staying off" << std::endl;
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
                    [oxi_db](const std::string& content, const std::string& filename) -> Json::Value {
                        Json::Value r;
                        auto session =
                            cpapdash::parser::readO2RingCsv(content, filename).session;
                        if (session.samples.empty()) {
                            r["error"] = "No readable rows in CSV (expected a Wellue / O2 Ring export)";
                            return r;
                        }
                        if (!oxi_db->saveOximetrySession(hms_cpap::kOximetryDeviceId, session)) {
                            r["error"] = "Failed to save oximetry session";
                            return r;
                        }
                        r["samples"]          = (int)session.samples.size();
                        r["valid_samples"]    = session.metrics.valid_samples;
                        r["avg_spo2"]         = session.metrics.avg_spo2;
                        r["min_spo2"]         = session.metrics.min_spo2;
                        r["sample_interval"]  = session.sample_interval;
                        r["duration_seconds"] = session.duration_seconds;
                        return r;
                    };
            }

            // Wire BackfillService whenever an archive/local DATALOG path is
            // known. It reparses from the permanent archive, so it must be
            // available in every source mode (ezshare/fysetc/local) — the
            // per-session UI reparse delegates to it.
            if (!config.local_dir.empty()) {
                hms_cpap::BackfillService::Config bf_cfg;
                bf_cfg.device_id = config.device_id;
                bf_cfg.device_name = config.device_name;
                bf_cfg.local_dir = config.local_dir;
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
                std::string archive_dir = hms_cpap::datalogDirFor(config.local_dir);
                std::string card_dir = config.local_dir;   // the card ROOT (SDD-010)
                hms_cpap::CpapController::cpap_zip_import_ =
                    [archive_dir, card_dir](const std::string& zip_path) -> Json::Value {
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
                        // Mirror the card into the archive: date folders under
                        // DATALOG, everything else at its own path from the card
                        // root, filtered only by residualSkip. See SDD-014 and
                        // utils/CardImport.h for why this is not a slice.
                        auto imported = hms_cpap::mirrorCardInto(
                            staging.string(), card_dir, archive_dir);
                        const auto& dates = imported.dates;

                        fs::remove_all(staging, ec);
                        if (dates.empty()) {
                            r["error"] = "No DATALOG date folders (YYYYMMDD) found in zip";
                            return r;
                        }
                        auto dash = [](const std::string& d) {
                            return d.substr(0, 4) + "-" + d.substr(4, 2) + "-" + d.substr(6, 2);
                        };
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
                .setThreadNum(2)
                .setDocumentRoot(static_dir)
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
                drogon::app().registerPreRoutingAdvice(
                    [serve_index](const drogon::HttpRequestPtr& req,
                                  drogon::AdviceCallback&& stop,
                                  drogon::AdviceChainCallback&& carry_on) {
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
        }
#else
        // No web UI — simple sleep loop
        while (!shutdown_requested) {
            std::this_thread::sleep_for(std::chrono::seconds(1));
        }
#endif

        // Cleanup
        if (ml_service) {
            ml_service->stop();
            ml_service.reset();
        }
#ifdef WITH_POSTGRESQL
        if (agent_service) {
            agent_service->stop();
            agent_service.reset();
        }
#endif
        if (burst_service) {
            burst_service->stop();
            burst_service.reset();
        }

        std::cout << "HMS-CPAP service stopped cleanly" << std::endl;
        return 0;

    } catch (const std::exception& e) {
        std::cerr << "Fatal error: " << e.what() << std::endl;
        return 1;
    }
}
