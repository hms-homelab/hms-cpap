//
// test_SessionSource.cpp: SDD-077, every file knows where it came from, and
// the card wins.
//
// Real nights, real parser, real database: the EDFs are what As11NightWriter
// writes from a made-up stream (the same card-shaped files a card holds), the
// backfill and the collector run as they do on an install, against each
// engine. SQLite always runs; MySQL runs with MYSQL_TEST_HOST set, PostgreSQL
// with a reachable PG* server, and each skips cleanly otherwise.
//
// SAFETY: never DROPs or TRUNCATEs a server's table. Every row is under a
// per-process device_id and removed in TearDown.
//
#include <gtest/gtest.h>
#include "PosixCompat.h"

#include "clients/LocalDataSource.h"
#include "database/IDatabase.h"
#include "database/SQLiteDatabase.h"
#include "database/SqlDialect.h"
#include "utils/TimeCompat.h"
#ifdef WITH_MYSQL
#include "database/MySQLDatabase.h"
#endif
#ifdef WITH_POSTGRESQL
#include "database/PostgresDatabase.h"
#include <pqxx/pqxx>
#endif
#include "services/As11NightImport.h"
#include "services/As11NightWriter.h"
#include "services/BackfillService.h"
#include "services/BurstCollectorService.h"
#include "services/DataPublisherService.h"
#include "services/SessionDiscoveryService.h"
#include "utils/CheckpointSizes.h"
#include "utils/NightQuiet.h"
#include "mqtt_client.h"

#include <chrono>
#include <cstdlib>
#include <ctime>
#include <filesystem>
#include <iomanip>
#include <memory>
#include <set>
#include <sstream>
#include <string>
#include <thread>
#include <vector>
#ifndef _WIN32
#include <unistd.h>
#else
#include <process.h>
#endif

using namespace hms_cpap;
using namespace hms_cpap::as11;
namespace fs = std::filesystem;
using std::chrono::seconds;
using std::chrono::system_clock;

