#ifdef BUILD_WITH_WEB

#include "controllers/CpapController.h"
#include "utils/AppConfig.h"
#include "services/SleepHqExportService.h"
#include "services/DeviceDiscoveryService.h"
#include "services/SetupService.h"
#include "utils/CardLayout.h"
#include "utils/ConfigManager.h"
#include <drogon/MultiPart.h>
#include <atomic>
#include <filesystem>
#include <fstream>
#include <regex>
#include <sstream>
#include <thread>
#include <chrono>
#ifndef _WIN32
  #include <unistd.h>
#else
  #include <process.h>
#endif
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
std::function<Json::Value(const std::string&, const std::string&)> CpapController::oxi_csv_import_;
std::function<Json::Value(const std::string&)> CpapController::cpap_zip_import_;

void CpapController::setQueryService(std::shared_ptr<QueryService> qs) { qs_ = qs; }

void CpapController::setConfig(hms_cpap::AppConfig* cfg, const std::string& path) {
    config_ = cfg;
    config_path_ = path;
}

void CpapController::setBurstService(BurstCollectorService* svc) { burst_service_ = svc; }

// Defined here rather than beside report_svc_: that one lives inside the
// POSIX-only report block, and putting this there compiled everywhere except
// MSVC, where it failed with "sync_ undeclared". The cloud mirror is not
// platform-specific.
std::shared_ptr<CpapDashSyncService> CpapController::sync_;

