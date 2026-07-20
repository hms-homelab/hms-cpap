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
#ifdef WITH_POSTGRESQL
#include "database/PostgresDatabase.h"
#include <pqxx/pqxx>
#include <unistd.h>
#endif

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

// The engines this contract is pinned against. SQLite always runs; Postgres runs
// when a server is reachable and skips cleanly otherwise, so a developer with no
// Postgres still gets a green suite. MySQL is deliberately absent: it cannot
// express the partial unique index that enforces the one-machine-per-profile
// rule, so it cannot satisfy this contract as written (see OneMachinePerProfile).
enum class Engine { SQLite, Postgres };

const char* engineName(Engine e) {
    return e == Engine::SQLite ? "SQLite" : "Postgres";
}

#ifdef WITH_POSTGRESQL

// Same env-var contract as tests/database/test_DatabaseService_pg.cpp:
// PGHOST/PGPORT/PGUSER/PGPASSWORD/PGDATABASE with the documented maestro
// defaults as a fallback.
std::string pgEnvOr(const char* key, const std::string& def) {
    const char* v = std::getenv(key);
    return (v && *v) ? std::string(v) : def;
}

// PGDATABASE defaults to cpap_monitoring, which is a REAL production database.
// Isolation therefore comes from a uniquely-named throwaway SCHEMA created
// inside it and dropped in TearDown — never from the database itself. A schema
// (rather than a database) is used so no CREATEDB privilege is required.
std::string pgDbName() { return pgEnvOr("PGDATABASE", "cpap_monitoring"); }

std::string pgConnInfo(const std::string& search_path = "") {
    std::string ci = "host=" + pgEnvOr("PGHOST", "localhost") +
                     " port=" + pgEnvOr("PGPORT", "5432") +
                     " user=" + pgEnvOr("PGUSER", "maestro") +
                     " password=" + pgEnvOr("PGPASSWORD", "REDACTED") +
                     " dbname=" + pgDbName() +
                     " connect_timeout=3";
    if (!search_path.empty()) {
        // NOTE: no ",public" fallback, unlike test_DatabaseService_pg.cpp. The
        // production cpap_equipment_* tables live in public of this very
        // database; a search_path that could fall through to them would let a
        // failed migration silently point these WRITE tests at real user data.
        // Pinned to the throwaway schema alone, any such slip errors instead.
        // Honoured by both the pqxx connection inside DatabaseService and the
        // separate libpq query connection PostgresDatabase uses for
        // executeQuery(), so every unqualified name resolves in the schema.
        ci += " options=-csearch_path=" + search_path;
    }
    return ci;
}

#endif // WITH_POSTGRESQL

class EquipmentBackend : public ::testing::TestWithParam<Engine> {
protected:
    std::unique_ptr<IDatabase> db_;
    std::string path_;      // SQLite only
    std::string schema_;    // Postgres only

    bool isPostgres() const { return GetParam() == Engine::Postgres; }

    void SetUp() override {
        if (GetParam() == Engine::SQLite) {
            path_ = (std::filesystem::temp_directory_path() /
                     ("hms_eq_" + std::to_string(::getpid()) + "_" +
                      std::to_string(reinterpret_cast<uintptr_t>(this)) + ".db")).string();
            std::filesystem::remove(path_);
            auto sqlite = std::make_unique<SQLiteDatabase>(path_);
            ASSERT_TRUE(sqlite->connect());
            db_ = std::move(sqlite);
            return;
        }

#ifndef WITH_POSTGRESQL
        GTEST_SKIP() << "built without PostgreSQL (-DBUILD_WITH_POSTGRESQL=OFF)";
#else
        // Reachability probe first, with connect_timeout=3, so a developer with
        // no server skips in milliseconds rather than hanging.
        try {
            pqxx::connection probe(pgConnInfo());
            if (!probe.is_open()) throw std::runtime_error("connection not open");
        } catch (const std::exception& e) {
            GTEST_SKIP() << "No usable PostgreSQL ("
                         << pgEnvOr("PGUSER", "maestro") << "@"
                         << pgEnvOr("PGHOST", "localhost") << "/" << pgDbName()
                         << ") — skipping equipment parity on Postgres (" << e.what() << ").";
        }

        static int counter = 0;
        schema_ = "cpap_eq_" + std::to_string(::getpid()) + "_" +
                  std::to_string(counter++);

        // Create the throwaway schema BEFORE connecting, so DatabaseService's
        // connect()-time SDD-004 auto-migration has somewhere to create into.
        try {
            pqxx::connection admin(pgConnInfo());
            pqxx::work txn(admin);
            txn.exec("CREATE SCHEMA IF NOT EXISTS " + schema_);
            txn.commit();
        } catch (const std::exception& e) {
            schema_.clear();
            GTEST_SKIP() << "Cannot create a throwaway schema in " << pgDbName()
                         << " — skipping (" << e.what() << ").";
        }

        // PostgresDatabase is the IDatabase wrapper that delegates every
        // equipment call to DatabaseService (the implementation under test) and
        // additionally provides executeQuery(), which DatabaseService itself
        // does not. connect() runs the SDD-004 migration, creating the equipment
        // tables, indexes and seed rows inside schema_.
        auto pg = std::make_unique<PostgresDatabase>(pgConnInfo(schema_));
        ASSERT_TRUE(pg->connect());
        db_ = std::move(pg);

        // Isolation assertion: if the migration did not land in schema_, abort
        // rather than run destructive tests against whatever else resolved.
        Json::Value where = db_->executeQuery(
            "SELECT to_regclass('" + schema_ + ".cpap_equipment_items') AS t");
        ASSERT_TRUE(where.isArray() && where.size() == 1u &&
                    !where[0]["t"].isNull())
            << "equipment tables were not created inside the throwaway schema "
            << schema_ << " — refusing to touch production tables";
#endif
    }

