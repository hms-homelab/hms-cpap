#ifdef BUILD_WITH_WEB

#include "controllers/CpapController.h"
#include "utils/AppConfig.h"
#include <filesystem>
#include <regex>
#include <sstream>
#ifdef WITH_BLE
#include <sdbus-c++/sdbus-c++.h>
#endif

namespace hms_cpap {

std::shared_ptr<QueryService> CpapController::qs_;
hms_cpap::AppConfig* CpapController::config_ = nullptr;
std::string CpapController::config_path_;
BurstCollectorService* CpapController::burst_service_ = nullptr;
std::function<void()> CpapController::ml_train_trigger_;
std::function<Json::Value()> CpapController::ml_status_getter_;
std::function<void(const std::string&, const std::string&, const std::string&)> CpapController::backfill_trigger_;
std::function<Json::Value()> CpapController::backfill_status_getter_;
std::function<Json::Value()> CpapController::sleep_stage_status_getter_;

void CpapController::setQueryService(std::shared_ptr<QueryService> qs) { qs_ = qs; }

void CpapController::setConfig(hms_cpap::AppConfig* cfg, const std::string& path) {
    config_ = cfg;
    config_path_ = path;
}

void CpapController::setBurstService(BurstCollectorService* svc) { burst_service_ = svc; }

static drogon::HttpResponsePtr jsonError(const std::string& msg, drogon::HttpStatusCode code) {
    auto resp = drogon::HttpResponse::newHttpResponse();
    resp->setContentTypeCode(drogon::CT_APPLICATION_JSON);
    resp->setStatusCode(code);
    resp->setBody("{\"error\":\"" + msg + "\"}");
    return resp;
}

// Bypass Drogon's newHttpJsonResponse (crashes in cross-compiled ARM binary)
static drogon::HttpResponsePtr jsonResp(const Json::Value& val) {
    Json::StreamWriterBuilder wb;
    wb["indentation"] = "";
    auto resp = drogon::HttpResponse::newHttpResponse();
    resp->setContentTypeCode(drogon::CT_APPLICATION_JSON);
    resp->setBody(Json::writeString(wb, val));
    return resp;
}

void CpapController::health(const drogon::HttpRequestPtr&,
                             std::function<void(const drogon::HttpResponsePtr&)>&& cb) {
    Json::Value j;
    j["status"] = "ok";
    j["version"] = "4.0.5";
    j["service"] = "hms-cpap";
    cb(jsonResp(j));
}

void CpapController::dashboard(const drogon::HttpRequestPtr&,
                                std::function<void(const drogon::HttpResponsePtr&)>&& cb) {
    try {
        cb(jsonResp(qs_->getDashboard()));
    } catch (const std::exception& e) {
        cb(jsonError(e.what(), drogon::k500InternalServerError));
    }
}

void CpapController::sessions(const drogon::HttpRequestPtr& req,
                               std::function<void(const drogon::HttpResponsePtr&)>&& cb) {
    int days = 30, limit = 20;
    if (auto p = req->getOptionalParameter<int>("days")) days = *p;
    if (auto p = req->getOptionalParameter<int>("limit")) limit = *p;
    try {
        cb(jsonResp(qs_->getSessions(days, limit)));
    } catch (const std::exception& e) {
        cb(jsonError(e.what(), drogon::k500InternalServerError));
    }
}

void CpapController::sessionDetail(const drogon::HttpRequestPtr&,
                                    std::function<void(const drogon::HttpResponsePtr&)>&& cb,
                                    const std::string& date) {
    try {
        cb(jsonResp(qs_->getSessionDetail(date)));
    } catch (const std::exception& e) {
        cb(jsonError(e.what(), drogon::k500InternalServerError));
    }
}

void CpapController::dailySummary(const drogon::HttpRequestPtr& req,
                                   std::function<void(const drogon::HttpResponsePtr&)>&& cb) {
    std::string start = req->getParameter("start");
    std::string end = req->getParameter("end");
    if (start.empty() || end.empty()) {
        cb(jsonError("start and end params required", drogon::k400BadRequest));
        return;
    }
    try {
        cb(jsonResp(qs_->getDailySummary(start, end)));
    } catch (const std::exception& e) {
        cb(jsonError(e.what(), drogon::k500InternalServerError));
    }
}

void CpapController::trend(const drogon::HttpRequestPtr& req,
                            std::function<void(const drogon::HttpResponsePtr&)>&& cb,
                            const std::string& metric) {
    int days = 30;
    if (auto p = req->getOptionalParameter<int>("days")) days = *p;
    try {
        cb(jsonResp(qs_->getTrend(metric, days)));
    } catch (const std::exception& e) {
        cb(jsonError(e.what(), drogon::k500InternalServerError));
    }
}

void CpapController::statistics(const drogon::HttpRequestPtr& req,
                                 std::function<void(const drogon::HttpResponsePtr&)>&& cb) {
    std::string start = req->getParameter("start");
    std::string end = req->getParameter("end");
    if (start.empty() || end.empty()) {
        cb(jsonError("start and end params required", drogon::k400BadRequest));
        return;
    }
    try {
        cb(jsonResp(qs_->getStatistics(start, end)));
    } catch (const std::exception& e) {
        cb(jsonError(e.what(), drogon::k500InternalServerError));
    }
}

void CpapController::summaries(const drogon::HttpRequestPtr& req,
                                std::function<void(const drogon::HttpResponsePtr&)>&& cb) {
    std::string period = req->getParameter("period");
    int limit = 10;
    if (auto p = req->getOptionalParameter<int>("limit")) limit = *p;
    try {
        cb(jsonResp(qs_->getSummaries(period, limit)));
    } catch (const std::exception& e) {
        cb(jsonError(e.what(), drogon::k500InternalServerError));
    }
}

void CpapController::sessionSignals(const drogon::HttpRequestPtr&,
                                    std::function<void(const drogon::HttpResponsePtr&)>&& cb,
                                    const std::string& date) {
    try {
        cb(jsonResp(qs_->getSessionSignals(date)));
    } catch (const std::exception& e) {
        cb(jsonError(e.what(), drogon::k500InternalServerError));
    }
}

void CpapController::sessionVitals(const drogon::HttpRequestPtr& req,
                                   std::function<void(const drogon::HttpResponsePtr&)>&& cb,
                                   const std::string& date) {
    int interval = 30;
    if (auto p = req->getOptionalParameter<int>("interval")) interval = *p;
    try {
        cb(jsonResp(qs_->getSessionVitals(date, interval)));
    } catch (const std::exception& e) {
        cb(jsonError(e.what(), drogon::k500InternalServerError));
    }
}

void CpapController::sessionEvents(const drogon::HttpRequestPtr&,
                                   std::function<void(const drogon::HttpResponsePtr&)>&& cb,
                                   const std::string& date) {
    try {
        cb(jsonResp(qs_->getSessionEvents(date)));
    } catch (const std::exception& e) {
        cb(jsonError(e.what(), drogon::k500InternalServerError));
    }
}

void CpapController::sessionOximetry(const drogon::HttpRequestPtr& req,
                                      std::function<void(const drogon::HttpResponsePtr&)>&& cb,
                                      const std::string& date) {
    int interval = 4;
    if (auto p = req->getOptionalParameter<int>("interval")) interval = *p;
    try {
        cb(jsonResp(qs_->getSessionOximetry(date, interval)));
    } catch (const std::exception& e) {
        cb(jsonError(e.what(), drogon::k500InternalServerError));
    }
}

void CpapController::realtime(const drogon::HttpRequestPtr&,
                               std::function<void(const drogon::HttpResponsePtr&)>&& cb) {
    Json::Value result;

    // Live session from most recent session
    if (qs_) {
        try {
            auto sessions = qs_->getSessions(1, 1);
            if (!sessions.empty() && sessions[0].isMember("has_live") &&
                sessions[0]["has_live"].asString() == "1") {
                result["session"] = sessions[0];
            }
        } catch (...) {}
    }

    // O2Ring live data from BurstCollectorService
    if (burst_service_ && burst_service_->getOximetryService()) {
        auto live = burst_service_->getOximetryService()->getLastLive();
        Json::Value oxi;
        oxi["active"] = live.active;
        oxi["spo2"] = live.spo2;
        oxi["hr"] = live.hr;
        oxi["motion"] = live.motion;
        oxi["valid"] = live.valid;
        result["oximetry"] = oxi;
    }

    cb(jsonResp(result));
}

void CpapController::getConfig(const drogon::HttpRequestPtr&,
                                std::function<void(const drogon::HttpResponsePtr&)>&& cb) {
    if (!config_) {
        cb(jsonError("Config not initialized", drogon::k500InternalServerError));
        return;
    }
    auto resp_json = config_->toJson();
    Json::Value result;
    Json::CharReaderBuilder builder;
    std::string errs;
    std::istringstream stream(resp_json.dump());
    Json::parseFromStream(builder, stream, &result, &errs);
    cb(jsonResp(result));
}

void CpapController::updateConfig(const drogon::HttpRequestPtr& req,
                                   std::function<void(const drogon::HttpResponsePtr&)>&& cb) {
    if (!config_) {
        cb(jsonError("Config not initialized", drogon::k500InternalServerError));
        return;
    }
    auto body = req->getJsonObject();
    if (!body) {
        cb(jsonError("Invalid JSON body", drogon::k400BadRequest));
        return;
    }
    auto& j = *body;

    // Update fields if present
    if (j.isMember("device_id")) config_->device_id = j["device_id"].asString();
    if (j.isMember("device_name")) config_->device_name = j["device_name"].asString();
    if (j.isMember("source")) config_->source = j["source"].asString();
    if (j.isMember("ezshare_url")) config_->ezshare_url = j["ezshare_url"].asString();
    if (j.isMember("ezshare_range")) config_->ezshare_range = j["ezshare_range"].asBool();
    if (j.isMember("local_dir")) config_->local_dir = j["local_dir"].asString();
    if (j.isMember("burst_interval")) config_->burst_interval = j["burst_interval"].asInt();

    if (j.isMember("database")) {
        auto& d = j["database"];
        if (d.isMember("type")) config_->database.type = d["type"].asString();
        if (d.isMember("sqlite_path")) config_->database.sqlite_path = d["sqlite_path"].asString();
        if (d.isMember("host")) config_->database.host = d["host"].asString();
        if (d.isMember("port")) config_->database.port = d["port"].asInt();
        if (d.isMember("name")) config_->database.name = d["name"].asString();
        if (d.isMember("user")) config_->database.user = d["user"].asString();
        if (d.isMember("password") && d["password"].asString() != "********")
            config_->database.password = d["password"].asString();
    }

    if (j.isMember("mqtt")) {
        auto& m = j["mqtt"];
        if (m.isMember("enabled")) config_->mqtt.enabled = m["enabled"].asBool();
        if (m.isMember("broker")) config_->mqtt.broker = m["broker"].asString();
        if (m.isMember("port")) config_->mqtt.port = m["port"].asInt();
        if (m.isMember("username")) config_->mqtt.username = m["username"].asString();
        if (m.isMember("password") && m["password"].asString() != "********")
            config_->mqtt.password = m["password"].asString();
    }

    if (j.isMember("llm")) {
        auto& l = j["llm"];
        if (l.isMember("enabled")) config_->llm.enabled = l["enabled"].asBool();
        if (l.isMember("provider")) config_->llm.provider = l["provider"].asString();
        if (l.isMember("endpoint")) config_->llm.endpoint = l["endpoint"].asString();
        if (l.isMember("model")) config_->llm.model = l["model"].asString();
        if (l.isMember("api_key") && l["api_key"].asString() != "********")
            config_->llm.api_key = l["api_key"].asString();
    }

    if (j.isMember("ml_training")) {
        auto& ml = j["ml_training"];
        if (ml.isMember("enabled")) config_->ml_training.enabled = ml["enabled"].asBool();
        if (ml.isMember("schedule")) config_->ml_training.schedule = ml["schedule"].asString();
        if (ml.isMember("model_dir")) config_->ml_training.model_dir = ml["model_dir"].asString();
        if (ml.isMember("min_days")) config_->ml_training.min_days = ml["min_days"].asInt();
        if (ml.isMember("max_training_days")) config_->ml_training.max_training_days = ml["max_training_days"].asInt();
    }

    if (j.isMember("o2ring")) {
        auto& o = j["o2ring"];
        if (o.isMember("enabled")) config_->o2ring.enabled = o["enabled"].asBool();
        if (o.isMember("mode")) config_->o2ring.mode = o["mode"].asString();
        if (o.isMember("mule_url")) config_->o2ring.mule_url = o["mule_url"].asString();
    }

    // Save to disk
    config_->save(config_path_);

    // Signal hot-reload to BurstCollectorService
    if (burst_service_) burst_service_->markConfigDirty();

    // Return redacted config
    auto resp_json = config_->toJson();
    Json::Value result;
    Json::CharReaderBuilder builder;
    std::string errs;
    std::istringstream stream(resp_json.dump());
    Json::parseFromStream(builder, stream, &result, &errs);

    cb(jsonResp(result));
}

void CpapController::setupComplete(const drogon::HttpRequestPtr&,
                                    std::function<void(const drogon::HttpResponsePtr&)>&& cb) {
    if (!config_) {
        cb(jsonError("Config not initialized", drogon::k500InternalServerError));
        return;
    }
    config_->setup_complete = true;
    config_->save(config_path_);

    Json::Value result;
    result["status"] = "ok";
    result["setup_complete"] = true;
    cb(jsonResp(result));
}

void CpapController::testEzshare(const drogon::HttpRequestPtr& req,
                                  std::function<void(const drogon::HttpResponsePtr&)>&& cb) {
    std::string url = req->getParameter("url");
    if (url.empty() && config_) url = config_->ezshare_url;

    Json::Value result;
    result["url"] = url;
    result["configured"] = !url.empty();
    result["status"] = url.empty() ? "not_configured" : "configured";
    cb(jsonResp(result));
}

void CpapController::testBle(const drogon::HttpRequestPtr&,
                              std::function<void(const drogon::HttpResponsePtr&)>&& cb) {
    Json::Value result;
#ifdef WITH_BLE
    result["compiled"] = true;
    // Check if BlueZ adapter exists via D-Bus
    try {
        auto conn = sdbus::createSystemBusConnection();
        auto proxy = sdbus::createProxy(*conn, sdbus::ServiceName{"org.bluez"}, sdbus::ObjectPath{"/"});
        std::map<sdbus::ObjectPath, std::map<std::string, std::map<std::string, sdbus::Variant>>> objects;
        proxy->callMethod("GetManagedObjects")
            .onInterface("org.freedesktop.DBus.ObjectManager")
            .storeResultsTo(objects);
        bool found = false;
        for (auto& [path, ifaces] : objects) {
            if (ifaces.count("org.bluez.Adapter1")) {
                result["adapter"] = std::string(path);
                found = true;
                break;
            }
        }
        result["available"] = found;
        result["status"] = found ? "adapter_found" : "no_adapter";
    } catch (const std::exception& e) {
        result["available"] = false;
        result["status"] = "bluez_error";
        result["error"] = e.what();
    }
#else
    result["compiled"] = false;
    result["available"] = false;
    result["status"] = "not_compiled";
#endif
    cb(jsonResp(result));
}

void CpapController::triggerMlTrain(const drogon::HttpRequestPtr&,
                                    std::function<void(const drogon::HttpResponsePtr&)>&& cb) {
    if (ml_train_trigger_) {
        ml_train_trigger_();
        Json::Value result;
        result["status"] = "training_started";
        cb(jsonResp(result));
    } else {
        cb(jsonError("ML training not configured", drogon::k503ServiceUnavailable));
    }
}

void CpapController::mlStatus(const drogon::HttpRequestPtr&,
                               std::function<void(const drogon::HttpResponsePtr&)>&& cb) {
    if (ml_status_getter_) {
        cb(jsonResp(ml_status_getter_()));
    } else {
        Json::Value result;
        result["status"] = "not_configured";
        result["last_trained"] = "never";
        result["models"] = Json::Value(Json::arrayValue);
        cb(jsonResp(result));
    }
}

void CpapController::getLlmPrompt(const drogon::HttpRequestPtr&,
                                   std::function<void(const drogon::HttpResponsePtr&)>&& cb) {
    auto resolve_prompt_path = [this]() -> std::string {
        if (config_ && !config_->llm.prompt_file.empty())
            return config_->llm.prompt_file;
        const char* env_path = std::getenv("LLM_PROMPT_FILE");
        if (env_path) return env_path;
        const char* home = std::getenv("HOME");
        return std::string(home ? home : ".") + "/.hms-cpap/llm_prompt.txt";
    };

    std::string path = resolve_prompt_path();

    Json::Value result;
    result["path"] = path;
    try {
        std::ifstream f(path);
        if (f.is_open()) {
            std::ostringstream ss;
            ss << f.rdbuf();
            result["prompt"] = ss.str();
        } else {
            result["prompt"] = "";
        }
    } catch (...) {
        result["prompt"] = "";
    }
    cb(jsonResp(result));
}

void CpapController::updateLlmPrompt(const drogon::HttpRequestPtr& req,
                                      std::function<void(const drogon::HttpResponsePtr&)>&& cb) {
    auto body = req->getJsonObject();
    if (!body || !(*body).isMember("prompt")) {
        cb(jsonError("Missing 'prompt' field", drogon::k400BadRequest));
        return;
    }

    auto resolve_prompt_path = [this]() -> std::string {
        if (config_ && !config_->llm.prompt_file.empty())
            return config_->llm.prompt_file;
        const char* env_path = std::getenv("LLM_PROMPT_FILE");
        if (env_path) return env_path;
        const char* home = std::getenv("HOME");
        return std::string(home ? home : ".") + "/.hms-cpap/llm_prompt.txt";
    };
    std::string path = resolve_prompt_path();

    try {
        std::ofstream f(path);
        f << (*body)["prompt"].asString();
        Json::Value result;
        result["status"] = "saved";
        result["path"] = path;
        cb(jsonResp(result));
    } catch (const std::exception& e) {
        cb(jsonError(std::string("Failed to write prompt: ") + e.what(),
                     drogon::k500InternalServerError));
    }
}

// ── Backfill ────────────────────────────────────────────────────────────

void CpapController::triggerBackfill(const drogon::HttpRequestPtr& req,
                                     std::function<void(const drogon::HttpResponsePtr&)>&& cb) {
    if (!backfill_trigger_) {
        cb(jsonError("Backfill not available (source is not local)",
                     drogon::k503ServiceUnavailable));
        return;
    }

    std::string start_date, end_date;
    auto body = req->getJsonObject();
    if (body) {
        if (body->isMember("start_date")) start_date = (*body)["start_date"].asString();
        if (body->isMember("end_date"))   end_date = (*body)["end_date"].asString();
    }

    std::string local_dir = config_ ? config_->local_dir : "";
    backfill_trigger_(start_date, end_date, local_dir);

    Json::Value result;
    result["status"] = "backfill_started";
    cb(jsonResp(result));
}

void CpapController::backfillStatus(const drogon::HttpRequestPtr&,
                                    std::function<void(const drogon::HttpResponsePtr&)>&& cb) {
    if (backfill_status_getter_) {
        cb(jsonResp(backfill_status_getter_()));
    } else {
        Json::Value result;
        result["status"] = "not_available";
        cb(jsonResp(result));
    }
}

void CpapController::backfillScan(const drogon::HttpRequestPtr&,
                                  std::function<void(const drogon::HttpResponsePtr&)>&& cb) {
    if (!config_) {
        cb(jsonError("Not configured", drogon::k503ServiceUnavailable));
        return;
    }

    std::string local_dir = config_->local_dir;
    if (local_dir.empty()) {
        cb(jsonError("No local_dir configured", drogon::k400BadRequest));
        return;
    }

    if (!std::filesystem::exists(local_dir)) {
        cb(jsonError("Directory not found: " + local_dir, drogon::k404NotFound));
        return;
    }

    // Scan YYYYMMDD folders
    std::vector<std::string> folders;
    std::regex date_re(R"(^\d{8}$)");

    for (const auto& entry : std::filesystem::directory_iterator(local_dir)) {
        if (!entry.is_directory()) continue;
        std::string name = entry.path().filename().string();
        if (std::regex_match(name, date_re)) {
            folders.push_back(name);
        }
    }

    if (folders.empty()) {
        Json::Value result;
        result["folders"] = 0;
        result["message"] = "No date folders found in " + local_dir;
        cb(jsonResp(result));
        return;
    }

    std::sort(folders.begin(), folders.end());

    // Convert YYYYMMDD to YYYY-MM-DD
    auto toIso = [](const std::string& f) -> std::string {
        return f.substr(0, 4) + "-" + f.substr(4, 2) + "-" + f.substr(6, 2);
    };

    Json::Value result;
    result["folders"] = static_cast<int>(folders.size());
    result["start_date"] = toIso(folders.front());
    result["end_date"] = toIso(folders.back());
    result["local_dir"] = local_dir;
    cb(jsonResp(result));
}

// ── Sleep Stages ───────────────────────────────────────────────────────────

void CpapController::sessionSleepStages(
        const drogon::HttpRequestPtr&,
        std::function<void(const drogon::HttpResponsePtr&)>&& cb,
        const std::string& date) {
    try {
        if (!qs_) {
            cb(jsonError("QueryService not initialized", drogon::k500InternalServerError));
            return;
        }

        // Validate date format (YYYY-MM-DD)
        if (date.size() != 10 || date[4] != '-' || date[7] != '-') {
            cb(jsonError("Invalid date format, expected YYYY-MM-DD", drogon::k400BadRequest));
            return;
        }

        // Query sleep stage epochs joined with sessions for this date.
        // The cpap_sleep_stages table uses session_id FK to cpap_sessions.
        std::string sql =
            "SELECT ss.epoch_start_ts, ss.epoch_duration_sec, ss.stage, "
            "ss.confidence, ss.provisional, ss.model_version "
            "FROM cpap_sleep_stages ss "
            "JOIN cpap_sessions s ON ss.session_id = s.id "
            "WHERE s.session_start::date = '" + date + "' "
            "ORDER BY ss.epoch_start_ts";

        // Use the raw DB connection via QueryService's db handle
        // Fall back to qs_ method if available, otherwise use static db_
        auto db = qs_->getDb();
        if (!db || !db->isConnected()) {
            cb(jsonError("Database not connected", drogon::k503ServiceUnavailable));
            return;
        }

        auto rows = db->executeQuery(sql);

        // Build epochs array
        Json::Value epochs(Json::arrayValue);
        int wake_epochs = 0, light_epochs = 0, deep_epochs = 0, rem_epochs = 0;
        int first_sleep = -1, first_rem = -1, first_deep = -1;
        std::string model_version;

        for (int i = 0; i < static_cast<int>(rows.size()); ++i) {
            const auto& row = rows[i];
            Json::Value e;
            e["epoch_start"] = row.get("epoch_start_ts", "").asString();
            e["epoch_duration_sec"] = row.get("epoch_duration_sec", "30").asInt();

            int stage = 0;
            auto stage_val = row.get("stage", "0");
            if (stage_val.isString()) {
                try { stage = std::stoi(stage_val.asString()); } catch (...) {}
            } else {
                stage = stage_val.asInt();
            }
            e["stage"] = stage;

            // Stage name
            const char* stage_names[] = {"Wake", "Light", "Deep", "REM"};
            e["stage_name"] = (stage >= 0 && stage <= 3) ? stage_names[stage] : "Unknown";

            double confidence = 0;
            auto conf_val = row.get("confidence", "0");
            if (conf_val.isString()) {
                try { confidence = std::stod(conf_val.asString()); } catch (...) {}
            } else {
                confidence = conf_val.asDouble();
            }
            e["confidence"] = confidence;

            bool provisional = false;
            auto prov_val = row.get("provisional", "false");
            if (prov_val.isString()) {
                provisional = (prov_val.asString() == "true" || prov_val.asString() == "1" ||
                               prov_val.asString() == "t");
            } else {
                provisional = prov_val.asBool();
            }
            e["provisional"] = provisional;

            epochs.append(e);

            // Accumulate for summary
            switch (stage) {
                case 0: ++wake_epochs; break;
                case 1: ++light_epochs; break;
                case 2: ++deep_epochs; break;
                case 3: ++rem_epochs; break;
            }
            if (first_sleep < 0 && stage != 0) first_sleep = i;
            if (first_rem < 0 && stage == 3) first_rem = i;
            if (first_deep < 0 && stage == 2) first_deep = i;

            if (model_version.empty()) {
                model_version = row.get("model_version", "").asString();
            }
        }

        // Build summary
        Json::Value summary;
        int total = static_cast<int>(rows.size());
        summary["wake_minutes"] = wake_epochs / 2;
        summary["light_minutes"] = light_epochs / 2;
        summary["deep_minutes"] = deep_epochs / 2;
        summary["rem_minutes"] = rem_epochs / 2;
        summary["total_epochs"] = total;

        int sleep_min = (light_epochs + deep_epochs + rem_epochs) / 2;
        int total_min = total / 2;
        summary["sleep_efficiency_pct"] = total_min > 0
            ? std::round(1000.0 * sleep_min / total_min) / 10.0 : 0.0;

        summary["rem_latency_min"] = (first_sleep >= 0 && first_rem >= 0)
            ? (first_rem - first_sleep) / 2 : 0;
        summary["first_deep_min"] = (first_sleep >= 0 && first_deep >= 0)
            ? (first_deep - first_sleep) / 2 : 0;
        summary["model_version"] = model_version;

        Json::Value result;
        result["date"] = date;
        result["epochs"] = epochs;
        result["summary"] = summary;
        cb(jsonResp(result));

    } catch (const std::exception& e) {
        cb(jsonError(e.what(), drogon::k500InternalServerError));
    }
}

void CpapController::sleepStageStatus(
        const drogon::HttpRequestPtr&,
        std::function<void(const drogon::HttpResponsePtr&)>&& cb) {
    if (sleep_stage_status_getter_) {
        cb(jsonResp(sleep_stage_status_getter_()));
    } else {
        Json::Value result;
        result["status"] = "not_configured";
        cb(jsonResp(result));
    }
}

void CpapController::insights(const drogon::HttpRequestPtr& req,
                               std::function<void(const drogon::HttpResponsePtr&)>&& cb) {
    int days = 90;
    if (auto p = req->getOptionalParameter<int>("days")) days = *p;
    try {
        cb(jsonResp(qs_->getInsights(days)));
    } catch (const std::exception& e) {
        cb(jsonError(e.what(), drogon::k500InternalServerError));
    }
}

} // namespace hms_cpap

#endif // BUILD_WITH_WEB
