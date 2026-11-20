//
// test_ComputedEventsBackends.cpp: SDD-058 D4, the events computed from the
// flow stored apart from the machine's, and the set a session's index came
// from, on every engine.
//
// SQLite always runs. MySQL runs when MYSQL_TEST_HOST is set, PostgreSQL when
// PGHOST is; both skip cleanly otherwise.
//
// SAFETY: every row is dated 2099 and namespaced to a per-process device id,
// and deleted in TearDown, so pointing MYSQL_TEST_*/PG* at a populated database
// cannot disturb real data.
//
#include <gtest/gtest.h>

#include "database/IDatabase.h"
#include "database/SQLiteDatabase.h"
#include "database/SqlDialect.h"
#include "services/ComputedEvents.h"
#ifdef WITH_MYSQL
#include "database/MySQLDatabase.h"
#endif
#ifdef WITH_POSTGRESQL
#include "database/PostgresDatabase.h"
#endif

#include <chrono>
#include <cstdlib>
#include <ctime>
#include <filesystem>
#include <memory>
#include <string>
#ifndef _WIN32
#include <unistd.h>
#else
#include <process.h>
#endif

using namespace hms_cpap;
using std::chrono::seconds;
using std::chrono::system_clock;
namespace fs = std::filesystem;

namespace {

std::string envOr(const char* key, const std::string& fallback) {
    const char* v = std::getenv(key);
    return (v && *v) ? std::string(v) : fallback;
}

system_clock::time_point local(int y, int mo, int d, int h, int mi = 0) {
    std::tm tm{};
    tm.tm_year = y - 1900;
    tm.tm_mon = mo - 1;
    tm.tm_mday = d;
    tm.tm_hour = h;
    tm.tm_min = mi;
    tm.tm_isdst = -1;
    return system_clock::from_time_t(std::mktime(&tm));
}

enum class Engine { SQLite, MySQL, Postgres };

const char* engineName(Engine e) {
    switch (e) {
        case Engine::SQLite:   return "SQLite";
        case Engine::MySQL:    return "MySQL";
        case Engine::Postgres: return "Postgres";
    }
    return "?";
}

class ComputedEventsBackendTest : public ::testing::TestWithParam<Engine> {
protected:
    void SetUp() override {
        device_ = "sdd058_test_" + std::to_string(::getpid());
        switch (GetParam()) {
            case Engine::SQLite: {
                path_ = (fs::temp_directory_path() /
                         ("hms_cpap_sdd058_" + std::to_string(::getpid()) + ".db")).string();
                fs::remove(path_);
                auto lite = std::make_shared<SQLiteDatabase>(path_);
                ASSERT_TRUE(lite->connect()) << "SQLite connect failed";
                db_ = std::move(lite);
                return;
            }
            case Engine::MySQL: {
#ifndef WITH_MYSQL
                GTEST_SKIP() << "built without MySQL";
#else
                const std::string host = envOr("MYSQL_TEST_HOST", "");
                if (host.empty()) GTEST_SKIP() << "MYSQL_TEST_HOST unset, skipping MySQL.";
                auto my = std::make_shared<MySQLDatabase>(
                    host, std::stoi(envOr("MYSQL_TEST_PORT", "3306")),
                    envOr("MYSQL_TEST_USER", ""), envOr("MYSQL_TEST_PASSWORD", ""),
                    envOr("MYSQL_TEST_DB", "hms_cpap_test"));
                if (!my->connect()) GTEST_SKIP() << "No usable MySQL at " << host;
                db_ = std::move(my);
                return;
#endif
            }
            case Engine::Postgres: {
#ifndef WITH_POSTGRESQL
                GTEST_SKIP() << "built without PostgreSQL";
#else
                const std::string host = envOr("PGHOST", "");
                if (host.empty()) GTEST_SKIP() << "PGHOST unset, skipping PostgreSQL.";
                const std::string conn =
                    "host=" + host + " port=" + envOr("PGPORT", "5432") +
                    " user=" + envOr("PGUSER", "maestro") +
                    " password=" + envOr("PGPASSWORD", "") +
                    " dbname=" + envOr("PGDATABASE", "cpap_monitoring") +
                    " connect_timeout=3";
                auto pg = std::make_shared<PostgresDatabase>(conn);
                if (!pg->connect()) GTEST_SKIP() << "No usable PostgreSQL at " << host;
                db_ = std::move(pg);
                return;
#endif
            }
        }
    }

    void TearDown() override {
        if (db_) {
            const auto p = sql::param(1, db_->dbType());
            db_->executeQuery("DELETE FROM cpap_session_files WHERE session_id IN "
                              "(SELECT id FROM cpap_sessions WHERE device_id = " + p + ")",
                              {device_});
            db_->executeQuery("DELETE FROM cpap_sessions WHERE device_id = " + p, {device_});
        }
        db_.reset();
        if (!path_.empty()) {
            for (const auto* suffix : {"", "-wal", "-shm"}) {
                std::error_code ec;
                fs::remove(path_ + suffix, ec);
            }
        }
    }

