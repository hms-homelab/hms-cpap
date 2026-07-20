//
// test_EquipmentController.cpp — SDD-004 REST surface, exercised in-process.
//
// Why not over a socket: the web server mounts an SPA fallback that serves
// index.html for anything it does not route, so a mistyped or missing route
// answers 200 with HTML and a socket-level test passes while the API is broken.
// The handlers are therefore called DIRECTLY — a hand-built HttpRequest in, the
// HttpResponsePtr captured out of the callback — and backed by a REAL
// SQLiteDatabase on a temp file. No mocks: the SQL, the seeded type catalog and
// the one-machine partial index are all part of what is under test here.
//
#include <gtest/gtest.h>

#include <drogon/HttpRequest.h>
#include <drogon/HttpResponse.h>
#include <drogon/HttpTypes.h>

#include "controllers/EquipmentController.h"
#include "database/SQLiteDatabase.h"

#include <cstdint>
#include <ctime>
#include <filesystem>
#include <functional>
#include <map>
#include <memory>
#include <string>
#include <vector>
#ifndef _WIN32
#include <unistd.h>
#endif

using namespace hms_cpap;

namespace {

using Callback = std::function<void(const drogon::HttpResponsePtr&)>;
using Handler0 = void (EquipmentController::*)(const drogon::HttpRequestPtr&, Callback&&);
using Handler1 = void (EquipmentController::*)(const drogon::HttpRequestPtr&, Callback&&, int);

/// What a handler answered: HTTP status plus the parsed body. Tests assert on the
/// parsed JSON, never on the serialized string.
struct Reply {
    int         status{0};
    Json::Value json;

    const Json::Value& operator[](const char* k) const { return json[k]; }
    std::string error() const { return json.get("error", "").asString(); }
};

std::string dumpJson(const Json::Value& v) {
    Json::StreamWriterBuilder wb;
    wb["indentation"] = "";
    return Json::writeString(wb, v);
}

/// ISO-8601 UTC instant `days` in the past, nudged an hour earlier so the handful
/// of seconds between building the request and the handler reading the clock can
/// never flip a truncated days_left across a boundary.
std::string daysAgoIso(int days) {
    const std::time_t t = std::time(nullptr) - static_cast<std::time_t>(days) * 86400 + 3600;
    std::tm tm{};
#ifdef _WIN32
    ::gmtime_s(&tm, &t);
#else
    ::gmtime_r(&t, &tm);
#endif
    char buf[32];
    std::strftime(buf, sizeof(buf), "%Y-%m-%dT%H:%M:%SZ", &tm);
    return buf;
}

class EquipmentControllerTest : public ::testing::Test {
protected:
    EquipmentController              ctrl_;
    std::shared_ptr<SQLiteDatabase>  db_;
    std::string                      path_;

    void SetUp() override {
        path_ = (std::filesystem::temp_directory_path() /
                 ("hms_eqctl_" + std::to_string(::getpid()) + "_" +
                  std::to_string(reinterpret_cast<uintptr_t>(this)) + ".db")).string();
        std::filesystem::remove(path_);
        db_ = std::make_shared<SQLiteDatabase>(path_);
        ASSERT_TRUE(db_->connect());
        EquipmentController::setDatabase(db_);
    }

    void TearDown() override {
        EquipmentController::setDatabase(nullptr);
        db_.reset();
        std::filesystem::remove(path_);
    }

    // ── request/response plumbing ────────────────────────────────────────────

    static drogon::HttpRequestPtr request(drogon::HttpMethod method,
                                          const std::string& path,
                                          const Json::Value* body = nullptr) {
        auto req = drogon::HttpRequest::newHttpRequest();
        req->setMethod(method);
        req->setPath(path);
        if (body) {
            req->setContentTypeCode(drogon::CT_APPLICATION_JSON);
            req->setBody(dumpJson(*body));
        }
        return req;
    }