namespace {

constexpr const char* kSerial = "23000000077";
// 2026-10-17 03:00:00 UTC: 22:00 on the 16th at UTC-5, the night of the 16th.
constexpr int64_t kNight16 = 1792206000000LL;

std::string envOr(const char* key, const std::string& fallback) {
    const char* v = std::getenv(key);
    return (v && *v) ? std::string(v) : fallback;
}

StreamBlock second(int64_t start) {
    StreamBlock b;
    b.start_ms = start;
    b.serial = kSerial;
    b.channels.push_back({kFlow, 0, std::vector<int16_t>(25, 300)});
    b.channels.push_back({kMaskPressure, 0, std::vector<int16_t>(25, 900)});
    return b;
}

std::optional<system_clock::time_point> wallFrom(const std::string& text) {
    if (text.size() < 19) return std::nullopt;
    std::tm tm{};
    std::istringstream in(text.substr(0, 19));
    in >> std::get_time(&tm, "%Y-%m-%d %H:%M:%S");
    if (in.fail()) return std::nullopt;
    return system_clock::from_time_t(timegm_utc(&tm));   // a label (SDD-074 D4)
}

/// "20261016_220000_BRP.edf" moved [by] seconds: "20261016_220003_BRP.edf".
std::string shiftedName(const std::string& name, int by) {
    std::tm tm{};
    std::istringstream in(name.substr(0, 15));
    in >> std::get_time(&tm, "%Y%m%d_%H%M%S");
    tm.tm_isdst = -1;
    std::time_t t = std::mktime(&tm) + by;
    std::tm out{};
#ifdef _WIN32
    localtime_s(&out, &t);
#else
    localtime_r(&t, &out);
#endif
    std::ostringstream s;
    s << std::put_time(&out, "%Y%m%d_%H%M%S") << name.substr(15);
    return s.str();
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

#ifdef WITH_POSTGRESQL
std::string pgConnInfo() {
    return "host=" + envOr("PGHOST", "localhost") + " port=" + envOr("PGPORT", "5432") +
           " user=" + envOr("PGUSER", "maestro") + " password=" + envOr("PGPASSWORD", "") +
           " dbname=" + envOr("PGDATABASE", "cpap_monitoring") + " connect_timeout=3";
}
#endif

class SessionSourceTest : public ::testing::TestWithParam<Engine> {
protected:
    fs::path dir_;    // the data dir: as11/ is written here
    fs::path card_;   // a card root: DATALOG/<night>/
    std::string path_;
    std::string device_;
    std::shared_ptr<IDatabase> db_;
    std::string skip_;

    /// A new connection to the same database, as a restart makes one.
    std::shared_ptr<IDatabase> open() {
        switch (GetParam()) {
            case Engine::SQLite: {
                auto lite = std::make_shared<SQLiteDatabase>(path_);
                if (!lite->connect()) return nullptr;
                return lite;
            }
            case Engine::MySQL: {
#ifndef WITH_MYSQL
                skip_ = "built without MySQL (-DBUILD_WITH_MYSQL=OFF)";
                return nullptr;
#else
                const std::string host = envOr("MYSQL_TEST_HOST", "");
                if (host.empty()) {
                    skip_ = "MYSQL_TEST_HOST unset";
                    return nullptr;
                }
                auto my = std::make_shared<MySQLDatabase>(
                    host, std::stoi(envOr("MYSQL_TEST_PORT", "3306")), envOr("MYSQL_TEST_USER", ""),
                    envOr("MYSQL_TEST_PASSWORD", ""), envOr("MYSQL_TEST_DB", "hms_cpap_test"));
                if (!my->connect()) {
                    skip_ = "No usable MySQL at " + host;
                    return nullptr;
                }
                return my;
#endif
            }
            case Engine::Postgres: {
#ifndef WITH_POSTGRESQL
                skip_ = "built without PostgreSQL";
                return nullptr;
#else
                try {
                    pqxx::connection probe(pgConnInfo());
                    if (!probe.is_open()) throw std::runtime_error("not open");
                } catch (const std::exception& e) {
                    skip_ = std::string("No usable PostgreSQL (") + e.what() + ")";
                    return nullptr;
                }
                auto pg = std::make_shared<PostgresDatabase>(pgConnInfo());
                if (!pg->connect()) {
                    skip_ = "PostgresDatabase connect failed";
                    return nullptr;
                }
                return pg;
#endif
            }
        }
        return nullptr;
    }

    void SetUp() override {
        const std::string tag = std::to_string(::getpid()) + "_" +
                                std::to_string(std::chrono::steady_clock::now().time_since_epoch().count());
        dir_ = fs::temp_directory_path() / ("sdd077_" + tag);
        card_ = dir_ / "card";
        fs::create_directories(card_ / "DATALOG");
        path_ = (dir_ / "test.db").string();
        device_ = std::string("sdd077_") + engineName(GetParam()) + "_" + std::to_string(::getpid());
        db_ = open();
        if (!db_) GTEST_SKIP() << skip_;

        setenv("HMS_CPAP_DATA_DIR", dir_.string().c_str(), 1);
        setenv("CPAP_TEMP_DIR", (dir_ / "tmp").string().c_str(), 1);
        setenv("CPAP_ARCHIVE_DIR", (dir_ / "archive").string().c_str(), 1);
        setenv("CPAP_DEVICE_ID", device_.c_str(), 1);
    }

    void TearDown() override {
        if (db_) {
            const DbType dt = db_->dbType();
            // The night's children with it, as each engine removes a night.
            std::set<std::string> folders = {"20261015", "20261016", "20261017"};
            std::error_code ec;
            for (const auto& root : {card_ / "DATALOG", dir_ / "as11" / kSerial / "DATALOG"})
                for (fs::directory_iterator it(root, ec), end; !ec && it != end; it.increment(ec))
                    folders.insert(it->path().filename().string());
            for (const auto& f : folders) db_->deleteSessionsByDateFolder(device_, f);
            for (const auto& r : rows()) As11NightImport::deleteSessionAt(*db_, device_, r.start);
            for (const char* t : {"cpap_daily_summary", "cpap_night_quiet", "cpap_folder_listing",
                                  "cpap_machine_clock", "cpap_devices"})
                db_->executeQuery(std::string("DELETE FROM ") + t + " WHERE device_id = " +
                                      sql::param(1, dt),
                                  {device_});
            db_.reset();
        }
        unsetenv("HMS_CPAP_DATA_DIR");
        unsetenv("CPAP_TEMP_DIR");
        unsetenv("CPAP_ARCHIVE_DIR");
        unsetenv("CPAP_DEVICE_ID");
        std::error_code ec;
        fs::remove_all(dir_, ec);
    }

    /// A night of [secs] seconds streamed from [t0] and completed, laid out
    /// under <data dir>/as11/<serial>/DATALOG/<night>/ as a card would be.
    void stream(int64_t t0, int secs = 120) {
        As11NightWriter w(dir_.string(), fixedOffsetClock(-300));
        for (int i = 0; i < secs; i++) w.onBlock(second(t0 + i * 1000LL));
        w.onStreamEnd(kSerial);
    }

    fs::path as11Night(const std::string& night) const {
        return dir_ / "as11" / kSerial / "DATALOG" / night;
    }

    /// Two nights streamed in the last two days, oldest first, by folder.
    /// Recent, so every engine's collector looks at them again: only SQLite
    /// implements getNthLatestSessionStart (SDD-010), and without it the
    /// collector re-reads a stored night only inside 48 hours.
    std::vector<std::string> streamTwoRecentNights() {
        const int64_t now_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                                   system_clock::now().time_since_epoch()).count();
        stream(now_ms - 40LL * 3600 * 1000);
        stream(now_ms - 16LL * 3600 * 1000);
        std::vector<std::string> nights;
        for (const auto& e : fs::directory_iterator(dir_ / "as11" / kSerial / "DATALOG"))
            nights.push_back(e.path().filename().string());
        std::sort(nights.begin(), nights.end());
        return nights;
    }

    static std::string dashed(const std::string& d) {
        return d.substr(0, 4) + "-" + d.substr(4, 2) + "-" + d.substr(6, 2);
    }

    /// The card's copy of a streamed night: the same files, named [shift]
    /// seconds later, in the card's folder [card_night].
    void copyToCard(const std::string& night, int shift = 0, std::string card_night = "") {
        if (card_night.empty()) card_night = night;
        const fs::path to = card_ / "DATALOG" / card_night;
        fs::create_directories(to);
        for (const auto& e : fs::directory_iterator(as11Night(night))) {
            const std::string name = e.path().filename().string();
            fs::copy_file(e.path(), to / (shift ? shiftedName(name, shift) : name),
                          fs::copy_options::overwrite_existing);
        }
    }

    /// The SD / ZIP import: a ZIP is mirrored into a card root and then runs
    /// exactly this, BackfillService over the dates it holds.
    void backfill(const std::string& from, const std::string& to) {
        BackfillService::Config cfg;
        cfg.local_dir = card_.string();
        cfg.device_id = device_;
        cfg.device_name = "AirSense 11";
        BackfillService svc(cfg, db_);
        svc.start();
        svc.trigger(from, to);
        for (int i = 0; i < 100; ++i) {
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
            const auto st = svc.getStatus()["status"].asString();
            if (st != "running" && st != "idle") break;
        }
        svc.stop();
        ASSERT_EQ(svc.getStatus()["status"].asString(), "complete");
    }

    As11NightImport importer() { return As11NightImport(dir_.string(), db_, device_, "AirSense 11"); }

    /// The collector over the card root read in place, the local-folder source.
    std::unique_ptr<BurstCollectorService> collector() {
        auto src = std::make_unique<LocalDataSource>(card_.string());
        auto discovery = std::make_unique<SessionDiscoveryService>(*src);
        auto publisher = std::make_unique<DataPublisherService>(std::shared_ptr<hms::MqttClient>{}, db_);
        auto svc = std::make_unique<BurstCollectorService>(60);
        svc->injectDependenciesForTest(db_, std::move(src), std::move(publisher), std::move(discovery));
        svc->setSourceForTest("local");
        svc->setLocalDirForTest(card_.string());
        svc->markStrReadForTest();
        return svc;
    }

    struct Row {
        system_clock::time_point start;
        std::string finish;   // "" = open
    };
    std::vector<Row> rows() {
        const DbType dt = db_->dbType();
        std::vector<Row> out;
        for (const auto& r : db_->executeQuery(
                 "SELECT " + sql::tsText("session_start", dt) + " AS start, " +
                     sql::tsText("session_end", dt) +
                     " AS finish FROM cpap_sessions WHERE device_id = " + sql::param(1, dt) +
                     " ORDER BY session_start",
                 {device_})) {
            const auto s = wallFrom(r["start"].asString());
            if (!s) continue;
            out.push_back({*s, r["finish"].isNull() ? "" : r["finish"].asString().substr(0, 19)});
        }
        return out;
    }

    /// The sources of every file of the session at [start].
    std::vector<std::string> sourcesAt(system_clock::time_point start) {
        const DbType dt = db_->dbType();
        const std::time_t t = system_clock::to_time_t(start);
        std::tm tm{};
        gmtime_r(&t, &tm);   // starts are labels (SDD-074 D4)
        std::ostringstream wall;
        wall << std::put_time(&tm, "%Y-%m-%d %H:%M:%S");
        std::vector<std::string> out;
        for (const auto& r : db_->executeQuery(
                 "SELECT f.source FROM cpap_session_files f JOIN cpap_sessions s ON s.id = f.session_id"
                 " WHERE s.device_id = " + sql::param(1, dt) +
                     " AND s.session_start = " + sql::castTimestamp(2, dt),
                 {device_, wall.str()}))
            out.push_back(r["source"].isNull() ? "NULL" : r["source"].asString());
        return out;
    }
};

}  // namespace

