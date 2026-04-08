#ifdef BUILD_WITH_WEB
#include <drogon/drogon.h>
#include <fstream>
#include "controllers/CpapController.h"
#include "web/QueryService.h"
#endif
#include "services/BurstCollectorService.h"
#include "services/SessionDiscoveryService.h"
#include "services/DataPublisherService.h"
#include "parsers/CpapdashBridge.h"
#include "database/DatabaseService.h"
#include "database/IDatabase.h"
#include "database/SQLiteDatabase.h"
#ifdef WITH_POSTGRESQL
#include "database/PostgresDatabase.h"
#endif
#include "agent/AgentService.h"
#include "services/MLTrainingService.h"
#include "services/BackfillService.h"
#include "agent/IAgentLLM.h"
#include "mqtt_client.h"
#include "llm_client.h"
#include "utils/ConfigManager.h"
#include "utils/AppConfig.h"
#include <iostream>
#include <iomanip>
#include <csignal>
#include <atomic>
#include <memory>
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
std::unique_ptr<hms_cpap::AgentService> agent_service;
std::unique_ptr<hms_cpap::MLTrainingService> ml_service;
std::unique_ptr<hms_cpap::BackfillService> backfill_service;

/**
 * Graceful shutdown logic (shared by all platforms)
 */
void requestShutdown() {
    shutdown_requested = true;
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
    std::cout << R"(
╔═══════════════════════════════════════════════════════════╗
║                                                           ║
║      HMS-CPAP - CPAP Data Collection Service             ║
║                                                           ║
║      ResMed AirSense 10 Data Collection                  ║
║      Sources: HTTP, local                                 ║
║                                                           ║
║      Version: 2.2.0                                        ║
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
 * the same session gap logic (SESSION_GAP_MINUTES, default 60), deletes old DB records, and re-parses fresh.
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
            if (!session.csl_file.empty()) stageFile(session.csl_file);
            if (!session.eve_file.empty()) stageFile(session.eve_file);

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
                db.updateCheckpointFileSizes(device_id, session.session_start, checkpoint_sizes);

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
    // ── Load AppConfig ──────────────────────────────────────────────
    std::string data_dir = hms_cpap::AppConfig::dataDir();
    std::string config_path = data_dir + "/config.json";

    // Allow --config <path> override (scan before CLI modes)
    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "--config") == 0 && i + 1 < argc) {
            config_path = argv[i + 1];
            break;
        }
    }

    hms_cpap::AppConfig config;
    bool config_existed = hms_cpap::AppConfig::load(config_path, config);

    // Env vars fill any empty fields (fallback for systemd Environment= lines)
    config.applyEnvFallbacks();

    // Default sqlite_path if empty
    if (config.database.sqlite_path.empty()) {
        config.database.sqlite_path = data_dir + "/cpap.db";
    }

    // Save merged config (creates file on first run, updates on subsequent)
    config.save(config_path);

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
    std::shared_ptr<hms_cpap::IDatabase> db;
    std::string pg_conn_str;  // reused later for web/agent
    if (config.database.type == "postgresql") {
#ifdef WITH_POSTGRESQL
        pg_conn_str = "host=" + config.database.host + " port=" + std::to_string(config.database.port) +
                      " dbname=" + config.database.name + " user=" + config.database.user +
                      " password=" + config.database.password;
        db = std::make_shared<hms_cpap::PostgresDatabase>(pg_conn_str);
#else
        std::cerr << "PostgreSQL support not compiled in" << std::endl;
        return 1;
#endif
    } else {
        // Default: SQLite
        db = std::make_shared<hms_cpap::SQLiteDatabase>(config.database.sqlite_path);
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
        burst_service->start();

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

            // Separate DB connection for ML (pqxx is not thread-safe)
            std::shared_ptr<hms_cpap::IDatabase> ml_db;
            if (config.database.type == "postgresql") {
#ifdef WITH_POSTGRESQL
                std::string ml_conn = "host=" + config.database.host +
                    " port=" + std::to_string(config.database.port) +
                    " dbname=" + config.database.name +
                    " user=" + config.database.user +
                    " password=" + config.database.password;
                ml_db = std::make_shared<hms_cpap::PostgresDatabase>(ml_conn);
                ml_db->connect();
#endif
            } else {
                ml_db = db; // SQLite has its own locking
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

        // Start Agent module if enabled
        std::string agent_enabled = hms_cpap::ConfigManager::get("AGENT_ENABLED", "false");
        if (agent_enabled == "true" || agent_enabled == "1") {
            std::string agent_provider = hms_cpap::ConfigManager::get("AGENT_LLM_PROVIDER", "ollama");
            std::string agent_endpoint = hms_cpap::ConfigManager::get("AGENT_LLM_ENDPOINT", "http://192.168.2.5:11434");
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
            std::string embed_endpoint = hms_cpap::ConfigManager::get("AGENT_EMBED_ENDPOINT", "http://192.168.2.5:11434");
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
            std::string a_db_pass = hms_cpap::ConfigManager::get("DB_PASSWORD", "maestro_postgres_2026_secure");
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
            amqtt_cfg.broker = hms_cpap::ConfigManager::get("MQTT_BROKER", "192.168.2.15");
            amqtt_cfg.port = std::stoi(hms_cpap::ConfigManager::get("MQTT_PORT", "1883"));
            amqtt_cfg.username = hms_cpap::ConfigManager::get("MQTT_USER", "aamat");
            amqtt_cfg.password = hms_cpap::ConfigManager::get("MQTT_PASSWORD", "exploracion");
            amqtt_cfg.client_id = hms_cpap::ConfigManager::get("MQTT_CLIENT_ID", "hms_cpap") + "_agent";
            auto agent_mqtt = std::make_shared<hms::MqttClient>(amqtt_cfg);
            agent_mqtt->connect();

            agent_service = std::make_unique<hms_cpap::AgentService>(agent_cfg, agent_mqtt, agent_llm);
            agent_service->start();

            std::cout << "Agent: AI module enabled (model: " << agent_model << ")" << std::endl;
        }

#ifdef BUILD_WITH_WEB
        // Start web UI server (Drogon blocks until shutdown)
        {
            int web_port = config.web_port;
            std::string static_dir = config.static_dir;

            // Separate DB connection for web queries (pqxx is not thread-safe)
            std::shared_ptr<hms_cpap::IDatabase> web_db;
            if (config.database.type == "postgresql") {
#ifdef WITH_POSTGRESQL
                web_db = std::make_shared<hms_cpap::PostgresDatabase>(pg_conn_str);
                web_db->connect();
#endif
            } else {
                web_db = db; // SQLite has its own locking
            }
            auto query_service = std::make_shared<hms_cpap::QueryService>(
                web_db ? web_db : db, config.device_id);
            hms_cpap::CpapController::setQueryService(query_service);
            hms_cpap::CpapController::setConfig(&config, config_path);

            // Wire ML service triggers to controller
            if (ml_service) {
                hms_cpap::CpapController::ml_train_trigger_ = [&]() {
                    ml_service->triggerTraining();
                };
                hms_cpap::CpapController::ml_status_getter_ = [&]() -> Json::Value {
                    return ml_service->getStatus();
                };
            }

            // Wire BackfillService (local source mode only)
            if (src == "local" || src == "ezshare" || src == "fysetc_poll") {
                hms_cpap::BackfillService::Config bf_cfg;
                bf_cfg.device_id = config.device_id;
                bf_cfg.device_name = config.device_name;
                bf_cfg.local_dir = config.local_dir;

                // Separate DB connection (pqxx not thread-safe)
                std::shared_ptr<hms_cpap::IDatabase> bf_db;
                if (config.database.type == "postgresql") {
#ifdef WITH_POSTGRESQL
                    bf_db = std::make_shared<hms_cpap::PostgresDatabase>(pg_conn_str);
                    bf_db->connect();
#endif
                } else {
                    bf_db = db;  // SQLite has its own locking
                }

                backfill_service = std::make_unique<hms_cpap::BackfillService>(
                    bf_cfg, bf_db ? bf_db : db);
                backfill_service->start();

                hms_cpap::CpapController::backfill_trigger_ =
                    [&](const std::string& start, const std::string& end, const std::string& local_dir) {
                        backfill_service->trigger(start, end, local_dir);
                    };
                hms_cpap::CpapController::backfill_status_getter_ = [&]() -> Json::Value {
                    return backfill_service->getStatus();
                };
            }

            drogon::app()
                .setLogLevel(trantor::Logger::kWarn)
                .addListener("0.0.0.0", web_port)
                .setThreadNum(2)
                .setDocumentRoot(static_dir)
                .setIdleConnectionTimeout(120);

            // SPA fallback: serve index.html content for 404s so Angular handles routing
            std::string index_html;
            {
                std::ifstream ifs(static_dir + "/index.html");
                if (ifs) index_html.assign(std::istreambuf_iterator<char>(ifs), {});
            }
            if (!index_html.empty()) {
                drogon::app().setCustomErrorHandler(
                    [index_html](drogon::HttpStatusCode code) -> drogon::HttpResponsePtr {
                        if (code == drogon::k404NotFound) {
                            auto resp = drogon::HttpResponse::newHttpResponse();
                            resp->setContentTypeCode(drogon::CT_TEXT_HTML);
                            resp->setBody(index_html);
                            return resp;
                        }
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
        if (agent_service) {
            agent_service->stop();
            agent_service.reset();
        }
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
