//
// test_SyncFolderBackends.cpp: SDD-008 ledger storage, across engines.
//
// Same reason test_CleaningBackends.cpp exists: 4.6.3 found four oximetry
// methods that had been inline header stubs on SQLite and MySQL for months,
// unnoticed because only PostgreSQL was ever exercised. A per-engine suite is
// the only thing that catches "implemented on the engine I happened to run".
//
// SQLite always runs (temp file, no server). MySQL runs when MYSQL_TEST_HOST is
// set and skips cleanly otherwise.
//
// SAFETY: never DROPs or TRUNCATEs. Every row is namespaced to a per-process
// date_folder prefix and removed in TearDown, so pointing MYSQL_TEST_* at a
// populated database cannot disturb real data.
//
#include <gtest/gtest.h>

#include "database/IDatabase.h"
#include "database/SQLiteDatabase.h"
#include "services/SyncFolderState.h"
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

class SyncFolderBackendTest : public ::testing::TestWithParam<Engine> {
protected:
    std::unique_ptr<IDatabase> db_;
    std::string path_;
    /// date_folder is VARCHAR(8) and holds YYYYMMDD, so the scratch rows have to
    /// look like real dates. Year 2999 keeps them clearly synthetic and sorted
    /// past anything real.
    std::string base_;

