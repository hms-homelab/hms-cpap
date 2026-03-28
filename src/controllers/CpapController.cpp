#ifdef BUILD_WITH_WEB

#include "controllers/CpapController.h"

namespace hms_cpap {

std::shared_ptr<QueryService> CpapController::qs_;

void CpapController::setQueryService(std::shared_ptr<QueryService> qs) { qs_ = qs; }

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

} // namespace hms_cpap

#endif // BUILD_WITH_WEB
