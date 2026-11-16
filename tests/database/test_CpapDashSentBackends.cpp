//
// test_CpapDashSentBackends.cpp: platform SDD-059 D5, the CpapDash resume
// index (cpapdash_sent), on every engine.
//
// The uploader resumes from this table on every connection, so a method that
// silently does nothing on one engine sends every night again on every
// reconnect there, or never moves past the first file. Same reason as
// test_SyncFolderBackends.cpp: only a per-engine suite catches "implemented
// on the engine I happened to run".
//
// SQLite always runs (temp file, no server). MySQL runs when MYSQL_TEST_HOST is
// set, PostgreSQL when a server answers (PGHOST etc.); both skip otherwise.
//
// SAFETY: never DROPs or TRUNCATEs. Every row lives under a per-process root
// and is deleted in TearDown; clearing every row is tested on SQLite's own
// temp file only.
//
#include <gtest/gtest.h>

#include "database/IDatabase.h"
#include "database/SQLiteDatabase.h"
#ifdef WITH_MYSQL
#include "database/MySQLDatabase.h"
#endif
#ifdef WITH_POSTGRESQL
#include "database/DatabaseService.h"
#include "database/PostgresDatabase.h"
#include <pqxx/pqxx>
#endif

#include <cstdlib>
#include <filesystem>
#include <memory>
#include <string>
#ifndef _WIN32
#include <unistd.h>
#else
#include <process.h>
#endif

using namespace hms_cpap;

namespace {

std::string envOr(const char* key, const std::string& fallback) {
    const char* v = std::getenv(key);
    return (v && *v) ? std::string(v) : fallback;
}

enum class Engine { SQLite, MySQL, Postgres, PostgresService };
const char* engineName(Engine e) {
    switch (e) {
        case Engine::SQLite:          return "SQLite";
        case Engine::MySQL:           return "MySQL";
        case Engine::Postgres:        return "Postgres";
        case Engine::PostgresService: return "PostgresService";
    }
    return "?";
}

#ifdef WITH_POSTGRESQL
std::string pgConnInfo() {
    return "host=" + envOr("PGHOST", "localhost") + " port=" + envOr("PGPORT", "5432") +
           " user=" + envOr("PGUSER", "maestro") + " password=" + envOr("PGPASSWORD", "") +
           " dbname=" + envOr("PGDATABASE", "cpap_monitoring") + " connect_timeout=3";
}
#endif

class CpapDashSentBackend : public ::testing::TestWithParam<Engine> {
protected:
    std::unique_ptr<IDatabase> db_;
    std::string path_;
    std::string root_a_, root_b_;   // per-process, so concurrent runs cannot collide

    void SetUp() override {
        const std::string pid = std::to_string(::getpid());
        root_a_ = "/hms-test-cpapdash-" + pid + "/as11/23243570851";
        root_b_ = "/hms-test-cpapdash-" + pid + "/cpap_data";

        switch (GetParam()) {
            case Engine::SQLite: {
                path_ = (std::filesystem::temp_directory_path() / ("hms_cpapdash_sent_" + pid + ".db")).string();
                std::filesystem::remove(path_);
                auto lite = std::make_unique<SQLiteDatabase>(path_);
                ASSERT_TRUE(lite->connect());
                db_ = std::move(lite);
                break;
            }
            case Engine::MySQL: {
#ifndef WITH_MYSQL
                GTEST_SKIP() << "built without MySQL (-DBUILD_WITH_MYSQL=OFF)";
#else
                const std::string host = envOr("MYSQL_TEST_HOST", "");
                if (host.empty()) GTEST_SKIP() << "MYSQL_TEST_HOST unset";
                auto my = std::make_unique<MySQLDatabase>(
                    host, std::stoi(envOr("MYSQL_TEST_PORT", "3306")), envOr("MYSQL_TEST_USER", ""),
                    envOr("MYSQL_TEST_PASSWORD", ""), envOr("MYSQL_TEST_DB", "hms_cpap_test"));
                if (!my->connect()) GTEST_SKIP() << "No usable MySQL at " << host;
                db_ = std::move(my);
#endif
                break;
            }
            case Engine::Postgres:
            case Engine::PostgresService: {
#ifndef WITH_POSTGRESQL
                GTEST_SKIP() << "built without PostgreSQL";
#else
                try {
                    pqxx::connection probe(pgConnInfo());
                    if (!probe.is_open()) throw std::runtime_error("not open");
                } catch (const std::exception& e) {
                    GTEST_SKIP() << "No usable PostgreSQL (" << e.what() << ")";
                }
                if (GetParam() == Engine::Postgres) {
                    auto pg = std::make_unique<PostgresDatabase>(pgConnInfo());
                    if (!pg->connect()) GTEST_SKIP() << "PostgresDatabase connect failed";
                    db_ = std::move(pg);
                } else {
                    auto svc = std::make_unique<DatabaseService>(pgConnInfo());
                    if (!svc->connect()) GTEST_SKIP() << "DatabaseService connect failed";
                    db_ = std::move(svc);
                }
#endif
                break;
            }
        }
    }

