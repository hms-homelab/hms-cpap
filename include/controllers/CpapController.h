#pragma once
#ifdef BUILD_WITH_WEB

#include <drogon/HttpController.h>
#include "web/QueryService.h"
#include "utils/AppConfig.h"
#include "services/BurstCollectorService.h"
#include "services/CpapDashSyncService.h"
#ifndef _WIN32
#include "services/ReportGeneratorService.h"
#endif
#include <functional>
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
    ADD_METHOD_TO(CpapController::sessionSignals, "/api/sessions/{date}/signals", drogon::Get);
    ADD_METHOD_TO(CpapController::sessionVitals,  "/api/sessions/{date}/vitals",  drogon::Get);
    ADD_METHOD_TO(CpapController::sessionEvents,  "/api/sessions/{date}/events",  drogon::Get);
    ADD_METHOD_TO(CpapController::sessionBreaths, "/api/sessions/{date}/breaths", drogon::Get);
    ADD_METHOD_TO(CpapController::events,         "/api/events",                  drogon::Get);
    ADD_METHOD_TO(CpapController::sessionOximetry, "/api/sessions/{date}/oximetry", drogon::Get);
    ADD_METHOD_TO(CpapController::realtime,      "/api/realtime",            drogon::Get);
    ADD_METHOD_TO(CpapController::getConfig,     "/api/config",              drogon::Get);
    ADD_METHOD_TO(CpapController::updateConfig,  "/api/config",              drogon::Put);
    ADD_METHOD_TO(CpapController::setupComplete, "/api/setup",               drogon::Post);
    ADD_METHOD_TO(CpapController::capabilities,  "/api/capabilities",        drogon::Get);
    // SDD-006 phase 2. All three refuse once setup_complete is true, so a
    // finished install does not expose database provisioning on the LAN forever.
    ADD_METHOD_TO(CpapController::setupTestDb,   "/api/setup/test-db",       drogon::Post);
    ADD_METHOD_TO(CpapController::setupCreateDb, "/api/setup/create-db",     drogon::Post);
    ADD_METHOD_TO(CpapController::setupApply,    "/api/setup/apply",         drogon::Post);
    ADD_METHOD_TO(CpapController::setupAutostart,"/api/setup/autostart",     drogon::Post);
    ADD_METHOD_TO(CpapController::autostartState,"/api/setup/autostart",     drogon::Get);
    ADD_METHOD_TO(CpapController::configRestart, "/api/config/restart",      drogon::Post);
    ADD_METHOD_TO(CpapController::testEzshare,   "/api/config/test-ezshare", drogon::Get);
    ADD_METHOD_TO(CpapController::testBle,       "/api/config/test-ble",     drogon::Get);
    ADD_METHOD_TO(CpapController::triggerMlTrain, "/api/ml/train",     drogon::Post);
    ADD_METHOD_TO(CpapController::mlStatus,       "/api/ml/status",    drogon::Get);
    ADD_METHOD_TO(CpapController::getLlmPrompt,   "/api/llm-prompt",   drogon::Get);
    ADD_METHOD_TO(CpapController::updateLlmPrompt,"/api/llm-prompt",   drogon::Put);
    ADD_METHOD_TO(CpapController::discoverDevices, "/api/discover/devices", drogon::Get);
    ADD_METHOD_TO(CpapController::syncNow,         "/api/sync/now",         drogon::Post);
    ADD_METHOD_TO(CpapController::triggerBackfill, "/api/backfill",        drogon::Post);
    ADD_METHOD_TO(CpapController::backfillStatus,  "/api/backfill/status", drogon::Get);
    ADD_METHOD_TO(CpapController::backfillScan,    "/api/backfill/scan",   drogon::Get);
    ADD_METHOD_TO(CpapController::sessionSleepStages, "/api/sessions/{date}/sleep_stages", drogon::Get);
    ADD_METHOD_TO(CpapController::sleepStageStatus,   "/api/sleep-stages/status",          drogon::Get);
    ADD_METHOD_TO(CpapController::insights,           "/api/insights",                     drogon::Get);
    ADD_METHOD_TO(CpapController::sessionForceComplete,   "/api/sessions/{date}/force-complete",   drogon::Post);
    ADD_METHOD_TO(CpapController::sessionGenerateSummary, "/api/sessions/{date}/generate-summary", drogon::Post);
    ADD_METHOD_TO(CpapController::sessionReparse,         "/api/sessions/{date}/reparse",          drogon::Post);
    ADD_METHOD_TO(CpapController::oximetryCollect,   "/api/oximetry/collect",       drogon::Post);
    ADD_METHOD_TO(CpapController::uploadOximetryCsv,  "/api/upload/oximetry",        drogon::Post);
    ADD_METHOD_TO(CpapController::uploadCpapZip,      "/api/upload/cpap",            drogon::Post);
    ADD_METHOD_TO(CpapController::sleephqExport,      "/api/sleephq/export/{date}",  drogon::Post);
