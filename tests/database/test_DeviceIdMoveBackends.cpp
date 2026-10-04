// SDD-051: re-filing an install's rows under a new device id.
//
// The move runs unattended at startup on every install still on an old fixed
// default id, on all three engines, and an install whose rows land half under
// each id shows half its history. These cases hold the parts that matter:
// every row under the old id moves, a row under any other id does not, and a
// second run is a harmless no-op.
//
// SAFETY: never DROPs or TRUNCATEs. Every row is namespaced to per-process
// device ids and deleted in TearDown.
#include <gtest/gtest.h>

#include "database/IDatabase.h"
#include "database/SQLiteDatabase.h"
#include "database/SqlDialect.h"
#ifdef WITH_MYSQL
#include "database/MySQLDatabase.h"
#endif
#ifdef WITH_POSTGRESQL
#include "database/DatabaseService.h"
#include "database/PostgresDatabase.h"
#include <pqxx/pqxx>
#endif

#include <chrono>
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

std::chrono::system_clock::time_point at(int day) {
    // 2099-06-<day> 23:00:00 UTC: far from any real night.
    return std::chrono::system_clock::time_point(std::chrono::seconds(4084815600LL + day * 86400LL));
}

class DeviceIdMove : public ::testing::TestWithParam<Engine> {
protected:
    std::unique_ptr<IDatabase> db_;
    std::string path_;
    std::string old_, new_, other_;

    void SetUp() override {
        const std::string pid = std::to_string(::getpid());
        old_ = "test-move-old-" + pid;
        new_ = "test-move-new-" + pid;
        other_ = "test-move-other-" + pid;

        switch (GetParam()) {
            case Engine::SQLite: {
                path_ = (std::filesystem::temp_directory_path() /
                         ("hms_move_" + pid + ".db")).string();
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
                    host, std::stoi(envOr("MYSQL_TEST_PORT", "3306")),
                    envOr("MYSQL_TEST_USER", ""), envOr("MYSQL_TEST_PASSWORD", ""),
                    envOr("MYSQL_TEST_DB", "hms_cpap_test"));
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
            for (const auto& id : {old_, new_, other_}) {
                for (const char* table : IDatabase::kDeviceIdTables) {
                    db_->executeQuery(std::string("DELETE FROM ") + table + " WHERE device_id=" +
                                          sql::param(1, db_->dbType()),
                                      {id});
                }
            }
        }
        db_.reset();
        if (!path_.empty()) std::filesystem::remove(path_);
    }

    IDatabase& db() { return *db_; }

    void session(const std::string& device, int day) {
        CPAPSession s;
        s.device_id = device;
        s.session_start = at(day);
        s.session_end = at(day) + std::chrono::hours(6);
        s.duration_seconds = 6 * 3600;
        ASSERT_TRUE(db().saveSession(s));
    }

    void removed(const std::string& device, const std::string& night) {
        db().executeQuery("INSERT INTO cpap_removed_nights (device_id, night) VALUES (" +
                              sql::param(1, db().dbType()) + "," + sql::param(2, db().dbType()) + ")",
                          {device, night});
    }
};

}  // namespace

TEST_P(DeviceIdMove, EveryRowUnderTheOldIdMovesAndNoOtherDoes) {
    session(old_, 1);
    session(old_, 2);
    session(other_, 1);
    removed(old_, "20990601");
    removed(other_, "20990602");

    EXPECT_EQ(db().moveDeviceId(old_, new_), 2);

    EXPECT_TRUE(db().sessionExists(new_, at(1)));
    EXPECT_TRUE(db().sessionExists(new_, at(2)));
    EXPECT_FALSE(db().sessionExists(old_, at(1)));
    EXPECT_FALSE(db().sessionExists(old_, at(2)));
    EXPECT_TRUE(db().sessionExists(other_, at(1))) << "another install's night moved";

    EXPECT_EQ(db().removedNights(new_), std::vector<std::string>{"20990601"});
    EXPECT_TRUE(db().removedNights(old_).empty());
    EXPECT_EQ(db().removedNights(other_), std::vector<std::string>{"20990602"});
}

TEST_P(DeviceIdMove, ASecondRunMovesNothing) {
    session(old_, 1);
    ASSERT_EQ(db().moveDeviceId(old_, new_), 1);
    EXPECT_EQ(db().moveDeviceId(old_, new_), 0);
    EXPECT_TRUE(db().sessionExists(new_, at(1)));
}

TEST_P(DeviceIdMove, RefusesANonsenseMove) {
    session(old_, 1);
    EXPECT_EQ(db().moveDeviceId(old_, old_), -1);
    EXPECT_EQ(db().moveDeviceId("", new_), -1);
    EXPECT_EQ(db().moveDeviceId(old_, ""), -1);
    EXPECT_TRUE(db().sessionExists(old_, at(1)));
}

INSTANTIATE_TEST_SUITE_P(Engines, DeviceIdMove,
                         ::testing::Values(Engine::SQLite, Engine::MySQL, Engine::Postgres,
                                           Engine::PostgresService),
                         [](const ::testing::TestParamInfo<Engine>& i) {
                             return std::string(engineName(i.param));
                         });