// §3.1: a completed night imported by the SD/ZIP backfill, then the collector
// over the same unchanged files: no change, nothing stored again, session_end
// kept. The backfill used to store KB and the collector compared bytes, so
// every imported night was downloaded again and reopened (#85).
TEST_P(SessionSourceTest, AnImportedNightIsUnchangedToTheCollector) {
    const auto nights = streamTwoRecentNights();
    ASSERT_EQ(nights.size(), 2u);
    for (const auto& n : nights) copyToCard(n);
    backfill(dashed(nights[0]), dashed(nights[1]));
    const auto before = rows();
    ASSERT_EQ(before.size(), 2u) << engineName(GetParam());
    for (const auto& r : before) ASSERT_FALSE(r.finish.empty()) << "the backfill closes what it imports";
    const auto stored = db_->getCheckpointFileSizes(device_, before[1].start);
    ASSERT_FALSE(stored.empty());

    auto svc = collector();
    testing::internal::CaptureStdout();
    svc->runBurstCycleForTest();
    const std::string out = testing::internal::GetCapturedStdout();

    // The collector looked at both nights, and found both unchanged.
    std::size_t unchanged = 0;
    for (auto at = out.find("stopped (all checkpoint files unchanged)"); at != std::string::npos;
         at = out.find("stopped (all checkpoint files unchanged)", at + 1))
        ++unchanged;
    EXPECT_EQ(unchanged, 2u) << out;

    const auto after = rows();
    ASSERT_EQ(after.size(), 2u);
    for (std::size_t i = 0; i < after.size(); ++i)
        EXPECT_EQ(after[i].finish, before[i].finish) << "session_end moved for night " << i;
    EXPECT_EQ(db_->getCheckpointFileSizes(device_, before[1].start), stored)
        << "an unchanged night was stored again";
}

