//
// test_CleaningBackends.cpp — SDD-007 cleaning storage, across engines.
//
// This file exists because of what 4.6.3 found. Four oximetry methods had been
// inline header stubs returning {} on SQLite and MySQL for months, and nothing
// noticed, because only PostgreSQL was ever exercised. A per-engine suite is the
// only thing that catches "implemented on the engine I happened to run".
//
// SQLite always runs (temp file, no server). MySQL runs when MYSQL_TEST_HOST is
// set and skips cleanly otherwise, so a developer with no server still gets a
// green suite.
//
// SAFETY: never DROPs or TRUNCATEs. Every row is namespaced to a per-process
// profile created here and removed in TearDown, so pointing MYSQL_TEST_* at a
// populated database cannot disturb real data.
//
#include <gtest/gtest.h>

#include "database/IDatabase.h"
#include "database/SQLiteDatabase.h"
#include "services/CleaningStatus.h"
#ifdef WITH_MYSQL
#include "database/MySQLDatabase.h"
#endif

#include <cstdlib>
#include <filesystem>
#include <memory>
#include <string>
#include <unistd.h>

using namespace hms_cpap;

namespace {

std::string envOr(const char* key, const std::string& fallback) {
    const char* v = std::getenv(key);
    return (v && *v) ? std::string(v) : fallback;
}

enum class Engine { SQLite, MySQL };
const char* engineName(Engine e) { return e == Engine::SQLite ? "SQLite" : "MySQL"; }

class CleaningBackendTest : public ::testing::TestWithParam<Engine> {
protected:
    std::unique_ptr<IDatabase> db_;
    std::string path_;
    int profile_id_{0};

    void SetUp() override {
        if (GetParam() == Engine::SQLite) {
            path_ = (std::filesystem::temp_directory_path() /
                     ("hms_clean_" + std::to_string(::getpid()) + ".db")).string();
            std::filesystem::remove(path_);
            auto lite = std::make_unique<SQLiteDatabase>(path_);
            ASSERT_TRUE(lite->connect());
            db_ = std::move(lite);
        } else {
#ifndef WITH_MYSQL
            GTEST_SKIP() << "built without MySQL (-DBUILD_WITH_MYSQL=OFF)";
#else
            const std::string host = envOr("MYSQL_TEST_HOST", "");
            if (host.empty()) {
                GTEST_SKIP() << "MYSQL_TEST_HOST unset — skipping cleaning parity on MySQL.";
            }
            auto my = std::make_unique<MySQLDatabase>(
                host, std::stoi(envOr("MYSQL_TEST_PORT", "3306")),
                envOr("MYSQL_TEST_USER", ""), envOr("MYSQL_TEST_PASSWORD", ""),
                envOr("MYSQL_TEST_DB", "hms_cpap_test"));
            if (!my->connect()) {
                GTEST_SKIP() << "No usable MySQL at " << host;
            }
            db_ = std::move(my);
#endif
        }

        // A task must belong to a profile (SDD-004's no-unassigned rule), so make
        // one scoped to this process rather than reusing whatever is there.
        IDatabase::EquipmentProfile p;
        p.name = "cleaning_test_" + std::to_string(::getpid());
        profile_id_ = db_->upsertEquipmentProfile(p, "");
        ASSERT_GT(profile_id_, 0) << "could not create a scratch profile";
    }

    void TearDown() override {
        if (db_ && profile_id_ > 0) {
            // Tasks first: the FK is ON DELETE CASCADE, but being explicit keeps
            // this correct if that ever changes.
            db_->executeQuery("DELETE FROM cleaning_tasks WHERE profile_id = ?",
                              {std::to_string(profile_id_)});
            db_->executeQuery("DELETE FROM cpap_equipment_profiles WHERE id = ?",
                              {std::to_string(profile_id_)});
        }
        db_.reset();
        if (!path_.empty()) std::filesystem::remove(path_);
    }

    IDatabase& db() { return *db_; }

