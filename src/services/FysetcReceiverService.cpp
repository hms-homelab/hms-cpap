#include "services/FysetcReceiverService.h"
#include "services/SessionDiscoveryService.h"
#include "parsers/EDFParser.h"
#include "utils/ConfigManager.h"

#include <json/json.h>
#include <openssl/evp.h>

#include <iostream>
#include <fstream>
#include <filesystem>
#include <chrono>
#include <ctime>
#include <iomanip>

namespace hms_cpap {

FysetcReceiverService::FysetcReceiverService(
    std::shared_ptr<hms::MqttClient> mqtt,
    std::shared_ptr<DatabaseService> db,
    std::shared_ptr<DataPublisherService> publisher)
    : mqtt_(std::move(mqtt)),
      db_(std::move(db)),
      publisher_(std::move(publisher)) {

    device_id_ = ConfigManager::get("CPAP_DEVICE_ID", "cpap_resmed_23243570851");
    device_name_ = ConfigManager::get("CPAP_DEVICE_NAME", "ResMed AirSense 10");
    data_dir_ = ConfigManager::get("CPAP_TEMP_DIR", "/tmp/cpap_data");
    archive_dir_ = ConfigManager::get("CPAP_ARCHIVE_DIR", "/mnt/public/cpap_data/DATALOG");
    topic_prefix_ = "cpap/fysetc/" + device_id_;

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
        llm_config.keep_alive_seconds = ConfigManager::getInt("LLM_KEEP_ALIVE", 0);

        llm_client_ = std::make_unique<hms::LLMClient>(llm_config);

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
}

FysetcReceiverService::~FysetcReceiverService() {
    stop();
}

bool FysetcReceiverService::start() {
    std::cout << "FysetcReceiverService: Starting..." << std::endl;
    std::cout << "  Topic prefix: " << topic_prefix_ << std::endl;
    std::cout << "  Data dir: " << data_dir_ << std::endl;
    std::cout << "  Archive dir: " << archive_dir_ << std::endl;

    // Clear stale session_active on startup
    mqtt_->publish("cpap/" + device_id_ + "/realtime/session_active", "OFF", 1, true);

    // Subscribe to FYSETC topics
    std::vector<std::string> topics = {
        topic_prefix_ + "/sync/request",
        topic_prefix_ + "/chunk",
        topic_prefix_ + "/session/active",
        topic_prefix_ + "/manifest"
    };

    mqtt_->subscribe(topics, [this](const std::string& topic, const std::string& payload) {
        if (topic.find("/manifest") != std::string::npos) {
            onManifest(topic, payload);
        } else if (topic.find("/sync/request") != std::string::npos) {
            onSyncRequest(topic, payload);
        } else if (topic.find("/chunk") != std::string::npos) {
            onChunk(topic, payload);
        } else if (topic.find("/session/active") != std::string::npos) {
            onSessionActive(topic, payload);
        }
    });

    // Request initial manifest from FYSETC so we know what's on the card
    mqtt_->publish(topic_prefix_ + "/cmd/rescan", "1", 1, false);

    std::cout << "FysetcReceiverService: Subscribed, sent initial rescan request" << std::endl;
    return true;
}

void FysetcReceiverService::stop() {
    std::cout << "FysetcReceiverService: Stopping" << std::endl;
}

// ---------------------------------------------------------------------------
// onSyncRequest: FYSETC asks "what bytes do you have for these files?"
// Used during realtime therapy for delta sync of BRP/PLD/SAD/STR.
//
// Incoming: {"date":"20260315","files":["20260315_235530_BRP.edf",...]}
// Response: {"offsets":{"20260315_235530_BRP.edf":184320,...}}
// ---------------------------------------------------------------------------
void FysetcReceiverService::onSyncRequest(const std::string& /*topic*/,
                                           const std::string& payload) {
    Json::Value root;
    Json::CharReaderBuilder builder;
    std::istringstream ss(payload);
    std::string errors;
    if (!Json::parseFromStream(builder, ss, &root, &errors)) {
        std::cerr << "FysetcReceiver: Failed to parse sync request: " << errors << std::endl;
        return;
    }

    std::string date = root.get("date", "").asString();
    if (date.empty()) {
        std::cerr << "FysetcReceiver: sync request missing 'date'" << std::endl;
        return;
    }

    const Json::Value& files = root["files"];
    if (!files.isArray()) {
        std::cerr << "FysetcReceiver: sync request missing 'files' array" << std::endl;
        return;
    }

    // Build response with local file sizes as offsets
    Json::Value offsets(Json::objectValue);
    std::string dir = data_dir_ + "/" + date;

    for (const auto& f : files) {
        std::string filename = f.asString();
        std::filesystem::path filepath = std::filesystem::path(dir) / filename;
        uint64_t size = 0;
        if (std::filesystem::exists(filepath)) {
            size = std::filesystem::file_size(filepath);
        }
        offsets[filename] = static_cast<Json::UInt64>(size);
    }

    Json::Value response;
    response["offsets"] = offsets;

    Json::StreamWriterBuilder wbuilder;
    wbuilder["indentation"] = "";
    std::string response_str = Json::writeString(wbuilder, response);

    mqtt_->publish(topic_prefix_ + "/sync/response", response_str, 1, false);

    // Track active date folder from sync requests (therapy is in progress)
    active_date_folder_ = date;

    std::cout << "FysetcReceiver: Sync response for " << date
              << " (" << files.size() << " files)" << std::endl;
}

// ---------------------------------------------------------------------------
// onChunk: FYSETC sends a base64-encoded EDF data chunk
//
// JSON: {"f":"filename.edf","d":"20260315","o":184320,"n":6144,"b64":"..."}
// ---------------------------------------------------------------------------
void FysetcReceiverService::onChunk(const std::string& /*topic*/,
                                     const std::string& payload) {
    Json::Value root;
    Json::CharReaderBuilder builder;
    std::istringstream ss(payload);
    std::string errors;
    if (!Json::parseFromStream(builder, ss, &root, &errors)) {
        std::cerr << "FysetcReceiver: Failed to parse chunk: " << errors << std::endl;
        return;
    }

    std::string filename = root.get("f", "").asString();
    std::string date = root.get("d", "").asString();
    uint64_t offset = root.get("o", 0).asUInt64();
    uint64_t expected_len = root.get("n", 0).asUInt64();
    std::string b64 = root.get("b64", "").asString();

    if (filename.empty() || b64.empty()) {
        std::cerr << "FysetcReceiver: chunk missing required fields" << std::endl;
        return;
    }

    // Base64 decode using OpenSSL EVP
    size_t b64_len = b64.size();
    size_t max_decoded = 3 * (b64_len / 4) + 3;
    std::vector<unsigned char> decoded(max_decoded);

    int decoded_len = EVP_DecodeBlock(decoded.data(),
                                       reinterpret_cast<const unsigned char*>(b64.c_str()),
                                       static_cast<int>(b64_len));
    if (decoded_len < 0) {
        std::cerr << "FysetcReceiver: base64 decode failed for " << filename << std::endl;
        return;
    }

    // Adjust for padding: EVP_DecodeBlock doesn't subtract padding bytes
    int padding = 0;
    if (b64_len >= 1 && b64[b64_len - 1] == '=') padding++;
    if (b64_len >= 2 && b64[b64_len - 2] == '=') padding++;
    decoded_len -= padding;

    if (static_cast<uint64_t>(decoded_len) != expected_len) {
        std::cerr << "FysetcReceiver: decoded " << decoded_len
                  << " bytes but expected " << expected_len
                  << " for " << filename << std::endl;
        return;
    }

    // Write to disk
    std::lock_guard<std::mutex> lock(file_mutex_);

    std::string dir;
    if (date.empty()) {
        // Root file (e.g. STR.edf) -- write directly to data_dir_
        dir = data_dir_;
    } else {
        dir = data_dir_ + "/" + date;
        // Track active date folder (only for session files, not STR)
        active_date_folder_ = date;
    }
    std::filesystem::create_directories(dir);

    std::string filepath = dir + "/" + filename;
    std::ofstream file(filepath, std::ios::binary | std::ios::in | std::ios::out);
    if (!file.is_open()) {
        file.open(filepath, std::ios::binary | std::ios::out);
    }
    if (!file.is_open()) {
        std::cerr << "FysetcReceiver: Failed to open " << filepath << std::endl;
        return;
    }

    file.seekp(static_cast<std::streamoff>(offset));
    file.write(reinterpret_cast<const char*>(decoded.data()), decoded_len);
    file.close();
}

// ---------------------------------------------------------------------------
// onManifest: FYSETC publishes full file listing after post-therapy upload.
// We diff against local files and request anything missing via cmd/fetch.
//
// JSON: {"date":"20260315","files":{"filename.edf":size,...}}
// ---------------------------------------------------------------------------
void FysetcReceiverService::onManifest(const std::string& /*topic*/,
                                        const std::string& payload) {
    Json::Value root;
    Json::CharReaderBuilder builder;
    std::istringstream ss(payload);
    std::string errors;
    if (!Json::parseFromStream(builder, ss, &root, &errors)) {
        std::cerr << "FysetcReceiver: Failed to parse manifest: " << errors << std::endl;
        return;
    }

    std::string date = root.get("date", "").asString();
    const Json::Value& files = root["files"];
    if (date.empty() || !files.isObject()) {
        std::cerr << "FysetcReceiver: manifest missing 'date' or 'files'" << std::endl;
        return;
    }

    // Store manifest for transfer completion tracking
    std::map<std::string, uint64_t> remote_files;
    for (const auto& name : files.getMemberNames()) {
        remote_files[name] = files[name].asUInt64();
    }

    {
        std::lock_guard<std::mutex> lock(file_mutex_);
        manifest_date_ = date;
        manifest_sizes_ = remote_files;
    }

    std::cout << "FysetcReceiver: Manifest for " << date
              << " (" << remote_files.size() << " files on SD)" << std::endl;

    // Diff against local files and request missing data
    requestMissingFiles(date, remote_files);
}

// ---------------------------------------------------------------------------
// requestMissingFiles: Compare manifest with local files, send cmd/fetch
// for anything we don't have or have incomplete.
// ---------------------------------------------------------------------------
void FysetcReceiverService::requestMissingFiles(
    const std::string& date,
    const std::map<std::string, uint64_t>& remote_files) {

    std::string dir = data_dir_ + "/" + date;

    Json::Value fetch_files(Json::arrayValue);
    int needed = 0;

    for (const auto& [filename, remote_size] : remote_files) {
        uint64_t local_size = 0;
        std::filesystem::path filepath = std::filesystem::path(dir) / filename;
        if (std::filesystem::exists(filepath)) {
            local_size = std::filesystem::file_size(filepath);
        }

        if (local_size < remote_size) {
            Json::Value entry;
            entry["f"] = filename;
            entry["o"] = static_cast<Json::UInt64>(local_size);
            fetch_files.append(entry);
            needed++;

            std::cout << "FysetcReceiver: Need " << filename
                      << " from offset " << local_size
                      << " (remote: " << remote_size << ")" << std::endl;
        }
    }

    if (needed == 0) {
        std::cout << "FysetcReceiver: All files complete for " << date << std::endl;
        return;
    }

    // Publish cmd/fetch
    Json::Value request;
    request["date"] = date;
    request["files"] = fetch_files;

    Json::StreamWriterBuilder wbuilder;
    wbuilder["indentation"] = "";
    std::string request_str = Json::writeString(wbuilder, request);

    mqtt_->publish(topic_prefix_ + "/cmd/fetch", request_str, 1, false);

    std::cout << "FysetcReceiver: Requested " << needed
              << " file(s) from FYSETC" << std::endl;
}

// ---------------------------------------------------------------------------
// onSessionActive: FYSETC signals therapy session ON/OFF (retained)
//
// Session ON = FYSETC confirmed BRP file growing (real therapy, not boot).
// Session OFF = post-therapy upload + manifest complete. By this point:
//   - BRP/PLD/SAD/EVE/CSL are at final size (written during therapy)
//   - STR.edf was updated immediately after mask-off (daily summary)
//   - Manifest has been published and any cmd/fetch fulfilled
// ---------------------------------------------------------------------------
void FysetcReceiverService::onSessionActive(const std::string& /*topic*/,
                                             const std::string& payload) {
    if (payload == "ON") {
        std::cout << "FysetcReceiver: Session started (BRP confirmed)" << std::endl;
        mqtt_->publish("cpap/" + device_id_ + "/realtime/session_active", "ON", 1, true);
    } else if (payload == "OFF") {
        std::cout << "FysetcReceiver: Session ended, processing..." << std::endl;
        processCompletedSession();
        mqtt_->publish("cpap/" + device_id_ + "/realtime/session_active", "OFF", 1, true);
    }
}

// ---------------------------------------------------------------------------
// processCompletedSession: Parse EDF files and publish via existing pipeline
//
// By the time we get here (session_active=OFF), the ResMed write sequence is:
//   During therapy: BRP/PLD/SAD/EVE/CSL all written continuously
//   After mask-off: STR.edf updated (daily therapy summary)
//   Then bus goes quiet, FYSETC uploads + publishes manifest
//
// So all files should be at their final sizes. We:
// 1. Parse BRP/PLD/SAD/EVE/CSL -> session metrics -> MQTT + DB
// 2. Mark session completed, publish nightly metrics
// 3. Process STR.edf if present (daily summary + LLM AI summary input)
// ---------------------------------------------------------------------------
void FysetcReceiverService::processCompletedSession() {
    std::string date_folder = active_date_folder_;
    if (date_folder.empty()) {
        auto now = std::chrono::system_clock::now();
        auto t = std::chrono::system_clock::to_time_t(now);
        std::tm tm = *std::localtime(&t);
        char buf[9];
        std::strftime(buf, sizeof(buf), "%Y%m%d", &tm);
        date_folder = buf;
    }

    std::string session_dir = data_dir_ + "/" + date_folder;
    if (!std::filesystem::exists(session_dir)) {
        std::cerr << "FysetcReceiver: No data directory at " << session_dir << std::endl;
        return;
    }

    // Check transfer completeness against manifest
    {
        std::lock_guard<std::mutex> lock(file_mutex_);
        if (!manifest_sizes_.empty() && manifest_date_ == date_folder) {
            for (const auto& [filename, remote_size] : manifest_sizes_) {
                std::filesystem::path filepath =
                    std::filesystem::path(session_dir) / filename;
                uint64_t local_size = 0;
                if (std::filesystem::exists(filepath)) {
                    local_size = std::filesystem::file_size(filepath);
                }
                if (local_size < remote_size) {
                    std::cerr << "FysetcReceiver: WARNING: " << filename
                              << " incomplete (" << local_size << "/"
                              << remote_size << "), parsing anyway" << std::endl;
                }
            }
        }
    }

    // Group files into sessions using existing logic
    auto sessions = SessionDiscoveryService::groupLocalFolder(session_dir, date_folder);
    if (sessions.empty()) {
        std::cerr << "FysetcReceiver: No sessions found in " << session_dir << std::endl;
        return;
    }

    std::cout << "FysetcReceiver: Found " << sessions.size()
              << " session group(s) in " << date_folder << std::endl;

    for (const auto& session : sessions) {
        // Create temp directory with only this session's files (symlinks)
        std::string temp_dir = "/tmp/cpap_fysetc/" + date_folder + "_" + session.session_prefix;
        std::filesystem::create_directories(temp_dir);

        for (const auto& entry : std::filesystem::directory_iterator(temp_dir)) {
            std::filesystem::remove(entry.path());
        }

        auto symlinkFile = [&](const std::string& filename) {
            std::filesystem::path src = std::filesystem::path(session_dir) / filename;
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

        // Parse all EDF files (BRP/PLD/SAD/EVE/CSL are complete by now)
        auto parsed = EDFParser::parseSession(
            temp_dir, device_id_, device_name_, session.session_start);

        if (!parsed) {
            std::cerr << "FysetcReceiver: Failed to parse session "
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

        // Publish to MQTT + save to DB
        if (publisher_->publishSession(*parsed)) {
            double hours = parsed->duration_seconds.value_or(0) / 3600.0;
            double ahi = parsed->metrics.has_value() ? parsed->metrics->ahi : 0.0;
            std::cout << "FysetcReceiver: Published session " << session.session_prefix
                      << " (" << std::fixed << std::setprecision(1) << hours << "h"
                      << ", AHI=" << std::setprecision(2) << ahi << ")" << std::endl;

            // Mark completed + publish nightly aggregated metrics
            if (parsed->session_start.has_value()) {
                db_->markSessionCompleted(device_id_, *parsed->session_start);
                auto nightly = db_->getNightlyMetrics(device_id_, *parsed->session_start);
                if (nightly.has_value()) {
                    publisher_->publishHistoricalState(nightly.value());
                }
                publisher_->publishSessionCompleted();
            }
        } else {
            std::cerr << "FysetcReceiver: Failed to publish session "
                      << session.session_prefix << std::endl;
        }

        std::filesystem::remove_all(temp_dir);
    }

    // Process STR.edf if present -- written immediately after mask-off,
    // contains daily therapy summary needed for LLM AI summary.
    auto str_record = processSTRFile(date_folder);

    // Generate LLM summary (non-fatal) -- needs nightly metrics + STR
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

    // Archive files to permanent storage
    std::string archive_dest = archive_dir_ + "/" + date_folder;
    std::filesystem::create_directories(archive_dest);

    for (const auto& entry : std::filesystem::directory_iterator(session_dir)) {
        if (entry.is_regular_file()) {
            std::filesystem::path dest = std::filesystem::path(archive_dest) / entry.path().filename();
            std::error_code ec;
            std::filesystem::copy_file(entry.path(), dest,
                                       std::filesystem::copy_options::overwrite_existing, ec);
            if (ec) {
                std::cerr << "FysetcReceiver: Failed to archive "
                          << entry.path().filename() << ": " << ec.message() << std::endl;
            }
        }
    }

    std::cout << "FysetcReceiver: Archived to " << archive_dest << std::endl;

    // Clear manifest state
    {
        std::lock_guard<std::mutex> lock(file_mutex_);
        manifest_sizes_.clear();
        manifest_date_.clear();
    }

    std::filesystem::remove_all("/tmp/cpap_fysetc");
}

// ---------------------------------------------------------------------------
// processSTRFile: Parse STR.edf and save daily records + publish to MQTT.
//
// STR.edf lives in /DATALOG/STR.edf on the SD card (root, not date folder).
// FYSETC's manifest only covers date folders, so STR is NOT in the manifest.
// We check if it was fetched separately (via cmd/fetch_root or manual copy).
//
// Timing: The AirSense 10 updates STR.edf after mask-off, but the exact
// delay is unknown (could be immediate or after an internal timeout).
// If STR isn't available yet, we skip -- it can be backfilled later
// via hms_cpap --backfill or the next session cycle.
// ---------------------------------------------------------------------------
std::optional<STRDailyRecord> FysetcReceiverService::processSTRFile(const std::string& date_folder) {
    // Check multiple possible locations for STR.edf
    std::vector<std::string> candidates = {
        data_dir_ + "/STR.edf",
        data_dir_ + "/STR.EDF",
        data_dir_ + "/" + date_folder + "/STR.edf",
        data_dir_ + "/" + date_folder + "/STR.EDF",
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
        std::cout << "FysetcReceiver: STR.edf not available "
                  << "(lives in SD root, sent separately by FYSETC). "
                  << "Use --backfill to process later." << std::endl;
        return std::nullopt;
    }

    auto records = EDFParser::parseSTRFile(str_path, device_id_);
    if (records.empty()) {
        std::cerr << "FysetcReceiver: No therapy days found in STR.edf" << std::endl;
        return std::nullopt;
    }

    // Save last 7 days (same as BurstCollector)
    size_t save_count = std::min(records.size(), static_cast<size_t>(7));
    std::vector<STRDailyRecord> recent(records.end() - save_count, records.end());

    if (db_->saveSTRDailyRecords(recent)) {
        std::cout << "FysetcReceiver: Saved " << save_count
                  << " STR daily record(s)" << std::endl;

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
            std::cout << "FysetcReceiver: STR published (AHI="
                      << std::fixed << std::setprecision(2) << latest.ahi << ")" << std::endl;
        }

        return records.back();
    }

    return std::nullopt;
}

void FysetcReceiverService::generateAndPublishSummary(const SessionMetrics& metrics,
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

    if (publisher_) {
        publisher_->publishSessionSummary(summary.value());
    }
}

std::string FysetcReceiverService::buildMetricsString(const SessionMetrics& metrics,
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

    if (metrics.avg_event_duration.has_value()) {
        oss << "Avg event duration: " << metrics.avg_event_duration.value() << "s";
        if (metrics.max_event_duration.has_value()) {
            oss << ", max: " << metrics.max_event_duration.value() << "s";
        }
        oss << "\n";
    }

    if (metrics.avg_pressure.has_value() || metrics.pressure_p95.has_value()) {
        oss << "Pressure:";
        if (metrics.avg_pressure.has_value()) oss << " avg=" << metrics.avg_pressure.value() << " cmH2O";
        if (metrics.min_pressure.has_value()) oss << ", min=" << metrics.min_pressure.value() << " cmH2O";
        if (metrics.max_pressure.has_value()) oss << ", max=" << metrics.max_pressure.value() << " cmH2O";
        if (metrics.pressure_p95.has_value()) oss << ", 95th=" << metrics.pressure_p95.value() << " cmH2O";
        oss << "\n";
    }

    if (metrics.avg_leak_rate.has_value() || metrics.max_leak_rate.has_value()) {
        oss << "Leak:";
        if (metrics.avg_leak_rate.has_value()) oss << " avg=" << metrics.avg_leak_rate.value() << " L/min";
        if (metrics.max_leak_rate.has_value()) oss << ", max=" << metrics.max_leak_rate.value() << " L/min";
        oss << "\n";
    }

    if (metrics.avg_respiratory_rate.has_value())
        oss << "Respiratory rate: " << metrics.avg_respiratory_rate.value() << " breaths/min\n";
    if (metrics.avg_tidal_volume.has_value())
        oss << "Tidal volume: " << metrics.avg_tidal_volume.value() << " mL\n";
    if (metrics.avg_minute_ventilation.has_value())
        oss << "Minute ventilation: " << metrics.avg_minute_ventilation.value() << " L/min\n";
    if (metrics.avg_flow_limitation.has_value())
        oss << "Flow limitation: " << metrics.avg_flow_limitation.value() << " (0-1 scale)\n";

    if (str_record) {
        oss << "\nResMed official daily summary:\n";
        oss << "  STR AHI: " << str_record->ahi << " events/hour\n";
        oss << "  Mask events: " << (str_record->mask_events / 2) << " (on/off pairs)\n";
        oss << "  95th leak: " << str_record->leak_95 << " L/min\n";
        oss << "  95th pressure: " << str_record->mask_press_95 << " cmH2O\n";
    }

    return oss.str();
}

} // namespace hms_cpap
