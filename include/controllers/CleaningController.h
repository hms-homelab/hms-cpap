#pragma once
//
// CleaningController (SDD-007) — cleaning schedules, local-first.
//
// Mirrors the cloud routes (hms-cpapdash-api SDD-043) route-for-route so the
// Angular section can be adapted rather than rewritten, but with no auth:
// hms-cpap is LAN-trusted and single-household, so there is no user scoping
// anywhere in this surface.
//
// Cleaning is deliberately NOT supplies. A mask is REPLACED every 90 days and
// WIPED every day; one interval cannot mean both, which is why this sits beside
// EquipmentController rather than inside it.
//
// `status` is COMPUTED on read by computeCleaningStatus, never stored, exactly
// like supply wear. There is therefore no cached column that can go stale, and
// no write path that has to remember to recompute anything.
//
#include <drogon/HttpController.h>
#include <memory>
#include "database/IDatabase.h"
#include "services/CpapDashSyncService.h"

namespace hms_cpap {

class CleaningController : public drogon::HttpController<CleaningController> {
public:
    METHOD_LIST_BEGIN
    // Preset catalog (the seven seeded system tasks).
    ADD_METHOD_TO(CleaningController::listTypes, "/api/cleaning/types",     drogon::Get);
    // Create the suggested set for a profile, all disabled. Registered before
    // the {id} routes so "suggest" is never parsed as an id.
    ADD_METHOD_TO(CleaningController::suggest,   "/api/cleaning/suggest",   drogon::Post);
    ADD_METHOD_TO(CleaningController::markDone,  "/api/cleaning/{id}/done", drogon::Post);
    ADD_METHOD_TO(CleaningController::list,      "/api/cleaning",           drogon::Get);
    ADD_METHOD_TO(CleaningController::create,    "/api/cleaning",           drogon::Post);
    ADD_METHOD_TO(CleaningController::update,    "/api/cleaning/{id}",      drogon::Put);
    ADD_METHOD_TO(CleaningController::remove,    "/api/cleaning/{id}",      drogon::Delete);
    METHOD_LIST_END

    void listTypes(const drogon::HttpRequestPtr& req,
                   std::function<void(const drogon::HttpResponsePtr&)>&& cb);
    void list(const drogon::HttpRequestPtr& req,
              std::function<void(const drogon::HttpResponsePtr&)>&& cb);
    void create(const drogon::HttpRequestPtr& req,
                std::function<void(const drogon::HttpResponsePtr&)>&& cb);
    void update(const drogon::HttpRequestPtr& req,
                std::function<void(const drogon::HttpResponsePtr&)>&& cb,
                std::string id);
    void remove(const drogon::HttpRequestPtr& req,
                std::function<void(const drogon::HttpResponsePtr&)>&& cb,
                std::string id);
    void markDone(const drogon::HttpRequestPtr& req,
                  std::function<void(const drogon::HttpResponsePtr&)>&& cb,
                  std::string id);
    void suggest(const drogon::HttpRequestPtr& req,
                 std::function<void(const drogon::HttpResponsePtr&)>&& cb);

    static void setDatabase(std::shared_ptr<IDatabase> db);
    /// Optional cloud mirror. A cleaning edit makes it stale exactly as an
    /// equipment edit does, so the same debounce applies.
    static void setSyncService(std::shared_ptr<CpapDashSyncService> sync);

private:
    static void markMirrorDirty();

    static std::shared_ptr<IDatabase> db_;
    static std::shared_ptr<CpapDashSyncService> sync_;
};

}  // namespace hms_cpap