    IDatabase::CleaningTask makeTask(const std::string& key,
                                     int interval = 7,
                                     bool enabled = true) {
        IDatabase::CleaningTask t;
        t.profile_id    = profile_id_;
        t.task_key      = key;
        t.label         = "Wash the " + key;
        t.interval_days = interval;
        t.time_minutes  = kCleaningDefaultTimeMinutes;
        t.start_date    = "2026-07-30";
        t.enabled       = enabled;
        return t;
    }
};

// ─────────────────────────────────────────────────────────────────────────────
// Preset catalog
// ─────────────────────────────────────────────────────────────────────────────

TEST_P(CleaningBackendTest, SevenSystemPresetsAreSeeded) {
    auto types = db().listCleaningTaskTypes();
    ASSERT_GE(types.size(), 7u) << engineName(GetParam()) << ": presets missing";

    // Keys are the shared vocabulary with the cloud; a rename here silently
    // desyncs the two products.
    for (const char* key : {"mask_wipe", "mask_wash", "headgear_wash", "tubing_wash",
                            "humidifier_empty", "humidifier_wash", "filter_check"}) {
        bool found = false;
        for (const auto& t : types) if (t.task_key == key) { found = true; break; }
        EXPECT_TRUE(found) << "preset " << key << " is missing";
    }
}

TEST_P(CleaningBackendTest, PresetIntervalsMatchTheCloudCatalog) {
    // These are manufacturer care intervals and are part of the shared contract.
    // Drifting from the cloud means the same mask wants washing on two schedules.
    for (const auto& t : db().listCleaningTaskTypes()) {
        if (t.task_key == "mask_wipe")        EXPECT_EQ(t.default_interval_days, 1);
        if (t.task_key == "mask_wash")        EXPECT_EQ(t.default_interval_days, 7);
        if (t.task_key == "humidifier_empty") EXPECT_EQ(t.default_interval_days, 1);
        if (t.task_key == "filter_check")     EXPECT_EQ(t.default_interval_days, 30);
    }
}

TEST_P(CleaningBackendTest, PresetsCarryTheirAppliesToType) {
    // /suggest uses this to skip water-tub tasks for a setup with no humidifier.
    for (const auto& t : db().listCleaningTaskTypes()) {
        if (t.task_key == "tubing_wash") EXPECT_EQ(t.applies_to_type_key, "tubing");
        if (t.task_key == "mask_wash")   EXPECT_EQ(t.applies_to_type_key, "mask");
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// CRUD
// ─────────────────────────────────────────────────────────────────────────────

TEST_P(CleaningBackendTest, CreateThenReadBackEveryField) {
    auto t = makeTask("mask_wash");
    t.time_minutes = 21 * 60 + 30;   // a deliberate bedtime slot
    const int id = db().upsertCleaningTask(t, "");
    ASSERT_GT(id, 0);

    auto got = db().getCleaningTask(id);
    ASSERT_TRUE(got.has_value());
    EXPECT_EQ(got->profile_id, profile_id_);
    EXPECT_EQ(got->task_key, "mask_wash");
    EXPECT_EQ(got->label, "Wash the mask_wash");
    EXPECT_EQ(got->interval_days, 7);
    EXPECT_EQ(got->time_minutes, 21 * 60 + 30)
        << "time-of-day did not survive; a bedtime task would fire in the morning";
    EXPECT_EQ(got->start_date, "2026-07-30");
    EXPECT_TRUE(got->enabled);
    EXPECT_FALSE(got->deleted);
    EXPECT_EQ(got->last_done_epoch, 0) << "a new task must read as never done";
}

TEST_P(CleaningBackendTest, StartDateEpochIsDerivedOnRead) {
    // The pure status function takes epochs, so every backend must derive them
    // the same way or the same task reads differently per engine.
    const int id = db().upsertCleaningTask(makeTask("tubing_wash"), "");
    ASSERT_GT(id, 0);
    auto got = db().getCleaningTask(id);
    ASSERT_TRUE(got.has_value());
    EXPECT_GT(got->start_date_epoch, 0) << "start_date was not converted";
}

TEST_P(CleaningBackendTest, UpdateChangesTheScheduleNotTheIdentity) {
    const int id = db().upsertCleaningTask(makeTask("filter_check", 30), "");
    ASSERT_GT(id, 0);

    auto t = *db().getCleaningTask(id);
    t.interval_days = 14;
    t.enabled = false;
    EXPECT_EQ(db().upsertCleaningTask(t, ""), id) << "update must not create a row";

    auto got = db().getCleaningTask(id);
    ASSERT_TRUE(got.has_value());
    EXPECT_EQ(got->interval_days, 14);
    EXPECT_FALSE(got->enabled);
    EXPECT_EQ(got->task_key, "filter_check");
}

TEST_P(CleaningBackendTest, ListIsScopedToTheProfile) {
    ASSERT_GT(db().upsertCleaningTask(makeTask("mask_wipe"), ""), 0);
    ASSERT_GT(db().upsertCleaningTask(makeTask("tubing_wash"), ""), 0);

    auto mine = db().listCleaningTasks(profile_id_);
    EXPECT_EQ(mine.size(), 2u);

    // A different setup has its own schedule, per SDD-004's no-unassigned rule.
    auto other = db().listCleaningTasks(profile_id_ + 99999);
    EXPECT_TRUE(other.empty());
}

TEST_P(CleaningBackendTest, SoftDeleteHidesTheTaskAndDisablesIt) {
    const int id = db().upsertCleaningTask(makeTask("headgear_wash"), "");
    ASSERT_GT(id, 0);
    ASSERT_TRUE(db().tombstoneCleaningTask(id, ""));

    EXPECT_TRUE(db().listCleaningTasks(profile_id_).empty())
        << "a tombstoned task must not surface in a list";

    // Still fetchable by id, and disabled, so a stale MQTT entity cannot keep
    // publishing for a task the user deleted.
    auto got = db().getCleaningTask(id);
    ASSERT_TRUE(got.has_value());
    EXPECT_TRUE(got->deleted);
    EXPECT_FALSE(got->enabled);
}

TEST_P(CleaningBackendTest, DeletingTwiceReportsNoSecondChange) {
    const int id = db().upsertCleaningTask(makeTask("mask_wash"), "");
    ASSERT_GT(id, 0);
    EXPECT_TRUE(db().tombstoneCleaningTask(id, ""));
    EXPECT_FALSE(db().tombstoneCleaningTask(id, ""))
        << "a second delete changed a row, so the caller cannot trust the result";
}

// ─────────────────────────────────────────────────────────────────────────────
// Idempotency: what makes /suggest safe to call twice
// ─────────────────────────────────────────────────────────────────────────────

TEST_P(CleaningBackendTest, DuplicateTaskKeyForOneProfileIsRejected) {
    ASSERT_GT(db().upsertCleaningTask(makeTask("mask_wipe"), ""), 0);
    EXPECT_LE(db().upsertCleaningTask(makeTask("mask_wipe"), ""), 0)
        << "a second task with the same key was created; /suggest would duplicate";
    EXPECT_EQ(db().listCleaningTasks(profile_id_).size(), 1u);
}

TEST_P(CleaningBackendTest, TheSameKeyCanReturnAfterDeletion) {
    // The unique index is partial (WHERE NOT deleted) so a user who removes a
    // task can add it back rather than being permanently blocked by a tombstone.
    const int first = db().upsertCleaningTask(makeTask("mask_wipe"), "");
    ASSERT_GT(first, 0);
    ASSERT_TRUE(db().tombstoneCleaningTask(first, ""));

    const int second = db().upsertCleaningTask(makeTask("mask_wipe"), "");
    EXPECT_GT(second, 0) << "a deleted task blocked its key from being reused";
    EXPECT_NE(second, first);
}

// ─────────────────────────────────────────────────────────────────────────────
// Mark done, which is what advances the clock
// ─────────────────────────────────────────────────────────────────────────────

TEST_P(CleaningBackendTest, MarkDoneStampsLastDone) {
    const int id = db().upsertCleaningTask(makeTask("mask_wash"), "");
    ASSERT_GT(id, 0);
    ASSERT_EQ(db().getCleaningTask(id)->last_done_epoch, 0);

    ASSERT_TRUE(db().markCleaningTaskDone(id, ""));

    auto got = db().getCleaningTask(id);
    ASSERT_TRUE(got.has_value());
    EXPECT_FALSE(got->last_done_at.empty());
    EXPECT_GT(got->last_done_epoch, 0)
        << "last_done_at did not convert to an epoch, so the status function "
           "would still treat this as never done";
}

TEST_P(CleaningBackendTest, MarkDoneMovesTheTaskOutOfDue) {
    // The end-to-end point of the feature: storage plus the pure function agree.
    const int id = db().upsertCleaningTask(makeTask("mask_wash", 7), "");
    ASSERT_GT(id, 0);

    auto before = *db().getCleaningTask(id);
    // Start date is in the past relative to a "now" well after it, so it is due.
    const long long now = before.start_date_epoch + 30LL * 24 * 3600;
    auto s1 = computeCleaningStatus(before.start_date_epoch, before.interval_days,
                                    before.time_minutes, before.last_done_epoch,
                                    before.enabled, now);
    EXPECT_EQ(s1.state, CleaningState::Due);

    ASSERT_TRUE(db().markCleaningTaskDone(id, ""));
    auto after = *db().getCleaningTask(id);
    auto s2 = computeCleaningStatus(after.start_date_epoch, after.interval_days,
                                    after.time_minutes, after.last_done_epoch,
                                    after.enabled, after.last_done_epoch + 60);
    EXPECT_EQ(s2.state, CleaningState::Upcoming)
        << "marking done did not clear the due state";
}

TEST_P(CleaningBackendTest, MarkDoneOnAMissingTaskReportsFailure) {
    EXPECT_FALSE(db().markCleaningTaskDone(999999, ""));
}

INSTANTIATE_TEST_SUITE_P(
    Engines, CleaningBackendTest,
    ::testing::Values(Engine::SQLite, Engine::MySQL),
    [](const ::testing::TestParamInfo<Engine>& info) {
        return std::string(engineName(info.param));
    });

}  // namespace
