//
// SDD-074 on every engine: the clock setting's storage, the offset a session
// was read with, a live night's end on the machine's clock (D5), the host's
// daylight saving no longer folding a fixed-offset machine's night (section
// 2 E, D4), and the row backup a "Re-read my nights" takes first.
//
// SQLite always runs. MySQL runs when MYSQL_TEST_HOST is set, PostgreSQL when
// PGHOST is; both skip cleanly otherwise. Rows are dated 2026 but namespaced
// to a per-process device id and deleted in TearDown.
//
// Run under TZ=UTC, America/New_York, Australia/Sydney, Pacific/Kiritimati;
// the tests that need a particular host zone set it themselves (HostZone).
//
#include <gtest/gtest.h>

#include "database/IDatabase.h"
#include "database/SQLiteDatabase.h"
#include "database/SqlDialect.h"
#include "utils/MachineClock.h"
#include "utils/MachineClocks.h"
#include "utils/MachineZone.h"
#include "utils/SessionLabels.h"
#include "web/ClockApi.h"
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
#include <set>
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

long long L(int y, int mo, int d, int h, int mi = 0, int s = 0) {
    std::tm t{};
    t.tm_year = y - 1900;
    t.tm_mon = mo - 1;
    t.tm_mday = d;
    t.tm_hour = h;
    t.tm_min = mi;
    t.tm_sec = s;
    return wall::labelOf(t);
}

/// The host's zone for one test, put back after.
///
/// The Windows C runtime does not read IANA names from TZ: it takes the first
/// three letters as the zone's name and the rest as an offset, so
/// "Australia/Sydney" is a host on UTC with no daylight saving. It reads only
/// "EST5EDT"-shaped values, and any daylight saving in them follows the United
/// States rules. New York is given in that shape there (the same rules);
/// a test that needs another zone's daylight saving picks its night per
/// platform.
class HostZone {
public:
    explicit HostZone(const char* tz) {
        const char* old = std::getenv("TZ");
        had_ = old != nullptr;
        if (had_) old_ = old;
        set(tz);
    }
    ~HostZone() { had_ ? set(old_.c_str()) : unset(); }

private:
    static void set(const char* tz) {
#ifdef _WIN32
        if (std::string(tz) == "America/New_York") tz = "EST5EDT";
        _putenv_s("TZ", tz);
        _tzset();
#else
        setenv("TZ", tz, 1);
        tzset();
#endif
    }
    static void unset() {
#ifdef _WIN32
        _putenv_s("TZ", "");
        _tzset();
#else
        unsetenv("TZ");
        tzset();
#endif
    }
    bool had_{false};
    std::string old_;
};

enum class Engine { SQLite, MySQL, Postgres };

const char* engineName(Engine e) {
    switch (e) {
        case Engine::SQLite:   return "SQLite";
        case Engine::MySQL:    return "MySQL";
        case Engine::Postgres: return "Postgres";
    }
    return "?";
}