    void SetUp() override {
        if (GetParam() == Engine::SQLite) {
            path_ = (std::filesystem::temp_directory_path() /
                     ("hms_syncfold_" + std::to_string(::getpid()) + ".db")).string();
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
                GTEST_SKIP() << "MYSQL_TEST_HOST unset, skipping ledger parity on MySQL.";
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
        // 2999 plus the low four digits of the pid: unique per process so two
        // concurrent runs against the same MySQL cannot collide.
        base_ = "2999" + std::to_string(1000 + (::getpid() % 9000));
    }

    void TearDown() override {
        if (db_ && !base_.empty()) {
            db_->executeQuery("DELETE FROM cpap_sync_folders WHERE date_folder LIKE ?",
                              {base_.substr(0, 4) + "%"});
        }
        db_.reset();
        if (!path_.empty()) std::filesystem::remove(path_);
    }

    IDatabase& db() { return *db_; }

    /// A fully populated ledger, so a dropped column shows up as a mismatch
    /// rather than as a default that happens to agree.
    FolderLedger makeLedger() {
        FolderLedger f;
        f.date_folder     = base_;
        f.files_listed    = true;
        f.complete        = true;
        f.stable          = true;
        f.last_total_size = 987654321LL;   // > 2^31, so a 32-bit column fails here
        f.last_file_count = 42;
        f.str_due         = true;
        f.sidecars_due    = true;
        f.resync_size     = 987654321LL;
        f.resync_count    = 2;
        return f;
    }

    static void expectSame(const FolderLedger& a, const FolderLedger& b,
                           const char* engine) {
        EXPECT_EQ(a.date_folder,     b.date_folder)     << engine;
        EXPECT_EQ(a.files_listed,    b.files_listed)    << engine;
        EXPECT_EQ(a.complete,        b.complete)        << engine;
        EXPECT_EQ(a.stable,          b.stable)          << engine;
        EXPECT_EQ(a.last_total_size, b.last_total_size) << engine;
        EXPECT_EQ(a.last_file_count, b.last_file_count) << engine;
        EXPECT_EQ(a.str_due,         b.str_due)         << engine;
        EXPECT_EQ(a.sidecars_due,    b.sidecars_due)    << engine;
        EXPECT_EQ(a.resync_size,     b.resync_size)     << engine;
        EXPECT_EQ(a.resync_count,    b.resync_count)    << engine;
    }
};

TEST_P(SyncFolderBackendTest, EveryFieldSurvivesARoundTrip) {
    const auto in = makeLedger();
    ASSERT_TRUE(db().upsertSyncFolder(in)) << engineName(GetParam());

    auto out = db().getSyncFolder(in.date_folder);
    ASSERT_TRUE(out.has_value()) << engineName(GetParam()) << ": row did not come back";
    expectSame(in, *out, engineName(GetParam()));
}

TEST_P(SyncFolderBackendTest, TheNeverObservedSentinelIsNotFlattenedToZero) {
    // -1 means "never observed" and 0 means "an empty folder", and the two lead
    // to opposite decisions: a column defaulting the sentinel away would make a
    // fresh folder look like it had already been seen as empty.
    FolderLedger f;
    f.date_folder     = base_;
    f.last_total_size = -1;
    f.last_file_count = -1;
    f.resync_size     = -1;
    ASSERT_TRUE(db().upsertSyncFolder(f));

    auto out = db().getSyncFolder(base_);
    ASSERT_TRUE(out.has_value());
    EXPECT_EQ(out->last_total_size, -1) << engineName(GetParam());
    EXPECT_EQ(out->last_file_count, -1) << engineName(GetParam());
    EXPECT_EQ(out->resync_size,     -1) << engineName(GetParam());
}

TEST_P(SyncFolderBackendTest, ReObservingAFolderUpdatesInPlace) {
    // date_folder is the natural key. If the upsert inserted instead, a night
    // re-scanned every 65 seconds would accumulate a row per burst.
    auto f = makeLedger();
    ASSERT_TRUE(db().upsertSyncFolder(f));

    f.last_total_size = 1000;
    f.last_file_count = 5;
    f.str_due         = false;
    ASSERT_TRUE(db().upsertSyncFolder(f));

    auto all = db().listSyncFolders();
    int matching = 0;
    for (const auto& row : all) if (row.date_folder == base_) ++matching;
    EXPECT_EQ(matching, 1) << engineName(GetParam()) << ": the upsert duplicated the night";

    auto out = db().getSyncFolder(base_);
    ASSERT_TRUE(out.has_value());
    EXPECT_EQ(out->last_total_size, 1000);
    EXPECT_FALSE(out->str_due) << "clearing debt did not persist";
}

TEST_P(SyncFolderBackendTest, ClearedDebtStaysCleared) {
    // The one that matters operationally: if a cleared debt came back on the
    // next read, the refetch would run forever.
    auto f = makeLedger();
    ASSERT_TRUE(db().upsertSyncFolder(f));

    auto loaded = db().getSyncFolder(base_);
    ASSERT_TRUE(loaded.has_value());
    ASSERT_TRUE(db().upsertSyncFolder(clearStrDebt(clearSidecarDebt(*loaded))));

    auto out = db().getSyncFolder(base_);
    ASSERT_TRUE(out.has_value());
    EXPECT_FALSE(out->str_due)      << engineName(GetParam());
    EXPECT_FALSE(out->sidecars_due) << engineName(GetParam());
}

TEST_P(SyncFolderBackendTest, AnUnknownFolderIsAbsentRatherThanDefaulted) {
    // Returning a default-constructed ledger here would read as "observed, empty,
    // not complete" and quietly restart a night's history.
    EXPECT_FALSE(db().getSyncFolder(base_.substr(0, 4) + "0101").has_value())
        << engineName(GetParam());
}

TEST_P(SyncFolderBackendTest, AnEmptyDateFolderIsRefused) {
    // The key is the identity. Writing a blank one gives every unidentified
    // folder the same row.
    FolderLedger f;
    f.date_folder = "";
    EXPECT_FALSE(db().upsertSyncFolder(f)) << engineName(GetParam());
}

TEST_P(SyncFolderBackendTest, ListingReturnsEveryStoredNight) {
    auto a = makeLedger();
    auto b = makeLedger();
    b.date_folder = base_.substr(0, 7) + "9";   // same prefix, different day
    ASSERT_TRUE(db().upsertSyncFolder(a));
    ASSERT_TRUE(db().upsertSyncFolder(b));

    auto all = db().listSyncFolders();
    bool saw_a = false, saw_b = false;
    for (const auto& row : all) {
        if (row.date_folder == a.date_folder) saw_a = true;
        if (row.date_folder == b.date_folder) saw_b = true;
    }
    EXPECT_TRUE(saw_a) << engineName(GetParam());
    EXPECT_TRUE(saw_b) << engineName(GetParam());
}

TEST_P(SyncFolderBackendTest, AStoredLedgerDrivesTheStateMachineUnchanged) {
    // The end-to-end point of the table: what comes back out has to be a valid
    // input to advanceFolder, with the same verdict it had before the round trip.
    auto f = makeLedger();
    f.str_due = false;
    ASSERT_TRUE(db().upsertSyncFolder(f));

    auto out = db().getSyncFolder(base_);
    ASSERT_TRUE(out.has_value());
    EXPECT_EQ(nightState(f), nightState(*out)) << engineName(GetParam());

    FolderObservation obs;
    obs.file_count       = f.last_file_count;
    obs.total_size       = f.last_total_size;
    obs.all_files_stored = true;
    // Persisted as already closed, so re-observing the same signature must not
    // re-arm: a restart cannot be allowed to re-trigger a night's refetch.
    EXPECT_FALSE(advanceFolder(*out, obs).armed_debt)
        << engineName(GetParam()) << ": a reload re-armed debt that was already spent";
}

INSTANTIATE_TEST_SUITE_P(
    Engines, SyncFolderBackendTest,
    ::testing::Values(Engine::SQLite, Engine::MySQL),
    [](const ::testing::TestParamInfo<Engine>& info) {
        return std::string(engineName(info.param));
    });

}  // namespace