    static Reply toReply(const drogon::HttpResponsePtr& resp) {
        Reply r;
        r.status = static_cast<int>(resp->getStatusCode());
        const std::string body{resp->getBody()};
        if (!body.empty()) {
            Json::CharReaderBuilder rb;
            std::unique_ptr<Json::CharReader> reader(rb.newCharReader());
            std::string errs;
            reader->parse(body.data(), body.data() + body.size(), &r.json, &errs);
        }
        return r;
    }

    Reply call(Handler0 fn, const drogon::HttpRequestPtr& req) {
        Reply captured;
        bool  fired = false;
        (ctrl_.*fn)(req, [&](const drogon::HttpResponsePtr& resp) {
            captured = toReply(resp);
            fired    = true;
        });
        EXPECT_TRUE(fired) << "handler never invoked its callback";
        return captured;
    }

    Reply call(Handler1 fn, const drogon::HttpRequestPtr& req, int id) {
        Reply captured;
        bool  fired = false;
        (ctrl_.*fn)(req, [&](const drogon::HttpResponsePtr& resp) {
            captured = toReply(resp);
            fired    = true;
        }, id);
        EXPECT_TRUE(fired) << "handler never invoked its callback";
        return captured;
    }

    // ── route shorthands ─────────────────────────────────────────────────────

    Reply getTypes() {
        return call(&EquipmentController::listTypes, request(drogon::Get, "/api/equipment/types"));
    }
    Reply postType(const Json::Value& b) {
        return call(&EquipmentController::createType,
                    request(drogon::Post, "/api/equipment/types", &b));
    }
    Reply getProfiles() {
        return call(&EquipmentController::listProfiles,
                    request(drogon::Get, "/api/equipment/profiles"));
    }
    Reply postProfile(const Json::Value& b) {
        return call(&EquipmentController::createProfile,
                    request(drogon::Post, "/api/equipment/profiles", &b));
    }
    Reply deleteProfile(int id) {
        return call(&EquipmentController::deleteProfile,
                    request(drogon::Delete, "/api/equipment/profiles/x"), id);
    }
    Reply postItem(const Json::Value& b) {
        return call(&EquipmentController::createItem, request(drogon::Post, "/api/equipment", &b));
    }
    Reply putItem(int id, const Json::Value& b) {
        return call(&EquipmentController::updateItem,
                    request(drogon::Put, "/api/equipment/x", &b), id);
    }
    Reply deleteItem(int id) {
        return call(&EquipmentController::deleteItem,
                    request(drogon::Delete, "/api/equipment/x"), id);
    }
    Reply getSupplies() {
        return call(&EquipmentController::supplies, request(drogon::Get, "/api/supplies"));
    }

    // ── payload builders ─────────────────────────────────────────────────────