class MachineClockBackendTest : public ::testing::TestWithParam<Engine> {
protected:
    void SetUp() override {
        device_ = "clock_test_" + std::to_string(::getpid());
        machine_clocks::clear();
        switch (GetParam()) {
            case Engine::SQLite: {
                path_ = (fs::temp_directory_path() /
                         ("hms_cpap_clock_" + std::to_string(::getpid()) + ".db")).string();
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
        machine_clocks::clear();
        if (db_) {
            const auto p = sql::param(1, db_->dbType());
            for (const auto* t : {"cpap_breathing_summary", "cpap_events", "cpap_session_metrics",
                                  "cpap_session_files", "cpap_vitals"})
                db_->executeQuery(std::string("DELETE FROM ") + t +
                                      " WHERE session_id IN (SELECT id FROM cpap_sessions WHERE device_id = " +
                                      p + ")",
                                  {device_});
            db_->executeQuery("DELETE FROM cpap_sessions WHERE device_id = " + p, {device_});
            db_->executeQuery("DELETE FROM cpap_daily_summary WHERE device_id = " + p, {device_});
            db_->executeQuery("DELETE FROM cpap_machine_clock WHERE device_id = " + p, {device_});
            for (const auto& t : backups_) db_->executeQuery("DROP TABLE " + t, {});
        }
        db_.reset();
        if (!path_.empty()) {
            for (const auto* suffix : {"", "-wal", "-shm"}) {
                std::error_code ec;
                fs::remove(path_ + suffix, ec);
            }
        }
    }

    CPAPSession session(long long start_label, int secs) {
        CPAPSession s;
        s.device_id = device_;
        s.device_name = "AirSense 11";
        s.serial_number = "SDD074";
        s.session_start = wall::point(start_label);
        s.duration_seconds = secs;
        s.data_records = secs / 60;
        return s;
    }

    Json::Value one(const std::string& sql) {
        auto rows = db_->executeQuery(sql, {device_});
        return rows.empty() ? Json::Value() : rows[0];
    }

    std::string p1() const { return sql::param(1, db_->dbType()); }

    std::shared_ptr<IDatabase> db_;
    std::string device_;
    std::string path_;
    std::vector<std::string> backups_;
};

}  // namespace

// ── storage ──────────────────────────────────────────────────────────────

TEST_P(MachineClockBackendTest, ClockSettingIsAddedToATableFromBeforeEmptyForEveryRow) {
    // The table as SDD-054 made it, with a learned row in it.
    db_->executeQuery("DROP TABLE IF EXISTS cpap_machine_clock", {});
    db_->executeQuery(
        "CREATE TABLE cpap_machine_clock (device_id VARCHAR(128) NOT NULL PRIMARY KEY,"
        " drift_s BIGINT NOT NULL DEFAULT 0, zone_s BIGINT NOT NULL DEFAULT 0,"
        " zone_known INTEGER NOT NULL DEFAULT 0, updated_at BIGINT NOT NULL DEFAULT 0)",
        {});
    MachineClock learned;
    learned.zone_s = 36000;
    learned.zone_known = true;
    // saveMachineClock runs ensureTable, which adds the column.
    ASSERT_TRUE(saveMachineClock(*db_, device_, learned, 1000)) << engineName(GetParam());
    EXPECT_TRUE(loadClockSettings(*db_).count(device_) == 0) << "empty for the existing row";
    const auto row = one("SELECT clock_setting FROM cpap_machine_clock WHERE device_id = " + p1());
    EXPECT_EQ(row["clock_setting"].asString(), "") << engineName(GetParam());
    // Issue #95: and where it was learned, on every engine.
    const auto from = one("SELECT learned_from, learned_night, learned_nights FROM cpap_machine_clock "
                          "WHERE device_id = " + p1());
    EXPECT_EQ(from["learned_from"].asString(), "") << engineName(GetParam());
    EXPECT_EQ(machine_clock::asLong(from["learned_nights"]), 0) << engineName(GetParam());
}

// ── issue #95: the learning's source, and what the Devices page offers ───

TEST_P(MachineClockBackendTest, WhereALearningCameFromIsStoredAndReadBack) {
    MachineClock c;
    c.drift_s = 900;
    c.zone_s = -18900;
    c.zone_known = true;
    c.from = machine_clock::kFromCardStamps;
    c.night = "20261119";
    c.nights = 2;
    ASSERT_TRUE(saveMachineClock(*db_, device_, c, 4242)) << engineName(GetParam());
    const auto back = loadMachineClock(*db_, device_);
    EXPECT_EQ(back.from, "card_stamps");
    EXPECT_EQ(back.night, "20261119");
    EXPECT_EQ(back.nights, 2);
    EXPECT_EQ(learnedOffset(back), -18000);
}

TEST_P(MachineClockBackendTest, ForgettingALearningKeepsTheSetting) {
    MachineClock c;
    c.drift_s = -18000;
    c.zone_known = true;
    ASSERT_TRUE(saveMachineClock(*db_, device_, c, 4242));
    std::string why;
    ASSERT_TRUE(machine_clocks::save(*db_, device_, "America/New_York", &why)) << why;
    EXPECT_TRUE(forgetMachineClockLearning(*db_, device_)) << engineName(GetParam());
    const auto back = loadMachineClock(*db_, device_);
    EXPECT_FALSE(back.zone_known);
    EXPECT_EQ(back.drift_s, 0);
    EXPECT_EQ(machineClockLearnedAt(*db_, device_), 0);
    EXPECT_EQ(loadClockSettings(*db_)[device_], "America/New_York");
    EXPECT_FALSE(forgetMachineClockLearning(*db_, device_)) << "nothing left to forget";
}

TEST_P(MachineClockBackendTest, TheBridgeSplitShowsTheWholeOffsetNoWarningAndNoButton) {
    // What a Sleep Bridge folder taught before the fix: drift -5 h, zone
    // +00:00, source unknown. On an Eastern host it is the host's offset.
    HostZone tz("EST5EDT");
    MachineClock c;
    c.drift_s = -18000;
    c.zone_s = 0;
    c.zone_known = true;
    ASSERT_TRUE(saveMachineClock(*db_, device_, c, 4242));
    const auto v = clockapi::view(*db_, device_, L(2026, 11, 20, 12) + 5 * 3600, true);
    EXPECT_EQ(v["learned"]["text"].asString(), "-05:00") << "the whole offset, never +00:00";
    EXPECT_EQ(v["difference_s"].asInt64(), 0);
    EXPECT_FALSE(v["warn"].asBool());
    EXPECT_FALSE(v["learned"]["offer"].asBool());
    EXPECT_TRUE(v["learned"]["preview"].isNull());
}

TEST_P(MachineClockBackendTest, UseThisIsOfferedOnlyForTheMachinesStampsOnTwoNightsWithAPreview) {
    HostZone tz("EST5EDT");
    // A night that started 22:10 on the machine's clock, read with the host's.
    ASSERT_TRUE(db_->saveSession(session(L(2026, 11, 19, 22, 10), 3600)));
    MachineClock c;
    c.zone_s = 36000;
    c.zone_known = true;
    c.from = machine_clock::kFromCardStamps;
    c.night = "20261118";
    c.nights = 1;
    ASSERT_TRUE(saveMachineClock(*db_, device_, c, 4242));
    const long long now = L(2026, 11, 20, 12) + 5 * 3600;
    auto v = clockapi::view(*db_, device_, now, true);
    EXPECT_TRUE(v["warn"].asBool()) << "+10:00 against the host's -05:00";
    EXPECT_FALSE(v["learned"]["offer"].asBool()) << "one night is not enough";

    c.nights = 2;
    ASSERT_TRUE(saveMachineClock(*db_, device_, c, 4243));
    v = clockapi::view(*db_, device_, now, true);
    ASSERT_TRUE(v["learned"]["offer"].asBool()) << engineName(GetParam());
    EXPECT_EQ(v["learned"]["use"].asString(), "+10:00");
    // 22:10 at -05:00 is 22:10 here; at +10:00 it is 07:10 the morning before.
    EXPECT_EQ(v["learned"]["preview"]["now"].asString(), "22:10") << engineName(GetParam());
    EXPECT_EQ(v["learned"]["preview"]["would_be"].asString(), "07:10") << engineName(GetParam());

    // The same offset learned while recording (a BRP) is never offered.
    c.from = machine_clock::kFromBrpGrowth;
    ASSERT_TRUE(saveMachineClock(*db_, device_, c, 4244));
    EXPECT_FALSE(clockapi::view(*db_, device_, now, true)["learned"]["offer"].asBool());
}

TEST_P(MachineClockBackendTest, ASettingIsStoredReadBackAndKeepsWhatWasLearned) {
    MachineClock learned;
    learned.zone_s = 36000;
    learned.zone_known = true;
    ASSERT_TRUE(saveMachineClock(*db_, device_, learned, 1234));
    std::string why;
    ASSERT_TRUE(machine_clocks::save(*db_, device_, "UTC+10", &why)) << why;
    EXPECT_EQ(loadClockSettings(*db_)[device_], "+10:00");
    // Learning again does not clear the setting, and the setting kept the
    // learning and when it was learned.
    ASSERT_TRUE(saveMachineClock(*db_, device_, learned, 5678));
    EXPECT_EQ(loadClockSettings(*db_)[device_], "+10:00");
    EXPECT_TRUE(loadMachineClock(*db_, device_).zone_known);
    EXPECT_EQ(machineClockLearnedAt(*db_, device_), 5678);

    machine_clocks::clear();
    machine_clocks::loadAll(*db_);
    EXPECT_EQ(machine_clocks::zoneFor(device_).setting(), "+10:00") << "survives a restart";
}

TEST_P(MachineClockBackendTest, AnInvalidSettingIsRefusedAndNothingIsStored) {
    const auto r = clockapi::set(*db_, device_, "+10:07", 0, true);
    EXPECT_EQ(r.status, 400);
    EXPECT_FALSE(r.body["error"].asString().empty());
    EXPECT_EQ(loadClockSettings(*db_).count(device_), 0u);
    EXPECT_TRUE(machine_clocks::zoneFor(device_).isHost());
}

TEST_P(MachineClockBackendTest, TheViewWarnsWhenTheLearnedClockDiffersByAQuarterHour) {
    MachineClock learned;
    learned.zone_s = 36000;
    learned.zone_known = true;
    ASSERT_TRUE(saveMachineClock(*db_, device_, learned, 1234));
    std::string why;
    ASSERT_TRUE(machine_clocks::save(*db_, device_, "+09:45", &why)) << why;
    auto v = clockapi::view(*db_, device_, L(2026, 11, 19, 0), true);
    EXPECT_EQ(v["learned"]["text"].asString(), "+10:00");
    EXPECT_EQ(v["difference_s"].asInt64(), 900);
    EXPECT_TRUE(v["warn"].asBool());
    ASSERT_TRUE(machine_clocks::save(*db_, device_, "+10:00", &why)) << why;
    v = clockapi::view(*db_, device_, L(2026, 11, 19, 0), true);
    EXPECT_FALSE(v["warn"].asBool());
    EXPECT_EQ(v["kind"].asString(), "fixed");
}

TEST_P(MachineClockBackendTest, EachSessionRecordsTheOffsetItWasReadWith) {
    std::string why;
    ASSERT_TRUE(machine_clocks::save(*db_, device_, "+10:00", &why)) << why;
    ASSERT_TRUE(db_->saveSession(session(L(2026, 1, 15, 22), 3600)));
    auto row = one("SELECT machine_offset_s FROM cpap_sessions WHERE device_id = " + p1());
    EXPECT_EQ(std::atoll(row["machine_offset_s"].asString().c_str()), 36000) << engineName(GetParam());

    // Same as this computer: the host's offset at that instant.
    ASSERT_TRUE(machine_clocks::save(*db_, device_, "", &why));
    ASSERT_TRUE(db_->saveSession(session(L(2026, 1, 15, 22), 3600)));
    row = one("SELECT machine_offset_s FROM cpap_sessions WHERE device_id = " + p1());
    const long long host_off = MachineZone::host().offsetAt(MachineZone::host().toInstant(L(2026, 1, 15, 22)));
    EXPECT_EQ(std::atoll(row["machine_offset_s"].asString().c_str()), host_off) << engineName(GetParam());
}

// ── D5: a live night ends on the machine's clock ─────────────────────────

TEST_P(MachineClockBackendTest, ALiveEndIsTheMachinesClockOneHourBehindOrAhead) {
    const long long host_off = MachineZone::host().offsetAt(std::time(nullptr));
    for (long long delta : {-3600LL, 3600LL}) {
        // On a host at UTC+14 there is no hour ahead to set: two behind instead.
        if (host_off + delta > 14 * 3600) delta = -7200;
        if (host_off + delta < -12 * 3600) delta = 7200;
        std::string why;
        ASSERT_TRUE(machine_clocks::save(*db_, device_, MachineZone::offsetText(host_off + delta), &why))
            << why;
        const long long now_wall = machine_clocks::nowWall(device_, std::time(nullptr));
        const long long start = now_wall - 2 * 3600;
        db_->executeQuery("DELETE FROM cpap_sessions WHERE device_id = " + p1(), {device_});
        ASSERT_TRUE(db_->saveSession(session(start, 7200)));
        ASSERT_TRUE(db_->markSessionCompleted(device_, wall::point(start))) << engineName(GetParam());
        const auto row = one("SELECT session_end FROM cpap_sessions WHERE device_id = " + p1());
        const auto end = wall::parseText(row["session_end"].asString());
        ASSERT_TRUE(end.has_value()) << engineName(GetParam()) << ": " << row["session_end"].asString();
        EXPECT_LE(std::llabs(*end - now_wall), 5) << engineName(GetParam()) << " delta " << delta
                                                    << ": end " << row["session_end"].asString()
                                                    << ", machine now " << wall::text(now_wall);
        EXPECT_GE(*end, start);
    }
}

TEST_P(MachineClockBackendTest, ALiveEndIsNeverBeforeTheStart) {
    std::string why;
    ASSERT_TRUE(machine_clocks::save(*db_, device_, "-12:00", &why));
    // A start the machine's clock has not reached: +14 written, -12 read.
    const long long start = machine_clocks::nowWall(device_, std::time(nullptr)) + 6 * 3600;
    ASSERT_TRUE(db_->saveSession(session(start, 60)));
    ASSERT_TRUE(db_->markSessionCompleted(device_, wall::point(start)));
    const auto row = one("SELECT session_end FROM cpap_sessions WHERE device_id = " + p1());
    EXPECT_EQ(wall::parseText(row["session_end"].asString()), start) << engineName(GetParam());
}

// ── section 2 E: the host's daylight saving and a machine without it ─────

namespace {

/// A night from real instants, one breathing minute and one event a minute,
/// as a parser of instants (a DreamStation) hands it over.
CPAPSession instantsNight(const std::string& device, long long first_utc, int minutes) {
    CPAPSession s;
    s.device_id = device;
    s.device_name = "AirSense 11";
    s.session_start = wall::point(first_utc);
    s.duration_seconds = minutes * 60;
    for (int m = 0; m < minutes; ++m) {
        const auto at = wall::point(first_utc + m * 60LL);
        BreathingSummary b(at);
        b.avg_flow_rate = 10;
        s.breathing_summary.push_back(b);
        s.events.emplace_back(EventType::HYPOPNEA, at, 10.0);
    }
    return s;
}

}  // namespace

TEST_P(MachineClockBackendTest, ANightAcrossTheHostsFallBackIsStoredWholeOnAFixedMachine) {
    HostZone tz("Australia/Sydney");
    // Sydney falls back at 2026-04-05 03:00 AEDT (16:00 UTC on the 4th). Three
    // hours of a +10:00 machine around it: 23:30 to 02:30 on the machine, no
    // hour twice.
    const long long first = L(2026, 4, 4, 13, 30);
    std::string why;
    ASSERT_TRUE(machine_clocks::save(*db_, device_, "+10:00", &why));
    CPAPSession s = instantsNight(device_, first, 180);
    putOnMachineClock(s, machine_clocks::zoneFor(device_));
    EXPECT_EQ(wall::text(*s.session_start), "2026-04-04 23:30:00");
    ASSERT_TRUE(db_->saveSession(s)) << engineName(GetParam());
    const auto p = p1();
    const auto minutes = one("SELECT COUNT(*) AS n FROM cpap_breathing_summary WHERE session_id IN "
                             "(SELECT id FROM cpap_sessions WHERE device_id = " + p + ")");
    EXPECT_EQ(std::atoi(minutes["n"].asString().c_str()), 180) << engineName(GetParam());
    const auto events = one("SELECT COUNT(*) AS n FROM cpap_events WHERE session_id IN "
                            "(SELECT id FROM cpap_sessions WHERE device_id = " + p + ")");
    EXPECT_EQ(std::atoi(events["n"].asString().c_str()), 180) << engineName(GetParam());
    const auto last = one("SELECT MAX(timestamp) AS t FROM cpap_breathing_summary WHERE session_id IN "
                          "(SELECT id FROM cpap_sessions WHERE device_id = " + p + ")");
    EXPECT_EQ(last["t"].asString().substr(0, 19), "2026-04-05 02:29:00") << engineName(GetParam());
}

TEST_P(MachineClockBackendTest, OnTheHostsClockTheRepeatedHourFoldsAsSection2ESays) {
    // The reading of section 2 E, confirmed: the same night read on the host's
    // zone (no setting, a machine that follows the host) has the repeated hour
    // twice in its text, and the second copy is not stored.
    // Four hours, 00:30 AEDT to 03:30 AEST, so the whole repeated hour is in it.
#ifdef _WIN32
    // The C runtime cannot be a Sydney host (see HostZone): the same four
    // hours across New York's fall back instead, 2026-11-01 00:30 EDT to
    // 03:30 EST (02:00 EDT is 06:00 UTC), the repeated hour 01:00 to 02:00.
    HostZone tz("America/New_York");
    const long long first = L(2026, 11, 1, 4, 30);
#else
    HostZone tz("Australia/Sydney");
    const long long first = L(2026, 4, 4, 13, 30);
#endif
    CPAPSession s = instantsNight(device_, first, 240);
    putOnMachineClock(s, MachineZone::host());
    std::set<std::string> texts;
    for (const auto& b : s.breathing_summary) texts.insert(wall::text(b.timestamp));
    EXPECT_EQ(texts.size(), 180u) << "60 of the 240 minutes read the same as another";
    ASSERT_TRUE(db_->saveSession(s));
    const auto n = one("SELECT COUNT(*) AS n FROM cpap_events WHERE session_id IN "
                       "(SELECT id FROM cpap_sessions WHERE device_id = " + p1() + ")");
    EXPECT_EQ(std::atoi(n["n"].asString().c_str()), 180) << engineName(GetParam());
}

TEST_P(MachineClockBackendTest, ANightAcrossTheHostsSpringForwardKeepsTheMachinesMinutes) {
    HostZone tz("America/New_York");
    // New York springs forward 2026-03-08 02:00 EST (07:00 UTC). A -05:00
    // machine has no gap: 01:00 to 04:00 on its own clock, every minute once.
    std::string why;
    ASSERT_TRUE(machine_clocks::save(*db_, device_, "-05:00", &why));
    CPAPSession s = instantsNight(device_, L(2026, 3, 8, 6, 0), 180);
    putOnMachineClock(s, machine_clocks::zoneFor(device_));
    ASSERT_TRUE(db_->saveSession(s));
    const auto p = p1();
    const auto first = one("SELECT MIN(timestamp) AS a, MAX(timestamp) AS b, COUNT(*) AS n FROM "
                           "cpap_breathing_summary WHERE session_id IN (SELECT id FROM cpap_sessions "
                           "WHERE device_id = " + p + ")");
    EXPECT_EQ(first["a"].asString().substr(0, 19), "2026-03-08 01:00:00") << engineName(GetParam());
    EXPECT_EQ(first["b"].asString().substr(0, 19), "2026-03-08 03:59:00") << engineName(GetParam());
    EXPECT_EQ(std::atoi(first["n"].asString().c_str()), 180);
}

TEST_P(MachineClockBackendTest, AStartInsideTheHostsSpringGapIsStoredAsTheMachineWroteIt) {
    // A card night named 02:30 on the host's spring-forward day, on a -05:00
    // machine: the label is stored as it is, not moved an hour by mktime.
    HostZone tz("America/New_York");
    std::string why;
    ASSERT_TRUE(machine_clocks::save(*db_, device_, "-05:00", &why));
    ASSERT_TRUE(db_->saveSession(session(L(2026, 3, 8, 2, 30), 3600)));
    const auto row = one("SELECT session_start FROM cpap_sessions WHERE device_id = " + p1());
    EXPECT_EQ(row["session_start"].asString().substr(0, 19), "2026-03-08 02:30:00") << engineName(GetParam());
    auto back = db_->getLastSessionStart(device_);
    ASSERT_TRUE(back.has_value());
    EXPECT_EQ(wall::seconds(*back), L(2026, 3, 8, 2, 30));
}

// ── decision 3: the backup a re-read takes first ─────────────────────────

TEST_P(MachineClockBackendTest, ReReadBacksUpTheMachinesRowsFirst) {
    ASSERT_TRUE(db_->saveSession(instantsNight(device_, L(2026, 1, 15, 22), 3)));
    std::string why;
    const auto made = clockapi::backupRows(*db_, device_, L(2026, 11, 19, 12, 0, 7), &why);
    backups_ = made;
    ASSERT_FALSE(made.empty()) << engineName(GetParam()) << ": " << why;
    EXPECT_EQ(made.front(), "cpap_sessions_bk_20261119120007");
    const auto n = one("SELECT COUNT(*) AS n FROM cpap_sessions_bk_20261119120007 WHERE device_id = " + p1());
    EXPECT_EQ(std::atoi(n["n"].asString().c_str()), 1) << engineName(GetParam());
    bool events = false;
    for (const auto& t : made) events = events || t == "cpap_events_bk_20261119120007";
    ASSERT_TRUE(events);
    auto ev = db_->executeQuery("SELECT COUNT(*) AS n FROM cpap_events_bk_20261119120007", {});
    EXPECT_EQ(std::atoi(ev[0]["n"].asString().c_str()), 3) << engineName(GetParam());
}

INSTANTIATE_TEST_SUITE_P(Engines, MachineClockBackendTest,
                         ::testing::Values(Engine::SQLite, Engine::MySQL, Engine::Postgres),
                         [](const ::testing::TestParamInfo<Engine>& i) { return engineName(i.param); });