// §3.2 and §3.3 on a real database: the current night (the newest folder on
// the source) that grew reopens; an older night that grew is read again and
// stays completed at its data end (D5).
TEST_P(SessionSourceTest, OnlyTheCurrentNightReopens) {
    const auto nights = streamTwoRecentNights();
    ASSERT_EQ(nights.size(), 2u);
    for (const auto& n : nights) copyToCard(n);
    backfill(dashed(nights[0]), dashed(nights[1]));
    const auto before = rows();
    ASSERT_EQ(before.size(), 2u);

    // Both nights' files read as grown: their stored BRP is a byte short.
    for (const auto& r : before) {
        auto sizes = db_->getCheckpointFileSizes(device_, r.start);
        for (auto& [name, size] : sizes)
            if (name.find("_BRP.edf") != std::string::npos) size -= 1;
        ASSERT_TRUE(db_->updateCheckpointFileSizes(device_, r.start, sizes));
    }

    auto svc = collector();
    testing::internal::CaptureStdout();
    svc->runBurstCycleForTest();
    const std::string out = testing::internal::GetCapturedStdout();
    EXPECT_NE(out.find("stored and kept completed"), std::string::npos) << out;

    const auto after = rows();
    ASSERT_EQ(after.size(), 2u);
    EXPECT_FALSE(after[0].finish.empty()) << "the older night was reopened ("
                                          << engineName(GetParam()) << ")";
    EXPECT_EQ(after[0].finish, before[0].finish) << "an older night closes at its data end";
    EXPECT_TRUE(after[1].finish.empty()) << "the current night grew and must read live";
    // D5: no quiet-hour restart for the older night.
    EXPECT_FALSE(nightIsOver(*db_, device_, nights[1], system_clock::now()))
        << "the current night grew just now";
    const auto old_quiet = db_->executeQuery(
        "SELECT last_growth FROM cpap_night_quiet WHERE device_id = " + sql::param(1, db_->dbType()) +
            " AND date_folder = " + sql::param(2, db_->dbType()),
        {device_, nights[0]});
    EXPECT_TRUE(old_quiet.empty()) << "the older night's hour was restarted";
}