    void TearDown() override {
        db_.reset();   // close every connection before dropping the schema
        if (!path_.empty()) std::filesystem::remove(path_);
#ifdef WITH_POSTGRESQL
        if (!schema_.empty()) {
            try {
                pqxx::connection admin(pgConnInfo());
                pqxx::work txn(admin);
                txn.exec("DROP SCHEMA IF EXISTS " + schema_ + " CASCADE");
                txn.commit();
            } catch (...) {
                // Best effort: a leaked empty test schema is harmless.
            }
        }
#endif
    }

    IDatabase& db() { return *db_; }

    // Positional placeholder for the engine under test: SQLite/MySQL use '?',
    // PostgreSQL uses $1..$n (see IDatabase::executeQuery).
    std::string ph(int n) const {
        return isPostgres() ? ("$" + std::to_string(n)) : std::string("?");
    }

    // Read a tombstoned item's updated_at the way CpapDashSyncService does: a
    // direct query, because a tombstone is invisible to both listEquipmentItems
    // and getEquipmentItem. Postgres stores updated_at as TIMESTAMPTZ and would
    // otherwise render it in the server's timezone, so it is normalised to the
    // same ISO-8601 UTC shape SQLite stores natively — the equipment readers
    // (profileCols/itemCols) apply exactly this AT TIME ZONE 'UTC' conversion.
    std::string rawItemUpdatedAt(int item_id) {
        const std::string sql =
            isPostgres()
                ? "SELECT to_char(updated_at AT TIME ZONE 'UTC', "
                  "'YYYY-MM-DD\"T\"HH24:MI:SS\"Z\"') AS updated_at "
                  "FROM cpap_equipment_items WHERE id = " + ph(1)
                : "SELECT updated_at FROM cpap_equipment_items WHERE id = " + ph(1);
        Json::Value rows = db().executeQuery(sql, {std::to_string(item_id)});
        EXPECT_TRUE(rows.isArray() && rows.size() == 1u)
            << "tombstoned row disappeared entirely";
        if (!rows.isArray() || rows.size() != 1u) return {};
        return rows[0]["updated_at"].asString();
    }

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

TEST_P(EquipmentBackend, SeedsSixSystemTypes) {
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

TEST_P(EquipmentBackend, EnsureDefaultProfileIsIdempotent) {
    int a = db().ensureDefaultEquipmentProfile();
    int b = db().ensureDefaultEquipmentProfile();
    EXPECT_GT(a, 0);
    EXPECT_EQ(a, b) << "must reuse the existing profile, not spawn a second";
    EXPECT_EQ(db().listEquipmentProfiles(false).size(), 1u);
}

// The drift that a green build hid: an item submitted with an empty category must
// have it resolved from its type. If it is stored as '', category='machine' never
// matches and the one-machine rule silently does not apply.
TEST_P(EquipmentBackend, EmptyCategoryIsResolvedFromTheType) {
    int pid = db().ensureDefaultEquipmentProfile();
    int id  = db().upsertEquipmentItem(mask(pid), "");
    ASSERT_GT(id, 0);

    auto got = db().getEquipmentItem(id);
    ASSERT_TRUE(got.has_value());
    EXPECT_EQ(got->category, "accessory") << "empty category must never be stored";

    IDatabase::EquipmentItem m;
    m.profile_id = pid;
    m.type_key   = "machine";
    m.brand      = "ResMed";
    int mid = db().upsertEquipmentItem(m, "");
    ASSERT_GT(mid, 0);
    EXPECT_EQ(db().getEquipmentItem(mid)->category, "machine");
}

TEST_P(EquipmentBackend, OneMachinePerProfile) {
    int pid = db().ensureDefaultEquipmentProfile();
    EXPECT_FALSE(db().profileHasMachine(pid, 0));

    IDatabase::EquipmentItem m;
    m.profile_id = pid;
    m.type_key   = "machine";
    int mid = db().upsertEquipmentItem(m, "");
    ASSERT_GT(mid, 0);

    EXPECT_TRUE(db().profileHasMachine(pid, 0));
    EXPECT_FALSE(db().profileHasMachine(pid, mid)) << "must ignore exclude_item_id";

    // A second machine in the same profile is rejected by the partial unique index.
    IDatabase::EquipmentItem m2;
    m2.profile_id = pid;
    m2.type_key   = "machine";
    EXPECT_LT(db().upsertEquipmentItem(m2, ""), 0);
}

TEST_P(EquipmentBackend, SentinelsRoundTrip) {
    int pid = db().ensureDefaultEquipmentProfile();
    auto in = mask(pid);
    in.replace_after_days = -1;    // -1 == NULL, "use the type default"
    in.client_uuid        = "";    // "" == NULL
    in.started_using_at   = "";    // "" == unset
    int id = db().upsertEquipmentItem(in, "");
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
TEST_P(EquipmentBackend, StartedEpochIsUtcAndStable) {
    int pid = db().ensureDefaultEquipmentProfile();
    auto in = mask(pid);
    in.started_using_at = "2024-03-01T00:00:00Z";
    int id = db().upsertEquipmentItem(in, "");
    ASSERT_GT(id, 0);

    auto got = db().getEquipmentItem(id);
    ASSERT_TRUE(got.has_value());
    EXPECT_EQ(got->started_epoch, 1709251200LL)   // 2024-03-01T00:00:00Z
        << "epoch must be UTC-anchored, not shifted by the server timezone";
    EXPECT_EQ(got->started_using_at, "2024-03-01T00:00:00Z")
        << "must round-trip in the same shape on every backend";
}

TEST_P(EquipmentBackend, DateOnlyStartNormalisesToIsoZ) {
    int pid = db().ensureDefaultEquipmentProfile();
    auto in = mask(pid);
    in.started_using_at = "2024-03-01";           // user-supplied, date-only
    int id = db().upsertEquipmentItem(in, "");
    ASSERT_GT(id, 0);
    EXPECT_EQ(db().getEquipmentItem(id)->started_using_at, "2024-03-01T00:00:00Z");
}

TEST_P(EquipmentBackend, TimestampsAreIsoZ) {
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

TEST_P(EquipmentBackend, TombstonesHideFromListsAndGetById) {
    int pid = db().ensureDefaultEquipmentProfile();
    int id  = db().upsertEquipmentItem(mask(pid), "");
    ASSERT_GT(id, 0);
    ASSERT_EQ(db().listEquipmentItems(false).size(), 1u);

    ASSERT_TRUE(db().tombstoneEquipmentItem(id, ""));
    EXPECT_EQ(db().listEquipmentItems(false).size(), 0u);
    EXPECT_EQ(db().listEquipmentItems(true).size(), 0u) << "history excludes tombstones";
    EXPECT_FALSE(db().getEquipmentItem(id).has_value()) << "deleted must read as absent";
    EXPECT_FALSE(db().tombstoneEquipmentItem(id, "")) << "second tombstone is a no-op";
}

TEST_P(EquipmentBackend, RetiredItemHiddenUnlessHistoryRequested) {
    int pid = db().ensureDefaultEquipmentProfile();
    auto in = mask(pid);
    in.active = false;                      // retired, not deleted
    int id = db().upsertEquipmentItem(in, "");
    ASSERT_GT(id, 0);

    EXPECT_EQ(db().listEquipmentItems(false).size(), 0u);
    EXPECT_EQ(db().listEquipmentItems(true).size(), 1u);
}

TEST_P(EquipmentBackend, TombstoningProfileCascadesToItems) {
    int pid = db().ensureDefaultEquipmentProfile();
    db().upsertEquipmentItem(mask(pid), "");
    IDatabase::EquipmentItem m;
    m.profile_id = pid; m.type_key = "machine";
    db().upsertEquipmentItem(m, "");
    ASSERT_EQ(db().listEquipmentItems(false).size(), 2u);

    ASSERT_TRUE(db().tombstoneEquipmentProfile(pid, ""));
    EXPECT_EQ(db().listEquipmentProfiles(false).size(), 0u);
    EXPECT_EQ(db().listEquipmentItems(false).size(), 0u) << "items must cascade";
}

TEST_P(EquipmentBackend, RetiringMachineFreesTheSlot) {
    int pid = db().ensureDefaultEquipmentProfile();
    IDatabase::EquipmentItem m;
    m.profile_id = pid; m.type_key = "machine"; m.brand = "ResMed";
    int mid = db().upsertEquipmentItem(m, "");
    ASSERT_GT(mid, 0);

    // Replace it: retire the old, then the new one must fit.
    m.id = mid; m.active = false;
    ASSERT_GE(db().upsertEquipmentItem(m, ""), 0);

    IDatabase::EquipmentItem m2;
    m2.profile_id = pid; m2.type_key = "machine"; m2.brand = "Lowenstein";
    EXPECT_GT(db().upsertEquipmentItem(m2, ""), 0)
        << "retiring the old machine must free the one-machine slot";
}

TEST_P(EquipmentBackend, CustomTypeAndSystemTypeProtection) {
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

TEST_P(EquipmentBackend, ProfileRenameAndItemUpdate) {
    int pid = db().ensureDefaultEquipmentProfile();
    auto p = db().getEquipmentProfile(pid);
    ASSERT_TRUE(p.has_value());
    p->name = "Travel";
    ASSERT_GE(db().upsertEquipmentProfile(*p, ""), 0);
    EXPECT_EQ(db().getEquipmentProfile(pid)->name, "Travel");

    int id = db().upsertEquipmentItem(mask(pid), "");
    auto it = db().getEquipmentItem(id);
    ASSERT_TRUE(it.has_value());
    it->replace_after_days = 45;           // per-item override beats the type default
    it->brand = "Philips";
    ASSERT_GE(db().upsertEquipmentItem(*it, ""), 0);

    auto after = db().getEquipmentItem(id);
    EXPECT_EQ(after->replace_after_days, 45);
    EXPECT_EQ(after->brand, "Philips");
}

TEST_P(EquipmentBackend, ItemsListsMachineFirst) {
    int pid = db().ensureDefaultEquipmentProfile();
    db().upsertEquipmentItem(mask(pid), "");            // accessory first
    IDatabase::EquipmentItem m;
    m.profile_id = pid; m.type_key = "machine";
    db().upsertEquipmentItem(m, "");

    auto items = db().listEquipmentItems(false);
    ASSERT_EQ(items.size(), 2u);
    EXPECT_EQ(items[0].category, "machine") << "ordering must agree across backends";
}

} // namespace

// -- updated_at override parity -----------------------------------------------
//
// Sync mirrors cloud rows locally and must keep the ORIGIN row's timestamp:
// restamping a mirror to now() makes the copy outrank the original under
// last-write-wins, silently discarding genuine edits, and leaves the row looking
// locally-modified so it is pushed straight back forever. Each engine expresses
// "this value, or now() if empty" differently -- COALESCE/NULLIF on SQLite and
// Postgres, and on MySQL an explicit assignment that has to beat the column's
// ON UPDATE NOW() clause -- so the contract is pinned per engine here.

TEST_P(EquipmentBackend, UpsertProfileHonoursAnExplicitUpdatedAt) {
    IDatabase::EquipmentProfile p;
    p.name = "Home";
    int id = db().upsertEquipmentProfile(p, "2020-01-02T03:04:05Z");
    ASSERT_GT(id, 0);

    auto stored = db().getEquipmentProfile(id);
    ASSERT_TRUE(stored.has_value());
    EXPECT_NE(stored->updated_at.find("2020-01-02"), std::string::npos)
        << "insert ignored the override; got: " << stored->updated_at;

    stored->name = "Renamed";
    ASSERT_GT(db().upsertEquipmentProfile(*stored, "2021-06-07T08:09:10Z"), 0);
    auto again = db().getEquipmentProfile(id);
    ASSERT_TRUE(again.has_value());
    EXPECT_NE(again->updated_at.find("2021-06-07"), std::string::npos)
        << "update ignored the override; got: " << again->updated_at;
}

TEST_P(EquipmentBackend, AnEmptyOverrideStampsNowNotAnEmptyString) {
    IDatabase::EquipmentProfile p;
    p.name = "Home";
    int id = db().upsertEquipmentProfile(p, "");
    ASSERT_GT(id, 0);

    auto stored = db().getEquipmentProfile(id);
    ASSERT_TRUE(stored.has_value());
    EXPECT_FALSE(stored->updated_at.empty())
        << "an empty override must fall back to now(), not write an empty stamp";
    EXPECT_NE(stored->updated_at.find("20"), std::string::npos)
        << "expected a real timestamp, got: " << stored->updated_at;
}

TEST_P(EquipmentBackend, UpsertItemHonoursAnExplicitUpdatedAt) {
    int pid = db().ensureDefaultEquipmentProfile();
    ASSERT_GT(pid, 0);

    IDatabase::EquipmentItem it;
    it.profile_id = pid;
    it.type_key   = "mask";
    it.brand      = "ResMed";
    int id = db().upsertEquipmentItem(it, "2020-01-02T03:04:05Z");
    ASSERT_GT(id, 0);

    auto stored = db().getEquipmentItem(id);
    ASSERT_TRUE(stored.has_value());
    EXPECT_NE(stored->updated_at.find("2020-01-02"), std::string::npos)
        << "item insert ignored the override; got: " << stored->updated_at;

    stored->brand = "Fisher & Paykel";
    ASSERT_GT(db().upsertEquipmentItem(*stored, "2021-06-07T08:09:10Z"), 0);
    auto again = db().getEquipmentItem(id);
    ASSERT_TRUE(again.has_value());
    EXPECT_NE(again->updated_at.find("2021-06-07"), std::string::npos)
        << "item update ignored the override; got: " << again->updated_at;
}

TEST_P(EquipmentBackend, TombstonesHonourAnExplicitUpdatedAt) {
    int pid = db().ensureDefaultEquipmentProfile();
    IDatabase::EquipmentItem it;
    it.profile_id = pid;
    it.type_key   = "mask";
    int iid = db().upsertEquipmentItem(it, "");
    ASSERT_GT(iid, 0);

    ASSERT_TRUE(db().tombstoneEquipmentItem(iid, "2020-01-02T03:04:05Z"));
    // A tombstoned item is invisible to BOTH listEquipmentItems(include_history)
    // and getEquipmentItem() -- TombstonesHideFromListsAndGetById pins that -- so
    // the stamp is read the same way CpapDashSyncService reads tombstones: a
    // direct query (rawItemUpdatedAt papers over nothing but the placeholder
    // syntax and Postgres' timezone rendering).
    const std::string item_stamp = rawItemUpdatedAt(iid);
    EXPECT_NE(item_stamp.find("2020-01-02"), std::string::npos)
        << "item tombstone ignored the override; got: " << item_stamp;

    ASSERT_TRUE(db().tombstoneEquipmentProfile(pid, "2020-03-04T05:06:07Z"));
    for (const auto& row : db().listEquipmentProfiles(/*include_deleted=*/true)) {
        if (row.id != pid) continue;
        EXPECT_TRUE(row.deleted);
        EXPECT_NE(row.updated_at.find("2020-03-04"), std::string::npos)
            << "profile tombstone ignored the override; got: " << row.updated_at;
    }
}

TEST_P(EquipmentBackend, AMalformedOverrideFallsBackToNowRatherThanCorruptingTheRow) {
    IDatabase::EquipmentProfile p;
    p.name = "Home";
    int id = db().upsertEquipmentProfile(p, "not-a-timestamp");
    ASSERT_GT(id, 0) << "a bad override must not fail the write";

    auto stored = db().getEquipmentProfile(id);
    ASSERT_TRUE(stored.has_value());
    EXPECT_EQ(stored->updated_at.find("not-a-timestamp"), std::string::npos)
        << "garbage was written straight into updated_at: " << stored->updated_at;
    EXPECT_FALSE(stored->updated_at.empty());
}

// Every TEST_P above runs once per engine. The Postgres instantiation is
// unconditional on purpose: when no server is reachable (or the build has no
// Postgres at all) SetUp() calls GTEST_SKIP, which reports as a skip rather than
// a failure — so the cases stay VISIBLE in the run instead of silently vanishing
// from a build that merely lacked a database.
INSTANTIATE_TEST_SUITE_P(
    Engines, EquipmentBackend,
    ::testing::Values(Engine::SQLite, Engine::Postgres),
    [](const ::testing::TestParamInfo<Engine>& info) {
        return std::string(engineName(info.param));
    });
