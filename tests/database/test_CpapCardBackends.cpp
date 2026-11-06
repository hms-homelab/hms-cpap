//
// test_CpapCardBackends.cpp: SDD-061, a .cpapcard's nights through the upload's
// own steps, on every engine.
//
// The same steps the upload route and the importer take, in their order: the
// layout read out of the file and checked; the file extracted; the layout
// taken out of the card; the card classified; the removed nights it carries
// restored (SDD-036 D2); every session the database lacks imported, one
// already stored skipped. The database is what differs between engines, so
// the restore and the import's lookups run on each one.
//
// The card is a synthetic Sefam S.Box card (manifests only, no channel data:
// the parser will not vouch for its sessions, so they are counted as refused,
// never saved) in the year 2099, and the layout a synthetic FAT32 card's.
//
// SQLite always runs (temp file). MySQL runs when MYSQL_TEST_HOST is set,
// PostgreSQL when PGHOST is; each skips cleanly otherwise.
//
// SAFETY: never DROPs or TRUNCATEs. Every row is namespaced to a per-process
// device_id and deleted in TearDown.
//
#include <gtest/gtest.h>

#include "services/CpapCardZip.h"
#include "sleepbridge/LearnedCardTestUtil.h"

#include "database/IDatabase.h"
#include "database/SQLiteDatabase.h"
#include "database/SqlDialect.h"
#include "services/CardUpload.h"
#include "services/CpapCard.h"
#include "services/PrismaIngestion.h"
#include "services/RemovedNights.h"
#include "services/SefamIngestion.h"
#include "sleepbridge/LearnedCard.h"
#ifdef WITH_MYSQL
#include "database/MySQLDatabase.h"
#endif
#ifdef WITH_POSTGRESQL
#include "database/PostgresDatabase.h"
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
namespace fs = std::filesystem;

