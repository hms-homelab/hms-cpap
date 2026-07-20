#pragma once
//
// EquipmentController (SDD-004) — equipment profiles + supplies, local-first.
//
// Mirrors the cloud routes (hms-cpapdash-api SDD-035) so the Angular page can be
// adapted rather than rewritten, but with no auth: hms-cpap is LAN-trusted and
// single-household, so there is no user scoping anywhere in this surface.
//
// Supply wear is COMPUTED on read by SupplyStatus, never stored. The effective
// interval is the item's own replace_after_days when set, else the default from
// the cpap_equipment_types catalog — which is why custom types (e.g. "battery")
// work without touching code.
//
#include <drogon/HttpController.h>
#include <memory>
#include "database/IDatabase.h"
#include "services/CpapDashSyncService.h"

namespace hms_cpap {

class EquipmentController : public drogon::HttpController<EquipmentController> {
public:
    METHOD_LIST_BEGIN
    // Type catalog (seeded system defaults + user customs)
    ADD_METHOD_TO(EquipmentController::listTypes,     "/api/equipment/types",        drogon::Get);
    ADD_METHOD_TO(EquipmentController::createType,    "/api/equipment/types",        drogon::Post);
    ADD_METHOD_TO(EquipmentController::updateType,    "/api/equipment/types/{id}",   drogon::Put);
    ADD_METHOD_TO(EquipmentController::deleteType,    "/api/equipment/types/{id}",   drogon::Delete);
    // Profiles ("setups"), with nested items + computed supply on GET
    ADD_METHOD_TO(EquipmentController::listProfiles,  "/api/equipment/profiles",      drogon::Get);
    ADD_METHOD_TO(EquipmentController::createProfile, "/api/equipment/profiles",      drogon::Post);
    ADD_METHOD_TO(EquipmentController::updateProfile, "/api/equipment/profiles/{id}", drogon::Put);
    ADD_METHOD_TO(EquipmentController::deleteProfile, "/api/equipment/profiles/{id}", drogon::Delete);
    // Items
    ADD_METHOD_TO(EquipmentController::createItem,    "/api/equipment",       drogon::Post);
    ADD_METHOD_TO(EquipmentController::updateItem,    "/api/equipment/{id}",  drogon::Put);
    ADD_METHOD_TO(EquipmentController::deleteItem,    "/api/equipment/{id}",  drogon::Delete);
    // Urgency-sorted wear view
    ADD_METHOD_TO(EquipmentController::supplies,      "/api/supplies",        drogon::Get);
    // Opt-in cloud mirror. Local is the source of truth; this is the button.
    ADD_METHOD_TO(EquipmentController::cloudSync,     "/api/equipment/cloud-sync", drogon::Post);
    METHOD_LIST_END

    void listTypes(const drogon::HttpRequestPtr& req,
                   std::function<void(const drogon::HttpResponsePtr&)>&& cb);
    void createType(const drogon::HttpRequestPtr& req,
                    std::function<void(const drogon::HttpResponsePtr&)>&& cb);
    void updateType(const drogon::HttpRequestPtr& req,
                    std::function<void(const drogon::HttpResponsePtr&)>&& cb, int id);
    void deleteType(const drogon::HttpRequestPtr& req,
                    std::function<void(const drogon::HttpResponsePtr&)>&& cb, int id);

    void listProfiles(const drogon::HttpRequestPtr& req,
                      std::function<void(const drogon::HttpResponsePtr&)>&& cb);
    void createProfile(const drogon::HttpRequestPtr& req,
                       std::function<void(const drogon::HttpResponsePtr&)>&& cb);
    void updateProfile(const drogon::HttpRequestPtr& req,
                       std::function<void(const drogon::HttpResponsePtr&)>&& cb, int id);
    void deleteProfile(const drogon::HttpRequestPtr& req,
                       std::function<void(const drogon::HttpResponsePtr&)>&& cb, int id);

    void createItem(const drogon::HttpRequestPtr& req,
                    std::function<void(const drogon::HttpResponsePtr&)>&& cb);
    void updateItem(const drogon::HttpRequestPtr& req,
                    std::function<void(const drogon::HttpResponsePtr&)>&& cb, int id);
    void deleteItem(const drogon::HttpRequestPtr& req,
                    std::function<void(const drogon::HttpResponsePtr&)>&& cb, int id);

    void supplies(const drogon::HttpRequestPtr& req,
                  std::function<void(const drogon::HttpResponsePtr&)>&& cb);

    /// Run a cloud sync now. 409 when cpapdash.enabled is false or no token is set —
    /// the feature must never silently pretend to have synced.
    void cloudSync(const drogon::HttpRequestPtr& req,
                   std::function<void(const drogon::HttpResponsePtr&)>&& cb);

    /// Injected from main.cpp, same pattern as CpapController::setQueryService.
    static void setDatabase(std::shared_ptr<IDatabase> db);

    /// Optional. When unset (or cloud sync disabled in config), /cloud-sync 409s
    /// rather than pretending a sync happened.
    static void setSyncService(std::shared_ptr<CpapDashSyncService> sync);

private:
    static std::shared_ptr<IDatabase> db_;
    static std::shared_ptr<CpapDashSyncService> sync_;
};

} // namespace hms_cpap