    static Json::Value item(const std::string& type_key, const std::string& started = "") {
        Json::Value j;
        j["type_key"] = type_key;
        if (!started.empty()) j["started_using_at"] = started;
        return j;
    }
};

// ── Type catalog ─────────────────────────────────────────────────────────────

TEST_F(EquipmentControllerTest, ListTypesReturnsTheSixSeededSystemTypes) {
    auto r = getTypes();
    ASSERT_EQ(r.status, 200);
    ASSERT_TRUE(r["types"].isArray());

    std::map<std::string, Json::Value> system;
    for (const auto& t : r["types"])
        if (t["is_system"].asBool()) system[t["type_key"].asString()] = t;

    // These six must match the phone app's supply_defaults.dart, or app, cloud and
    // self-hosted compute different due dates for the same mask.
    ASSERT_EQ(system.size(), 6u);
    for (const char* k : {"machine", "mask", "tubing", "filter", "humidifier", "headgear"})
        EXPECT_TRUE(system.count(k)) << "missing seeded system type " << k;

    EXPECT_EQ(system["mask"]["category"].asString(), "accessory");
    EXPECT_EQ(system["mask"]["default_replace_after_days"].asInt(), 90);
    EXPECT_EQ(system["filter"]["default_replace_after_days"].asInt(), 30);
    EXPECT_EQ(system["machine"]["category"].asString(), "machine");
    EXPECT_TRUE(system["machine"]["default_replace_after_days"].isNull())
        << "a machine is never a consumable";
    EXPECT_FALSE(system["mask"]["custom"].asBool());
}

TEST_F(EquipmentControllerTest, CreateCustomTypeThenDuplicateIsConflict) {
    Json::Value b;
    b["type_key"] = "battery";
    b["label"]    = "Battery Pack";
    b["category"] = "accessory";
    b["default_replace_after_days"] = 365;

    auto created = postType(b);
    ASSERT_EQ(created.status, 201) << created.error();
    EXPECT_GT(created["id"].asInt(), 0);
    EXPECT_EQ(created["type_key"].asString(), "battery");
    EXPECT_EQ(created["default_replace_after_days"].asInt(), 365);
    EXPECT_TRUE(created["custom"].asBool());
    EXPECT_FALSE(created["is_system"].asBool());

    // Visible to the catalog, and the seeded six are untouched.
    auto types = getTypes();
    bool found = false;
    for (const auto& t : types["types"]) if (t["type_key"].asString() == "battery") found = true;
    EXPECT_TRUE(found);

    EXPECT_EQ(postType(b).status, 409) << "type_key is unique";
}

TEST_F(EquipmentControllerTest, CreateTypeRejectsBadCategoryAndMissingFields) {
    Json::Value bad;
    bad["type_key"] = "gizmo";
    bad["label"]    = "Gizmo";
    bad["category"] = "widget";
    EXPECT_EQ(postType(bad).status, 400) << "category is machine|accessory only";

    Json::Value noLabel;
    noLabel["type_key"] = "gizmo";
    EXPECT_EQ(postType(noLabel).status, 400);

    Json::Value noKey;
    noKey["label"] = "Gizmo";
    EXPECT_EQ(postType(noKey).status, 400);

    // None of the rejects may have leaked a row into the catalog.
    for (const auto& t : getTypes()["types"])
        EXPECT_NE(t["type_key"].asString(), "gizmo");
}

TEST_F(EquipmentControllerTest, DeleteUnknownTypeIsNotFound) {
    auto r = call(&EquipmentController::deleteType,
                  request(drogon::Delete, "/api/equipment/types/x"), 987654);
    EXPECT_EQ(r.status, 404);
}

// ── Profiles ─────────────────────────────────────────────────────────────────

TEST_F(EquipmentControllerTest, CreateProfileWithSeededItemsNestsThemWithSupply) {
    Json::Value b;
    b["name"] = "Bedroom";
    b["items"].append(item("machine"));
    b["items"].append(item("mask", daysAgoIso(80)));     // 90d default -> 10 left
    b["items"].append(item("filter", daysAgoIso(1)));    // 30d default -> 29 left

    auto r = postProfile(b);
    ASSERT_EQ(r.status, 201) << r.error();
    EXPECT_GT(r["id"].asInt(), 0);
    EXPECT_EQ(r["name"].asString(), "Bedroom");
    ASSERT_TRUE(r["items"].isArray());
    ASSERT_EQ(r["items"].size(), 3u);

    std::map<std::string, Json::Value> by_type;
    for (const auto& it : r["items"]) {
        by_type[it["type_key"].asString()] = it;
        EXPECT_EQ(it["profile_id"].asInt(), r["id"].asInt());
        ASSERT_TRUE(it.isMember("supply")) << "every item carries a computed supply block";
        EXPECT_TRUE(it["supply"].isMember("state"));
        EXPECT_TRUE(it["supply"].isMember("days_left"));
        EXPECT_TRUE(it["supply"].isMember("wear_fraction"));
        EXPECT_TRUE(it["supply"].isMember("replace_by"));
    }

    // Category is resolved from the type, not trusted from the payload.
    EXPECT_EQ(by_type["machine"]["category"].asString(), "machine");
    EXPECT_EQ(by_type["mask"]["category"].asString(), "accessory");

    EXPECT_EQ(by_type["machine"]["supply"]["state"].asString(), "untracked");
    EXPECT_EQ(by_type["mask"]["supply"]["state"].asString(), "due_soon");
    EXPECT_EQ(by_type["mask"]["supply"]["days_left"].asInt(), 10);
    EXPECT_EQ(by_type["filter"]["supply"]["state"].asString(), "fresh");
    EXPECT_EQ(by_type["filter"]["supply"]["days_left"].asInt(), 29);
    EXPECT_NEAR(by_type["filter"]["supply"]["wear_fraction"].asDouble(), 1.0 / 30.0, 0.02);

    // And the same shape comes back from the list route.
    auto list = getProfiles();
    ASSERT_EQ(list.status, 200);
    ASSERT_EQ(list["profiles"].size(), 1u);
    EXPECT_EQ(list["profiles"][0]["items"].size(), 3u);
}

TEST_F(EquipmentControllerTest, ProfileNameIsRequired) {
    Json::Value b;
    b["name"] = "";
    EXPECT_EQ(postProfile(b).status, 400);
}

// The one-machine rule inside a seed payload. Postgres and SQLite also hold this
// with a partial unique index, but MySQL cannot express one — so this controller
// check is the ONLY enforcement there and must be tested at this layer.
TEST_F(EquipmentControllerTest, SecondMachineInSeedPayloadIsRejected) {
    Json::Value b;
    b["name"] = "Two machines";
    b["items"].append(item("machine"));
    b["items"].append(item("machine"));

    auto r = postProfile(b);
    EXPECT_EQ(r.status, 400);
    EXPECT_NE(r.error().find("only one machine"), std::string::npos) << r.error();
}

TEST_F(EquipmentControllerTest, SecondMachineViaItemRouteIsRejected) {
    ASSERT_EQ(postItem(item("machine")).status, 201);

    auto r = postItem(item("machine"));
    EXPECT_EQ(r.status, 400);
    EXPECT_NE(r.error().find("only one machine"), std::string::npos) << r.error();

    // The rejected machine must not have been written.
    auto profiles = getProfiles();
    ASSERT_EQ(profiles["profiles"].size(), 1u);
    EXPECT_EQ(profiles["profiles"][0]["items"].size(), 1u);
}

TEST_F(EquipmentControllerTest, DeleteProfileCascadesToItsItems) {
    Json::Value b;
    b["name"] = "Travel";
    b["items"].append(item("machine"));
    b["items"].append(item("mask", daysAgoIso(5)));
    auto created = postProfile(b);
    ASSERT_EQ(created.status, 201);
    const int pid = created["id"].asInt();

    auto del = deleteProfile(pid);
    EXPECT_EQ(del.status, 200) << del.error();

    EXPECT_EQ(getProfiles()["profiles"].size(), 0u);
    EXPECT_EQ(getSupplies()["items"].size(), 0u) << "items must cascade with the profile";

    EXPECT_EQ(deleteProfile(pid).status, 404) << "second delete is a 404, not a 500";
    EXPECT_EQ(deleteProfile(987654).status, 404);
}

TEST_F(EquipmentControllerTest, UpdateUnknownProfileIsNotFound) {
    Json::Value b;
    b["name"] = "Nope";
    auto r = call(&EquipmentController::updateProfile,
                  request(drogon::Put, "/api/equipment/profiles/x", &b), 987654);
    EXPECT_EQ(r.status, 404);
}

// ── Items ────────────────────────────────────────────────────────────────────

TEST_F(EquipmentControllerTest, CreateItemWithUnknownTypeIsRejected) {
    auto r = postItem(item("flux_capacitor"));
    EXPECT_EQ(r.status, 400);
    EXPECT_NE(r.error().find("Unknown equipment type"), std::string::npos) << r.error();

    EXPECT_EQ(getSupplies()["items"].size(), 0u);
}

TEST_F(EquipmentControllerTest, CreateItemRequiresTypeKey) {
    Json::Value b;
    b["brand"] = "ResMed";
    EXPECT_EQ(postItem(b).status, 400);
}

TEST_F(EquipmentControllerTest, ItemWithNoProfileIdLandsInTheDefaultProfile) {
    ASSERT_EQ(getProfiles()["profiles"].size(), 0u) << "no profile exists yet";

    auto created = postItem(item("mask", daysAgoIso(3)));
    ASSERT_EQ(created.status, 201) << created.error();
    const int pid = created["profile_id"].asInt();
    EXPECT_GT(pid, 0) << "an item must always be homed in a profile";

    auto profiles = getProfiles();
    ASSERT_EQ(profiles["profiles"].size(), 1u) << "the default profile was created for it";
    EXPECT_EQ(profiles["profiles"][0]["id"].asInt(), pid);
    ASSERT_EQ(profiles["profiles"][0]["items"].size(), 1u);

    // A second unhomed item reuses the same default profile rather than spawning one.
    auto second = postItem(item("tubing"));
    ASSERT_EQ(second.status, 201);
    EXPECT_EQ(second["profile_id"].asInt(), pid);
    EXPECT_EQ(getProfiles()["profiles"].size(), 1u);
}

TEST_F(EquipmentControllerTest, CreateItemIntoAnUnknownProfileIsRejected) {
    Json::Value b = item("mask");
    b["profile_id"] = 987654;
    EXPECT_EQ(postItem(b).status, 400);
}

TEST_F(EquipmentControllerTest, UpdateItemEditsFieldsAndRecomputesSupply) {
    auto created = postItem(item("mask", daysAgoIso(3)));
    ASSERT_EQ(created.status, 201);
    const int id = created["id"].asInt();
    EXPECT_EQ(created["supply"]["state"].asString(), "fresh");   // 90d default
    EXPECT_TRUE(created["replace_after_days"].isNull()) << "no override -> null, not 0";

    Json::Value edit;
    edit["type_key"]           = "mask";
    edit["brand"]              = "Philips";
    edit["model"]              = "DreamWear";
    edit["started_using_at"]   = daysAgoIso(40);
    edit["replace_after_days"] = 45;

    auto updated = putItem(id, edit);
    ASSERT_EQ(updated.status, 200) << updated.error();
    EXPECT_EQ(updated["id"].asInt(), id);
    EXPECT_EQ(updated["brand"].asString(), "Philips");
    EXPECT_EQ(updated["model"].asString(), "DreamWear");
    EXPECT_EQ(updated["replace_after_days"].asInt(), 45);
    EXPECT_EQ(updated["started_using_at"].asString(), edit["started_using_at"].asString());
    // 40 days into a 45-day override: due soon, not the 50 days the 90d default gives.
    EXPECT_EQ(updated["supply"]["days_left"].asInt(), 5);
    EXPECT_EQ(updated["supply"]["state"].asString(), "due_soon");

    // The edit is persisted, not just echoed.
    auto listed = getProfiles()["profiles"][0]["items"][0];
    EXPECT_EQ(listed["brand"].asString(), "Philips");
    EXPECT_EQ(listed["replace_after_days"].asInt(), 45);
}

TEST_F(EquipmentControllerTest, DeleteItemRemovesItAndThen404s) {
    auto created = postItem(item("mask", daysAgoIso(3)));
    ASSERT_EQ(created.status, 201);
    const int id = created["id"].asInt();

    auto del = deleteItem(id);
    EXPECT_EQ(del.status, 200) << del.error();
    EXPECT_EQ(getSupplies()["items"].size(), 0u);

    EXPECT_EQ(deleteItem(id).status, 404) << "a tombstoned item reads as absent";
    EXPECT_EQ(deleteItem(987654).status, 404);

    Json::Value edit = item("mask");
    EXPECT_EQ(putItem(987654, edit).status, 404);
}

// ── Supplies ─────────────────────────────────────────────────────────────────

TEST_F(EquipmentControllerTest, SuppliesAreUrgencySorted) {
    Json::Value b;
    b["name"] = "Bedroom";
    // Deliberately inserted in the WRONG order, so passing means the route sorted.
    b["items"].append(item("filter", daysAgoIso(1)));      // 30d  -> fresh (29 left)
    b["items"].append(item("machine"));                    //      -> untracked
    b["items"].append(item("mask", daysAgoIso(200)));      // 90d  -> overdue
    b["items"].append(item("tubing", daysAgoIso(80)));     // 90d  -> due_soon (10 left)
    ASSERT_EQ(postProfile(b).status, 201);

    auto r = getSupplies();
    ASSERT_EQ(r.status, 200);
    ASSERT_EQ(r["items"].size(), 4u);

    std::vector<std::string> states, types;
    for (const auto& it : r["items"]) {
        states.push_back(it["supply"]["state"].asString());
        types.push_back(it["type_key"].asString());
    }
    EXPECT_EQ(states[0], "overdue")   << "worn out first: " << types[0];
    EXPECT_EQ(states[1], "due_soon")  << "then due soon: " << types[1];
    EXPECT_EQ(states[2], "fresh")     << "then fresh: " << types[2];
    EXPECT_EQ(states[3], "untracked") << "machines sink to the bottom: " << types[3];

    EXPECT_EQ(types[0], "mask");
    EXPECT_EQ(types[1], "tubing");
    EXPECT_EQ(types[2], "filter");
    EXPECT_EQ(types[3], "machine");

    EXPECT_LT(r["items"][0]["supply"]["days_left"].asInt(), 0) << "overdue is negative";
    EXPECT_NEAR(r["items"][0]["supply"]["wear_fraction"].asDouble(), 1.0, 1e-9)
        << "wear clamps at 1.0";
}

TEST_F(EquipmentControllerTest, SuppliesSortsMostUrgentFirstWithinAState) {
    Json::Value b;
    b["name"] = "Bedroom";
    b["items"].append(item("tubing", daysAgoIso(82)));   // 8 days left
    b["items"].append(item("mask",   daysAgoIso(88)));   // 2 days left
    ASSERT_EQ(postProfile(b).status, 201);

    auto r = getSupplies();
    ASSERT_EQ(r["items"].size(), 2u);
    EXPECT_EQ(r["items"][0]["type_key"].asString(), "mask")
        << "fewest days left first within the same state";
    EXPECT_EQ(r["items"][0]["supply"]["days_left"].asInt(), 2);
    EXPECT_EQ(r["items"][1]["supply"]["days_left"].asInt(), 8);
}

// The per-item override is what makes a 30-day filter user tell the app so; it
// must beat the catalog default in the computed block, not merely be stored.
TEST_F(EquipmentControllerTest, PerItemIntervalOverridesTheTypeDefault) {
    Json::Value b = item("mask", daysAgoIso(20));
    b["replace_after_days"] = 25;                       // vs the 90-day type default

    auto created = postItem(b);
    ASSERT_EQ(created.status, 201) << created.error();
    EXPECT_EQ(created["replace_after_days"].asInt(), 25);
    EXPECT_EQ(created["supply"]["days_left"].asInt(), 5) << "70 would mean the default won";
    EXPECT_EQ(created["supply"]["state"].asString(), "due_soon");
    EXPECT_NEAR(created["supply"]["wear_fraction"].asDouble(), 20.0 / 25.0, 0.02);

    auto listed = getSupplies()["items"][0];
    EXPECT_EQ(listed["supply"]["days_left"].asInt(), 5) << "same interval on every route";
}

// A custom type's default lives only in the DB, so this is the path that proves
// the catalog — not the hardcoded supplyDefaultDays table — drives the interval.
TEST_F(EquipmentControllerTest, CustomTypeDefaultDrivesSupply) {
    Json::Value t;
    t["type_key"] = "battery";
    t["label"]    = "Battery Pack";
    t["category"] = "accessory";
    t["default_replace_after_days"] = 30;
    ASSERT_EQ(postType(t).status, 201);

    auto created = postItem(item("battery", daysAgoIso(25)));
    ASSERT_EQ(created.status, 201) << created.error();
    EXPECT_EQ(created["category"].asString(), "accessory");
    EXPECT_TRUE(created["replace_after_days"].isNull()) << "no override: the catalog supplies it";
    EXPECT_EQ(created["supply"]["days_left"].asInt(), 5);
    EXPECT_EQ(created["supply"]["state"].asString(), "due_soon");
}

TEST_F(EquipmentControllerTest, SuppliesIsEmptyWithNoEquipment) {
    auto r = getSupplies();
    ASSERT_EQ(r.status, 200);
    ASSERT_TRUE(r["items"].isArray());
    EXPECT_EQ(r["items"].size(), 0u);
}

} // namespace
