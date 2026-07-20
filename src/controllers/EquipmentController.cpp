#include "controllers/EquipmentController.h"
#include "services/SupplyStatus.h"

#include <algorithm>
#include <ctime>
#include <map>
#include <utility>
#include <vector>

namespace hms_cpap {

std::shared_ptr<IDatabase> EquipmentController::db_;
std::shared_ptr<CpapDashSyncService> EquipmentController::sync_;

void EquipmentController::setDatabase(std::shared_ptr<IDatabase> db) { db_ = std::move(db); }
void EquipmentController::setSyncService(std::shared_ptr<CpapDashSyncService> sync) {
    sync_ = std::move(sync);
}

namespace {

// Bypass Drogon's newHttpJsonResponse (crashes in the cross-compiled ARM binary) —
// same workaround CpapController uses.
drogon::HttpResponsePtr jsonResp(const Json::Value& val,
                                 drogon::HttpStatusCode code = drogon::k200OK) {
    Json::StreamWriterBuilder wb;
    wb["indentation"] = "";
    auto resp = drogon::HttpResponse::newHttpResponse();
    resp->setContentTypeCode(drogon::CT_APPLICATION_JSON);
    resp->setStatusCode(code);
    resp->setBody(Json::writeString(wb, val));
    return resp;
}

drogon::HttpResponsePtr jsonError(const std::string& msg, drogon::HttpStatusCode code) {
    Json::Value j;
    j["error"] = msg;
    return jsonResp(j, code);
}

/// Resolves a type's default interval once per type_key per request.
class TypeDefaults {
public:
    explicit TypeDefaults(IDatabase& db) : db_(db) {}