#ifndef _WIN32
    ADD_METHOD_TO(CpapController::generateReport,    "/api/reports/generate",       drogon::Post);
    ADD_METHOD_TO(CpapController::listReports,       "/api/reports",                drogon::Get);
    ADD_METHOD_TO(CpapController::reportStatus,      "/api/reports/{id}/status",    drogon::Get);
    ADD_METHOD_TO(CpapController::downloadReport,    "/api/reports/{id}/download",  drogon::Get);
#endif
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

    void sessionSignals(const drogon::HttpRequestPtr& req,
                        std::function<void(const drogon::HttpResponsePtr&)>&& cb,
                        const std::string& date);
    void sessionVitals(const drogon::HttpRequestPtr& req,
                       std::function<void(const drogon::HttpResponsePtr&)>&& cb,
                       const std::string& date);
    void sessionEvents(const drogon::HttpRequestPtr& req,
                       std::function<void(const drogon::HttpResponsePtr&)>&& cb,
                       const std::string& date);
    void sessionBreaths(const drogon::HttpRequestPtr& req,
                        std::function<void(const drogon::HttpResponsePtr&)>&& cb,
                        const std::string& date);
    void events(const drogon::HttpRequestPtr& req,
                std::function<void(const drogon::HttpResponsePtr&)>&& cb);
    void sessionOximetry(const drogon::HttpRequestPtr& req,
                          std::function<void(const drogon::HttpResponsePtr&)>&& cb,
                          const std::string& date);

    void realtime(const drogon::HttpRequestPtr& req,
                  std::function<void(const drogon::HttpResponsePtr&)>&& cb);
    void getConfig(const drogon::HttpRequestPtr& req,
                   std::function<void(const drogon::HttpResponsePtr&)>&& cb);
    void updateConfig(const drogon::HttpRequestPtr& req,
                      std::function<void(const drogon::HttpResponsePtr&)>&& cb);
    void setupComplete(const drogon::HttpRequestPtr& req,
                       std::function<void(const drogon::HttpResponsePtr&)>&& cb);

    /// SDD-006 phase 2. Connect with the candidate credentials and report
    /// whether the target already holds sessions, so the wizard can say "this
    /// database already has 412 sessions, they will be reused" instead of
    /// leaving the user to guess whether they are merging into someone's data.
    void setupTestDb(const drogon::HttpRequestPtr& req,
                     std::function<void(const drogon::HttpResponsePtr&)>&& cb);

    /// SDD-006 phase 2. Provision the database and its owner using
    /// REQUEST-SCOPED admin credentials, then reconnect as the ordinary user to
    /// prove the result is usable: a successful CREATE DATABASE followed by a
    /// failed GRANT otherwise looks like success and fails at first boot.
    /// Admin credentials are never written to config.json, never returned and
    /// never logged.
    void setupCreateDb(const drogon::HttpRequestPtr& req,
                       std::function<void(const drogon::HttpResponsePtr&)>&& cb);

    /// SDD-006 phase 2. Write config.json, set setup_complete, answer 202, and
    /// only THEN restart, so the response is not lost with the process.
    void setupApply(const drogon::HttpRequestPtr& req,
                    std::function<void(const drogon::HttpResponsePtr&)>&& cb);

    /// SDD-006 phase 4. Install or remove the start-at-login entry.
    /// Starts at LOGIN, not at boot; the wizard's label says so.
    void setupAutostart(const drogon::HttpRequestPtr& req,
                        std::function<void(const drogon::HttpResponsePtr&)>&& cb);

    /// Whether autostart is installed, and whether we may manage it at all
    /// (the SDD-005 shell owns it when there is an installer).
    void autostartState(const drogon::HttpRequestPtr& req,
                        std::function<void(const drogon::HttpResponsePtr&)>&& cb);
    /// SDD-006: what this build can actually do. Thin passthrough to
    /// SetupService, where the logic is testable; the test binary excludes
    /// this file.
    void capabilities(const drogon::HttpRequestPtr& req,
                      std::function<void(const drogon::HttpResponsePtr&)>&& cb);
    /// SDD-012: restart the process so restart-required settings take effect.
    /// The Settings-page counterpart to the wizard's setupApply, which refuses
    /// once setup_complete. Restarts only; touches no config.
    void configRestart(const drogon::HttpRequestPtr& req,
                       std::function<void(const drogon::HttpResponsePtr&)>&& cb);
    void testEzshare(const drogon::HttpRequestPtr& req,
                     std::function<void(const drogon::HttpResponsePtr&)>&& cb);
    void testBle(const drogon::HttpRequestPtr& req,
                  std::function<void(const drogon::HttpResponsePtr&)>&& cb);

    void triggerMlTrain(const drogon::HttpRequestPtr& req,
                        std::function<void(const drogon::HttpResponsePtr&)>&& cb);
    void mlStatus(const drogon::HttpRequestPtr& req,
                  std::function<void(const drogon::HttpResponsePtr&)>&& cb);
    void getLlmPrompt(const drogon::HttpRequestPtr& req,
                      std::function<void(const drogon::HttpResponsePtr&)>&& cb);
    void updateLlmPrompt(const drogon::HttpRequestPtr& req,
                         std::function<void(const drogon::HttpResponsePtr&)>&& cb);

    /// SDD-005: browse the LAN for CpapDash Mule and Miner units. Thin
    /// passthrough to DeviceDiscoveryService; all the logic lives there
    /// because the test binary excludes this file.
    void discoverDevices(const drogon::HttpRequestPtr& req,
                         std::function<void(const drogon::HttpResponsePtr&)>&& cb);
    /// SDD-005: force one burst cycle now. Deliberately NOT /api/backfill,
    /// which means "go re-read historical data" in this codebase.
    void syncNow(const drogon::HttpRequestPtr& req,
                 std::function<void(const drogon::HttpResponsePtr&)>&& cb);

    void triggerBackfill(const drogon::HttpRequestPtr& req,
                         std::function<void(const drogon::HttpResponsePtr&)>&& cb);
    void backfillStatus(const drogon::HttpRequestPtr& req,
                        std::function<void(const drogon::HttpResponsePtr&)>&& cb);
    void backfillScan(const drogon::HttpRequestPtr& req,
                      std::function<void(const drogon::HttpResponsePtr&)>&& cb);

    void sessionSleepStages(const drogon::HttpRequestPtr& req,
                            std::function<void(const drogon::HttpResponsePtr&)>&& cb,
                            const std::string& date);
    void sleepStageStatus(const drogon::HttpRequestPtr& req,
                          std::function<void(const drogon::HttpResponsePtr&)>&& cb);
    void insights(const drogon::HttpRequestPtr& req,
                  std::function<void(const drogon::HttpResponsePtr&)>&& cb);
    void sessionForceComplete(const drogon::HttpRequestPtr& req,
                              std::function<void(const drogon::HttpResponsePtr&)>&& cb,
                              const std::string& date);
    void sessionGenerateSummary(const drogon::HttpRequestPtr& req,
                                std::function<void(const drogon::HttpResponsePtr&)>&& cb,
                                const std::string& date);
    void sessionReparse(const drogon::HttpRequestPtr& req,
                        std::function<void(const drogon::HttpResponsePtr&)>&& cb,
                        const std::string& date);
    void oximetryCollect(const drogon::HttpRequestPtr& req,
                         std::function<void(const drogon::HttpResponsePtr&)>&& cb);
    void uploadOximetryCsv(const drogon::HttpRequestPtr& req,
                           std::function<void(const drogon::HttpResponsePtr&)>&& cb);
    void uploadCpapZip(const drogon::HttpRequestPtr& req,
                       std::function<void(const drogon::HttpResponsePtr&)>&& cb);
    void sleephqExport(const drogon::HttpRequestPtr& req,
                       std::function<void(const drogon::HttpResponsePtr&)>&& cb,
                       const std::string& date);

