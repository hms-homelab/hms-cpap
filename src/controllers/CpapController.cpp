#ifdef BUILD_WITH_WEB

#include "controllers/CpapController.h"
#include "utils/AppConfig.h"
#include <sstream>

namespace hms_cpap {

std::shared_ptr<QueryService> CpapController::qs_;
hms_cpap::AppConfig* CpapController::config_ = nullptr;
std::string CpapController::config_path_;
std::function<void()> CpapController::ml_train_trigger_;
std::function<Json::Value()> CpapController::ml_status_getter_;

void CpapController::setQueryService(std::shared_ptr<QueryService> qs) { qs_ = qs; }

void CpapController::setConfig(hms_cpap::AppConfig* cfg, const std::string& path) {
    config_ = cfg;
    config_path_ = path;
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
    j["version"] = "3.0.0";
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

    // Save to disk
    config_->save(config_path_);

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
    std::string path;
    if (config_ && !config_->llm.prompt_file.empty()) {
        path = config_->llm.prompt_file;
    } else {
        const char* env_path = std::getenv("LLM_PROMPT_FILE");
        path = env_path ? env_path : "./llm_prompt.txt";
    }

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

    std::string path;
    if (config_ && !config_->llm.prompt_file.empty()) {
        path = config_->llm.prompt_file;
    } else {
        const char* env_path = std::getenv("LLM_PROMPT_FILE");
        path = env_path ? env_path : "./llm_prompt.txt";
    }

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

} // namespace hms_cpap

#endif // BUILD_WITH_WEB
