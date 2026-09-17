//
// test_SessionEndBackends.cpp: SDD-037 D2, closing a session with the end the
// DATA knows, on every engine.
//
// markSessionCompleted() stamps the clock, which is right for a live night and
// wrong for an import: a user's first local import of twenty nights closed the
// two it could and gave both the timestamp of the run (CpapDash ticket 129).
// markSessionCompletedAt() takes the end instead, and every bulk path now uses
// it. Three engines write that column, so three engines are tested; a backend
// that never overrode the new method would fall back to the clock and fail the
// value assertion here.
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
#include "utils/SessionEnd.h"
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
using std::chrono::system_clock;
using std::chrono::seconds;
namespace fs = std::filesystem;

namespace {

std::string envOr(const char* key, const std::string& fallback) {
    const char* v = std::getenv(key);
    return (v && *v) ? std::string(v) : fallback;
}

/// A start on the LOCAL clock, as an EDF wall clock is read.
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

class SessionEndBackendTest : public ::testing::TestWithParam<Engine> {
protected:
    void SetUp() override {
        device_ = "send_test_" + std::to_string(::getpid());
        switch (GetParam()) {
            case Engine::SQLite: {
                path_ = (fs::temp_directory_path() /
                         ("hms_cpap_send_" + std::to_string(::getpid()) + ".db")).string();
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

    CPAPSession makeSession(system_clock::time_point start, int duration_seconds) {
        CPAPSession s;
        s.device_id = device_;
        s.device_name = "AirSense 11";
        s.serial_number = "SDD037";
        s.session_start = start;
        s.duration_seconds = duration_seconds;
        s.data_records = duration_seconds / 60;
        return s;
    }

    /// session_end as the row holds it, "" when NULL.
    std::string storedEnd() {
        const auto p = sql::param(1, db_->dbType());
        auto rows = db_->executeQuery(
            "SELECT session_end FROM cpap_sessions WHERE device_id = " + p, {device_});
        if (rows.empty() || rows[0]["session_end"].isNull()) return "";
        return rows[0]["session_end"].asString();
    }

    std::shared_ptr<IDatabase> db_;
    std::string device_;
    std::string path_;
};

}  // namespace

TEST_P(SessionEndBackendTest, TheEndWrittenIsTheOneGivenNotTheClock) {
    const auto start = local(2099, 3, 14, 23, 0);
    const auto end   = start + seconds(7 * 3600 + 15 * 60);   // 06:15 the next day
    ASSERT_TRUE(db_->saveSession(makeSession(start, 7 * 3600 + 15 * 60)));
    EXPECT_EQ(storedEnd(), "") << engineName(GetParam()) << ": a new session starts open";

    ASSERT_TRUE(db_->markSessionCompletedAt(device_, start, end))
        << engineName(GetParam());

    // 2099 is the point: a clock stamp could never look like this.
    const auto written = storedEnd();
    EXPECT_NE(written.find("2099-03-15 06:15"), std::string::npos)
        << engineName(GetParam()) << ": session_end is " << written;
}

TEST_P(SessionEndBackendTest, AnAlreadyClosedSessionKeepsTheEndItHas) {
    const auto start = local(2099, 3, 14, 23, 0);
    const auto first = start + seconds(3600);
    ASSERT_TRUE(db_->saveSession(makeSession(start, 3600)));
    ASSERT_TRUE(db_->markSessionCompletedAt(device_, start, first));
    const auto after_first = storedEnd();

    // Both forms refuse: the IS NULL guard is what makes a re-import safe.
    EXPECT_FALSE(db_->markSessionCompletedAt(device_, start, start + seconds(9999)))
        << engineName(GetParam());
    EXPECT_FALSE(db_->markSessionCompleted(device_, start)) << engineName(GetParam());
    EXPECT_EQ(storedEnd(), after_first) << engineName(GetParam());
}

TEST_P(SessionEndBackendTest, NoSuchSessionIsFalse) {
    EXPECT_FALSE(db_->markSessionCompletedAt(device_, local(2099, 3, 14, 23, 0),
                                             local(2099, 3, 15, 6, 0)))
        << engineName(GetParam());
}

TEST_P(SessionEndBackendTest, TwoNightsImportedTogetherGetTheirOwnEnds) {
    // The ticket-129 shape: a bulk import must not give every night one stamp.
    const auto a = local(2099, 3, 12, 22, 30);
    const auto b = local(2099, 3, 13, 23, 45);
    ASSERT_TRUE(db_->saveSession(makeSession(a, 6 * 3600)));
    ASSERT_TRUE(db_->saveSession(makeSession(b, 8 * 3600)));

    ASSERT_TRUE(closeWithDataEnd(*db_, device_, a, makeSession(a, 6 * 3600)));
    ASSERT_TRUE(closeWithDataEnd(*db_, device_, b, makeSession(b, 8 * 3600)));

    const auto p = sql::param(1, db_->dbType());
    auto rows = db_->executeQuery(
        "SELECT session_end FROM cpap_sessions WHERE device_id = " + p +
        " ORDER BY session_start", {device_});
    ASSERT_EQ(rows.size(), 2u) << engineName(GetParam());
    const auto end_a = rows[0]["session_end"].asString();
    const auto end_b = rows[1]["session_end"].asString();
    EXPECT_NE(end_a, end_b) << engineName(GetParam()) << ": both nights share one end";
    EXPECT_NE(end_a.find("2099-03-13 04:30"), std::string::npos) << end_a;
    EXPECT_NE(end_b.find("2099-03-14 07:45"), std::string::npos) << end_b;
}

INSTANTIATE_TEST_SUITE_P(
    Engines, SessionEndBackendTest,
    ::testing::Values(Engine::SQLite, Engine::MySQL, Engine::Postgres),
    [](const ::testing::TestParamInfo<Engine>& info) {
        return std::string(engineName(info.param));
    });

// ── dataEndOf: what the parse knows, in order ───────────────────────────────

TEST(SessionDataEnd, TheParsersOwnEndWins) {
    CPAPSession s;
    s.session_start = local(2099, 3, 14, 23, 0);
    s.duration_seconds = 3600;                       // disagrees on purpose
    s.session_end = local(2099, 3, 15, 5, 30);
    const auto end = dataEndOf(s);
    ASSERT_TRUE(end.has_value());
    EXPECT_EQ(end.value(), s.session_end.value());
}

TEST(SessionDataEnd, TheDurationFillsInWhenThereIsNoEnd) {
    CPAPSession s;
    s.session_start = local(2099, 3, 14, 23, 0);
    s.duration_seconds = 2 * 3600;
    const auto end = dataEndOf(s);
    ASSERT_TRUE(end.has_value());
    EXPECT_EQ(end.value(), s.session_start.value() + seconds(2 * 3600));
}

// ── The 5-day window a local night settles after (SDD-037 D1) ───────────────

TEST(LocalNightSettled, OlderThanTheWindowSettlesAndNewerDoesNot) {
    const auto now = local(2099, 3, 20, 12, 0);
    const auto window = std::chrono::hours(24 * 5);

    // Michael's case: nights days old on a first import, all closable at once.
    EXPECT_TRUE(localNightHasSettled(local(2099, 3, 10, 23, 0), now, window));
    EXPECT_TRUE(localNightHasSettled(local(2099, 3, 15, 11, 59), now, window));

    // Last night, and a folder that is a card still being written: left to the
    // checkpoint path, which closes them when their files stop changing.
    EXPECT_FALSE(localNightHasSettled(local(2099, 3, 19, 23, 0), now, window));
    EXPECT_FALSE(localNightHasSettled(local(2099, 3, 20, 3, 0), now, window));

    // Exactly on the boundary counts as settled.
    EXPECT_TRUE(localNightHasSettled(now - window, now, window));
    EXPECT_FALSE(localNightHasSettled(now - window + seconds(1), now, window));
}

TEST(SessionDataEnd, NothingKnownIsEmptySoTheCallerCanLeaveItOpen) {
    CPAPSession s;
    s.session_start = local(2099, 3, 14, 23, 0);
    EXPECT_FALSE(dataEndOf(s).has_value());

    s.duration_seconds = 0;
    EXPECT_FALSE(dataEndOf(s).has_value());

    CPAPSession no_start;
    no_start.duration_seconds = 3600;
    EXPECT_FALSE(dataEndOf(no_start).has_value());
}
