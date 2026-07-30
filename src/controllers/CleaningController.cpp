#include "controllers/CleaningController.h"

#include "services/CleaningStatus.h"
#include "utils/TimeCompat.h"

#include <chrono>
#include <cstdlib>
#include <ctime>
#include <set>
#include <string>

namespace hms_cpap {

std::shared_ptr<IDatabase> CleaningController::db_;

void CleaningController::setDatabase(std::shared_ptr<IDatabase> db) { db_ = std::move(db); }

namespace {

drogon::HttpResponsePtr jsonResp(const Json::Value& v,
                                 drogon::HttpStatusCode code = drogon::k200OK) {
    auto resp = drogon::HttpResponse::newHttpJsonResponse(v);
    resp->setStatusCode(code);
    return resp;
}

drogon::HttpResponsePtr jsonError(const std::string& msg, drogon::HttpStatusCode code) {
    Json::Value e;
    e["error"] = msg;
    return jsonResp(e, code);
}

long long nowEpoch() {
    return static_cast<long long>(
        std::chrono::system_clock::to_time_t(std::chrono::system_clock::now()));
}

/// Today as YYYY-MM-DD, used when a caller does not supply a start date.
std::string todayIso() {
    const auto t = std::chrono::system_clock::to_time_t(std::chrono::system_clock::now());
    std::tm tm{};
#ifdef _WIN32
    localtime_s(&tm, &t);
#else
    localtime_r(&t, &tm);
#endif
    char buf[11];
    std::strftime(buf, sizeof(buf), "%Y-%m-%d", &tm);
    return buf;
}

/// One task plus its computed state. `status` is never stored; recomputing on
/// every read is what keeps state and storage from disagreeing.
Json::Value taskToJson(const IDatabase::CleaningTask& t) {
    Json::Value j;
    j["id"]            = t.id;
    j["profile_id"]    = t.profile_id;
    j["item_id"]       = t.item_id;                 // 0 == setup-wide
    j["client_uuid"]   = t.client_uuid;
    j["task_key"]      = t.task_key;
    j["label"]         = t.label;
    j["interval_days"] = t.interval_days;
    j["time_minutes"]  = t.time_minutes;
    j["start_date"]    = t.start_date;
    j["enabled"]       = t.enabled;
    j["last_done_at"]  = t.last_done_at;
    j["created_at"]    = t.created_at;
    j["updated_at"]    = t.updated_at;

    const auto s = computeCleaningStatus(t.start_date_epoch, t.interval_days,
                                         t.time_minutes, t.last_done_epoch,
                                         t.enabled, nowEpoch());
    Json::Value st;
    st["state"]          = cleaningStateString(s.state);
    st["next_due_epoch"] = static_cast<Json::Int64>(s.next_due_epoch);
    st["days_until"]     = s.days_until;
    j["status"] = st;
    return j;
}

/// Reads a task out of a request body. `existing` supplies defaults so a PUT can
/// be partial rather than requiring the client to echo every field back.
IDatabase::CleaningTask taskFromJson(const Json::Value& b,
                                     const IDatabase::CleaningTask& existing) {
    IDatabase::CleaningTask t = existing;
    if (b.isMember("profile_id"))    t.profile_id    = b["profile_id"].asInt();
    if (b.isMember("item_id"))       t.item_id       = b["item_id"].asInt();
    if (b.isMember("client_uuid"))   t.client_uuid   = b["client_uuid"].asString();
    if (b.isMember("task_key"))      t.task_key      = b["task_key"].asString();
    if (b.isMember("label"))         t.label         = b["label"].asString();
    if (b.isMember("interval_days")) t.interval_days = b["interval_days"].asInt();
    if (b.isMember("time_minutes"))  t.time_minutes  = b["time_minutes"].asInt();
    if (b.isMember("start_date"))    t.start_date    = b["start_date"].asString();
    if (b.isMember("enabled"))       t.enabled       = b["enabled"].asBool();
    return t;
}

/// Shared validation. Returns "" when the task is acceptable.
///
/// interval_days and time_minutes are checked here rather than left to the
/// schema because SQLite does not enforce CHECK constraints added after the
/// fact and MySQL only gained them in 8.0.16: relying on the database would mean
/// the same bad input is rejected on one backend and stored on another.
std::string validate(const IDatabase::CleaningTask& t) {
    if (t.profile_id <= 0)     return "profile_id is required";
    if (t.task_key.empty())    return "task_key is required";
    if (t.label.empty())       return "label is required";
    if (t.interval_days <= 0)  return "interval_days must be greater than 0";
    if (t.time_minutes < 0 || t.time_minutes > 1439)
        return "time_minutes must be between 0 and 1439";
    return "";
}

}  // namespace

void CleaningController::listTypes(const drogon::HttpRequestPtr&,
                                   std::function<void(const drogon::HttpResponsePtr&)>&& cb) {
    if (!db_) { cb(jsonError("Database not initialized", drogon::k500InternalServerError)); return; }

    Json::Value out(Json::arrayValue);
    for (const auto& t : db_->listCleaningTaskTypes()) {
        Json::Value j;
        j["id"]                    = t.id;
        j["task_key"]              = t.task_key;
        j["label"]                 = t.label;
        j["applies_to_type_key"]   = t.applies_to_type_key;
        j["default_interval_days"] = t.default_interval_days;
        j["is_system"]             = t.is_system;
        out.append(j);
    }
    cb(jsonResp(out));
}

void CleaningController::list(const drogon::HttpRequestPtr& req,
                              std::function<void(const drogon::HttpResponsePtr&)>&& cb) {
    if (!db_) { cb(jsonError("Database not initialized", drogon::k500InternalServerError)); return; }

    // ?profile_id=N narrows to one setup; absent means every setup.
    int profile_id = 0;
    const auto p = req->getParameter("profile_id");
    if (!p.empty()) profile_id = std::atoi(p.c_str());

    Json::Value out(Json::arrayValue);
    for (const auto& t : db_->listCleaningTasks(profile_id)) out.append(taskToJson(t));
    cb(jsonResp(out));
}

void CleaningController::create(const drogon::HttpRequestPtr& req,
                                std::function<void(const drogon::HttpResponsePtr&)>&& cb) {
    if (!db_) { cb(jsonError("Database not initialized", drogon::k500InternalServerError)); return; }

    auto body = req->getJsonObject();
    if (!body) { cb(jsonError("Invalid JSON body", drogon::k400BadRequest)); return; }

    IDatabase::CleaningTask blank;
    blank.time_minutes = kCleaningDefaultTimeMinutes;
    blank.start_date   = todayIso();
    auto t = taskFromJson(*body, blank);
    t.id = 0;   // create never updates, whatever the body claims

    const auto err = validate(t);
    if (!err.empty()) { cb(jsonError(err, drogon::k400BadRequest)); return; }

    const int id = db_->upsertCleaningTask(t, "");
    if (id <= 0) {
        // The most likely cause by far is the one-live-task-per-(profile,key)
        // rule, which is what makes /suggest idempotent. Say so rather than
        // reporting a generic failure.
        cb(jsonError("Could not create the task; this setup may already have one "
                     "for '" + t.task_key + "'", drogon::k409Conflict));
        return;
    }

    auto created = db_->getCleaningTask(id);
    cb(jsonResp(created ? taskToJson(*created) : Json::Value(), drogon::k201Created));
}

void CleaningController::update(const drogon::HttpRequestPtr& req,
                                std::function<void(const drogon::HttpResponsePtr&)>&& cb,
                                std::string id) {
    if (!db_) { cb(jsonError("Database not initialized", drogon::k500InternalServerError)); return; }

    auto body = req->getJsonObject();
    if (!body) { cb(jsonError("Invalid JSON body", drogon::k400BadRequest)); return; }

    const int task_id = std::atoi(id.c_str());
    auto existing = db_->getCleaningTask(task_id);
    if (!existing || existing->deleted) {
        cb(jsonError("Task not found", drogon::k404NotFound));
        return;
    }

    auto t = taskFromJson(*body, *existing);
    t.id = task_id;

    const auto err = validate(t);
    if (!err.empty()) { cb(jsonError(err, drogon::k400BadRequest)); return; }

    if (db_->upsertCleaningTask(t, "") <= 0) {
        cb(jsonError("Could not update the task", drogon::k500InternalServerError));
        return;
    }
    auto updated = db_->getCleaningTask(task_id);
    cb(jsonResp(updated ? taskToJson(*updated) : Json::Value()));
}

void CleaningController::remove(const drogon::HttpRequestPtr&,
                                std::function<void(const drogon::HttpResponsePtr&)>&& cb,
                                std::string id) {
    if (!db_) { cb(jsonError("Database not initialized", drogon::k500InternalServerError)); return; }

    if (!db_->tombstoneCleaningTask(std::atoi(id.c_str()), "")) {
        cb(jsonError("Task not found", drogon::k404NotFound));
        return;
    }
    Json::Value ok;
    ok["status"] = "deleted";
    cb(jsonResp(ok));
}

void CleaningController::markDone(const drogon::HttpRequestPtr&,
                                  std::function<void(const drogon::HttpResponsePtr&)>&& cb,
                                  std::string id) {
    if (!db_) { cb(jsonError("Database not initialized", drogon::k500InternalServerError)); return; }

    const int task_id = std::atoi(id.c_str());
    if (!db_->markCleaningTaskDone(task_id, "")) {
        cb(jsonError("Task not found", drogon::k404NotFound));
        return;
    }
    // Return the RECOMPUTED task: marking done is what advances the clock, and
    // the caller wants the new due date without a second round trip.
    auto t = db_->getCleaningTask(task_id);
    cb(jsonResp(t ? taskToJson(*t) : Json::Value()));
}

void CleaningController::suggest(const drogon::HttpRequestPtr& req,
                                 std::function<void(const drogon::HttpResponsePtr&)>&& cb) {
    if (!db_) { cb(jsonError("Database not initialized", drogon::k500InternalServerError)); return; }

    auto body = req->getJsonObject();
    const int profile_id = (body && body->isMember("profile_id"))
                               ? (*body)["profile_id"].asInt() : 0;
    if (profile_id <= 0) {
        cb(jsonError("profile_id is required", drogon::k400BadRequest));
        return;
    }

    // Only suggest what this setup can actually have: a profile with no
    // humidifier has no water tub to empty, and offering the task anyway is how
    // a checklist starts getting ignored.
    std::set<std::string> owned;
    for (const auto& item : db_->listEquipmentItems(false)) {
        if (item.profile_id == profile_id) owned.insert(item.type_key);
    }

    const std::string today = todayIso();
    Json::Value created(Json::arrayValue);
    int skipped = 0;

    for (const auto& type : db_->listCleaningTaskTypes()) {
        // A preset with no applies_to_type_key is setup-wide and always offered.
        if (!type.applies_to_type_key.empty() && owned.count(type.applies_to_type_key) == 0)
            continue;

        IDatabase::CleaningTask t;
        t.profile_id    = profile_id;
        t.task_key      = type.task_key;
        // Snapshot the label: renaming the catalog later must not rewrite what
        // the user actually scheduled.
        t.label         = type.label;
        t.interval_days = type.default_interval_days;
        t.time_minutes  = kCleaningDefaultTimeMinutes;
        t.start_date    = today;
        // Everything arrives OFF. Nothing should start nagging because the user
        // pressed one button to see what was on offer.
        t.enabled       = false;

        const int id = db_->upsertCleaningTask(t, "");
        if (id > 0) {
            auto made = db_->getCleaningTask(id);
            if (made) created.append(taskToJson(*made));
        } else {
            // Already present. This is what makes /suggest idempotent, so it is
            // reported as a count rather than an error.
            ++skipped;
        }
    }

    Json::Value out;
    out["created"]         = created;
    out["already_present"] = skipped;
    cb(jsonResp(out, drogon::k201Created));
}

}  // namespace hms_cpap