// §3.6: a Bluetooth night, then the card's copy by ZIP with its start a few
// seconds off: one session remains, every file of it the card's (D2).
TEST_P(SessionSourceTest, TheCardsCopyByZipReplacesTheBluetoothNight) {
    stream(kNight16);
    ASSERT_EQ(importer().run({}).imported, 1);
    const auto bt = rows();
    ASSERT_EQ(bt.size(), 1u);
    for (const auto& s : sourcesAt(bt[0].start)) EXPECT_EQ(s, kSourceBleAs11);

    copyToCard("20261016", 3);
    backfill("2026-10-16", "2026-10-16");

    const auto after = rows();
    ASSERT_EQ(after.size(), 1u) << "both copies of one night are stored (" << engineName(GetParam()) << ")";
    EXPECT_EQ(after[0].start, bt[0].start + seconds(3)) << "the card's row is the one left";
    const auto sources = sourcesAt(after[0].start);
    ASSERT_FALSE(sources.empty());
    for (const auto& s : sources) EXPECT_EQ(s, kSourceCard);
}

// D2 itself, not the backfill's delete by folder: the stream filed its night
// under the day before (a night the machine cut at its noon boundary), so only
// the overlap says the two rows are one stretch.
TEST_P(SessionSourceTest, TheCardWinsAStretchFiledUnderAnotherNight) {
    stream(kNight16);
    fs::rename(as11Night("20261016"), as11Night("20261015"));
    ASSERT_EQ(importer().run({}).imported, 1);
    const auto bt = rows();
    ASSERT_EQ(bt.size(), 1u);

    copyToCard("20261015", 3, "20261016");
    backfill("2026-10-16", "2026-10-16");

    const auto after = rows();
    ASSERT_EQ(after.size(), 1u) << engineName(GetParam());
    EXPECT_EQ(after[0].start, bt[0].start + seconds(3));
    for (const auto& s : sourcesAt(after[0].start)) EXPECT_EQ(s, kSourceCard);
}

