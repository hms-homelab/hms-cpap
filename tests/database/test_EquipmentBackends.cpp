//
// test_EquipmentBackends.cpp — SDD-004 equipment layer.
//
// Two jobs:
//
//  1. SCHEMA-DRIFT GUARD. hms-cpap ships three engines and three checked-in
//     scripts/schema*.sql. Release v4.4.10 exists because those scripts drifted
//     behind the in-code migrations: databases created from the scripts were
//     missing columns the code read, and session saves failed at runtime on a
//     user's install. These tests assert every equipment column the code reads is
//     declared in all three scripts, so drift fails CI instead of an install.
//
//  2. BACKEND PARITY. Three agents implementing one contract produced ten
//     behavioural divergences (empty category defeating the one-machine rule,
//     timezone-dependent epochs, mismatched timestamp shapes). The same suite
//     therefore runs against a real backend: SQLite in-memory always, and
//     Postgres/MySQL when reachable, so "works on my engine" cannot survive.
//
#include <gtest/gtest.h>

#include "database/IDatabase.h"
#include "database/SQLiteDatabase.h"

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <memory>
#include <sstream>
#include <string>
#include <vector>

using namespace hms_cpap;

namespace {

// ─────────────────────────────────────────────────────────────────────────────
// 1. Schema-drift guard (no DB needed — reads the checked-in scripts)
// ─────────────────────────────────────────────────────────────────────────────

std::string readSchemaScript(const std::string& name) {
    // Tests run from the build dir; walk up to find scripts/.
    for (const char* prefix : {"", "../", "../../", "../../../"}) {
        std::filesystem::path p = std::string(prefix) + "scripts/" + name;
        if (std::filesystem::exists(p)) {
            std::ifstream f(p);
            std::stringstream ss;
            ss << f.rdbuf();
            return ss.str();
        }
    }
    return {};
}

std::string lower(std::string s) {
    for (auto& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

// Every column the equipment code reads or writes. If you add one, add it here —
// that is the point: the guard must fail until all three scripts carry it.
const std::vector<std::string> kTypeCols = {
    "type_key", "label", "category", "default_replace_after_days", "is_system", "active",
};
const std::vector<std::string> kProfileCols = {
    "client_uuid", "name", "active", "deleted", "created_at", "updated_at",
};
const std::vector<std::string> kItemCols = {
    "profile_id", "client_uuid", "type_key", "category", "brand", "model", "variant",
    "started_using_at", "replace_after_days", "notes", "active", "deleted",
    "created_at", "updated_at",
};

class SchemaScriptDrift : public ::testing::TestWithParam<const char*> {};

TEST_P(SchemaScriptDrift, DeclaresEveryEquipmentTableAndColumn) {
    const std::string script = readSchemaScript(GetParam());
    ASSERT_FALSE(script.empty())
        << "could not locate scripts/" << GetParam()
        << " — the drift guard cannot protect what it cannot read";

    const std::string s = lower(script);

    for (const char* table : {"cpap_equipment_types",
                              "cpap_equipment_profiles",
                              "cpap_equipment_items"}) {
        EXPECT_NE(s.find(table), std::string::npos)
            << GetParam() << " is missing table " << table
            << " — a database created from this script would fail at runtime (v4.4.10)";
    }

    auto expectCols = [&](const std::vector<std::string>& cols, const char* which) {
        for (const auto& c : cols) {
            EXPECT_NE(s.find(lower(c)), std::string::npos)
                << GetParam() << " does not declare " << which << " column '" << c
                << "' that the code reads";
        }
    };
    expectCols(kTypeCols, "cpap_equipment_types");
    expectCols(kProfileCols, "cpap_equipment_profiles");
    expectCols(kItemCols, "cpap_equipment_items");
}

TEST_P(SchemaScriptDrift, SeedsTheSixSystemTypes) {
    const std::string s = lower(readSchemaScript(GetParam()));
    ASSERT_FALSE(s.empty());
    // These must match the phone app's supply_defaults.dart, or local, cloud and
    // app compute different due dates for the same mask.
    for (const char* t : {"machine", "mask", "tubing", "filter", "humidifier", "headgear"}) {
        EXPECT_NE(s.find(std::string("'") + t + "'"), std::string::npos)
            << GetParam() << " does not seed system type '" << t << "'";
    }
}

INSTANTIATE_TEST_SUITE_P(AllThreeEngines, SchemaScriptDrift,
                         ::testing::Values("schema.sql",
                                           "schema_sqlite.sql",
                                           "schema_mysql.sql"));

// ─────────────────────────────────────────────────────────────────────────────
// 2. Backend parity — the same contract exercised against a real engine
// ─────────────────────────────────────────────────────────────────────────────

class EquipmentBackend : public ::testing::Test {
protected:
    std::unique_ptr<SQLiteDatabase> db_;
    std::string path_;

    void SetUp() override {
        path_ = (std::filesystem::temp_directory_path() /
                 ("hms_eq_" + std::to_string(::getpid()) + "_" +
                  std::to_string(reinterpret_cast<uintptr_t>(this)) + ".db")).string();
        std::filesystem::remove(path_);
        db_ = std::make_unique<SQLiteDatabase>(path_);
        ASSERT_TRUE(db_->connect());
    }
    void TearDown() override {
        db_.reset();
        std::filesystem::remove(path_);
    }

    IDatabase& db() { return *db_; }

    IDatabase::EquipmentItem mask(int profile_id, const std::string& uuid = "") {
        IDatabase::EquipmentItem it;
        it.profile_id  = profile_id;
        it.client_uuid = uuid;
        it.type_key    = "mask";
        it.brand       = "ResMed";
        it.model       = "AirFit P10";
        return it;   // category deliberately EMPTY — must be resolved from the type
    }
};

TEST_F(EquipmentBackend, SeedsSixSystemTypes) {
    auto types = db().listEquipmentTypes();
    ASSERT_GE(types.size(), 6u);

    int system_count = 0;
    for (const auto& t : types) if (t.is_system) ++system_count;
    EXPECT_EQ(system_count, 6);

    auto m = db().resolveEquipmentType("mask");
    ASSERT_TRUE(m.has_value());
    EXPECT_EQ(m->category, "accessory");
    EXPECT_EQ(m->default_replace_after_days, 90);

    auto machine = db().resolveEquipmentType("machine");
    ASSERT_TRUE(machine.has_value());
    EXPECT_EQ(machine->category, "machine");
    EXPECT_EQ(machine->default_replace_after_days, -1) << "machine is never tracked";
}

TEST_F(EquipmentBackend, EnsureDefaultProfileIsIdempotent) {
    int a = db().ensureDefaultEquipmentProfile();
    int b = db().ensureDefaultEquipmentProfile();
    EXPECT_GT(a, 0);
    EXPECT_EQ(a, b) << "must reuse the existing profile, not spawn a second";
    EXPECT_EQ(db().listEquipmentProfiles(false).size(), 1u);
}

// The drift that a green build hid: an item submitted with an empty category must
// have it resolved from its type. If it is stored as '', category='machine' never
// matches and the one-machine rule silently does not apply.
TEST_F(EquipmentBackend, EmptyCategoryIsResolvedFromTheType) {
    int pid = db().ensureDefaultEquipmentProfile();
    int id  = db().upsertEquipmentItem(mask(pid));
    ASSERT_GT(id, 0);

    auto got = db().getEquipmentItem(id);
    ASSERT_TRUE(got.has_value());
    EXPECT_EQ(got->category, "accessory") << "empty category must never be stored";

    IDatabase::EquipmentItem m;
    m.profile_id = pid;
    m.type_key   = "machine";
    m.brand      = "ResMed";
    int mid = db().upsertEquipmentItem(m);
    ASSERT_GT(mid, 0);
    EXPECT_EQ(db().getEquipmentItem(mid)->category, "machine");
}

TEST_F(EquipmentBackend, OneMachinePerProfile) {
    int pid = db().ensureDefaultEquipmentProfile();
    EXPECT_FALSE(db().profileHasMachine(pid, 0));

    IDatabase::EquipmentItem m;
    m.profile_id = pid;
    m.type_key   = "machine";
    int mid = db().upsertEquipmentItem(m);
    ASSERT_GT(mid, 0);

    EXPECT_TRUE(db().profileHasMachine(pid, 0));
    EXPECT_FALSE(db().profileHasMachine(pid, mid)) << "must ignore exclude_item_id";

    // A second machine in the same profile is rejected by the partial unique index.
    IDatabase::EquipmentItem m2;
    m2.profile_id = pid;
    m2.type_key   = "machine";
    EXPECT_LT(db().upsertEquipmentItem(m2), 0);
}

TEST_F(EquipmentBackend, SentinelsRoundTrip) {
    int pid = db().ensureDefaultEquipmentProfile();
    auto in = mask(pid);
    in.replace_after_days = -1;    // -1 == NULL, "use the type default"
    in.client_uuid        = "";    // "" == NULL
    in.started_using_at   = "";    // "" == unset
    int id = db().upsertEquipmentItem(in);
    ASSERT_GT(id, 0);

    auto got = db().getEquipmentItem(id);
    ASSERT_TRUE(got.has_value());
    EXPECT_EQ(got->replace_after_days, -1);
    EXPECT_EQ(got->client_uuid, "");
    EXPECT_EQ(got->started_using_at, "");
    EXPECT_EQ(got->started_epoch, 0) << "unset start must not fabricate an epoch";
}

// started_epoch drove supply due-dates and was timezone-dependent on one backend.
// A fixed instant must yield a fixed epoch regardless of engine or server TZ.
TEST_F(EquipmentBackend, StartedEpochIsUtcAndStable) {
    int pid = db().ensureDefaultEquipmentProfile();
    auto in = mask(pid);
    in.started_using_at = "2024-03-01T00:00:00Z";
    int id = db().upsertEquipmentItem(in);
    ASSERT_GT(id, 0);

    auto got = db().getEquipmentItem(id);
    ASSERT_TRUE(got.has_value());
    EXPECT_EQ(got->started_epoch, 1709251200LL)   // 2024-03-01T00:00:00Z
        << "epoch must be UTC-anchored, not shifted by the server timezone";
    EXPECT_EQ(got->started_using_at, "2024-03-01T00:00:00Z")
        << "must round-trip in the same shape on every backend";
}

TEST_F(EquipmentBackend, DateOnlyStartNormalisesToIsoZ) {
    int pid = db().ensureDefaultEquipmentProfile();
    auto in = mask(pid);
    in.started_using_at = "2024-03-01";           // user-supplied, date-only
    int id = db().upsertEquipmentItem(in);
    ASSERT_GT(id, 0);
    EXPECT_EQ(db().getEquipmentItem(id)->started_using_at, "2024-03-01T00:00:00Z");
}

TEST_F(EquipmentBackend, TimestampsAreIsoZ) {
    int pid = db().ensureDefaultEquipmentProfile();
    auto p = db().getEquipmentProfile(pid);
    ASSERT_TRUE(p.has_value());
    // "YYYY-MM-DDTHH:MM:SSZ" — the sync layer compares these strings.
    ASSERT_EQ(p->created_at.size(), 20u) << "got: " << p->created_at;
    EXPECT_EQ(p->created_at[10], 'T');
    EXPECT_EQ(p->created_at.back(), 'Z');
    ASSERT_EQ(p->updated_at.size(), 20u) << "got: " << p->updated_at;
    EXPECT_EQ(p->updated_at.back(), 'Z');
}

TEST_F(EquipmentBackend, TombstonesHideFromListsAndGetById) {
    int pid = db().ensureDefaultEquipmentProfile();
    int id  = db().upsertEquipmentItem(mask(pid));
    ASSERT_GT(id, 0);
    ASSERT_EQ(db().listEquipmentItems(false).size(), 1u);

    ASSERT_TRUE(db().tombstoneEquipmentItem(id));
    EXPECT_EQ(db().listEquipmentItems(false).size(), 0u);
    EXPECT_EQ(db().listEquipmentItems(true).size(), 0u) << "history excludes tombstones";
    EXPECT_FALSE(db().getEquipmentItem(id).has_value()) << "deleted must read as absent";
    EXPECT_FALSE(db().tombstoneEquipmentItem(id)) << "second tombstone is a no-op";
}

TEST_F(EquipmentBackend, RetiredItemHiddenUnlessHistoryRequested) {
    int pid = db().ensureDefaultEquipmentProfile();
    auto in = mask(pid);
    in.active = false;                      // retired, not deleted
    int id = db().upsertEquipmentItem(in);
    ASSERT_GT(id, 0);

    EXPECT_EQ(db().listEquipmentItems(false).size(), 0u);
    EXPECT_EQ(db().listEquipmentItems(true).size(), 1u);
}

TEST_F(EquipmentBackend, TombstoningProfileCascadesToItems) {
    int pid = db().ensureDefaultEquipmentProfile();
    db().upsertEquipmentItem(mask(pid));
    IDatabase::EquipmentItem m;
    m.profile_id = pid; m.type_key = "machine";
    db().upsertEquipmentItem(m);
    ASSERT_EQ(db().listEquipmentItems(false).size(), 2u);

    ASSERT_TRUE(db().tombstoneEquipmentProfile(pid));
    EXPECT_EQ(db().listEquipmentProfiles(false).size(), 0u);
    EXPECT_EQ(db().listEquipmentItems(false).size(), 0u) << "items must cascade";
}

TEST_F(EquipmentBackend, RetiringMachineFreesTheSlot) {
    int pid = db().ensureDefaultEquipmentProfile();
    IDatabase::EquipmentItem m;
    m.profile_id = pid; m.type_key = "machine"; m.brand = "ResMed";
    int mid = db().upsertEquipmentItem(m);
    ASSERT_GT(mid, 0);

    // Replace it: retire the old, then the new one must fit.
    m.id = mid; m.active = false;
    ASSERT_GE(db().upsertEquipmentItem(m), 0);

    IDatabase::EquipmentItem m2;
    m2.profile_id = pid; m2.type_key = "machine"; m2.brand = "Lowenstein";
    EXPECT_GT(db().upsertEquipmentItem(m2), 0)
        << "retiring the old machine must free the one-machine slot";
}

TEST_F(EquipmentBackend, CustomTypeAndSystemTypeProtection) {
    IDatabase::EquipmentType battery;
    battery.type_key = "battery";
    battery.label    = "Battery";
    battery.category = "accessory";
    battery.default_replace_after_days = 365;
    int id = db().addEquipmentType(battery);
    ASSERT_GT(id, 0);
    EXPECT_TRUE(db().resolveEquipmentType("battery").has_value());
    EXPECT_LT(db().addEquipmentType(battery), 0) << "duplicate type_key must fail";

    // A system row's identity must be immutable: renaming the seeded 'machine'
    // type_key would orphan every item referencing it.
    int machine_id = 0;
    for (const auto& t : db().listEquipmentTypes())
        if (t.is_system && t.type_key == "machine") machine_id = t.id;
    ASSERT_GT(machine_id, 0);

    IDatabase::EquipmentType hijack;
    hijack.type_key = "hijacked";
    hijack.label    = "Renamed";
    hijack.category = "accessory";
    hijack.active   = true;
    db().updateEquipmentType(machine_id, hijack);

    EXPECT_TRUE(db().resolveEquipmentType("machine").has_value())
        << "system type_key must not be reassignable";
    EXPECT_EQ(db().resolveEquipmentType("machine")->category, "machine")
        << "system category must not be reassignable";
}

TEST_F(EquipmentBackend, ProfileRenameAndItemUpdate) {
    int pid = db().ensureDefaultEquipmentProfile();
    auto p = db().getEquipmentProfile(pid);
    ASSERT_TRUE(p.has_value());
    p->name = "Travel";
    ASSERT_GE(db().upsertEquipmentProfile(*p), 0);
    EXPECT_EQ(db().getEquipmentProfile(pid)->name, "Travel");

    int id = db().upsertEquipmentItem(mask(pid));
    auto it = db().getEquipmentItem(id);
    ASSERT_TRUE(it.has_value());
    it->replace_after_days = 45;           // per-item override beats the type default
    it->brand = "Philips";
    ASSERT_GE(db().upsertEquipmentItem(*it), 0);

    auto after = db().getEquipmentItem(id);
    EXPECT_EQ(after->replace_after_days, 45);
    EXPECT_EQ(after->brand, "Philips");
}

TEST_F(EquipmentBackend, ItemsListsMachineFirst) {
    int pid = db().ensureDefaultEquipmentProfile();
    db().upsertEquipmentItem(mask(pid));            // accessory first
    IDatabase::EquipmentItem m;
    m.profile_id = pid; m.type_key = "machine";
    db().upsertEquipmentItem(m);

    auto items = db().listEquipmentItems(false);
    ASSERT_EQ(items.size(), 2u);
    EXPECT_EQ(items[0].category, "machine") << "ordering must agree across backends";
}

} // namespace
