//
// test_CpapDashSyncService.cpp — SDD-004 phase 6, optional cloud sync.
//
// Backed by a REAL SQLiteDatabase (the reconcile is mostly about what the DB layer
// does with tombstones and sentinels, so a mock database would test nothing) and a
// FAKE transport, so the whole suite runs with no network and no cloud account.
//
// The properties that matter, and why:
//   * a row with no client_uuid gets one before the first push — without it a
//     retried sync inserts a second copy of everything in the cloud,
//   * last-write-wins in BOTH directions, with local winning ties: the cloud is a
//     mirror, so it must never overwrite an edit the user made more recently,
//   * tombstones travel both ways,
//   * the cursor advances so the next sync is incremental,
//   * a transport failure leaves the local database byte-for-byte as it was —
//     "cloud is down" must degrade to local-only, never to data loss.
//
#include <gtest/gtest.h>

#include "database/SQLiteDatabase.h"
#include "services/CpapDashSyncService.h"

#include <nlohmann/json.hpp>

#include <filesystem>
#include <memory>
#include <string>
#include <vector>

using namespace hms_cpap;
using json = nlohmann::json;

namespace {

class CpapDashSync : public ::testing::Test {
protected:
    std::shared_ptr<SQLiteDatabase> db_;
    std::string db_path_;
    std::string state_path_;
    CpapDashSyncService svc_;

    // Fake cloud: records every request and replays a scripted response.
    std::vector<json> requests_;
    json              next_response_ = json::object();
    bool              transport_ok_  = true;

    void SetUp() override {
        const std::string tag = std::to_string(::getpid()) + "_" +
                                std::to_string(reinterpret_cast<uintptr_t>(this));
        db_path_    = (std::filesystem::temp_directory_path() / ("hms_sync_" + tag + ".db")).string();
        state_path_ = (std::filesystem::temp_directory_path() / ("hms_sync_" + tag + ".json")).string();
        std::filesystem::remove(db_path_);
        std::filesystem::remove(state_path_);

        db_ = std::make_shared<SQLiteDatabase>(db_path_);
        ASSERT_TRUE(db_->connect());

        CpapDashSyncService::Settings s;
        s.enabled  = true;
        s.api_url  = "https://api.example.test";
        s.token    = "test-token";
        s.auto_sync = false;
        svc_.setDatabase(db_);
        svc_.setSettings(s);
        svc_.setStatePath(state_path_);
        svc_.setTransport([this](const std::string&, const std::string& body, std::string& out) {
            json req = json::parse(body, nullptr, false);
            requests_.push_back(req.is_discarded() ? json::object() : req);
            if (!transport_ok_) return false;
            out = next_response_.dump();
            return true;
        });

        next_response_ = emptyResponse();
    }

    void TearDown() override {
        db_.reset();
        std::filesystem::remove(db_path_);
        std::filesystem::remove(state_path_);
    }

    static json emptyResponse(const std::string& server_time = "2026-07-19T12:00:00Z") {
        json r;
        r["profiles"]           = json::array();
        r["items"]              = json::array();
        r["default_profile_id"] = 1;
        r["server_time"]        = server_time;
        return r;
    }

    // The DB layer owns updated_at (it stamps now() on every write), so tests that
    // need a specific age set it directly. SQLite-only, which is fine: this suite
    // is about the reconcile, and the backend parity suite covers the engines.
    void setUpdatedAt(const char* table, int id, const std::string& ts) {
        db_->executeQuery(std::string("UPDATE ") + table + " SET updated_at = ? WHERE id = ?",
                          {ts, std::to_string(id)});
    }

    int addMask(int profile_id, const std::string& brand = "ResMed",
                const std::string& uuid = "") {
        IDatabase::EquipmentItem it;
        it.profile_id  = profile_id;
        it.client_uuid = uuid;
        it.type_key    = "mask";
        it.brand       = brand;
        it.model       = "AirFit P10";
        return db_->upsertEquipmentItem(it);
    }

    const json& lastRequest() const { return requests_.back(); }

    // Find a pushed row by client_uuid in the last request's array.
    static const json* findByUuid(const json& arr, const std::string& uuid) {
        if (!arr.is_array()) return nullptr;
        for (const auto& e : arr)
            if (e.value("client_uuid", std::string{}) == uuid) return &e;
        return nullptr;
    }