void CpapController::setSyncService(std::shared_ptr<CpapDashSyncService> sync) {
    sync_ = std::move(sync);
}

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
    j["version"] = HMS_CPAP_VERSION;
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
    int limit = 20, offset = 0;
    if (auto p = req->getOptionalParameter<int>("limit")) limit = *p;
    if (auto p = req->getOptionalParameter<int>("offset")) offset = *p;
    if (limit < 1) limit = 20;
    if (offset < 0) offset = 0;
    try {
        cb(jsonResp(qs_->getSessions(limit, offset)));
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

void CpapController::sessionBreaths(const drogon::HttpRequestPtr&,
                                    std::function<void(const drogon::HttpResponsePtr&)>&& cb,
                                    const std::string& date) {
    try {
        cb(jsonResp(qs_->getSessionBreaths(date)));
    } catch (const std::exception& e) {
        cb(jsonError(e.what(), drogon::k500InternalServerError));
    }
}

void CpapController::events(const drogon::HttpRequestPtr& req,
                            std::function<void(const drogon::HttpResponsePtr&)>&& cb) {
    // SDD-009: cross-night event search. All filters optional; see the SDD
    // for the parameter contract.
    std::string start = req->getParameter("start");
    std::string end = req->getParameter("end");

    std::vector<std::string> types;
    std::istringstream ts(req->getParameter("types"));
    std::string t;
    while (std::getline(ts, t, ',')) {
        if (!t.empty()) types.push_back(t);
    }

    int min_duration = req->getOptionalParameter<int>("min_duration").value_or(0);
    int limit = req->getOptionalParameter<int>("limit").value_or(100);
    int offset = req->getOptionalParameter<int>("offset").value_or(0);
    limit = std::max(1, std::min(limit, 500));
    offset = std::max(0, offset);

    try {
        cb(jsonResp(qs_->getEvents(start, end, types, min_duration, limit, offset)));
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
            auto sessions = qs_->getSessions(1, 0);
            if (!sessions.empty()) {
                const auto& s0 = sessions[0];
                // SDD-008: the ledger-backed night_state is the authority on
                // liveness; has_live counts never-finalized segments, which is
                // what kept this banner up after the night had settled.
                const bool live =
                    s0.isMember("night_state") && !s0["night_state"].isNull()
                        ? s0["night_state"].asString() == "live"
                        : s0.isMember("has_live") &&
                              std::stoi(s0["has_live"].asString()) > 0;
                if (live) result["session"] = s0;
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
    // SDD-012: archive_dir was reachable only through setupApply, so it could be
    // set during the wizard and never again. That is the one setting a Mule and
    // Miner install REQUIRES (AppConfig.h), which left onboarded users editing
    // config.json by hand to move where their nights land.
    //
    // It is NOT part of BurstCollectorService's ConfigSnapshot, so
    // markConfigDirty() cannot carry it. Every consumer instead reads
    // CPAP_ARCHIVE_DIR through ConfigManager at use time, and main.cpp writes
    // that env var once at startup. Re-exporting it here is what makes a folder
    // changed in Settings take effect on the next burst rather than at the next
    // restart, which matters because this is the setting people change most.
    if (j.isMember("archive_dir")) {
        config_->archive_dir = j["archive_dir"].asString();
        if (!config_->archive_dir.empty()) {
#ifdef _WIN32
            _putenv_s("CPAP_ARCHIVE_DIR", config_->archive_dir.c_str());
#else
            setenv("CPAP_ARCHIVE_DIR", config_->archive_dir.c_str(), 1);
#endif
        }
    }
    // web_port only takes effect on the next start; Drogon's listener is already
    // bound by the time any request reaches here.
    if (j.isMember("web_port")) config_->web_port = j["web_port"].asInt();
    // Likewise resolved once at startup by SetupService::resolveStaticDir().
    if (j.isMember("static_dir")) config_->static_dir = j["static_dir"].asString();

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
        if (m.isMember("client_id")) config_->mqtt.client_id = m["client_id"].asString();
        if (m.isMember("password") && m["password"].asString() != "********")
            config_->mqtt.password = m["password"].asString();
    }

    if (j.isMember("llm")) {
        auto& l = j["llm"];
        if (l.isMember("enabled")) config_->llm.enabled = l["enabled"].asBool();
        if (l.isMember("provider")) config_->llm.provider = l["provider"].asString();
        if (l.isMember("endpoint")) config_->llm.endpoint = l["endpoint"].asString();
        if (l.isMember("model")) config_->llm.model = l["model"].asString();
        if (l.isMember("max_tokens")) config_->llm.max_tokens = std::max(1, l["max_tokens"].asInt());
        if (l.isMember("prompt_file")) config_->llm.prompt_file = l["prompt_file"].asString();
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

    if (j.isMember("agent")) {
        auto& a = j["agent"];
        if (a.isMember("enabled")) config_->agent.enabled = a["enabled"].asBool();
        if (a.isMember("embed_model")) config_->agent.embed_model = a["embed_model"].asString();
        if (a.isMember("temperature")) config_->agent.temperature = a["temperature"].asDouble();
        if (a.isMember("max_iterations")) config_->agent.max_iterations = a["max_iterations"].asInt();
    }

    if (j.isMember("sleep_stage")) {
        auto& ss = j["sleep_stage"];
        if (ss.isMember("enabled")) config_->sleep_stage.enabled = ss["enabled"].asBool();
        if (ss.isMember("live_inference")) config_->sleep_stage.live_inference = ss["live_inference"].asBool();
        if (ss.isMember("model_dir")) config_->sleep_stage.model_dir = ss["model_dir"].asString();
        if (ss.isMember("model_version")) config_->sleep_stage.model_version = ss["model_version"].asString();
    }

    if (j.isMember("cpapdash")) {
        auto& cd = j["cpapdash"];
        if (cd.isMember("enabled")) config_->cpapdash.enabled = cd["enabled"].asBool();
        if (cd.isMember("api_url")) config_->cpapdash.api_url = cd["api_url"].asString();
        if (cd.isMember("token") && cd["token"].asString() != "********")
            config_->cpapdash.token = cd["token"].asString();
        if (cd.isMember("auto_sync")) config_->cpapdash.auto_sync = cd["auto_sync"].asBool();
    }

    if (j.isMember("fysetc")) {
        auto& f = j["fysetc"];
        if (f.isMember("enabled")) config_->fysetc.enabled = f["enabled"].asBool();
        if (f.isMember("listen_port")) config_->fysetc.listen_port = f["listen_port"].asInt();
        if (f.isMember("listen_bind")) config_->fysetc.listen_bind = f["listen_bind"].asString();
        if (f.isMember("connection_timeout_s"))
            config_->fysetc.connection_timeout_s = f["connection_timeout_s"].asInt();
        if (f.isMember("archive_dir")) config_->fysetc.archive_dir = f["archive_dir"].asString();
        if (f.isMember("log_dir")) config_->fysetc.log_dir = f["log_dir"].asString();
    }

    if (j.isMember("sleephq")) {
        auto& sh = j["sleephq"];
        if (sh.isMember("enabled")) config_->sleephq.enabled = sh["enabled"].asBool();
        if (sh.isMember("client_id")) config_->sleephq.client_id = sh["client_id"].asString();
        if (sh.isMember("client_secret") && sh["client_secret"].asString() != "********")
            config_->sleephq.client_secret = sh["client_secret"].asString();
        if (sh.isMember("auto_on_session")) config_->sleephq.auto_on_session = sh["auto_on_session"].asBool();
        if (sh.isMember("auto_on_backfill")) config_->sleephq.auto_on_backfill = sh["auto_on_backfill"].asBool();
        if (sh.isMember("quiet_minutes")) config_->sleephq.quiet_minutes = std::max(1, sh["quiet_minutes"].asInt());
    }

    // Save to disk
    config_->save(config_path_);

    // Signal hot-reload to BurstCollectorService
    if (burst_service_) burst_service_->markConfigDirty();

    // SDD-006 section 5: the cloud mirror took its settings once, in main.cpp,
    // and nothing re-applied them. A token pasted into the Settings page
    // therefore reached the file and the in-memory config but never the running
    // service, so the next sync still used the old one. The wizard's restart hid
    // this; the Settings page does not restart.
    if (sync_) {
        sync_->setSettings(CpapDashSyncService::Settings::from(
            config_->cpapdash.enabled, config_->cpapdash.api_url,
            config_->cpapdash.token, config_->cpapdash.auto_sync));
    }

    // Return redacted config
    auto resp_json = config_->toJson();
    Json::Value result;
    Json::CharReaderBuilder builder;
    std::string errs;
    std::istringstream stream(resp_json.dump());
    Json::parseFromStream(builder, stream, &result, &errs);

    cb(jsonResp(result));
}

// ---------------------------------------------------------------------------
// SDD-006 phase 2: database provisioning and apply
//
// Thin passthroughs. Everything decidable lives in SetupService, because the
// test binary excludes src/controllers entirely, so logic placed here cannot be
// tested at all.
// ---------------------------------------------------------------------------

namespace {

/// All three endpoints refuse once setup is finished, so a completed install
/// does not leave database provisioning exposed on the LAN forever.
bool setupStillOpen(const hms_cpap::AppConfig* cfg) {
    return cfg && !cfg->setup_complete;
}

/// SDD-012: the restart half of setupApply, shared with POST /api/config/restart.
///
/// Extracted rather than duplicated because the two callers must agree on the
/// answer. The Settings page needs the same restart the wizard already performs,
/// and a second copy of the mode decision is a second thing to get wrong.
///
/// Answers the request FIRST and restarts on a detached thread afterwards, so
/// the 202 is flushed to the browser rather than dying with the process. The
/// caller supplies `result` so each endpoint can add its own fields.
void respondAndRestart(std::function<void(const drogon::HttpResponsePtr&)>&& cb,
                       Json::Value result) {
    const auto mode = SetupService::restartMode(SetupService::isSupervised(),
                                                SetupService::executablePath());

    result["restarting"] = (mode != SetupService::RestartMode::Unsupported);
    result["restart_mode"] =
        mode == SetupService::RestartMode::SupervisedExit ? "supervised"
      : mode == SetupService::RestartMode::ReExec        ? "reexec"
                                                         : "manual";
    if (mode == SetupService::RestartMode::Unsupported) {
        // Said plainly rather than pretended: a database change cannot take
        // effect without a restart, and if we cannot perform one the user has to.
        result["message"] = "Settings saved. Restart hms_cpap for them to take effect.";
    }

    auto resp = drogon::HttpResponse::newHttpJsonResponse(result);
    resp->setStatusCode(drogon::k202Accepted);
    cb(resp);

    if (mode == SetupService::RestartMode::Unsupported) return;

    // Only AFTER the response has been handed back, and on a detached thread, so
    // the 202 is actually flushed to the browser rather than dying with us.
    const std::string exe = SetupService::executablePath();
    std::thread([mode, exe]() {
        std::this_thread::sleep_for(std::chrono::milliseconds(750));
        if (mode == SetupService::RestartMode::SupervisedExit) {
            std::exit(0);            // the SDD-005 shell respawns us
        }
#ifdef _WIN32
        const char* argv[] = {exe.c_str(), nullptr};
        _execv(exe.c_str(), const_cast<char* const*>(argv));
#else
        char* const argv[] = {const_cast<char*>(exe.c_str()), nullptr};
        ::execv(exe.c_str(), argv);
#endif
        // execv only returns on failure, and by now the old process is no longer
        // serving usefully, so exiting lets a supervisor notice.
        std::exit(1);
    }).detach();
}

}  // namespace

void CpapController::setupTestDb(const drogon::HttpRequestPtr& req,
                                 std::function<void(const drogon::HttpResponsePtr&)>&& cb) {
    if (!setupStillOpen(config_)) {
        cb(jsonError("Setup is already complete", drogon::k403Forbidden));
        return;
    }
    auto body = req->getJsonObject();
    if (!body) { cb(jsonError("Invalid JSON body", drogon::k400BadRequest)); return; }
    auto& d = *body;

    const std::string type = d.get("type", "sqlite").asString();
    if (!SetupService::supportsBackend(type)) {
        cb(jsonError("This build cannot open '" + type + "'", drogon::k400BadRequest));
        return;
    }

    const auto probe = SetupService::probeDatabase(
        type, d.get("host", "").asString(), d.get("port", 0).asInt(),
        d.get("name", "").asString(), d.get("user", "").asString(),
        d.get("password", "").asString(), d.get("sqlite_path", "").asString());

    Json::Value result;
    result["ok"] = probe.ok;
    result["error"] = probe.error;
    result["schema_present"] = probe.schema_present;
    result["session_count"] = static_cast<Json::Int64>(probe.session_count);
    cb(drogon::HttpResponse::newHttpJsonResponse(result));
}

void CpapController::setupCreateDb(const drogon::HttpRequestPtr& req,
                                   std::function<void(const drogon::HttpResponsePtr&)>&& cb) {
    if (!setupStillOpen(config_)) {
        cb(jsonError("Setup is already complete", drogon::k403Forbidden));
        return;
    }
    auto body = req->getJsonObject();
    if (!body) { cb(jsonError("Invalid JSON body", drogon::k400BadRequest)); return; }
    auto& d = *body;

    const std::string type = d.get("type", "").asString();
    if (!SetupService::supportsBackend(type)) {
        cb(jsonError("This build cannot open '" + type + "'", drogon::k400BadRequest));
        return;
    }

    // Admin credentials are read here and never leave this scope: not written to
    // config.json, not echoed in the response, not logged.
    const auto result_probe = SetupService::provisionDatabase(
        type, d.get("host", "").asString(), d.get("port", 0).asInt(),
        d.get("name", "").asString(), d.get("user", "").asString(),
        d.get("password", "").asString(),
        d.get("admin_user", "").asString(), d.get("admin_password", "").asString());

    Json::Value result;
    result["ok"] = result_probe.ok;
    result["error"] = result_probe.error;
    result["schema_present"] = result_probe.schema_present;
    result["session_count"] = static_cast<Json::Int64>(result_probe.session_count);
    cb(drogon::HttpResponse::newHttpJsonResponse(result));
}

void CpapController::setupApply(const drogon::HttpRequestPtr& req,
                                std::function<void(const drogon::HttpResponsePtr&)>&& cb) {
    if (!config_) {
        cb(jsonError("Config not initialized", drogon::k500InternalServerError));
        return;
    }
    if (!setupStillOpen(config_)) {
        cb(jsonError("Setup is already complete", drogon::k403Forbidden));
        return;
    }
    auto body = req->getJsonObject();
    if (!body) { cb(jsonError("Invalid JSON body", drogon::k400BadRequest)); return; }
    auto& j = *body;

    if (j.isMember("database")) {
        auto& d = j["database"];
        const std::string type = d.get("type", "sqlite").asString();
        if (!SetupService::supportsBackend(type)) {
            cb(jsonError("This build cannot open '" + type + "'; refusing to write "
                         "a config that would stop the service from starting",
                         drogon::k400BadRequest));
            return;
        }
        config_->database.type = type;
        if (d.isMember("sqlite_path")) config_->database.sqlite_path = d["sqlite_path"].asString();
        if (d.isMember("host")) config_->database.host = d["host"].asString();
        if (d.isMember("port")) config_->database.port = d["port"].asInt();
        if (d.isMember("name")) config_->database.name = d["name"].asString();
        if (d.isMember("user")) config_->database.user = d["user"].asString();
        if (d.isMember("password")) config_->database.password = d["password"].asString();
    }
    if (j.isMember("source"))      config_->source = j["source"].asString();
    if (j.isMember("ezshare_url")) config_->ezshare_url = j["ezshare_url"].asString();
    if (j.isMember("local_dir"))   config_->local_dir = j["local_dir"].asString();
    if (j.isMember("archive_dir")) config_->archive_dir = j["archive_dir"].asString();

    // SDD-006 phase 3: the advanced groups. Each is applied ONLY when the wizard
    // sent it, because an untouched section must not overwrite what an upgrading
    // user already has in config.json.
    if (j.isMember("mqtt")) {
        auto& m = j["mqtt"];
        if (m.isMember("enabled"))  config_->mqtt.enabled  = m["enabled"].asBool();
        if (m.isMember("broker"))   config_->mqtt.broker   = m["broker"].asString();
        if (m.isMember("port"))     config_->mqtt.port     = m["port"].asInt();
        if (m.isMember("username")) config_->mqtt.username = m["username"].asString();
        if (m.isMember("password")) config_->mqtt.password = m["password"].asString();
    }
    if (j.isMember("llm")) {
        auto& l = j["llm"];
        if (l.isMember("enabled"))  config_->llm.enabled  = l["enabled"].asBool();
        if (l.isMember("provider")) config_->llm.provider = l["provider"].asString();
        if (l.isMember("endpoint")) config_->llm.endpoint = l["endpoint"].asString();
        if (l.isMember("model"))    config_->llm.model    = l["model"].asString();
    }
    if (j.isMember("ml_training") && j["ml_training"].isMember("enabled"))
        config_->ml_training.enabled = j["ml_training"]["enabled"].asBool();
    if (j.isMember("sleep_stage") && j["sleep_stage"].isMember("enabled"))
        config_->sleep_stage.enabled = j["sleep_stage"]["enabled"].asBool();
    if (j.isMember("cpapdash")) {
        auto& c = j["cpapdash"];
        if (c.isMember("enabled"))   config_->cpapdash.enabled   = c["enabled"].asBool();
        if (c.isMember("api_url"))   config_->cpapdash.api_url   = c["api_url"].asString();
        if (c.isMember("token"))     config_->cpapdash.token     = c["token"].asString();
        if (c.isMember("auto_sync")) config_->cpapdash.auto_sync = c["auto_sync"].asBool();
    }

    config_->setup_complete = true;
    config_->save(config_path_);

    Json::Value result;
    result["accepted"] = true;
    respondAndRestart(std::move(cb), result);
}

/// SDD-012: POST /api/config/restart
///
/// The Settings page counterpart to the wizard's restart. PUT /api/config
/// re-applies only the burst collector and the cloud mirror; every other
/// setting reaches the file and the in-memory config but never the running
/// service, and until now the page had no way to say so or fix it. setupApply
/// is gated by setupStillOpen(), so a completed install could not restart at
/// all.
///
/// This restarts and nothing else. It does not touch config, does not set
/// setup_complete, and does not reopen any of the SDD-006 provisioning surface
/// to a finished install.
void CpapController::configRestart(const drogon::HttpRequestPtr&,
                                   std::function<void(const drogon::HttpResponsePtr&)>&& cb) {
    Json::Value result;
    result["accepted"] = true;
    respondAndRestart(std::move(cb), result);
}

void CpapController::autostartState(const drogon::HttpRequestPtr&,
                                    std::function<void(const drogon::HttpResponsePtr&)>&& cb) {
    const bool supervised = SetupService::isSupervised();
    const bool containerised = SetupService::isContainerised();
    const auto owner = SetupService::autostartOwner(supervised, containerised);
    const auto entry = SetupService::autostartEntry(SetupService::executablePath(),
                                                    hms_cpap::AppConfig::homeDir());

    Json::Value result;
    result["can_manage"] = (owner == SetupService::AutostartOwner::Us);
    result["owner"] = SetupService::autostartOwnerString(owner);
    result["supervised"] = supervised;
    result["containerised"] = containerised;
    // Said rather than left blank: a user who cannot tick the box deserves to
    // know what IS keeping the service running, and what to do if nothing is.
    if (owner == SetupService::AutostartOwner::Container) {
        result["message"] =
            "Docker starts this service, so there is nothing to enable here. "
            "To survive a reboot, make sure the container uses "
            "restart: unless-stopped and that Docker itself starts at boot.";
    } else if (owner == SetupService::AutostartOwner::DesktopShell) {
        result["message"] = "Managed by CpapDash Desktop.";
    }
    result["supported"] = entry.error.empty();
    result["error"] = entry.error;

    bool installed = false;
#ifndef _WIN32
    if (!entry.path.empty()) {
        std::error_code ec;
        installed = std::filesystem::exists(entry.path, ec) && !ec;
    }
#endif
    result["installed"] = installed;
    cb(drogon::HttpResponse::newHttpJsonResponse(result));
}

void CpapController::setupAutostart(const drogon::HttpRequestPtr& req,
                                    std::function<void(const drogon::HttpResponsePtr&)>&& cb) {
    auto body = req->getJsonObject();
    const bool enable = body && body->isMember("enabled")
                        ? (*body)["enabled"].asBool() : true;

    const auto owner = SetupService::autostartOwner(SetupService::isSupervised(),
                                                    SetupService::isContainerised());
    if (owner != SetupService::AutostartOwner::Us) {
        // Not an error, a division of labour. Refusing with the reason beats
        // writing a systemd unit into a container that has no systemd, which
        // would silently look like it worked.
        cb(jsonError(owner == SetupService::AutostartOwner::Container
                         ? "Docker manages this service; use a restart policy instead"
                         : "Autostart is managed by CpapDash Desktop",
                     drogon::k409Conflict));
        return;
    }

    const bool boot = body && body->isMember("scope") &&
                      (*body)["scope"].asString() == "boot";
    const auto scope = boot ? SetupService::AutostartScope::Boot
                            : SetupService::AutostartScope::Login;

    const auto entry = SetupService::autostartEntry(SetupService::executablePath(),
                                                    hms_cpap::AppConfig::homeDir(),
                                                    scope);
    if (!entry.error.empty()) {
        cb(jsonError(entry.error, drogon::k400BadRequest));
        return;
    }

    Json::Value result;

    if (boot) {
        // A boot service lives somewhere only root can write, and this process
        // never elevates itself. So stage the generated file where the user can
        // reach it and hand back the exact command to run. Telling someone what
        // to run beats silently failing to write /Library or /etc.
        std::string staged;
        if (!entry.content.empty()) {
            staged = hms_cpap::AppConfig::dataDir() + "/" +
                     std::filesystem::path(entry.path).filename().string();
            try {
                std::filesystem::create_directories(hms_cpap::AppConfig::dataDir());
                std::ofstream out(staged);
                if (!out) throw std::runtime_error("cannot write " + staged);
                out << entry.content;
            } catch (const std::exception& e) {
                cb(jsonError(e.what(), drogon::k500InternalServerError));
                return;
            }
        }
        std::string cmd = enable ? entry.install_command : entry.uninstall_command;
        if (!staged.empty()) {
            const std::string ph = "<generated>";
            const auto pos = cmd.find(ph);
            if (pos != std::string::npos) cmd.replace(pos, ph.size(), staged);
        }

        result["ok"] = true;
        result["scope"] = "boot";
        result["needs_elevation"] = entry.needs_elevation;
        result["staged_file"] = staged;
        result["command"] = cmd;
        result["target_path"] = entry.path;
        result["requires_helper"] = entry.requires_helper;
        result["alternative_command"] = entry.alternative_command;
        result["note"] = "Starts at boot, before anyone logs in. Run the command "
                         "above once; it needs administrator rights.";
        cb(drogon::HttpResponse::newHttpJsonResponse(result));
        return;
    }
#ifdef _WIN32
    // Registry work is left to the installer rather than hand-rolled here; the
    // zip user on Windows is told what to do instead of being lied to.
    result["ok"] = false;
    result["message"] = "On Windows, use the CpapDash Desktop installer to start "
                        "at login.";
    cb(drogon::HttpResponse::newHttpJsonResponse(result));
    return;
#else
    try {
        const auto path = std::filesystem::path(entry.path);
        if (enable) {
            std::filesystem::create_directories(path.parent_path());
            std::ofstream out(path);
            if (!out) throw std::runtime_error("cannot write " + entry.path);
            out << entry.content;
            out.close();
        } else {
            std::error_code ec;
            std::filesystem::remove(path, ec);
        }
        result["ok"] = true;
        result["installed"] = enable;
        result["path"] = entry.path;
        // Said explicitly, because "autostart" reads as "on boot" to most people
        // and this is not that.
        result["note"] = enable
            ? "Starts when you log in, not when the machine boots."
            : "";
    } catch (const std::exception& e) {
        cb(jsonError(e.what(), drogon::k500InternalServerError));
        return;
    }
    cb(drogon::HttpResponse::newHttpJsonResponse(result));
#endif
}

void CpapController::capabilities(const drogon::HttpRequestPtr&,
                                   std::function<void(const drogon::HttpResponsePtr&)>&& cb) {
    const auto caps = SetupService::capabilities();

    Json::Value result;
    result["version"] = caps.version;
    result["platform"] = caps.platform;
    result["data_dir"] = caps.data_dir;

    Json::Value backends(Json::arrayValue);
    for (const auto& b : caps.backends) backends.append(b);
    result["backends"] = backends;

    Json::Value features;
    features["pdf_reports"] = caps.pdf_reports;
    features["mdns_discovery"] = caps.mdns_discovery;
    result["features"] = features;

    // SDD-010: the local folder must be the card ROOT, the folder holding both
    // STR.edf and DATALOG. When it is not, ingestion stops but the dashboard
    // keeps serving whatever is already stored, so the ONLY way the user learns
    // why nothing new is arriving is a banner. This is what feeds it.
    //
    // Computed here, on demand, rather than cached from the collector. It costs
    // a couple of stat calls, it can never go stale, and a share that mounts
    // late clears the banner on the next page load with no restart.
    //
    // ONLY for source=local. Lowenstein points local_dir at a Prisma tree that
    // has no DATALOG at all, so classifying it would hard-fail a setup that is
    // working perfectly well.
    if (ConfigManager::get("CPAP_SOURCE", "ezshare") == "local") {
        const std::string dir = ConfigManager::get("CPAP_LOCAL_DIR", "");
        const auto layout = classifyLocalDir(dir);
        if (layout != LocalDirLayout::Root) {
            Json::Value err;
            err["layout"]  = localDirLayoutString(layout);
            err["path"]    = dir;
            err["problem"] = localDirProblem(layout, dir);
            err["remedy"]  = localDirRemedy(layout, dir);
            result["config_error"] = err;
        }
    }

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

// SDD-005: both handlers below are deliberately thin. tests/CMakeLists.txt
// excludes src/controllers/ from the test binary, so anything with a decision
// in it belongs in the service layer where it can actually be covered.
void CpapController::discoverDevices(const drogon::HttpRequestPtr&,
                                     std::function<void(const drogon::HttpResponsePtr&)>&& cb) {
    // 2.5s: long enough for a unit on the same LAN to answer, short enough
    // that a user staring at a Scan button does not think it hung. This runs
    // on a Drogon worker thread and blocks it for the duration.
    auto svc = DeviceDiscoveryService::withMulticast();
    const auto devices = svc.browse(std::chrono::milliseconds(2500));

    Json::Value result;
    result["devices"] = Json::arrayValue;
    for (const auto& d : devices) {
        Json::Value j;
        j["instance"]       = d.instance;
        j["host"]           = d.host;
        j["port"]           = d.port;
        j["serial"]         = d.serial;
        j["fw"]             = d.fw;
        j["mode"]           = d.mode;
        j["local_capable"]  = d.isLocalCapable();
        j["base_url"]       = d.baseUrl();
        result["devices"].append(j);
    }
    // Nobody answering is a successful browse with nothing found, never an
    // error: multicast is routinely blocked and that is not a fault worth
    // showing a user a red box over.
    result["count"] = static_cast<int>(devices.size());
    cb(jsonResp(result));
}

void CpapController::syncNow(const drogon::HttpRequestPtr&,
                             std::function<void(const drogon::HttpResponsePtr&)>&& cb) {
    if (!burst_service_) {
        cb(jsonError("Collector not available", drogon::k503ServiceUnavailable));
        return;
    }

    const auto outcome = burst_service_->requestSyncNow();

    Json::Value result;
    result["outcome"] = BurstCollectorService::syncNowOutcomeString(outcome);
    result["accepted"] =
        (outcome == BurstCollectorService::SyncNowOutcome::Requested ||
         outcome == BurstCollectorService::SyncNowOutcome::AlreadyRequested ||
         outcome == BurstCollectorService::SyncNowOutcome::AlreadyRunning);
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

void CpapController::sessionForceComplete(const drogon::HttpRequestPtr&,
                                           std::function<void(const drogon::HttpResponsePtr&)>&& cb,
                                           const std::string& date) {
    if (!burst_service_) {
        cb(jsonError("Service not available", drogon::k503ServiceUnavailable));
        return;
    }
    bool ok = burst_service_->forceCompleteSession(date);
    Json::Value result;
    result["status"] = ok ? "completed" : "already_completed";
    result["date"] = date;
    cb(jsonResp(result));
}

void CpapController::sessionGenerateSummary(const drogon::HttpRequestPtr&,
                                             std::function<void(const drogon::HttpResponsePtr&)>&& cb,
                                             const std::string& date) {
    if (!burst_service_) {
        cb(jsonError("Service not available", drogon::k503ServiceUnavailable));
        return;
    }
    bool ok = burst_service_->generateSummaryForDate(date);
    Json::Value result;
    result["status"] = ok ? "started" : "failed";
    result["date"] = date;
    cb(jsonResp(result));
}

void CpapController::sessionReparse(const drogon::HttpRequestPtr&,
                                     std::function<void(const drogon::HttpResponsePtr&)>&& cb,
                                     const std::string& date) {
    // Reparse re-reads the night's files from the permanent archive and
    // rebuilds the DB rows for that sleep-day. We delegate to BackfillService
    // with a single-day range: that path works for sessions of ANY age,
    // unlike the burst collector which only revisits the last ~2 nights (so
    // an older session's reparse silently never ran). BackfillService maps
    // YYYY-MM-DD → folder YYYYMMDD and scopes delete+reparse to that one
    // folder, leaving the adjacent night untouched.
    if (!backfill_trigger_) {
        cb(jsonError("Reparse not available (archive source not configured)",
                     drogon::k503ServiceUnavailable));
        return;
    }
    // Empty local_dir → BackfillService uses its configured archive path.
    backfill_trigger_(date, date, "");
    Json::Value result;
    result["status"] = "queued";
    result["date"] = date;
    result["message"] = "Reparsing from archive; poll /api/backfill/status";
    cb(jsonResp(result));
}

void CpapController::oximetryCollect(const drogon::HttpRequestPtr&,
                                      std::function<void(const drogon::HttpResponsePtr&)>&& cb) {
    if (!burst_service_ || !burst_service_->getOximetryService()) {
        cb(jsonError("Oximetry service not available", drogon::k503ServiceUnavailable));
        return;
    }
    bool ok = burst_service_->getOximetryService()->collectAndPublish();
    Json::Value result;
    result["status"] = ok ? "collected" : "no_new_files";
    cb(jsonResp(result));
}

// Upload a Wellue / O2 Ring CSV export (multipart form field "file"). Parses
// server-side and stores it as an oximetry session, mirroring the cpapdash-api
// flow. Synchronous: returns the parsed summary.
void CpapController::uploadOximetryCsv(const drogon::HttpRequestPtr& req,
                                       std::function<void(const drogon::HttpResponsePtr&)>&& cb) {
    if (!oxi_csv_import_) {
        cb(jsonError("Oximetry import not available", drogon::k503ServiceUnavailable));
        return;
    }
    drogon::MultiPartParser parser;
    if (parser.parse(req) != 0 || parser.getFiles().empty()) {
        cb(jsonError("No file uploaded (multipart field 'file')", drogon::k400BadRequest));
        return;
    }
    const auto& f = parser.getFiles()[0];
    std::string content(f.fileContent());
    std::string filename = f.getFileName();
    try {
        Json::Value result = oxi_csv_import_(content, filename);
        if (result.isMember("error")) {
            cb(jsonError(result["error"].asString(), drogon::k400BadRequest));
            return;
        }
        cb(jsonResp(result));
    } catch (const std::exception& e) {
        cb(jsonError(e.what(), drogon::k500InternalServerError));
    }
}

// Upload a compressed CPAP data set (multipart form field "file"): a zip of the
// SD card's DATALOG folders. Files are extracted into the archive and parsed
// asynchronously via the backfill pipeline; poll /api/backfill/status.
void CpapController::uploadCpapZip(const drogon::HttpRequestPtr& req,
                                   std::function<void(const drogon::HttpResponsePtr&)>&& cb) {
    if (!cpap_zip_import_) {
        cb(jsonError("CPAP upload not available (archive directory not configured)",
                     drogon::k503ServiceUnavailable));
        return;
    }
    drogon::MultiPartParser parser;
    if (parser.parse(req) != 0 || parser.getFiles().empty()) {
        cb(jsonError("No file uploaded (multipart field 'file')", drogon::k400BadRequest));
        return;
    }
    const auto& f = parser.getFiles()[0];
    static std::atomic<uint64_t> seq{0};
    std::filesystem::path tmp = std::filesystem::temp_directory_path() /
        ("cpap_upload_" + std::to_string(seq++) + ".zip");
    {
        auto content = f.fileContent();
        std::ofstream o(tmp, std::ios::binary);
        o.write(content.data(), static_cast<std::streamsize>(content.size()));
    }
    try {
        Json::Value result = cpap_zip_import_(tmp.string());
        std::error_code ec;
        std::filesystem::remove(tmp, ec);
        if (result.isMember("error")) {
            cb(jsonError(result["error"].asString(), drogon::k400BadRequest));
            return;
        }
        cb(jsonResp(result));
    } catch (const std::exception& e) {
        std::error_code ec;
        std::filesystem::remove(tmp, ec);
        cb(jsonError(e.what(), drogon::k500InternalServerError));
    }
}

// Manual per-night export to SleepHQ (Sessions menu "Upload to SleepHQ").
// Fires the archive-based export for the given date (YYYYMMDD or YYYY-MM-DD);
// local-only setups are covered by the auto_on_backfill trigger.
void CpapController::sleephqExport(const drogon::HttpRequestPtr&,
                                   std::function<void(const drogon::HttpResponsePtr&)>&& cb,
                                   const std::string& date) {
    if (!config_ || !config_->sleephq.enabled) {
        cb(jsonError("SleepHQ not enabled", drogon::k400BadRequest));
        return;
    }
    std::string folder;
    for (char c : date) if (c != '-') folder += c;   // accept YYYYMMDD or YYYY-MM-DD
    if (folder.size() != 8) {
        cb(jsonError("Invalid date (expected YYYYMMDD)", drogon::k400BadRequest));
        return;
    }
    SleepHqExportService::getInstance().exportDateAsync(folder);
    Json::Value result;
    result["status"] = "queued";
    result["date"]   = folder;
    cb(jsonResp(result));
}

// ----- Report endpoints ------------------------------------------------
#ifndef _WIN32

std::shared_ptr<ReportGeneratorService> CpapController::report_svc_;

void CpapController::setReportService(std::shared_ptr<ReportGeneratorService> svc) {
    report_svc_ = svc;
}

void CpapController::generateReport(const drogon::HttpRequestPtr& req,
                                     std::function<void(const drogon::HttpResponsePtr&)>&& cb) {
    if (!report_svc_) {
        cb(jsonError("Report service not available", drogon::k503ServiceUnavailable));
        return;
    }
    auto body = req->getJsonObject();
    if (!body || !(*body)["start"].isString() || !(*body)["end"].isString()) {
        cb(jsonError("Body must include 'start' and 'end' (YYYY-MM-DD)", drogon::k400BadRequest));
        return;
    }
    std::string start = (*body)["start"].asString();
    std::string end   = (*body)["end"].asString();
    if (start > end) {
        cb(jsonError("'start' must be before 'end'", drogon::k400BadRequest));
        return;
    }

    int id = report_svc_->triggerReport(start, end);
    if (id < 0) {
        cb(jsonError("Failed to create report job", drogon::k500InternalServerError));
        return;
    }
    Json::Value result;
    result["report_id"] = id;
    result["status"]    = "generating";
    result["start"]     = start;
    result["end"]       = end;
    cb(jsonResp(result));
}

void CpapController::listReports(const drogon::HttpRequestPtr&,
                                  std::function<void(const drogon::HttpResponsePtr&)>&& cb) {
    if (!report_svc_) {
        cb(jsonError("Report service not available", drogon::k503ServiceUnavailable));
        return;
    }
    auto jobs = report_svc_->listReports(50);
    Json::Value arr(Json::arrayValue);
    for (const auto& j : jobs) {
        Json::Value o;
        o["id"]           = j.id;
        o["range_start"]  = j.range_start;
        o["range_end"]    = j.range_end;
        o["nights_count"] = j.nights_count;
        o["filename"]     = j.filename;
        o["status"]       = j.status;
        o["error_msg"]    = j.error_msg;
        o["created_at"]   = j.created_at;
        o["completed_at"] = j.completed_at;
        arr.append(o);
    }
    cb(jsonResp(arr));
}

void CpapController::reportStatus(const drogon::HttpRequestPtr&,
                                   std::function<void(const drogon::HttpResponsePtr&)>&& cb,
                                   const std::string& id) {
    if (!report_svc_) {
        cb(jsonError("Report service not available", drogon::k503ServiceUnavailable));
        return;
    }
    int rid = 0;
    try { rid = std::stoi(id); } catch (...) {
        cb(jsonError("Invalid report id", drogon::k400BadRequest));
        return;
    }
    auto job = report_svc_->getReport(rid);
    if (!job) {
        cb(jsonError("Report not found", drogon::k404NotFound));
        return;
    }
    Json::Value o;
    o["id"]           = job->id;
    o["range_start"]  = job->range_start;
    o["range_end"]    = job->range_end;
    o["nights_count"] = job->nights_count;
    o["filename"]     = job->filename;
    o["status"]       = job->status;
    o["error_msg"]    = job->error_msg;
    o["created_at"]   = job->created_at;
    o["completed_at"] = job->completed_at;
    cb(jsonResp(o));
}

void CpapController::downloadReport(const drogon::HttpRequestPtr&,
                                     std::function<void(const drogon::HttpResponsePtr&)>&& cb,
                                     const std::string& id) {
    if (!report_svc_) {
        cb(jsonError("Report service not available", drogon::k503ServiceUnavailable));
        return;
    }
    int rid = 0;
    try { rid = std::stoi(id); } catch (...) {
        cb(jsonError("Invalid report id", drogon::k400BadRequest));
        return;
    }
    auto job = report_svc_->getReport(rid);
    if (!job) {
        cb(jsonError("Report not found", drogon::k404NotFound));
        return;
    }
    if (job->status != "ready") {
        cb(jsonError("Report not ready (status: " + job->status + ")", drogon::k409Conflict));
        return;
    }
    if (!std::filesystem::exists(job->filepath)) {
        cb(jsonError("Report file missing from disk", drogon::k404NotFound));
        return;
    }
    auto resp = drogon::HttpResponse::newFileResponse(job->filepath);
    resp->addHeader("Content-Disposition",
                    "attachment; filename=\"" + job->filename + "\"");
    resp->setContentTypeString("application/pdf");
    cb(resp);
}

#endif // _WIN32

} // namespace hms_cpap

#endif // BUILD_WITH_WEB
