#pragma once
#ifdef BUILD_WITH_WEB

#include <drogon/HttpController.h>
#include "web/QueryService.h"
#include "utils/AppConfig.h"
#include <memory>
#include <string>

namespace hms_cpap {

class CpapController : public drogon::HttpController<CpapController> {
public:
    METHOD_LIST_BEGIN
    ADD_METHOD_TO(CpapController::health,        "/health",                 drogon::Get);
    ADD_METHOD_TO(CpapController::dashboard,     "/api/dashboard",          drogon::Get);
    ADD_METHOD_TO(CpapController::sessions,      "/api/sessions",           drogon::Get);
    ADD_METHOD_TO(CpapController::sessionDetail, "/api/sessions/{date}",    drogon::Get);
    ADD_METHOD_TO(CpapController::dailySummary,  "/api/daily-summary",      drogon::Get);
    ADD_METHOD_TO(CpapController::trend,         "/api/trends/{metric}",    drogon::Get);
    ADD_METHOD_TO(CpapController::statistics,    "/api/statistics",         drogon::Get);
    ADD_METHOD_TO(CpapController::summaries,     "/api/summaries",          drogon::Get);
    ADD_METHOD_TO(CpapController::getConfig,     "/api/config",              drogon::Get);
    ADD_METHOD_TO(CpapController::updateConfig,  "/api/config",              drogon::Put);
    ADD_METHOD_TO(CpapController::setupComplete, "/api/setup",               drogon::Post);
    ADD_METHOD_TO(CpapController::testEzshare,   "/api/config/test-ezshare", drogon::Get);
    METHOD_LIST_END

    void health(const drogon::HttpRequestPtr& req,
                std::function<void(const drogon::HttpResponsePtr&)>&& cb);
    void dashboard(const drogon::HttpRequestPtr& req,
                   std::function<void(const drogon::HttpResponsePtr&)>&& cb);
    void sessions(const drogon::HttpRequestPtr& req,
                  std::function<void(const drogon::HttpResponsePtr&)>&& cb);
    void sessionDetail(const drogon::HttpRequestPtr& req,
                       std::function<void(const drogon::HttpResponsePtr&)>&& cb,
                       const std::string& date);
    void dailySummary(const drogon::HttpRequestPtr& req,
                      std::function<void(const drogon::HttpResponsePtr&)>&& cb);
    void trend(const drogon::HttpRequestPtr& req,
               std::function<void(const drogon::HttpResponsePtr&)>&& cb,
               const std::string& metric);
    void statistics(const drogon::HttpRequestPtr& req,
                    std::function<void(const drogon::HttpResponsePtr&)>&& cb);
    void summaries(const drogon::HttpRequestPtr& req,
                   std::function<void(const drogon::HttpResponsePtr&)>&& cb);

    void getConfig(const drogon::HttpRequestPtr& req,
                   std::function<void(const drogon::HttpResponsePtr&)>&& cb);
    void updateConfig(const drogon::HttpRequestPtr& req,
                      std::function<void(const drogon::HttpResponsePtr&)>&& cb);
    void setupComplete(const drogon::HttpRequestPtr& req,
                       std::function<void(const drogon::HttpResponsePtr&)>&& cb);
    void testEzshare(const drogon::HttpRequestPtr& req,
                     std::function<void(const drogon::HttpResponsePtr&)>&& cb);

    static void setQueryService(std::shared_ptr<QueryService> qs);
    static void setConfig(hms_cpap::AppConfig* cfg, const std::string& config_path);

private:
    static std::shared_ptr<QueryService> qs_;
    static hms_cpap::AppConfig* config_;
    static std::string config_path_;
};

} // namespace hms_cpap

#endif // BUILD_WITH_WEB