    CPAPSession makeSession(system_clock::time_point start) {
        CPAPSession s;
        s.device_id = device_;
        s.device_name = "Luna G3";
        s.serial_number = "SDD058";
        s.session_start = start;
        s.duration_seconds = 3600;
        s.data_records = 60;
        s.events = {CPAPEvent(EventType::OBSTRUCTIVE, start + seconds(60), 12)};
        s.calculateMetrics();
        return s;
    }

    int count(const std::string& table) {
        const auto p = sql::param(1, db_->dbType());
        auto rows = db_->executeQuery("SELECT COUNT(*) AS n FROM " + table +
                                          " t JOIN cpap_sessions s ON s.id = t.session_id"
                                          " WHERE s.device_id = " + p,
                                      {device_});
        if (rows.empty()) return -1;
        const auto& v = rows[0]["n"];
        return v.isString() ? std::stoi(v.asString()) : v.asInt();
    }

    std::string indexEvents() {
        const auto p = sql::param(1, db_->dbType());
        auto rows = db_->executeQuery(
            "SELECT m.index_events AS v FROM cpap_session_metrics m JOIN cpap_sessions s"
            " ON s.id = m.session_id WHERE s.device_id = " + p,
            {device_});
        if (rows.empty() || rows[0]["v"].isNull()) return "";
        return rows[0]["v"].asString();
    }

    std::shared_ptr<IDatabase> db_;
    std::string device_;
    std::string path_;
};

}  // namespace

TEST_P(ComputedEventsBackendTest, ASessionReadsAsTheMachinesUntilToldOtherwise) {
    const auto start = local(2099, 3, 14, 23, 0);
    ASSERT_TRUE(db_->saveSession(makeSession(start)));
    EXPECT_EQ(indexEvents(), "machine") << engineName(GetParam());
    EXPECT_EQ(count("cpap_computed_events"), 0) << engineName(GetParam());
}

TEST_P(ComputedEventsBackendTest, TheComputedSetIsStoredApartAndReplacedWhole) {
    const auto start = local(2099, 3, 14, 23, 0);
    ASSERT_TRUE(db_->saveSession(makeSession(start)));
    const std::vector<CPAPEvent> two = {CPAPEvent(EventType::APNEA, start + seconds(180), 20),
                                        CPAPEvent(EventType::APNEA, start + seconds(900), 15)};
    ASSERT_TRUE(storeComputedEvents(*db_, device_, start, two, IndexEvents::Computed))
        << engineName(GetParam());
    EXPECT_EQ(count("cpap_computed_events"), 2) << engineName(GetParam());
    EXPECT_EQ(count("cpap_events"), 1) << engineName(GetParam()) << ": the machine's untouched";
    EXPECT_EQ(indexEvents(), "computed") << engineName(GetParam());

    // Saved again (the session grew): the metrics upsert keeps the field.
    ASSERT_TRUE(db_->saveSession(makeSession(start)));
    EXPECT_EQ(indexEvents(), "computed") << engineName(GetParam());

    ASSERT_TRUE(storeComputedEvents(*db_, device_, start, {two[0]}, IndexEvents::Machine))
        << engineName(GetParam());
    EXPECT_EQ(count("cpap_computed_events"), 1) << engineName(GetParam()) << ": replaced";
    EXPECT_EQ(indexEvents(), "machine") << engineName(GetParam());
}

TEST_P(ComputedEventsBackendTest, ASessionNotStoredTakesNothing) {
    EXPECT_FALSE(storeComputedEvents(*db_, device_, local(2099, 3, 15, 23, 0), {},
                                     IndexEvents::Computed))
        << engineName(GetParam());
}

TEST_P(ComputedEventsBackendTest, RemovingTheSessionRemovesItsComputedEvents) {
    const auto start = local(2099, 3, 14, 23, 0);
    ASSERT_TRUE(db_->saveSession(makeSession(start)));
    ASSERT_TRUE(storeComputedEvents(*db_, device_, start,
                                    {CPAPEvent(EventType::APNEA, start + seconds(180), 20)},
                                    IndexEvents::Machine));
    const auto p = sql::param(1, db_->dbType());
    db_->executeQuery("DELETE FROM cpap_session_files WHERE session_id IN "
                      "(SELECT id FROM cpap_sessions WHERE device_id = " + p + ")",
                      {device_});
    db_->executeQuery("DELETE FROM cpap_sessions WHERE device_id = " + p, {device_});
    auto rows = db_->executeQuery("SELECT COUNT(*) AS n FROM cpap_computed_events c"
                                  " WHERE NOT EXISTS (SELECT 1 FROM cpap_sessions s"
                                  " WHERE s.id = c.session_id)");
    ASSERT_FALSE(rows.empty());
    const auto& v = rows[0]["n"];
    EXPECT_EQ(v.isString() ? std::stoi(v.asString()) : v.asInt(), 0)
        << engineName(GetParam()) << ": no orphan left";
}

INSTANTIATE_TEST_SUITE_P(
    Engines, ComputedEventsBackendTest,
    ::testing::Values(Engine::SQLite, Engine::MySQL, Engine::Postgres),
    [](const ::testing::TestParamInfo<Engine>& info) {
        return std::string(engineName(info.param));
    });