    void TearDown() override {
        if (db_) {
            db_->clearCpapDashSent(root_a_);
            db_->clearCpapDashSent(root_b_);
        }
        db_.reset();
        if (!path_.empty()) std::filesystem::remove(path_);
    }

    IDatabase& db() { return *db_; }

    IDatabase::CpapDashSent row(const std::string& root, const std::string& path, long long to,
                                const std::string& md5) {
        IDatabase::CpapDashSent r;
        r.root = root;
        r.path = path;
        r.machine = "23243570851";
        r.acked_to = to;
        r.acked_md5 = md5;
        r.updated_at = "2026-11-16T03:04:05Z";
        return r;
    }
};

TEST_P(CpapDashSentBackend, NothingAckedReadsAsNoRow) {
    EXPECT_FALSE(db().getCpapDashSent(root_a_, "STR.edf").has_value()) << engineName(GetParam());
    EXPECT_TRUE(db().listCpapDashSent(root_a_).empty()) << engineName(GetParam());
}

TEST_P(CpapDashSentBackend, AnUpsertIsReadBackWhole) {
    // > 2^31, so a 32-bit column fails here.
    const auto in = row(root_a_, "DATALOG/20261116/20261116_221502_BRP.edf", 4294967396LL,
                        "0123456789abcdef0123456789abcdef");
    ASSERT_TRUE(db().upsertCpapDashSent(in)) << engineName(GetParam());
    const auto out = db().getCpapDashSent(root_a_, in.path);
    ASSERT_TRUE(out.has_value()) << engineName(GetParam());
    EXPECT_EQ(out->root, in.root);
    EXPECT_EQ(out->path, in.path);
    EXPECT_EQ(out->machine, in.machine);
    EXPECT_EQ(out->acked_to, in.acked_to);
    EXPECT_EQ(out->acked_md5, in.acked_md5);
    EXPECT_EQ(out->updated_at, in.updated_at);
}

TEST_P(CpapDashSentBackend, ASecondAckMovesTheSameRow) {
    const std::string p = "DATALOG/20261116/20261116_221502_PLD.edf";
    ASSERT_TRUE(db().upsertCpapDashSent(row(root_a_, p, 1000, std::string(32, 'a'))));
    auto later = row(root_a_, p, 5000, std::string(32, 'b'));
    later.updated_at = "2026-11-16T03:05:05Z";
    ASSERT_TRUE(db().upsertCpapDashSent(later)) << engineName(GetParam());
    const auto all = db().listCpapDashSent(root_a_);
    ASSERT_EQ(all.size(), 1u) << engineName(GetParam());
    EXPECT_EQ(all[0].acked_to, 5000);
    EXPECT_EQ(all[0].acked_md5, std::string(32, 'b'));
    EXPECT_EQ(all[0].updated_at, "2026-11-16T03:05:05Z");
}

TEST_P(CpapDashSentBackend, TheSamePathUnderTwoRootsIsTwoRows) {
    // Each machine kept apart (D4): an AirSense 11's STR.edf and the card's.
    ASSERT_TRUE(db().upsertCpapDashSent(row(root_a_, "STR.edf", 10, std::string(32, '1'))));
    ASSERT_TRUE(db().upsertCpapDashSent(row(root_b_, "STR.edf", 20, std::string(32, '2'))));
    EXPECT_EQ(db().getCpapDashSent(root_a_, "STR.edf")->acked_to, 10) << engineName(GetParam());
    EXPECT_EQ(db().getCpapDashSent(root_b_, "STR.edf")->acked_to, 20) << engineName(GetParam());
}

TEST_P(CpapDashSentBackend, ListIsByRootInPathOrder) {
    ASSERT_TRUE(db().upsertCpapDashSent(row(root_a_, "STR.edf", 1, std::string(32, '0'))));
    ASSERT_TRUE(db().upsertCpapDashSent(row(root_a_, "DATALOG/20261115/a_EVE.edf", 1, std::string(32, '0'))));
    ASSERT_TRUE(db().upsertCpapDashSent(row(root_b_, "Identification.json", 1, std::string(32, '0'))));
    const auto a = db().listCpapDashSent(root_a_);
    ASSERT_EQ(a.size(), 2u) << engineName(GetParam());
    EXPECT_EQ(a[0].path, "DATALOG/20261115/a_EVE.edf");
    EXPECT_EQ(a[1].path, "STR.edf");
}

TEST_P(CpapDashSentBackend, ClearingARootLeavesTheOthers) {
    ASSERT_TRUE(db().upsertCpapDashSent(row(root_a_, "STR.edf", 1, std::string(32, '0'))));
    ASSERT_TRUE(db().upsertCpapDashSent(row(root_b_, "STR.edf", 1, std::string(32, '0'))));
    ASSERT_TRUE(db().clearCpapDashSent(root_a_)) << engineName(GetParam());
    EXPECT_FALSE(db().getCpapDashSent(root_a_, "STR.edf").has_value()) << engineName(GetParam());
    EXPECT_TRUE(db().getCpapDashSent(root_b_, "STR.edf").has_value()) << engineName(GetParam());
}

TEST_P(CpapDashSentBackend, ClearingEverythingEmptiesTheTable) {
    // Every row: only on the temp SQLite file this suite owns.
    if (GetParam() != Engine::SQLite) GTEST_SKIP() << "clears the whole table: SQLite temp file only";
    ASSERT_TRUE(db().upsertCpapDashSent(row(root_a_, "STR.edf", 1, std::string(32, '0'))));
    ASSERT_TRUE(db().upsertCpapDashSent(row(root_b_, "STR.edf", 1, std::string(32, '0'))));
    ASSERT_TRUE(db().clearCpapDashSent(""));
    EXPECT_TRUE(db().listCpapDashSent("").empty());
}

TEST_P(CpapDashSentBackend, AnEmptyKeyIsRefused) {
    EXPECT_FALSE(db().upsertCpapDashSent(row("", "STR.edf", 1, std::string(32, '0'))))
        << engineName(GetParam());
    EXPECT_FALSE(db().upsertCpapDashSent(row(root_a_, "", 1, std::string(32, '0'))))
        << engineName(GetParam());
}

INSTANTIATE_TEST_SUITE_P(Engines, CpapDashSentBackend,
                         ::testing::Values(Engine::SQLite, Engine::MySQL, Engine::Postgres,
                                           Engine::PostgresService),
                         [](const ::testing::TestParamInfo<Engine>& i) {
                             return std::string(engineName(i.param));
                         });

}  // namespace