namespace {

std::string envOr(const char* key, const std::string& fallback) {
    const char* v = std::getenv(key);
    return (v && *v) ? std::string(v) : fallback;
}

/// A Sefam session manifest the discovery accepts. CRLF, as the device writes.
std::string manifest(int year, int month, int day, int hour, int min) {
    std::string s;
    auto line = [&s](const std::string& t) { s += t + "\r\n"; };
    line("[Create Info]");
    line("Created By=S.Box_AUTO ");
    line("Serial Number=1263R00000000");
    line("Version=VER :A020400");
    line("Date=10/11/25 22:25:16");
    line("[Start Record]");
    line("Hour=" + std::to_string(hour));
    line("Min=" + std::to_string(min));
    line("Sec=0");
    line("Day=" + std::to_string(day));
    line("Month=" + std::to_string(month));
    line("Year=" + std::to_string(year));
    line("Programmed Record Duration=28800");
    line("Real Record Duration=28800");
    line("[Chan0]");
    line("Name=FLW");
    line("Description=NO");
    line("Type=4");
    line("Unit=lpm");
    line("Min=-180");
    line("Max=280");
    line("Freq=25");
    line("Bit=8");
    return s;
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

class CpapCardBackendTest : public ::testing::TestWithParam<Engine> {
protected:
    void SetUp() override {
        device_ = "cpapcard_test_" + std::to_string(::getpid());
        dir_    = fs::temp_directory_path() / ("hms_cpapcard_be_" + std::to_string(::getpid()) + "_" +
                                            engineName(GetParam()));
        fs::remove_all(dir_);
        fs::create_directories(dir_);
        switch (GetParam()) {
            case Engine::SQLite: {
                path_ = (dir_ / "cpap.db").string();
                auto lite = std::make_shared<SQLiteDatabase>(path_);
                ASSERT_TRUE(lite->connect()) << "SQLite connect failed";
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
            const auto p = sql::param(1, db_->dbType());
            db_->executeQuery("DELETE FROM cpap_session_files WHERE session_id IN "
                              "(SELECT id FROM cpap_sessions WHERE device_id = " + p + ")",
                              {device_});
            for (const char* t : {"cpap_sessions", "cpap_daily_summary", "cpap_summaries",
                                  "cpap_removed_nights"})
                db_->executeQuery(std::string("DELETE FROM ") + t + " WHERE device_id = " + p,
                                  {device_});
        }
        db_.reset();
        std::error_code ec;
        fs::remove_all(dir_, ec);
    }

    /// The .cpapcard: three S.Box sessions in 2099, and the card's layout.
    std::string writeCpapcard() {
        const std::string path = (dir_ / "00000000-20990612.cpapcard").string();
        EXPECT_TRUE(cpapcardtest::writeZip(
            path, {{"1263R/0/DATA_1/DATA_1.INI", manifest(2099, 6, 9, 22, 25)},
                   {"1263R/0/DATA_2/DATA_2.INI", manifest(2099, 6, 10, 22, 30)},
                   {"1263R/0/DATA_3/DATA_3.INI", manifest(2099, 6, 11, 22, 35)},
                   {"baseline.cbl1", lctest::card().cbl1(), true}}));
        return path;
    }

    std::string device_;
    fs::path dir_;
    std::string path_;
    std::shared_ptr<IDatabase> db_;
};

}  // namespace

TEST_P(CpapCardBackendTest, TheNightsGoInAsACardZipsDoAndTheLayoutStaysOut) {
    const char* eng = engineName(GetParam());
    const std::string file = writeCpapcard();

    // The route: the layout, read and checked before the card is queued.
    std::string why;
    const auto cbl1 = readCardLayout(file, &why);
    ASSERT_TRUE(cbl1) << eng << ": " << why;
    ASSERT_TRUE(sleepbridge::decodeLearnedCard(*cbl1, &why)) << eng << ": " << why;

    // What the database holds before: night 10 stored, night 11 removed.
    {
        const fs::path pre = dir_ / "pre";
        ASSERT_TRUE(PrismaIngestion::extractZip(file, pre.string()));
        SefamIngestion s(pre.string());
        ASSERT_TRUE(s.initialize());
        const auto sessions = s.discoverSessions(std::nullopt);
        ASSERT_EQ(sessions.size(), 3u) << eng;
        CPAPSession stored;
        stored.device_id        = device_;
        stored.session_start    = sessions[1].session_start;
        stored.duration_seconds = 60;
        ASSERT_TRUE(db_->saveSession(stored)) << eng;
    }
    ASSERT_TRUE(db_->removeNight(device_, "20990611").ok) << eng;
    ASSERT_EQ(db_->removedNights(device_).size(), 1u) << eng;

    // The importer: extract, take the layout out, classify, restore, import.
    const fs::path staging = dir_ / "staging";
    ASSERT_TRUE(PrismaIngestion::extractZip(file, staging.string()));
    EXPECT_EQ(removeCardLayout(staging.string()), 1);
    EXPECT_FALSE(fs::exists(staging / "baseline.cbl1"));
    ASSERT_EQ(classifyUploadedCard(staging.string()), UploadedCard::Sefam) << eng;

    const auto nights = cardNights(UploadedCard::Sefam, staging.string());
    EXPECT_EQ(nights.size(), 3u) << eng;
    const auto restored = restoreUploadedNights(*db_, device_, nights);
    ASSERT_EQ(restored.size(), 1u) << eng;
    EXPECT_EQ(restored[0], "2099-06-11") << eng;
    EXPECT_TRUE(db_->removedNights(device_).empty()) << eng;

    const auto c = importCardSessions(*db_, UploadedCard::Sefam, staging.string(), device_, "S.Box");
    EXPECT_EQ(c.found, 3) << eng;
    EXPECT_EQ(c.already_stored, 1) << eng;
    EXPECT_EQ(c.removed, 0) << eng;    // restored first, so not skipped as removed
    EXPECT_EQ(c.refused, 2) << eng;    // manifests without channel data
    EXPECT_EQ(c.imported, 0) << eng;
}

INSTANTIATE_TEST_SUITE_P(Engines, CpapCardBackendTest,
                         ::testing::Values(Engine::SQLite, Engine::MySQL, Engine::Postgres),
                         [](const auto& info) { return engineName(info.param); });