// §3.7: the card first, then the Bluetooth import of the same night: the card
// row is untouched, even with the very same start.
TEST_P(SessionSourceTest, ABluetoothImportNeverTouchesTheCardsRow) {
    stream(kNight16);
    copyToCard("20261016");
    backfill("2026-10-16", "2026-10-16");
    const auto card = rows();
    ASSERT_EQ(card.size(), 1u);
    const auto card_sources = sourcesAt(card[0].start);

    const auto r = importer().run({});
    EXPECT_EQ(r.imported, 0);
    EXPECT_EQ(r.card_wins, 1);

    const auto after = rows();
    ASSERT_EQ(after.size(), 1u) << "the card's row was deleted (" << engineName(GetParam()) << ")";
    EXPECT_EQ(after[0].start, card[0].start);
    EXPECT_EQ(after[0].finish, card[0].finish);
    EXPECT_EQ(sourcesAt(after[0].start), card_sources);
    for (const auto& s : card_sources) EXPECT_EQ(s, kSourceCard);
}

// §3.8: a file row that predates the column is filled from its path, once.
TEST_P(SessionSourceTest, TheMigrationFillsTheSourceFromThePath) {
    const DbType dt = db_->dbType();
    const auto t0 = system_clock::from_time_t(1792206000);
    struct { std::chrono::seconds at; std::vector<SessionFileRef> refs; } nights[] = {
        {seconds(0), {{"brp", "DATALOG/20261016/20261016_220000_BRP.edf"}}},
        {seconds(3600), {{"brp", "as11/23000000077/DATALOG/20261016/20261016_230000_BRP.edf"}}},
        {seconds(7200), {{"b01", "ds2/P0000000000/20261017/0001.B01"}}},
    };
    for (const auto& n : nights) {
        CPAPSession s;
        s.device_id = device_;
        s.device_name = "test";
        s.session_start = t0 + n.at;
        s.duration_seconds = 60;
        s.brp_file_path = n.refs[0].rel_path;
        ASSERT_TRUE(db_->saveSession(s));
        ASSERT_TRUE(db_->replaceSessionFiles(device_, t0 + n.at, n.refs));
    }
    // The rows as an install before SDD-077 holds them: no source.
    db_->executeQuery("UPDATE cpap_session_files SET source = NULL WHERE session_id IN"
                      " (SELECT id FROM cpap_sessions WHERE device_id = " + sql::param(1, dt) + ")",
                      {device_});
    EXPECT_EQ(sourcesAt(t0), std::vector<std::string>{"NULL"});

    // A restart runs the migration.
    db_ = open();
    ASSERT_TRUE(db_);
    EXPECT_EQ(sourcesAt(t0), std::vector<std::string>{kSourceCard});
    EXPECT_EQ(sourcesAt(t0 + seconds(3600)), std::vector<std::string>{kSourceBleAs11});
    EXPECT_EQ(sourcesAt(t0 + seconds(7200)), std::vector<std::string>{kSourceBleDs2});

    // From now on the source decides, not the path: a card file is a card
    // file wherever its path says it lives.
    ASSERT_TRUE(db_->replaceSessionFiles(
        device_, t0 + seconds(3600),
        {{"brp", "as11/23000000077/DATALOG/20261016/20261016_230000_BRP.edf", kSourceCard}}));
    db_ = open();
    EXPECT_EQ(sourcesAt(t0 + seconds(3600)), std::vector<std::string>{kSourceCard});
    const auto files = db_->getSessionFilesForDateFolder(device_, "20261016");
    ASSERT_EQ(files.size(), 2u);
    for (const auto& f : files) EXPECT_EQ(f.source, kSourceCard) << f.rel_path;
}

