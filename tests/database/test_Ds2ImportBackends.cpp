//
// test_Ds2ImportBackends.cpp: SDD-064, a DreamStation 2 night read over
// Bluetooth saved on every engine. The import writes only through IDatabase
// (saveSession, replaceSessionFiles, close at its data end, the daily row),
// and those are implemented three times.
//
// SQLite always runs (temp file). MySQL runs when MYSQL_TEST_HOST is set,
// PostgreSQL when PGHOST answers; each skips cleanly otherwise.
//
// SAFETY: never DROPs or TRUNCATEs. Rows are under a per-process device id
// and removed in TearDown.
//
#include <gtest/gtest.h>

#include "../clients/Ds2TestMachine.h"
#include "database/IDatabase.h"
#include "database/SQLiteDatabase.h"
#include "database/SqlDialect.h"
#include "services/As11NightImport.h"
#include "services/Ds2NightImport.h"
#include "services/Ds2Store.h"
#ifdef WITH_MYSQL
#include "database/MySQLDatabase.h"
#endif
#ifdef WITH_POSTGRESQL
#include "database/DatabaseService.h"
#include <pqxx/pqxx>
#endif

#include <chrono>
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
namespace fs = std::filesystem;
namespace ds2 = cpapdash::ds2ble;

namespace {

std::string envOr(const char* key, const std::string& fallback) {
    const char* v = std::getenv(key);
    return (v && *v) ? std::string(v) : fallback;
}

enum class Engine { SQLite, MySQL, Postgres };
const char* engineName(Engine e) {
    return e == Engine::SQLite ? "SQLite" : e == Engine::MySQL ? "MySQL" : "Postgres";
}

constexpr uint32_t kT0 = 1893456000;   // 2030-01-01, made up

class Ds2ImportBackend : public ::testing::TestWithParam<Engine> {
protected:
    std::shared_ptr<IDatabase> db_;
    fs::path dir_;
    std::string device_;
    std::vector<std::chrono::system_clock::time_point> saved_;

    void SetUp() override {
        device_ = "test-ds2-dev-" + std::to_string(::getpid());
        dir_ = fs::temp_directory_path() / ("ds2_backends_" + std::to_string(::getpid()) + "_" + engineName(GetParam()));
        fs::remove_all(dir_);
        fs::create_directories(dir_);
        switch (GetParam()) {
            case Engine::SQLite: {
                auto lite = std::make_shared<SQLiteDatabase>((dir_ / "t.db").string());
                ASSERT_TRUE(lite->connect());
                db_ = lite;
                break;
            }
            case Engine::MySQL: {
#ifndef WITH_MYSQL
                GTEST_SKIP() << "built without MySQL";
#else
                const std::string host = envOr("MYSQL_TEST_HOST", "");
                if (host.empty()) GTEST_SKIP() << "MYSQL_TEST_HOST unset";
                auto my = std::make_shared<MySQLDatabase>(host, std::stoi(envOr("MYSQL_TEST_PORT", "3306")),
                                                          envOr("MYSQL_TEST_USER", ""),
                                                          envOr("MYSQL_TEST_PASSWORD", ""),
                                                          envOr("MYSQL_TEST_DB", "hms_cpap_test"));
                if (!my->connect()) GTEST_SKIP() << "No usable MySQL at " << host;
                db_ = my;
#endif
                break;
            }
            case Engine::Postgres: {
#ifndef WITH_POSTGRESQL
                GTEST_SKIP() << "built without PostgreSQL";
#else
                const std::string ci = "host=" + envOr("PGHOST", "localhost") + " port=" + envOr("PGPORT", "5432") +
                                       " user=" + envOr("PGUSER", "maestro") + " password=" +
                                       envOr("PGPASSWORD", "") + " dbname=" + envOr("PGDATABASE", "cpap_monitoring") +
                                       " connect_timeout=3";
                try {
                    pqxx::connection probe(ci);
                    if (!probe.is_open()) throw std::runtime_error("not open");
                } catch (const std::exception& e) {
                    GTEST_SKIP() << "No usable PostgreSQL (" << e.what() << ")";
                }
                auto svc = std::make_shared<DatabaseService>(ci);
                if (!svc->connect()) GTEST_SKIP() << "DatabaseService connect failed";
                db_ = svc;
#endif
                break;
            }
        }
    }

    void TearDown() override {
        if (db_) {
            for (const auto& s : saved_) As11NightImport::deleteSessionAt(*db_, device_, s);
            db_->executeQuery("DELETE FROM cpap_daily_summary WHERE device_id=" + sql::param(1, db_->dbType()),
                              {device_});
        }
        db_.reset();
        std::error_code ec;
        fs::remove_all(dir_, ec);
    }

    void store(uint32_t session, uint32_t t, unsigned secs, int oa) {
        Ds2Store st(dir_.string());
        ds2::Identity id;
        id.serial = "TEST0000000004";
        id.model = "520X110C";
        st.writeIdentity(id);
        auto [s, e] = ds2test::night(session, t, secs, oa);
        ASSERT_FALSE(st.writeRecord(id.serial, ds2::Record{1, session, s, {}, {}}).empty());
        ASSERT_FALSE(st.writeRecord(id.serial, ds2::Record{2, session, e, {}, {}}).empty());
    }
};

}  // namespace

TEST_P(Ds2ImportBackend, NightsAreSavedOnceWithTheirFiles) {
    store(500, kT0, 3600, 2);
    store(501, kT0 + 86400, 1800, 1);
    Ds2NightImport imp(dir_.string(), db_, device_, "DreamStation 2");
    const auto r = imp.run({});
    for (const auto& s : r.saved) saved_.push_back(*s.session_start);
    ASSERT_EQ(r.imported, 2);
    EXPECT_EQ(r.failed, 0);
    const auto start = std::chrono::system_clock::time_point(std::chrono::seconds(kT0 + 1));
    EXPECT_TRUE(db_->sessionExists(device_, start));

    const auto files = db_->executeQuery(
        "SELECT f.rel_path FROM cpap_session_files f JOIN cpap_sessions s ON s.id = f.session_id "
        "WHERE s.device_id = " + sql::param(1, db_->dbType()),
        {device_});
    EXPECT_EQ(files.size(), 4u);
    for (const auto& f : files) EXPECT_EQ(f["rel_path"].asString().rfind("ds2/TEST0000000004/P0/", 0), 0u);

    // A second process imports nothing again.
    Ds2NightImport again(dir_.string(), db_, device_, "DreamStation 2");
    EXPECT_EQ(again.run({}).imported, 0);
}

INSTANTIATE_TEST_SUITE_P(Engines, Ds2ImportBackend,
                         ::testing::Values(Engine::SQLite, Engine::MySQL, Engine::Postgres),
                         [](const auto& info) { return std::string(engineName(info.param)); });