    /// Effective interval for an item: its own override, else the catalog default,
    /// else the built-in table (covers a type row that has since been removed).
    int intervalFor(const IDatabase::EquipmentItem& it) {
        if (it.replace_after_days >= 0) return it.replace_after_days;
        auto found = cache_.find(it.type_key);
        if (found != cache_.end()) return found->second;
        auto type = db_.resolveEquipmentType(it.type_key);
        int def = type ? type->default_replace_after_days : supplyDefaultDays(it.type_key);
        cache_[it.type_key] = def;
        return def;
    }

private:
    IDatabase& db_;
    std::map<std::string, int> cache_;
};

Json::Value typeToJson(const IDatabase::EquipmentType& t) {
    Json::Value j;
    j["id"]        = t.id;
    j["type_key"]  = t.type_key;
    j["label"]     = t.label;
    j["category"]  = t.category;
    if (t.default_replace_after_days >= 0) j["default_replace_after_days"] = t.default_replace_after_days;
    else                                   j["default_replace_after_days"] = Json::nullValue;
    j["is_system"] = t.is_system;
    j["custom"]    = !t.is_system;
    j["active"]    = t.active;
    return j;
}

Json::Value itemToJson(const IDatabase::EquipmentItem& it, int interval, long long now) {
    Json::Value j;
    j["id"]               = it.id;
    j["profile_id"]       = it.profile_id;
    j["client_uuid"]      = it.client_uuid;
    j["type_key"]         = it.type_key;
    j["slot"]             = it.type_key;      // alias: the phone app reads "slot"
    j["category"]         = it.category;
    j["brand"]            = it.brand;
    j["model"]            = it.model;
    j["variant"]          = it.variant;
    j["started_using_at"] = it.started_using_at;
    j["notes"]            = it.notes;
    j["active"]           = it.active;
    if (it.replace_after_days >= 0) j["replace_after_days"] = it.replace_after_days;
    else                            j["replace_after_days"] = Json::nullValue;
    j["created_at"]       = it.created_at;
    j["updated_at"]       = it.updated_at;

    SupplyStatus st = computeSupplyStatusForInterval(it.started_epoch, interval, now);
    Json::Value s;
    s["state"]         = supplyStateString(st.state);
    s["days_left"]     = st.days_left;
    s["wear_fraction"] = st.wear_fraction;
    s["replace_by"]    = static_cast<Json::Int64>(st.replace_by_epoch);
    j["supply"]        = s;
    return j;
}

/// Parse a client payload into an item. Accepts "type_key" or "slot" (app alias).
/// Absent/null replace_after_days means "use the type default" (-1 sentinel).
IDatabase::EquipmentItem parseItem(const Json::Value& b) {
    IDatabase::EquipmentItem it;
    if (b.isMember("id") && b["id"].isIntegral())                 it.id = b["id"].asInt();
    if (b.isMember("profile_id") && b["profile_id"].isIntegral()) it.profile_id = b["profile_id"].asInt();
    it.client_uuid      = b.get("client_uuid", "").asString();
    it.type_key         = b.get("type_key", "").asString();
    if (it.type_key.empty()) it.type_key = b.get("slot", "").asString();
    it.brand            = b.get("brand", "").asString();
    it.model            = b.get("model", "").asString();
    it.variant          = b.get("variant", "").asString();
    it.started_using_at = b.get("started_using_at", "").asString();
    it.notes            = b.get("notes", "").asString();
    it.active           = b.get("active", true).asBool();
    it.replace_after_days =
        (b.isMember("replace_after_days") && b["replace_after_days"].isIntegral())
            ? b["replace_after_days"].asInt() : -1;
    return it;
}

long long nowEpoch() { return static_cast<long long>(::time(nullptr)); }

} // namespace

// ── Type catalog ─────────────────────────────────────────────────────────────

void EquipmentController::listTypes(const drogon::HttpRequestPtr&,
                                    std::function<void(const drogon::HttpResponsePtr&)>&& cb) {
    if (!db_) { cb(jsonError("Database unavailable", drogon::k503ServiceUnavailable)); return; }
    Json::Value arr(Json::arrayValue);
    for (const auto& t : db_->listEquipmentTypes()) arr.append(typeToJson(t));
    Json::Value j; j["types"] = arr;
    cb(jsonResp(j));
}

void EquipmentController::createType(const drogon::HttpRequestPtr& req,
                                     std::function<void(const drogon::HttpResponsePtr&)>&& cb) {
    if (!db_) { cb(jsonError("Database unavailable", drogon::k503ServiceUnavailable)); return; }
    auto body = req->getJsonObject();
    if (!body) { cb(jsonError("Invalid JSON body", drogon::k400BadRequest)); return; }

    IDatabase::EquipmentType t;
    t.type_key = body->get("type_key", "").asString();
    t.label    = body->get("label", "").asString();
    t.category = body->get("category", "accessory").asString();
    t.active   = true;
    t.default_replace_after_days =
        (body->isMember("default_replace_after_days") && (*body)["default_replace_after_days"].isIntegral())
            ? (*body)["default_replace_after_days"].asInt() : -1;

    if (t.type_key.empty() || t.label.empty()) {
        cb(jsonError("type_key and label are required", drogon::k400BadRequest)); return;
    }
    if (t.category != "machine" && t.category != "accessory") {
        cb(jsonError("category must be 'machine' or 'accessory'", drogon::k400BadRequest)); return;
    }

    int id = db_->addEquipmentType(t);
    if (id < 0) { cb(jsonError("A type with that key already exists", drogon::k409Conflict)); return; }
    t.id = id;
    cb(jsonResp(typeToJson(t), drogon::k201Created));
}

void EquipmentController::updateType(const drogon::HttpRequestPtr& req,
                                     std::function<void(const drogon::HttpResponsePtr&)>&& cb, int id) {
    if (!db_) { cb(jsonError("Database unavailable", drogon::k503ServiceUnavailable)); return; }
    auto body = req->getJsonObject();
    if (!body) { cb(jsonError("Invalid JSON body", drogon::k400BadRequest)); return; }

    IDatabase::EquipmentType t;
    t.type_key = body->get("type_key", "").asString();
    t.label    = body->get("label", "").asString();
    t.category = body->get("category", "accessory").asString();
    t.active   = body->get("active", true).asBool();
    t.default_replace_after_days =
        (body->isMember("default_replace_after_days") && (*body)["default_replace_after_days"].isIntegral())
            ? (*body)["default_replace_after_days"].asInt() : -1;
    if (t.label.empty()) { cb(jsonError("label is required", drogon::k400BadRequest)); return; }

    // The DB layer pins type_key/category on system rows, so a rename attempt is
    // silently ignored there rather than orphaning every item using that type.
    if (!db_->updateEquipmentType(id, t)) {
        cb(jsonError("Type not found", drogon::k404NotFound)); return;
    }
    Json::Value j; j["message"] = "Type updated";
    cb(jsonResp(j));
}

void EquipmentController::deleteType(const drogon::HttpRequestPtr&,
                                     std::function<void(const drogon::HttpResponsePtr&)>&& cb, int id) {
    if (!db_) { cb(jsonError("Database unavailable", drogon::k503ServiceUnavailable)); return; }
    if (!db_->deleteEquipmentType(id)) {
        cb(jsonError("Custom type not found (system types cannot be removed)", drogon::k404NotFound));
        return;
    }
    Json::Value j; j["message"] = "Type removed";
    cb(jsonResp(j));
}

// ── Profiles ─────────────────────────────────────────────────────────────────

void EquipmentController::listProfiles(const drogon::HttpRequestPtr& req,
                                       std::function<void(const drogon::HttpResponsePtr&)>&& cb) {
    if (!db_) { cb(jsonError("Database unavailable", drogon::k503ServiceUnavailable)); return; }
    const bool history = req->getParameter("include") == "history";
    const long long now = nowEpoch();

    auto profiles = db_->listEquipmentProfiles(false);
    auto items    = db_->listEquipmentItems(history);
    TypeDefaults defaults(*db_);

    Json::Value arr(Json::arrayValue);
    for (const auto& p : profiles) {
        Json::Value pj;
        pj["id"]          = p.id;
        pj["client_uuid"] = p.client_uuid;
        pj["name"]        = p.name;
        pj["active"]      = p.active;
        pj["created_at"]  = p.created_at;
        pj["updated_at"]  = p.updated_at;
        Json::Value its(Json::arrayValue);
        for (const auto& it : items)
            if (it.profile_id == p.id) its.append(itemToJson(it, defaults.intervalFor(it), now));
        pj["items"] = its;
        arr.append(pj);
    }
    Json::Value j; j["profiles"] = arr;
    cb(jsonResp(j));
}

void EquipmentController::createProfile(const drogon::HttpRequestPtr& req,
                                        std::function<void(const drogon::HttpResponsePtr&)>&& cb) {
    if (!db_) { cb(jsonError("Database unavailable", drogon::k503ServiceUnavailable)); return; }
    auto body = req->getJsonObject();
    if (!body) { cb(jsonError("Invalid JSON body", drogon::k400BadRequest)); return; }

    IDatabase::EquipmentProfile p;
    p.name        = body->get("name", "").asString();
    p.client_uuid = body->get("client_uuid", "").asString();
    p.active      = body->get("active", true).asBool();
    if (p.name.empty()) { cb(jsonError("Profile name is required", drogon::k400BadRequest)); return; }

    int pid = db_->upsertEquipmentProfile(p);
    if (pid < 0) { cb(jsonError("Failed to create profile", drogon::k500InternalServerError)); return; }

    // Optionally seed items in the same call. One machine only, checked here
    // because MySQL cannot express the rule as a partial index.
    bool machine_seen = false;
    if (body->isMember("items") && (*body)["items"].isArray()) {
        for (const auto& ij : (*body)["items"]) {
            auto it = parseItem(ij);
            if (it.type_key.empty()) continue;
            auto type = db_->resolveEquipmentType(it.type_key);
            if (!type) continue;                       // unknown type: skip, not fatal
            it.category   = type->category;
            it.profile_id = pid;
            it.id         = 0;
            if (it.category == "machine") {
                if (machine_seen) {
                    cb(jsonError("A profile can have only one machine", drogon::k400BadRequest));
                    return;
                }
                machine_seen = true;
            }
            db_->upsertEquipmentItem(it);
        }
    }

    auto stored = db_->getEquipmentProfile(pid);
    auto items  = db_->listEquipmentItems(false);
    TypeDefaults defaults(*db_);
    const long long now = nowEpoch();

    Json::Value pj;
    if (stored) {
        pj["id"]          = stored->id;
        pj["client_uuid"] = stored->client_uuid;
        pj["name"]        = stored->name;
        pj["active"]      = stored->active;
        pj["created_at"]  = stored->created_at;
        pj["updated_at"]  = stored->updated_at;
        Json::Value its(Json::arrayValue);
        for (const auto& it : items)
            if (it.profile_id == pid) its.append(itemToJson(it, defaults.intervalFor(it), now));
        pj["items"] = its;
    }
    cb(jsonResp(pj, drogon::k201Created));
}

void EquipmentController::updateProfile(const drogon::HttpRequestPtr& req,
                                        std::function<void(const drogon::HttpResponsePtr&)>&& cb, int id) {
    if (!db_) { cb(jsonError("Database unavailable", drogon::k503ServiceUnavailable)); return; }
    auto body = req->getJsonObject();
    if (!body) { cb(jsonError("Invalid JSON body", drogon::k400BadRequest)); return; }

    auto existing = db_->getEquipmentProfile(id);
    if (!existing) { cb(jsonError("Profile not found", drogon::k404NotFound)); return; }

    IDatabase::EquipmentProfile p = *existing;
    p.name   = body->get("name", existing->name).asString();
    p.active = body->get("active", existing->active).asBool();
    if (p.name.empty()) { cb(jsonError("Profile name is required", drogon::k400BadRequest)); return; }

    if (db_->upsertEquipmentProfile(p) < 0) {
        cb(jsonError("Failed to update profile", drogon::k500InternalServerError)); return;
    }
    auto stored = db_->getEquipmentProfile(id);
    Json::Value pj;
    if (stored) {
        pj["id"]         = stored->id;
        pj["name"]       = stored->name;
        pj["active"]     = stored->active;
        pj["updated_at"] = stored->updated_at;
    }
    cb(jsonResp(pj));
}

void EquipmentController::deleteProfile(const drogon::HttpRequestPtr&,
                                        std::function<void(const drogon::HttpResponsePtr&)>&& cb, int id) {
    if (!db_) { cb(jsonError("Database unavailable", drogon::k503ServiceUnavailable)); return; }
    if (!db_->getEquipmentProfile(id)) { cb(jsonError("Profile not found", drogon::k404NotFound)); return; }
    if (!db_->tombstoneEquipmentProfile(id)) {
        cb(jsonError("Failed to remove profile", drogon::k500InternalServerError)); return;
    }
    Json::Value j; j["message"] = "Profile removed";
    cb(jsonResp(j));
}

// ── Items ────────────────────────────────────────────────────────────────────

void EquipmentController::createItem(const drogon::HttpRequestPtr& req,
                                     std::function<void(const drogon::HttpResponsePtr&)>&& cb) {
    if (!db_) { cb(jsonError("Database unavailable", drogon::k503ServiceUnavailable)); return; }
    auto body = req->getJsonObject();
    if (!body) { cb(jsonError("Invalid JSON body", drogon::k400BadRequest)); return; }

    auto it = parseItem(*body);
    it.id = 0;
    if (it.type_key.empty()) { cb(jsonError("type_key is required", drogon::k400BadRequest)); return; }

    auto type = db_->resolveEquipmentType(it.type_key);
    if (!type) { cb(jsonError("Unknown equipment type", drogon::k400BadRequest)); return; }
    it.category = type->category;

    // Home the item: the given profile, or the default setup when none was named.
    if (it.profile_id > 0) {
        if (!db_->getEquipmentProfile(it.profile_id)) {
            cb(jsonError("Profile not found", drogon::k400BadRequest)); return;
        }
    } else {
        it.profile_id = db_->ensureDefaultEquipmentProfile();
    }

    // The portable one-machine guard. Postgres and SQLite also enforce this with a
    // partial unique index, but MySQL has no such thing, so this check is the only
    // enforcement there — it must stay ahead of the write.
    if (it.category == "machine" && db_->profileHasMachine(it.profile_id, 0)) {
        cb(jsonError("A profile can have only one machine", drogon::k400BadRequest)); return;
    }

    int id = db_->upsertEquipmentItem(it);
    if (id < 0) { cb(jsonError("Failed to create equipment", drogon::k500InternalServerError)); return; }

    auto stored = db_->getEquipmentItem(id);
    TypeDefaults defaults(*db_);
    cb(jsonResp(stored ? itemToJson(*stored, defaults.intervalFor(*stored), nowEpoch())
                       : Json::Value(Json::objectValue),
                drogon::k201Created));
}

void EquipmentController::updateItem(const drogon::HttpRequestPtr& req,
                                     std::function<void(const drogon::HttpResponsePtr&)>&& cb, int id) {
    if (!db_) { cb(jsonError("Database unavailable", drogon::k503ServiceUnavailable)); return; }
    auto body = req->getJsonObject();
    if (!body) { cb(jsonError("Invalid JSON body", drogon::k400BadRequest)); return; }

    auto existing = db_->getEquipmentItem(id);
    if (!existing) { cb(jsonError("Equipment not found", drogon::k404NotFound)); return; }

    auto it = parseItem(*body);
    it.id = id;
    if (it.type_key.empty()) it.type_key = existing->type_key;   // type rarely changes
    auto type = db_->resolveEquipmentType(it.type_key);
    if (!type) { cb(jsonError("Unknown equipment type", drogon::k400BadRequest)); return; }
    it.category = type->category;

    if (it.profile_id <= 0) {
        it.profile_id = existing->profile_id;
    } else if (it.profile_id != existing->profile_id && !db_->getEquipmentProfile(it.profile_id)) {
        cb(jsonError("Profile not found", drogon::k400BadRequest)); return;
    }

    // Same guard as create, excluding this item so re-saving a machine is fine.
    if (it.category == "machine" && it.active &&
        db_->profileHasMachine(it.profile_id, id)) {
        cb(jsonError("A profile can have only one machine", drogon::k400BadRequest)); return;
    }

    if (db_->upsertEquipmentItem(it) < 0) {
        cb(jsonError("Failed to update equipment", drogon::k500InternalServerError)); return;
    }
    auto stored = db_->getEquipmentItem(id);
    TypeDefaults defaults(*db_);
    cb(jsonResp(stored ? itemToJson(*stored, defaults.intervalFor(*stored), nowEpoch())
                       : Json::Value(Json::objectValue)));
}

void EquipmentController::deleteItem(const drogon::HttpRequestPtr&,
                                     std::function<void(const drogon::HttpResponsePtr&)>&& cb, int id) {
    if (!db_) { cb(jsonError("Database unavailable", drogon::k503ServiceUnavailable)); return; }
    if (!db_->getEquipmentItem(id)) { cb(jsonError("Equipment not found", drogon::k404NotFound)); return; }
    if (!db_->tombstoneEquipmentItem(id)) {
        cb(jsonError("Failed to remove equipment", drogon::k500InternalServerError)); return;
    }
    Json::Value j; j["message"] = "Equipment removed";
    cb(jsonResp(j));
}

// ── Supplies (urgency-sorted wear view) ──────────────────────────────────────

void EquipmentController::supplies(const drogon::HttpRequestPtr&,
                                   std::function<void(const drogon::HttpResponsePtr&)>&& cb) {
    if (!db_) { cb(jsonError("Database unavailable", drogon::k503ServiceUnavailable)); return; }
    const long long now = nowEpoch();
    auto items = db_->listEquipmentItems(false);
    TypeDefaults defaults(*db_);

    // Overdue first, then due-soon, then fresh; fewest days left first within a
    // state. Untracked (machines, undated items) sink to the bottom.
    auto rank = [](SupplyState s) {
        switch (s) {
            case SupplyState::Overdue: return 0;
            case SupplyState::DueSoon: return 1;
            case SupplyState::Fresh:   return 2;
            default:                   return 3;
        }
    };

    std::vector<std::pair<long long, Json::Value>> scored;
    scored.reserve(items.size());
    for (const auto& it : items) {
        const int interval = defaults.intervalFor(it);
        SupplyStatus st = computeSupplyStatusForInterval(it.started_epoch, interval, now);
        const long long key =
            static_cast<long long>(rank(st.state)) * 1000000 + (st.days_left + 500000);
        scored.emplace_back(key, itemToJson(it, interval, now));
    }
    std::stable_sort(scored.begin(), scored.end(),
                     [](const auto& a, const auto& b) { return a.first < b.first; });

    Json::Value arr(Json::arrayValue);
    for (auto& p : scored) arr.append(p.second);
    Json::Value j; j["items"] = arr;
    cb(jsonResp(j));
}

// ── Cloud sync (opt-in) ──────────────────────────────────────────────────────

void EquipmentController::cloudSync(const drogon::HttpRequestPtr&,
                                    std::function<void(const drogon::HttpResponsePtr&)>&& cb) {
    // Refuse loudly rather than reporting a successful no-op: a user who pressed
    // "Sync" must never be told it worked when the feature is off.
    if (!sync_ || !sync_->enabled()) {
        cb(jsonError("Cloud sync is disabled - set cpapdash.enabled and a token in settings",
                     drogon::k409Conflict));
        return;
    }

    auto r = sync_->syncNow();
    Json::Value j;
    j["ok"]                = r.ok;
    j["uuids_backfilled"]  = r.uuids_backfilled;
    j["pushed_profiles"]   = r.pushed_profiles;
    j["pushed_items"]      = r.pushed_items;
    j["applied_profiles"]  = r.applied_profiles;
    j["applied_items"]     = r.applied_items;
    j["deleted_locally"]   = r.deleted_locally;
    j["kept_local"]        = r.kept_local;   // remote rows rejected: local was newer
    j["cursor"]            = r.cursor;
    if (!r.ok) j["error"] = r.error;

    // A transport failure is not a client error: local data is intact and the user
    // can retry. 502 says "the cloud leg failed", not "your request was bad".
    cb(jsonResp(j, r.ok ? drogon::k200OK : drogon::k502BadGateway));
}

} // namespace hms_cpap
