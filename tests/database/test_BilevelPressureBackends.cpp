/**
 * SDD-030 (#33): the PLD's Press channel per minute, stored on every engine.
 *
 * On an AirCurve that channel is IPAP and EprPress is EPAP. The parser handed
 * hms-cpap Press as `therapy_pressure` all along, and every backend threw it
 * away: the per-minute table had no column for it. These pin the column, the
 * fill-in-never-erase upsert it shares with its neighbours, and the nightly
 * read that turns the minutes into the night's IPAP beside its EPAP.
 *
 * SAFETY: rows are namespaced to a per-process device id, dated 2099, and
 * deleted in TearDown.
 */

#include <gtest/gtest.h>

#include "database/SQLiteDatabase.h"
#include "database/SqlDialect.h"
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
#include <unistd.h>

using namespace hms_cpap;
using namespace std::chrono;
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

class BilevelPressureBackendTest : public ::testing::TestWithParam<Engine> {
protected:
    void SetUp() override {
        device_ = "bilevel_test_" + std::to_string(::getpid());
        switch (GetParam()) {
            case Engine::SQLite: {
                path_ = (fs::temp_directory_path() /
                         ("hms_cpap_bilevel_" + std::to_string(::getpid()) + ".db")).string();
                fs::remove(path_);
                auto lite = std::make_shared<SQLiteDatabase>(path_);
                ASSERT_TRUE(lite->connect());
                db_ = std::move(lite);
                return;
            }
            case Engine::MySQL: {
#ifndef WITH_MYSQL
                GTEST_SKIP() << "built without MySQL (-DBUILD_WITH_MYSQL=OFF)";
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
            db_->executeQuery("DELETE FROM cpap_sessions WHERE device_id = " +
                                  sql::param(1, db_->dbType()),
                              {device_});
            db_->executeQuery("DELETE FROM cpap_daily_summary WHERE device_id = " +
                                  sql::param(1, db_->dbType()),
                              {device_});
        }
        db_.reset();
        if (!path_.empty()) {
            for (const auto* suffix : {"", "-wal", "-shm"}) {
                std::error_code ec;
                fs::remove(path_ + suffix, ec);
            }
        }
    }

    /// One night of an AirCurve: each minute's Press (IPAP) and EprPress
    /// (EPAP), four apart, the reporter's pressure support.
    CPAPSession night(const std::vector<std::pair<double, double>>& ipap_epap,
                      bool with_ipap = true) {
        CPAPSession s;
        s.device_id = device_;
        s.device_name = "AirCurve 11 VAuto";
        s.serial_number = "SDD030";
        s.session_start = start();
        s.duration_seconds = static_cast<int>(ipap_epap.size()) * 60;
        s.data_records = static_cast<int>(ipap_epap.size());
        for (size_t i = 0; i < ipap_epap.size(); ++i) {
            BreathingSummary b;
            b.timestamp = start() + minutes(i);
            b.respiratory_rate = 14.0;
            if (with_ipap) b.therapy_pressure = ipap_epap[i].first;
            b.epr_pressure = ipap_epap[i].second;
            s.breathing_summary.push_back(b);
        }
        SessionMetrics m;
        m.ahi = 1.0;
        s.metrics = m;
        return s;
    }
    static system_clock::time_point start() { return local(2099, 7, 1, 23); }

    std::string path_;
    std::string device_;
    std::shared_ptr<IDatabase> db_;
};

}  // namespace

TEST_P(BilevelPressureBackendTest, TheNightCarriesIpapBesideEpap) {
    ASSERT_TRUE(db_->saveSession(night({{9.0, 5.0}, {9.5, 5.5}, {10.0, 6.0}})))
        << engineName(GetParam());

    const auto m = db_->getNightlyMetrics(device_, start());
    ASSERT_TRUE(m.has_value()) << engineName(GetParam());
    ASSERT_TRUE(m->avg_therapy_pressure.has_value())
        << engineName(GetParam()) << ": PLD Press was not stored";
    EXPECT_NEAR(*m->avg_therapy_pressure, 9.5, 1e-6) << engineName(GetParam());
    ASSERT_TRUE(m->avg_epr_pressure.has_value()) << engineName(GetParam());
    EXPECT_NEAR(*m->avg_epr_pressure, 5.5, 1e-6) << engineName(GetParam());
    // The two are averaged over the same minutes, so their difference is the
    // pressure support exactly.
    EXPECT_NEAR(*m->avg_therapy_pressure - *m->avg_epr_pressure, 4.0, 1e-6);
}

TEST_P(BilevelPressureBackendTest, ALaterSaveWithoutPressDoesNotEraseIt) {
    // A session is saved after each checkpoint file; a save whose minutes lack
    // Press (a BRP before its PLD) must fill in, never erase.
    ASSERT_TRUE(db_->saveSession(night({{9.0, 5.0}, {9.0, 5.0}})));
    ASSERT_TRUE(db_->saveSession(night({{0, 5.0}, {0, 5.0}}, /*with_ipap=*/false)));
    const auto m = db_->getNightlyMetrics(device_, start());
    ASSERT_TRUE(m.has_value() && m->avg_therapy_pressure.has_value())
        << engineName(GetParam());
    EXPECT_NEAR(*m->avg_therapy_pressure, 9.0, 1e-6) << engineName(GetParam());
}

TEST_P(BilevelPressureBackendTest, AMachineWithoutPressHasNoIpap) {
    ASSERT_TRUE(db_->saveSession(night({{0, 5.0}, {0, 5.0}}, /*with_ipap=*/false)));
    const auto m = db_->getNightlyMetrics(device_, start());
    ASSERT_TRUE(m.has_value()) << engineName(GetParam());
    EXPECT_FALSE(m->avg_therapy_pressure.has_value())
        << engineName(GetParam()) << ": absent must read as absent, not 0";
}

INSTANTIATE_TEST_SUITE_P(
    Engines, BilevelPressureBackendTest,
    ::testing::Values(Engine::SQLite, Engine::MySQL, Engine::Postgres),
    [](const ::testing::TestParamInfo<Engine>& info) {
        return std::string(engineName(info.param));
    });