#ifndef _WIN32
    void generateReport(const drogon::HttpRequestPtr& req,
                        std::function<void(const drogon::HttpResponsePtr&)>&& cb);
    void listReports(const drogon::HttpRequestPtr& req,
                     std::function<void(const drogon::HttpResponsePtr&)>&& cb);
    void reportStatus(const drogon::HttpRequestPtr& req,
                      std::function<void(const drogon::HttpResponsePtr&)>&& cb,
                      const std::string& id);
    void downloadReport(const drogon::HttpRequestPtr& req,
                        std::function<void(const drogon::HttpResponsePtr&)>&& cb,
                        const std::string& id);

    static void setReportService(std::shared_ptr<ReportGeneratorService> svc);
#endif

    static void setQueryService(std::shared_ptr<QueryService> qs);
    static void setConfig(hms_cpap::AppConfig* cfg, const std::string& config_path);
    static void setBurstService(BurstCollectorService* svc);

    /// SDD-006 section 5: the live cloud-mirror instance, so PUT /api/config can
    /// re-apply changed sync settings instead of only writing them to disk.
    static void setSyncService(std::shared_ptr<CpapDashSyncService> sync);

    static std::function<void()> ml_train_trigger_;
    static std::function<Json::Value()> ml_status_getter_;
    static std::function<void(const std::string&, const std::string&, const std::string&)> backfill_trigger_;
    static std::function<Json::Value()> backfill_status_getter_;
    static std::function<Json::Value()> sleep_stage_status_getter_;
    // CSV content + filename -> result Json ("error" key on failure).
    static std::function<Json::Value(const std::string&, const std::string&)> oxi_csv_import_;
    // Path to an uploaded zip on disk -> result Json ("error" key on failure).
    static std::function<Json::Value(const std::string&)> cpap_zip_import_;

private:
    static std::shared_ptr<QueryService>          qs_;
#ifndef _WIN32
    // The PDF report stack is POSIX-only (gnuplot + libharu), so this member is
    // genuinely platform-gated.
    static std::shared_ptr<ReportGeneratorService> report_svc_;
#endif
    // NOT gated: the cloud mirror works on every platform. It briefly lived
    // inside the guard above and broke the MSVC build with "sync_ undeclared",
    // which no macOS or Linux build could have caught.
    static std::shared_ptr<CpapDashSyncService>   sync_;
    static hms_cpap::AppConfig* config_;
    static std::string config_path_;
    static BurstCollectorService* burst_service_;
};

} // namespace hms_cpap

#endif // BUILD_WITH_WEB