INSTANTIATE_TEST_SUITE_P(Engines, SessionSourceTest,
                         ::testing::Values(Engine::SQLite, Engine::MySQL, Engine::Postgres),
                         [](const ::testing::TestParamInfo<Engine>& i) { return engineName(i.param); });

// §3.8 on SQLite from a database file made before SDD-077, without the column.
TEST(SessionSourceMigration, AnOldSQLiteFileGainsTheColumnFilledFromThePath) {
    const fs::path dir = fs::temp_directory_path() / ("sdd077_old_" + std::to_string(::getpid()));
    fs::remove_all(dir);
    fs::create_directories(dir);
    const std::string path = (dir / "old.db").string();
    const auto t0 = system_clock::from_time_t(1792206000);
    {
        SQLiteDatabase db(path);
        ASSERT_TRUE(db.connect());
        for (int i = 0; i < 3; ++i) {
            CPAPSession s;
            s.device_id = "old";
            s.device_name = "test";
            s.session_start = t0 + seconds(3600 * i);
            s.duration_seconds = 60;
            ASSERT_TRUE(db.saveSession(s));
        }
        ASSERT_TRUE(db.executeQueryChecked("ALTER TABLE cpap_session_files DROP COLUMN source", {}).ok);
        const auto ids = db.executeQuery("SELECT id FROM cpap_sessions ORDER BY session_start", {});
        ASSERT_EQ(ids.size(), 3u);
        const char* paths[] = {"DATALOG/20261016/20261016_220000_BRP.edf",
                               "as11/23000000077/DATALOG/20261016/20261016_230000_BRP.edf",
                               "ds2/P0000000000/20261017/0001.B01"};
        for (int i = 0; i < 3; ++i)
            ASSERT_TRUE(db.executeQueryChecked(
                              "INSERT INTO cpap_session_files (session_id, kind, rel_path) VALUES (?, 'brp', ?)",
                              {ids[i]["id"].asString(), paths[i]})
                            .ok);
        db.disconnect();
    }
    SQLiteDatabase db(path);
    ASSERT_TRUE(db.connect());
    const auto rows = db.executeQuery(
        "SELECT f.source FROM cpap_session_files f JOIN cpap_sessions s ON s.id = f.session_id"
        " ORDER BY s.session_start", {});
    ASSERT_EQ(rows.size(), 3u);
    EXPECT_EQ(rows[0]["source"].asString(), kSourceCard);
    EXPECT_EQ(rows[1]["source"].asString(), kSourceBleAs11);
    EXPECT_EQ(rows[2]["source"].asString(), kSourceBleDs2);
    db.disconnect();
    fs::remove_all(dir);
}

// ── D3 and D5, the pure decisions ────────────────────────────────────────

namespace {
SessionFileSet listedWithBytes() {
    SessionFileSet s;
    s.file_sizes_bytes = {{"20261016_220000_BRP.edf", 1645572}, {"20261016_220000_PLD.edf", 151000},
                          {"20261016_220000_SAD.edf", 67000},   {"20261016_220000_SA2.edf", 66500},
                          {"20261016_220000_EVE.edf", 900},     {"20261016_220000_CSL.edf", 300},
                          {"20261016_220000_TCV.edf", 9000}};
    for (const auto& [n, b] : s.file_sizes_bytes) s.file_sizes_kb[n] = static_cast<int>(b / 1024);
    return s;
}
}  // namespace

