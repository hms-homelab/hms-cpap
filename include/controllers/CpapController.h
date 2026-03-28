#pragma once
#ifdef BUILD_WITH_WEB

#include <drogon/HttpController.h>
#include "web/QueryService.h"
#include <memory>

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

    static void setQueryService(std::shared_ptr<QueryService> qs);

private:
    static std::shared_ptr<QueryService> qs_;
};

} // namespace hms_cpap

#endif // BUILD_WITH_WEB
