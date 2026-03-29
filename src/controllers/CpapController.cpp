#ifdef BUILD_WITH_WEB

#include "controllers/CpapController.h"
#include "utils/AppConfig.h"
#include <sstream>

namespace hms_cpap {

std::shared_ptr<QueryService> CpapController::qs_;
hms_cpap::AppConfig* CpapController::config_ = nullptr;
std::string CpapController::config_path_;

void CpapController::setQueryService(std::shared_ptr<QueryService> qs) { qs_ = qs; }

void CpapController::setConfig(hms_cpap::AppConfig* cfg, const std::string& path) {
    config_ = cfg;
    config_path_ = path;
}

static drogon::HttpResponsePtr jsonError(const std::string& msg, drogon::HttpStatusCode code) {
    Json::Value err;
    err["error"] = msg;
    auto resp = drogon::HttpResponse::newHttpJsonResponse(err);
    resp->setStatusCode(code);
    return resp;
}

void CpapController::health(const drogon::HttpRequestPtr&,
                             std::function<void(const drogon::HttpResponsePtr&)>&& cb) {
    Json::Value j;
    j["status"] = "ok";
    j["version"] = "2.1.0";
    j["service"] = "hms-cpap";
    cb(drogon::HttpResponse::newHttpJsonResponse(j));
}

void CpapController::dashboard(const drogon::HttpRequestPtr&,
                                std::function<void(const drogon::HttpResponsePtr&)>&& cb) {
    try {
        cb(drogon::HttpResponse::newHttpJsonResponse(qs_->getDashboard()));
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
        cb(drogon::HttpResponse::newHttpJsonResponse(qs_->getSessions(days, limit)));
    } catch (const std::exception& e) {
        cb(jsonError(e.what(), drogon::k500InternalServerError));
    }
}

void CpapController::sessionDetail(const drogon::HttpRequestPtr&,
                                    std::function<void(const drogon::HttpResponsePtr&)>&& cb,
                                    const std::string& date) {
    try {
        cb(drogon::HttpResponse::newHttpJsonResponse(qs_->getSessionDetail(date)));
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
        cb(drogon::HttpResponse::newHttpJsonResponse(qs_->getDailySummary(start, end)));
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
        cb(drogon::HttpResponse::newHttpJsonResponse(qs_->getTrend(metric, days)));
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
        cb(drogon::HttpResponse::newHttpJsonResponse(qs_->getStatistics(start, end)));
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
        cb(drogon::HttpResponse::newHttpJsonResponse(qs_->getSummaries(period, limit)));
    } catch (const std::exception& e) {
        cb(jsonError(e.what(), drogon::k500InternalServerError));
    }
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
    cb(drogon::HttpResponse::newHttpJsonResponse(result));
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

    // Save to disk
    config_->save(config_path_);

    // Return redacted config
    auto resp_json = config_->toJson();
    Json::Value result;
    Json::CharReaderBuilder builder;
    std::string errs;
    std::istringstream stream(resp_json.dump());
    Json::parseFromStream(builder, stream, &result, &errs);

    cb(drogon::HttpResponse::newHttpJsonResponse(result));
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
    cb(drogon::HttpResponse::newHttpJsonResponse(result));
}

void CpapController::testEzshare(const drogon::HttpRequestPtr& req,
                                  std::function<void(const drogon::HttpResponsePtr&)>&& cb) {
    std::string url = req->getParameter("url");
    if (url.empty() && config_) url = config_->ezshare_url;

    Json::Value result;
    result["url"] = url;
    result["configured"] = !url.empty();
    result["status"] = url.empty() ? "not_configured" : "configured";
    cb(drogon::HttpResponse::newHttpJsonResponse(result));
}

// ── Fysetc poll server ──────────────────────────────────────────────────────

std::shared_ptr<FysetcPollService> CpapController::fysetc_poll_svc_;

void CpapController::setFysetcPollService(std::shared_ptr<FysetcPollService> svc) {
    fysetc_poll_svc_ = svc;
}

void CpapController::fysetcAnnounce(const drogon::HttpRequestPtr& req,
                                     std::function<void(const drogon::HttpResponsePtr&)>&& cb) {
    auto body = req->getJsonObject();
    if (!body) {
        cb(jsonError("Invalid JSON", drogon::k400BadRequest));
        return;
    }

    auto& j = *body;
    std::string ip = j.get("ip", "").asString();
    std::string device_id = j.get("device_id", "").asString();
    int poll_sec = j.get("poll_interval_sec", 65).asInt();
    std::string fw = j.get("fw", "").asString();

    if (ip.empty() || device_id.empty()) {
        cb(jsonError("Missing ip or device_id", drogon::k400BadRequest));
        return;
    }

    std::cout << "Fysetc announce: " << device_id << " at " << ip
              << " (poll=" << poll_sec << "s, fw=" << fw << ")" << std::endl;

    if (fysetc_poll_svc_) {
        fysetc_poll_svc_->onAnnounce(ip, device_id, poll_sec, fw);
    }

    Json::Value resp;
    resp["status"] = "ok";
    cb(drogon::HttpResponse::newHttpJsonResponse(resp));
}

} // namespace hms_cpap

#endif // BUILD_WITH_WEB