// §3.5: every checkpoint type, the same from either writer.
TEST(SessionSourceCheckpoint, EveryTypeIsTheSameFromEitherWriter) {
    const auto listed = listedWithBytes();
    const auto written = checkpointSizes(listed);   // what the backfill and the collector store
    EXPECT_EQ(written.size(), 4u) << "BRP, PLD, SAD and SA2; EVE, CSL and TCV are not checkpoints";
    for (const char* n : {"20261016_220000_BRP.edf", "20261016_220000_PLD.edf",
                          "20261016_220000_SAD.edf", "20261016_220000_SA2.edf"})
        EXPECT_EQ(written.at(n), static_cast<int>(listed.file_sizes_bytes.at(n))) << n;
    const auto c = compareCheckpoints(written, listed);
    EXPECT_TRUE(c.all_unchanged);
    EXPECT_FALSE(c.has_new_files);
    EXPECT_FALSE(c.legacy_kb);
}

// §3.5: a row in the old KB shape (the backfill's before SDD-077) reads as
// unchanged once, says it wants rewriting, and once rewritten reads exactly.
TEST(SessionSourceCheckpoint, AKbShapeRowIsUnchangedOnceAndRewrittenInBytes) {
    const auto listed = listedWithBytes();
    std::map<std::string, int> old_row;
    for (const auto& [n, kb] : listed.file_sizes_kb)
        if (isCheckpointFile(n)) old_row[n] = kb;
    const auto first = compareCheckpoints(old_row, listed);
    EXPECT_TRUE(first.all_unchanged);
    EXPECT_TRUE(first.legacy_kb) << "the caller must rewrite the row in bytes";

    const auto rewritten = checkpointSizes(listed);
    const auto second = compareCheckpoints(rewritten, listed);
    EXPECT_TRUE(second.all_unchanged);
    EXPECT_FALSE(second.legacy_kb);

    // A real growth is still a change, in either shape.
    auto grown = listed;
    grown.file_sizes_bytes["20261016_220000_BRP.edf"] += 300;
    EXPECT_FALSE(compareCheckpoints(rewritten, grown).all_unchanged) << "300 bytes of growth";
    grown.file_sizes_bytes["20261016_220000_BRP.edf"] += 4096;
    EXPECT_FALSE(compareCheckpoints(old_row, grown).all_unchanged) << "4 KB of growth";
}

// A source that lists KB only (an ez Share) reading a row the backfill wrote
// in bytes: the KB it lists is all it can see, and it is the same file.
TEST(SessionSourceCheckpoint, AByteRowReadThroughAKbOnlySourceIsUnchanged) {
    const auto listed = listedWithBytes();
    const auto row = checkpointSizes(listed);
    SessionFileSet kb_only;
    kb_only.file_sizes_kb = listed.file_sizes_kb;
    const auto c = compareCheckpoints(row, kb_only);
    EXPECT_TRUE(c.all_unchanged);
    EXPECT_FALSE(c.legacy_kb) << "nothing finer to rewrite it with";
    kb_only.file_sizes_kb["20261016_220000_BRP.edf"] += 2;
    EXPECT_FALSE(compareCheckpoints(row, kb_only).all_unchanged);
}

// §4.1: the current night is the newest folder on the source, never the clock.
TEST(SessionSourceCurrentNight, TheNewestFolderOnTheSourceIsTheCurrentNight) {
    SessionFileSet a, b;
    a.date_folder = "20261015";
    b.date_folder = "20261016";
    EXPECT_EQ(BurstCollectorService::currentNightFolder({"20261001", "20261014"}, {a}), "20261015")
        << "a folder this burst found counts even before the index has it";
    EXPECT_EQ(BurstCollectorService::currentNightFolder({"20261014", "20261020"}, {a, b}), "20261020");
    EXPECT_EQ(BurstCollectorService::currentNightFolder({}, {b, a}), "20261016");
    EXPECT_EQ(BurstCollectorService::currentNightFolder({}, {}), "");
}