    std::string uuidOfItem(int id) {
        auto it = db_->getEquipmentItem(id);
        return it ? it->client_uuid : std::string{};
    }
};

// ── uuid backfill ────────────────────────────────────────────────────────────

TEST_F(CpapDashSync, BackfillsClientUuidsBeforeTheFirstPush) {
    const int pid = db_->ensureDefaultEquipmentProfile();
    const int iid = addMask(pid);
    ASSERT_GT(iid, 0);
    ASSERT_EQ(uuidOfItem(iid), "") << "precondition: the row starts with no uuid";

    const auto r = svc_.syncNow();
    ASSERT_TRUE(r.ok) << r.error;
    EXPECT_GE(r.uuids_backfilled, 2) << "both the profile and the item needed one";

    const std::string item_uuid = uuidOfItem(iid);
    EXPECT_EQ(item_uuid.size(), 36u);
    EXPECT_EQ(db_->listEquipmentProfiles(false).at(0).client_uuid.size(), 36u);

    // And the uuid is what actually went over the wire — that is the whole point.
    ASSERT_FALSE(requests_.empty());
    EXPECT_NE(findByUuid(lastRequest()["items"], item_uuid), nullptr);
}

TEST_F(CpapDashSync, ExistingUuidsAreNeverRewritten) {
    const int pid = db_->ensureDefaultEquipmentProfile();
    const int iid = addMask(pid, "ResMed", "fixed-uuid-1234");
    ASSERT_GT(iid, 0);

    ASSERT_TRUE(svc_.syncNow().ok);
    EXPECT_EQ(uuidOfItem(iid), "fixed-uuid-1234")
        << "a stable uuid is the identity the cloud matches on";
}

// ── idempotency ──────────────────────────────────────────────────────────────

TEST_F(CpapDashSync, ResyncIsIdempotentAndCreatesNoDuplicates) {
    const int pid = db_->ensureDefaultEquipmentProfile();
    ASSERT_GT(addMask(pid), 0);
    ASSERT_TRUE(svc_.syncNow().ok);

    const std::string puuid = db_->listEquipmentProfiles(false).at(0).client_uuid;
    const std::string iuuid = db_->listEquipmentItems(false).at(0).client_uuid;

    // The cloud echoes the rows back with its own ids, as it does on a real sync.
    json resp = emptyResponse("2026-07-19T12:05:00Z");
    json p; p["id"] = 41; p["client_uuid"] = puuid; p["name"] = "My CPAP";
    p["active"] = true; p["deleted"] = false; p["updated_at"] = "2020-01-01T00:00:00Z";
    resp["profiles"].push_back(p);
    json i; i["id"] = 91; i["client_uuid"] = iuuid; i["profile_id"] = 41;
    i["type_key"] = "mask"; i["brand"] = "ResMed"; i["active"] = true;
    i["deleted"] = false; i["updated_at"] = "2020-01-01T00:00:00Z";
    resp["items"].push_back(i);
    next_response_ = resp;

    ASSERT_TRUE(svc_.syncNow().ok);
    ASSERT_TRUE(svc_.syncNow().ok);

    EXPECT_EQ(db_->listEquipmentProfiles(false).size(), 1u);
    EXPECT_EQ(db_->listEquipmentItems(false).size(), 1u);
    EXPECT_EQ(db_->listEquipmentItems(false).at(0).client_uuid, iuuid);
}

// ── last-write-wins, both directions ─────────────────────────────────────────

TEST_F(CpapDashSync, RemoteWinsWhenItIsNewer) {
    const int pid = db_->ensureDefaultEquipmentProfile();
    const int iid = addMask(pid, "OldBrand");
    ASSERT_TRUE(svc_.syncNow().ok);
    const std::string iuuid = uuidOfItem(iid);
    setUpdatedAt("cpap_equipment_items", iid, "2020-01-01 00:00:00");

    json resp = emptyResponse("2026-07-19T13:00:00Z");
    json i; i["id"] = 91; i["client_uuid"] = iuuid; i["type_key"] = "mask";
    i["brand"] = "NewBrand"; i["model"] = "F30i"; i["active"] = true;
    i["deleted"] = false; i["updated_at"] = "2030-01-01T00:00:00Z";
    resp["items"].push_back(i);
    next_response_ = resp;

    const auto r = svc_.syncNow();
    ASSERT_TRUE(r.ok) << r.error;
    EXPECT_EQ(r.applied_items, 1);
    EXPECT_EQ(db_->getEquipmentItem(iid)->brand, "NewBrand");
}

TEST_F(CpapDashSync, LocalWinsWhenItIsNewer) {
    const int pid = db_->ensureDefaultEquipmentProfile();
    const int iid = addMask(pid, "LocalBrand");
    ASSERT_TRUE(svc_.syncNow().ok);
    const std::string iuuid = uuidOfItem(iid);
    setUpdatedAt("cpap_equipment_items", iid, "2030-01-01 00:00:00");

    json resp = emptyResponse("2026-07-19T13:00:00Z");
    json i; i["id"] = 91; i["client_uuid"] = iuuid; i["type_key"] = "mask";
    i["brand"] = "StaleCloudBrand"; i["active"] = true; i["deleted"] = false;
    i["updated_at"] = "2020-01-01T00:00:00Z";
    resp["items"].push_back(i);
    next_response_ = resp;

    const auto r = svc_.syncNow();
    ASSERT_TRUE(r.ok) << r.error;
    EXPECT_EQ(r.applied_items, 0);
    EXPECT_GE(r.kept_local, 1);
    EXPECT_EQ(db_->getEquipmentItem(iid)->brand, "LocalBrand")
        << "the cloud is a mirror; it must not overwrite a newer local edit";
}

// A same-second timestamp is common (SQLite stamps to the second). A tie must not
// hand the row to the cloud.
TEST_F(CpapDashSync, TieGoesToLocal) {
    const int pid = db_->ensureDefaultEquipmentProfile();
    const int iid = addMask(pid, "LocalBrand");
    ASSERT_TRUE(svc_.syncNow().ok);
    const std::string iuuid = uuidOfItem(iid);
    setUpdatedAt("cpap_equipment_items", iid, "2026-05-28 04:00:00");

    json resp = emptyResponse();
    json i; i["id"] = 91; i["client_uuid"] = iuuid; i["type_key"] = "mask";
    i["brand"] = "CloudBrand"; i["active"] = true; i["deleted"] = false;
    i["updated_at"] = "2026-05-28T04:00:00Z";
    resp["items"].push_back(i);
    next_response_ = resp;

    ASSERT_TRUE(svc_.syncNow().ok);
    EXPECT_EQ(db_->getEquipmentItem(iid)->brand, "LocalBrand");
}

// The two sides write different timestamp shapes. A string compare would call
// "2026-07-19T10:00:00+02" newer than "2026-07-19T09:30:00Z" — it is not.
TEST_F(CpapDashSync, TimestampsAreComparedAsInstantsNotStrings) {
    EXPECT_EQ(CpapDashSyncService::parseTimestampEpoch("2026-07-19T10:00:00+02:00"),
              CpapDashSyncService::parseTimestampEpoch("2026-07-19T08:00:00Z"));
    EXPECT_EQ(CpapDashSyncService::parseTimestampEpoch("2026-07-19 08:00:00"),
              CpapDashSyncService::parseTimestampEpoch("2026-07-19T08:00:00Z"));
    EXPECT_LT(CpapDashSyncService::parseTimestampEpoch("2026-07-19T10:00:00+02"),
              CpapDashSyncService::parseTimestampEpoch("2026-07-19T09:30:00Z"));
    EXPECT_EQ(CpapDashSyncService::parseTimestampEpoch("not a timestamp"), -1);
}

// ── tombstones ───────────────────────────────────────────────────────────────

TEST_F(CpapDashSync, RemoteTombstoneDeletesLocally) {
    const int pid = db_->ensureDefaultEquipmentProfile();
    const int iid = addMask(pid);
    ASSERT_TRUE(svc_.syncNow().ok);
    const std::string iuuid = uuidOfItem(iid);
    setUpdatedAt("cpap_equipment_items", iid, "2020-01-01 00:00:00");

    json resp = emptyResponse("2026-07-19T14:00:00Z");
    json i; i["id"] = 91; i["client_uuid"] = iuuid; i["type_key"] = "mask";
    i["active"] = false; i["deleted"] = true; i["updated_at"] = "2030-01-01T00:00:00Z";
    resp["items"].push_back(i);
    next_response_ = resp;

    const auto r = svc_.syncNow();
    ASSERT_TRUE(r.ok) << r.error;
    EXPECT_EQ(r.deleted_locally, 1);
    EXPECT_FALSE(db_->getEquipmentItem(iid).has_value()) << "a tombstone reads as absent";
}

TEST_F(CpapDashSync, RemoteTombstoneIsRefusedWhenLocalIsNewer) {
    const int pid = db_->ensureDefaultEquipmentProfile();
    const int iid = addMask(pid);
    ASSERT_TRUE(svc_.syncNow().ok);
    const std::string iuuid = uuidOfItem(iid);
    setUpdatedAt("cpap_equipment_items", iid, "2030-01-01 00:00:00");

    json resp = emptyResponse();
    json i; i["id"] = 91; i["client_uuid"] = iuuid; i["type_key"] = "mask";
    i["deleted"] = true; i["updated_at"] = "2020-01-01T00:00:00Z";
    resp["items"].push_back(i);
    next_response_ = resp;

    const auto r = svc_.syncNow();
    ASSERT_TRUE(r.ok) << r.error;
    EXPECT_EQ(r.deleted_locally, 0);
    EXPECT_TRUE(db_->getEquipmentItem(iid).has_value())
        << "a stale cloud delete must not destroy a row the user just edited";
}

TEST_F(CpapDashSync, LocalTombstoneIsPushedToTheCloud) {
    const int pid = db_->ensureDefaultEquipmentProfile();
    const int iid = addMask(pid);
    ASSERT_TRUE(svc_.syncNow().ok);
    const std::string iuuid = uuidOfItem(iid);

    ASSERT_TRUE(db_->tombstoneEquipmentItem(iid));
    // Force it past the push watermark (SQLite stamps whole seconds).
    setUpdatedAt("cpap_equipment_items", iid, "2030-01-01 00:00:00");

    ASSERT_TRUE(svc_.syncNow().ok);
    const json* pushed = findByUuid(lastRequest()["items"], iuuid);
    ASSERT_NE(pushed, nullptr)
        << "listEquipmentItems() hides tombstones, so a local delete would never "
           "reach the cloud without the raw read";
    EXPECT_TRUE(pushed->value("deleted", false));
    EXPECT_EQ(pushed->value("type_key", std::string{}), "mask")
        << "the cloud drops items with no type_key";
}

TEST_F(CpapDashSync, RemoteProfileTombstoneRemovesTheProfileAndItsItems) {
    const int pid = db_->ensureDefaultEquipmentProfile();
    ASSERT_GT(addMask(pid), 0);
    ASSERT_TRUE(svc_.syncNow().ok);
    const std::string puuid = db_->listEquipmentProfiles(false).at(0).client_uuid;
    setUpdatedAt("cpap_equipment_profiles", pid, "2020-01-01 00:00:00");

    json resp = emptyResponse();
    json p; p["id"] = 41; p["client_uuid"] = puuid; p["name"] = "My CPAP";
    p["deleted"] = true; p["updated_at"] = "2030-01-01T00:00:00Z";
    resp["profiles"].push_back(p);
    next_response_ = resp;

    const auto r = svc_.syncNow();
    ASSERT_TRUE(r.ok) << r.error;
    EXPECT_EQ(r.deleted_locally, 1);
    EXPECT_TRUE(db_->listEquipmentProfiles(false).empty());
}

// ── new remote rows ──────────────────────────────────────────────────────────

TEST_F(CpapDashSync, UnknownRemoteRowsAreInsertedLocally) {
    json resp = emptyResponse();
    json p; p["id"] = 41; p["client_uuid"] = "cloud-profile-uuid"; p["name"] = "Travel setup";
    p["active"] = true; p["deleted"] = false; p["updated_at"] = "2026-07-19T11:00:00Z";
    resp["profiles"].push_back(p);
    json i; i["id"] = 91; i["client_uuid"] = "cloud-item-uuid"; i["profile_id"] = 41;
    i["type_key"] = "tubing"; i["brand"] = "ResMed"; i["active"] = true;
    i["deleted"] = false; i["updated_at"] = "2026-07-19T11:00:00Z";
    resp["items"].push_back(i);
    next_response_ = resp;

    const auto r = svc_.syncNow();
    ASSERT_TRUE(r.ok) << r.error;
    EXPECT_EQ(r.applied_profiles, 1);
    EXPECT_EQ(r.applied_items, 1);

    const auto profiles = db_->listEquipmentProfiles(false);
    ASSERT_EQ(profiles.size(), 1u);
    EXPECT_EQ(profiles.at(0).name, "Travel setup");

    const auto items = db_->listEquipmentItems(false);
    ASSERT_EQ(items.size(), 1u);
    EXPECT_EQ(items.at(0).type_key, "tubing");
    EXPECT_EQ(items.at(0).profile_id, profiles.at(0).id)
        << "the item must land in the profile the server id maps to";
    EXPECT_EQ(items.at(0).category, "accessory")
        << "category is resolved from the type, not trusted from the wire";
}

// A remote tombstone for a row we never had is not a resurrection.
TEST_F(CpapDashSync, UnknownRemoteTombstoneInsertsNothing) {
    json resp = emptyResponse();
    json i; i["id"] = 91; i["client_uuid"] = "never-seen"; i["type_key"] = "mask";
    i["deleted"] = true; i["updated_at"] = "2026-07-19T11:00:00Z";
    resp["items"].push_back(i);
    next_response_ = resp;

    ASSERT_TRUE(svc_.syncNow().ok);
    EXPECT_TRUE(db_->listEquipmentItems(true).empty());
}

// ── cursor ───────────────────────────────────────────────────────────────────

TEST_F(CpapDashSync, CursorAdvancesAndIsSentOnTheNextSync) {
    db_->ensureDefaultEquipmentProfile();
    EXPECT_EQ(svc_.cursor(), "") << "never synced";

    next_response_ = emptyResponse("2026-07-19T12:00:00Z");
    ASSERT_TRUE(svc_.syncNow().ok);
    EXPECT_EQ(requests_.at(0).value("since", std::string{}), "");
    EXPECT_EQ(svc_.cursor(), "2026-07-19T12:00:00Z");

    next_response_ = emptyResponse("2026-07-19T12:30:00Z");
    ASSERT_TRUE(svc_.syncNow().ok);
    EXPECT_EQ(requests_.at(1).value("since", std::string{}), "2026-07-19T12:00:00Z")
        << "the second sync must be incremental from the stored cursor";
    EXPECT_EQ(svc_.cursor(), "2026-07-19T12:30:00Z");
}

TEST_F(CpapDashSync, CursorSurvivesAcrossServiceInstances) {
    db_->ensureDefaultEquipmentProfile();
    next_response_ = emptyResponse("2026-07-19T12:00:00Z");
    ASSERT_TRUE(svc_.syncNow().ok);

    CpapDashSyncService fresh;
    fresh.setStatePath(state_path_);
    EXPECT_EQ(fresh.cursor(), "2026-07-19T12:00:00Z");
}

// ── failure degrades to local-only ───────────────────────────────────────────

TEST_F(CpapDashSync, TransportFailureLeavesLocalDataIntact) {
    const int pid = db_->ensureDefaultEquipmentProfile();
    const int iid = addMask(pid, "ResMed");
    ASSERT_GT(iid, 0);

    transport_ok_ = false;
    const auto r = svc_.syncNow();
    EXPECT_FALSE(r.ok);
    EXPECT_FALSE(r.error.empty());

    // Everything the user owns is still exactly there, and nothing was deleted.
    EXPECT_EQ(db_->listEquipmentProfiles(false).size(), 1u);
    ASSERT_EQ(db_->listEquipmentItems(false).size(), 1u);
    EXPECT_EQ(db_->getEquipmentItem(iid)->brand, "ResMed");
    EXPECT_EQ(svc_.cursor(), "") << "a failed sync must not advance the cursor";

    // The uuid backfill is deliberately kept: it is what makes the retry idempotent.
    EXPECT_EQ(uuidOfItem(iid).size(), 36u);

    // ...and the retry succeeds against the same rows, still without duplicating.
    transport_ok_ = true;
    ASSERT_TRUE(svc_.syncNow().ok);
    EXPECT_EQ(db_->listEquipmentItems(false).size(), 1u);
}

TEST_F(CpapDashSync, MalformedResponseIsTreatedAsFailure) {
    db_->ensureDefaultEquipmentProfile();
    svc_.setTransport([](const std::string&, const std::string&, std::string& out) {
        out = "<html>502 Bad Gateway</html>";
        return true;
    });

    const auto r = svc_.syncNow();
    EXPECT_FALSE(r.ok);
    EXPECT_EQ(svc_.cursor(), "");
}

// ── opt-in: everything must work with sync off ───────────────────────────────

TEST_F(CpapDashSync, DisabledSyncTouchesNothing) {
    CpapDashSyncService::Settings s = svc_.settings();
    s.enabled = false;
    svc_.setSettings(s);

    const int pid = db_->ensureDefaultEquipmentProfile();
    const int iid = addMask(pid);

    const auto r = svc_.syncNow();
    EXPECT_FALSE(r.ok);
    EXPECT_TRUE(requests_.empty()) << "disabled sync must not reach the network";
    EXPECT_EQ(uuidOfItem(iid), "") << "and must not write to the local database";
    EXPECT_FALSE(std::filesystem::exists(state_path_));
}

TEST_F(CpapDashSync, AnEmptyTokenCountsAsDisabled) {
    CpapDashSyncService::Settings s = svc_.settings();
    s.token.clear();
    svc_.setSettings(s);
    db_->ensureDefaultEquipmentProfile();

    EXPECT_FALSE(svc_.enabled());
    EXPECT_FALSE(svc_.syncNow().ok);
    EXPECT_TRUE(requests_.empty());
}

// ── burst-loop debounce (the SleepHQ pattern) ────────────────────────────────

TEST_F(CpapDashSync, SweepOnlyRunsWhenAutoSyncIsOnAndSomethingChanged) {
    db_->ensureDefaultEquipmentProfile();

    svc_.markDirty();
    EXPECT_FALSE(svc_.isDirtyForTest()) << "auto_sync is off";
    svc_.sweep();
    EXPECT_TRUE(requests_.empty());

    CpapDashSyncService::Settings s = svc_.settings();
    s.auto_sync = true;
    svc_.setSettings(s);

    svc_.sweep();
    EXPECT_TRUE(requests_.empty()) << "nothing changed, nothing to do";

    svc_.markDirty();
    EXPECT_TRUE(svc_.isDirtyForTest());
    svc_.sweep();
    EXPECT_EQ(requests_.size(), 1u);
    EXPECT_FALSE(svc_.isDirtyForTest());
}

TEST_F(CpapDashSync, SweepStaysDirtyWhenTheCloudIsDown) {
    db_->ensureDefaultEquipmentProfile();
    CpapDashSyncService::Settings s = svc_.settings();
    s.auto_sync = true;
    svc_.setSettings(s);

    transport_ok_ = false;
    svc_.markDirty();
    svc_.sweep();
    EXPECT_TRUE(svc_.isDirtyForTest()) << "a failed sweep must retry on the next burst";
    EXPECT_EQ(db_->listEquipmentProfiles(false).size(), 1u);
}

// ── uuid shape ───────────────────────────────────────────────────────────────

TEST(CpapDashSyncUuid, LooksLikeAV4UuidAndDoesNotRepeat) {
    const std::string a = CpapDashSyncService::makeUuid();
    const std::string b = CpapDashSyncService::makeUuid();
    ASSERT_EQ(a.size(), 36u);
    EXPECT_EQ(a[8], '-');
    EXPECT_EQ(a[13], '-');
    EXPECT_EQ(a[14], '4');
    EXPECT_EQ(a[18], '-');
    EXPECT_EQ(a[23], '-');
    EXPECT_NE(a, b);
}

} // namespace
