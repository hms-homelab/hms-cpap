// SDD-080: a DreamStation 2 card read through the user's own OSCAR.
//
// No OSCAR here and no OSCAR-made file (section 7 item 7): the database is
// built by this file's own SQL, with the tables and columns OSCAR 2.1.1's
// database was observed to hold, and OSCAR itself is a fake that "imports" by
// writing rows into it (or a tiny shell script, for the real process path).
// The card files are made-up bytes: nothing here is a card, nothing is
// decrypted.

#include "database/SQLiteDatabase.h"
#include "devices/Devices.h"
#include "devices/DevicesStore.h"
#include "services/As11NightImport.h"
#include "services/BurstCollectorService.h"
#include "services/DataPublisherService.h"
#include "services/OscarCard.h"
#include "services/OscarCardReader.h"
#include "services/OscarDb.h"
#include "sleepbridge/BridgeSource.h"
#include "OscarFakes.h"

#include <gtest/gtest.h>
#include <json/json.h>
#include <sqlite3.h>

#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <sstream>

using namespace hms_cpap;
using namespace hms_cpap::oscar;
namespace fs = std::filesystem;
using std::chrono::system_clock;

namespace {

// The synthetic oscar.db, the fake OSCAR and the made-up card (OscarFakes.h,
// shared with SDD-081's suite).
using namespace oscar_fakes;

class OscarTest : public ::testing::Test {
protected:
    fs::path dir_, data_, card_;
    std::shared_ptr<SQLiteDatabase> db_;
    std::unique_ptr<FakeOscarDb> oscar_;
    std::shared_ptr<FakeOscar> fake_ = std::make_shared<FakeOscar>();
    Settings settings_;

    void SetUp() override {
        dir_ = fs::temp_directory_path() /
               ("oscar_sdd080_" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
        data_ = dir_ / "OSCAR_Data";
        card_ = dir_ / "sleepbridge" / "TAP-S3-00000080";
        oscar_ = std::make_unique<FakeOscarDb>(data_);
        cardFile(card_, "P-SERIES/1A2B3C4D/PROP.BIN");
        db_ = std::make_shared<SQLiteDatabase>((dir_ / "hms.db").string());
        ASSERT_TRUE(db_->connect());
        settings_.program = "/opt/oscar/OSCAR20";
        settings_.data_dir = data_.string();
        settings_.profile = kDefaultProfile;
    }
    void TearDown() override {
        db_->disconnect();
        oscar_.reset();
        std::error_code ec;
        fs::remove_all(dir_, ec);
    }

    OscarCardReader::Options options() {
        OscarCardReader::Options o;
        o.settings = [this] { return settings_; };
        o.card_root = [this] { return card_.string(); };
        o.db = db_;
        o.device_id = kDevice;
        o.device_name = "DreamStation 2";
        o.runner = fake_;
        return o;
    }

    int count(const std::string& sql) {
        const auto rows = db_->executeQuery(sql, {kDevice});
        const Json::Value& v = rows[0]["v"];
        return v.isString() ? std::stoi(v.asString()) : v.asInt();
    }
};

}  // namespace

// ── the command and what its exit means (D2, D3) ──────────────────────────

TEST(OscarCommand, HeadlessImportOfTheCardRootIntoHmsCpapsProfileNeverUpgrading) {
    Settings s;
    s.program = "/usr/bin/OSCAR20";
    s.data_dir = "/home/u/Documents/OSCAR_Data";
    const auto argv = importCommand(s, "/d/sleepbridge/TAP");
    EXPECT_EQ(argv, (std::vector<std::string>{"/usr/bin/OSCAR20", "--datadir", "/home/u/Documents/OSCAR_Data",
                                              "--profile", "hms-cpap", "--import", "PRS1", "/d/sleepbridge/TAP"}));
    for (const auto& a : argv) EXPECT_NE(a, "--upgrade-database") << "E4: never";
    EXPECT_EQ(helpCommand("/usr/bin/OSCAR20"), (std::vector<std::string>{"/usr/bin/OSCAR20", "--help"}));
}

TEST(OscarCommand, TheJobLimitScalesWithTheCard) {
    EXPECT_EQ(jobLimitSeconds(0), 120 + 600);
    EXPECT_EQ(jobLimitSeconds(217ull * 1024 * 1024), static_cast<int>(218 * 43 / 10) + 600);
    EXPECT_EQ(jobLimitSeconds(100000ull * 1024 * 1024), 3600 + 600) << "at most an hour, plus ten minutes";
}

TEST(OscarCommand, ASessionCloseIsAB01UnderPSeries) {
    EXPECT_TRUE(isSessionClose("P-SERIES/1A2B3C4D/P0/0000001E.B01"));
    EXPECT_TRUE(isSessionClose("p-series/1a2b3c4d/p3/0000001e.b01"));
    EXPECT_FALSE(isSessionClose("P-SERIES/1A2B3C4D/P0/0000001E.B05")) << "the open session's flow";
    EXPECT_FALSE(isSessionClose("P-SERIES/1A2B3C4D/P0/0000001E.B02"));
    EXPECT_FALSE(isSessionClose("DATALOG/20300101/x.B01"));
    EXPECT_EQ(cardRootOf("/d/sb/TAP/P-SERIES/1A2B3C4D"), "/d/sb/TAP");
    EXPECT_EQ(cardRootOf("/d/card"), "/d/card");
}

TEST(OscarCommand, TheExitIsTheOutcome) {
    ProcessResult none;
    EXPECT_EQ(classifyExit(none, false), Outcome::NoProgram);
    auto t = exited(137);
    t.timed_out = true;
    EXPECT_EQ(classifyExit(t, true), Outcome::Timeout) << "never 'no data'";
    EXPECT_EQ(classifyExit(exited(75), false), Outcome::Busy);
    EXPECT_EQ(classifyExit(exited(0), true), Outcome::Imported);
    EXPECT_EQ(classifyExit(exited(0), false), Outcome::NothingNew);
    EXPECT_EQ(classifyExit(exited(1), false), Outcome::ReadNothing);
    EXPECT_EQ(classifyExit(exited(2), false), Outcome::Failed);
}

#ifndef _WIN32
// The real process path, against a script standing in for OSCAR.
TEST(OscarProcess, ItsExitAndOutputComeBackAndItRunsWithNoWindow) {
    const fs::path d = fs::temp_directory_path() / "oscar_fake_proc";
    fs::create_directories(d);
    const fs::path script = d / "OSCAR20";
    std::ofstream(script) << "#!/bin/sh\necho \"platform=$QT_QPA_PLATFORM args=$*\"\nexit 75\n";
    fs::permissions(script, fs::perms::owner_all);
    SystemProcessRunner runner;
    const auto r = runner.run({script.string(), "--import", "PRS1", "/c"}, 30);
    EXPECT_TRUE(r.started);
    EXPECT_FALSE(r.timed_out);
    EXPECT_EQ(r.exit_code, 75);
    EXPECT_NE(r.output.find("platform=offscreen"), std::string::npos) << r.output;
    EXPECT_NE(r.output.find("args=--import PRS1 /c"), std::string::npos) << r.output;
    fs::remove_all(d);
}

TEST(OscarProcess, PastTheJobLimitItIsKilledAndSaysTimeout) {
    const fs::path d = fs::temp_directory_path() / "oscar_fake_slow";
    fs::create_directories(d);
    const fs::path script = d / "OSCAR20";
    std::ofstream(script) << "#!/bin/sh\nexec sleep 30\n";
    fs::permissions(script, fs::perms::owner_all);
    SystemProcessRunner runner;
    const auto r = runner.run({script.string()}, 1);
    EXPECT_TRUE(r.started);
    EXPECT_TRUE(r.timed_out);
    EXPECT_EQ(classifyExit(r, false), Outcome::Timeout);
    fs::remove_all(d);
}

TEST(OscarProcess, AProgramThatIsNotThereDoesNotStart) {
    SystemProcessRunner runner;
    const auto r = runner.run({"/nonexistent/OSCAR20", "--help"}, 5);
    EXPECT_FALSE(r.started);
    EXPECT_EQ(classifyExit(r, false), Outcome::NoProgram);
}
#endif

// ── the database (D4, D6, E10) ────────────────────────────────────────────

TEST_F(OscarTest, TheGateLetsInOnlyAProvenVersionAndSchema) {
    std::string why;
    auto db = OscarDb::open(data_.string(), &why);
    ASSERT_TRUE(db) << why;
    const auto g = db->gate();
    EXPECT_TRUE(g.ok) << g.why;
    EXPECT_EQ(g.schema, 19);

    // Section 7 item 5: another schema, another version.
    for (auto [version, schema] : std::vector<std::pair<std::string, int>>{{"2.1.1+00000000", 20}, {"2.2.0+1", 19},
                                                                           {"2.0.1+377654fe", 17}}) {
        const fs::path other = dir_ / ("other_" + std::to_string(schema) + version.substr(0, 3));
        FakeOscarDb o(other, version, schema);
        auto odb = OscarDb::open(other.string(), &why);
        ASSERT_TRUE(odb);
        const auto og = odb->gate();
        EXPECT_FALSE(og.ok) << version << " schema " << schema;
        EXPECT_NE(og.why.find("not supported yet"), std::string::npos);
    }
}

TEST_F(OscarTest, NoOscarDbIsSaidNotCreated) {
    std::string why;
    EXPECT_FALSE(OscarDb::open((dir_ / "empty").string(), &why));
    EXPECT_FALSE(fs::exists(dir_ / "empty" / "oscar.db")) << "never created";
}

TEST_F(OscarTest, OnlyHmsCpapsOwnProfileIsRead) {
    oscar_->session(oscar_->user_, 30, kT0, 10);         // the user's own night
    oscar_->session(oscar_->hms_, 30, kT0, 10);          // the same card, in hms-cpap's profile
    oscar_->session(oscar_->hms_, 31, kT0 + 86400000, 10);
    std::string why;
    auto db = OscarDb::open(data_.string(), &why);
    ASSERT_TRUE(db);
    EXPECT_EQ(db->profiles(), (std::vector<std::string>{"hms-cpap", "someone"}));
    const auto heads = db->sessions(kDefaultProfile);
    ASSERT_EQ(heads.size(), 2u) << "the journal and the other profile left out";
    EXPECT_EQ(heads[0].serial, kSerial);
    EXPECT_EQ(heads[0].session_id, 30);
    EXPECT_EQ(db->sessions("someone").size(), 1u);
    EXPECT_EQ(db->signature("nobody"), "");
}

TEST_F(OscarTest, ASessionDecodesIntoEventsMinutesBreathsAndTrends) {
    oscar_->session(oscar_->hms_, 30, kT0, 10, 2);
    std::string why;
    auto db = OscarDb::open(data_.string(), &why);
    ASSERT_TRUE(db);
    const auto heads = db->sessions(kDefaultProfile);
    ASSERT_EQ(heads.size(), 1u);
    auto s = db->read(heads[0], kDevice, "DreamStation 2", &why);
    ASSERT_TRUE(s) << why;
    EXPECT_EQ(s->manufacturer, DeviceManufacturer::PHILIPS);
    EXPECT_EQ(s->serial_number, kSerial);
    EXPECT_EQ(s->model_id.value_or(0), 520);
    EXPECT_EQ(*s->session_start, sessionClock(kT0));
    EXPECT_EQ(s->duration_seconds.value_or(0), 600);
    // Two OA flagged at their end, 12 s long: onset 12 s before.
    int oa = 0, leak = 0;
    for (const auto& e : s->events) {
        if (e.event_type == EventType::OBSTRUCTIVE) {
            if (oa == 0) EXPECT_EQ(e.timestamp, sessionClock(kT0 + 120000 - 12000));
            EXPECT_DOUBLE_EQ(e.duration_seconds, 12.0);
            ++oa;
        }
        if (e.event_type == EventType::LARGE_LEAK) {
            EXPECT_EQ(e.timestamp, sessionClock(kT0 + 300000 - 30000));
            ++leak;
        }
    }
    EXPECT_EQ(oa, 2);
    EXPECT_EQ(leak, 1);
    ASSERT_TRUE(s->metrics);
    EXPECT_EQ(s->metrics->obstructive_apneas, 2);
    // Ten minutes of flow, a breath every four seconds.
    EXPECT_GE(s->breathing_summary.size(), 9u);
    EXPECT_GT(s->breaths.size(), 100u);
    ASSERT_TRUE(s->breathing_summary.front().therapy_pressure);
    EXPECT_NEAR(*s->breathing_summary.front().therapy_pressure, 9.4, 1e-6);
    EXPECT_FALSE(s->breathing_summary.front().leak_rate) << "no unintentional leak for a Philips minute";
    ASSERT_TRUE(s->metrics->pressure_p95);
    EXPECT_NEAR(*s->metrics->pressure_p95, 10.8, 0.5);
}

TEST_F(OscarTest, ReadingNeverWritesOscarsFile) {
    oscar_->session(oscar_->hms_, 30, kT0, 5);
    oscar_->exec("PRAGMA wal_checkpoint(TRUNCATE)");
    auto bytes = [&] {
        std::ifstream in(data_ / "oscar.db", std::ios::binary);
        return std::string((std::istreambuf_iterator<char>(in)), {});
    };
    const std::string before = bytes();
    {
        std::string why;
        auto db = OscarDb::open(data_.string(), &why);
        ASSERT_TRUE(db);
        db->begin();
        for (const auto& h : db->sessions(kDefaultProfile)) EXPECT_TRUE(db->read(h, kDevice, "x", &why));
        db->end();
    }
    EXPECT_EQ(bytes(), before);
}

// ── the reader, start to end ──────────────────────────────────────────────

TEST_F(OscarTest, AnImportStoresTheNewSessionsAsCardAndANothingNewStoresNone) {
    cardFile(card_, "P-SERIES/1A2B3C4D/P0/0000001E.B01");
    cardFile(card_, "P-SERIES/1A2B3C4D/P0/0000001E.B05");
    fake_->on_run = [&](const std::vector<std::string>&) {
        if (fake_->runs.size() == 1) oscar_->session(oscar_->hms_, 0x1E, kT0, 10);   // OSCAR imports it
        return exited(0);
    };
    OscarCardReader reader(options());
    auto r = reader.readOnce();
    EXPECT_EQ(r.outcome, Outcome::Imported) << r.message;
    EXPECT_EQ(r.stored, 1);
    EXPECT_EQ(r.version, "2.1.1+00000000");
    ASSERT_EQ(fake_->runs.size(), 1u);
    EXPECT_EQ(fake_->runs[0], importCommand(settings_, card_.string()));
    EXPECT_TRUE(db_->sessionExists(kDevice, sessionClock(kT0)));
    // Its files are the card's (SDD-077 D1), by the session's number.
    EXPECT_EQ(count("SELECT COUNT(*) AS v FROM cpap_session_files f JOIN cpap_sessions s ON s.id = f.session_id "
                    "WHERE s.device_id = ? AND f.source = 'card'"),
              2);
    EXPECT_GT(count("SELECT COUNT(*) AS v FROM cpap_events e JOIN cpap_sessions s ON s.id = e.session_id "
                    "WHERE s.device_id = ?"),
              0);

    r = reader.readOnce();
    EXPECT_EQ(r.outcome, Outcome::NothingNew);
    EXPECT_EQ(r.stored, 0);
    EXPECT_EQ(count("SELECT COUNT(*) AS v FROM cpap_sessions WHERE device_id = ?"), 1);
    EXPECT_EQ(reader.status()["state"].asString(), "nothing_new");
}

TEST_F(OscarTest, BusyTimeoutAndReadNothingStoreNothingAndSaySo) {
    OscarCardReader reader(options());
    fake_->on_run = [](const std::vector<std::string>&) { return exited(75); };
    EXPECT_EQ(reader.readOnce().outcome, Outcome::Busy);
    fake_->on_run = [](const std::vector<std::string>&) {
        auto r = exited(137);
        r.timed_out = true;
        return r;
    };
    EXPECT_EQ(reader.readOnce().outcome, Outcome::Timeout);
    fake_->on_run = [](const std::vector<std::string>&) {
        return exited(1, "Batch: Import failed for loader PRS1\n");
    };
    const auto r = reader.readOnce();
    EXPECT_EQ(r.outcome, Outcome::ReadNothing);
    EXPECT_NE(r.message.find("Import failed"), std::string::npos);
    EXPECT_EQ(reader.status()["state"].asString(), "read_nothing");
    EXPECT_EQ(count("SELECT COUNT(*) AS v FROM cpap_sessions WHERE device_id = ?"), 0);
}

TEST_F(OscarTest, NothingRunsWithoutSettingsAProvenDatabaseOrTheProfile) {
    OscarCardReader reader(options());
    settings_.program.clear();
    EXPECT_EQ(reader.readOnce().outcome, Outcome::NotSetUp);
    settings_.program = "/opt/oscar/OSCAR20";
    settings_.profile = "missing";
    EXPECT_EQ(reader.readOnce().outcome, Outcome::NoProfile);
    settings_.profile = kDefaultProfile;
    oscar_->exec("UPDATE app_preferences SET value = '2.3.0+0' WHERE key = 'VersionString'");
    EXPECT_EQ(reader.readOnce().outcome, Outcome::NotSupported);
    EXPECT_TRUE(fake_->runs.empty()) << "OSCAR never started";
}

TEST_F(OscarTest, AStoredCardSessionIsNotStoredAgainAfterARestart) {
    oscar_->session(oscar_->hms_, 0x1E, kT0, 10);
    {
        OscarCardReader reader(options());
        EXPECT_EQ(reader.readOnce().stored, 1);
    }
    OscarCardReader again(options());   // a new process
    const auto r = again.readOnce();
    EXPECT_EQ(r.stored, 0);
    EXPECT_EQ(count("SELECT COUNT(*) AS v FROM cpap_sessions WHERE device_id = ?"), 1);
}

// Section 7 item 4 (D5): a Bluetooth copy of the stretch is replaced by the card's.
TEST_F(OscarTest, TheCardReplacesABluetoothCopyOfTheSameStretch) {
    CPAPSession ble;
    ble.device_id = kDevice;
    ble.device_name = "DreamStation 2";
    ble.manufacturer = DeviceManufacturer::PHILIPS;
    ble.session_start = sessionClock(kT0 + 1000);
    ble.session_end = sessionClock(kT0 + 600000);
    ble.duration_seconds = 599;
    ble.calculateMetrics();
    ASSERT_TRUE(db_->saveSession(ble));
    db_->replaceSessionFiles(kDevice, *ble.session_start, {{"b01", "ds2/X/P0/0000001E.B01", kSourceBleDs2}});

    cardFile(card_, "P-SERIES/1A2B3C4D/P0/0000001E.B01");
    oscar_->session(oscar_->hms_, 0x1E, kT0, 10);
    OscarCardReader reader(options());
    EXPECT_EQ(reader.readOnce().stored, 1);
    EXPECT_EQ(count("SELECT COUNT(*) AS v FROM cpap_sessions WHERE device_id = ?"), 1) << "one session remains";
    EXPECT_EQ(count("SELECT COUNT(*) AS v FROM cpap_session_files f JOIN cpap_sessions s ON s.id = f.session_id "
                    "WHERE s.device_id = ? AND f.source <> 'card'"),
              0);
}

// ── the trigger: a .B01 landing, one read, never a queue (D2) ─────────────

TEST_F(OscarTest, ANewOrChangedB01RequestsAReadAndTheSameOneDoesNot) {
    OscarCardReader reader(options());   // not started: requests are only noted
    cardFile(card_, "P-SERIES/1A2B3C4D/P0/0000001E.B05");
    EXPECT_FALSE(reader.noteCloses(card_.string())) << "flow alone: the session is open";
    cardFile(card_, "P-SERIES/1A2B3C4D/P0/0000001E.B01");
    EXPECT_TRUE(reader.noteCloses(card_.string()));
    EXPECT_FALSE(reader.noteCloses(card_.string()));
    EXPECT_FALSE(reader.noteCloses(card_.string() + "/", {"P-SERIES/1A2B3C4D/P0/0000001E.B01"}))
        << "the rebuild naming it again, the root spelled with a slash";
    cardFile(card_, "P-SERIES/1A2B3C4D/P0/0000001E.B01", "made-up bytes, longer now");
    EXPECT_TRUE(reader.noteCloses(card_.string(), {"P-SERIES/1A2B3C4D/P0/0000001E.B01"}));
    EXPECT_FALSE(reader.noteCloses(card_.string(), {"P-SERIES/1A2B3C4D/P0/0000001F.B05"}));
}

TEST_F(OscarTest, ClosesDuringARunAreOneMoreRunNeverAQueue) {
    std::mutex mu;
    std::condition_variable cv;
    bool entered = false, release = false;
    int runs = 0;
    fake_->on_run = [&](const std::vector<std::string>&) {
        std::unique_lock<std::mutex> lk(mu);
        ++runs;
        entered = true;
        cv.notify_all();
        cv.wait(lk, [&] { return release || runs > 1; });
        return exited(0);
    };
    OscarCardReader reader(options());
    reader.start();
    reader.request("a session closed");
    {
        std::unique_lock<std::mutex> lk(mu);
        cv.wait(lk, [&] { return entered; });
    }
    for (int i = 0; i < 3; ++i) reader.request("another close");   // while OSCAR runs
    {
        std::lock_guard<std::mutex> lk(mu);
        release = true;
    }
    cv.notify_all();
    {
        std::unique_lock<std::mutex> lk(mu);
        cv.wait(lk, [&] { return runs == 2; });
    }
    // The second read's status is set before the worker waits again.
    reader.stop();
    EXPECT_EQ(runs, 2) << "one read after the one running covers every close in between";
}

// The burst, end to end: an encrypted card in the source folder builds the
// reader, a .B01 on it starts OSCAR, and what OSCAR stored becomes sessions.
TEST_F(OscarTest, AnEncryptedCardInTheSourceFolderIsReadThroughOscar) {
    const fs::path fixture = fs::path(__FILE__).parent_path().parent_path() / "fixtures/philips/ds2_encrypted";
    if (!fs::exists(fixture)) GTEST_SKIP() << "tests/fixtures/philips/ds2_encrypted not found";
    const fs::path card = dir_ / "encrypted";
    fs::copy(fixture, card, fs::copy_options::recursive);
    cardFile(card, "P-SERIES/PXENC001/P0/00000041.B01", "made-up closing bytes");

    std::mutex mu;
    std::condition_variable cv;
    bool ran = false;
    fake_->on_run = [&](const std::vector<std::string>& argv) {
        EXPECT_EQ(argv.back(), card.string()) << "the card root, never the machine folder";
        oscar_->session(oscar_->hms_, 0x41, kT0, 10);
        std::lock_guard<std::mutex> lk(mu);
        ran = true;
        cv.notify_all();
        return exited(0);
    };
    BurstCollectorService svc(60);
    svc.injectDependenciesForTest(db_, nullptr,
                                  std::make_unique<DataPublisherService>(std::shared_ptr<hms::MqttClient>{}, db_));
    svc.setOscarRunnerForTest(fake_);
    svc.setOscarSettings(settings_);
    svc.usePhilipsCardForTest(card.string());
    EXPECT_FALSE(svc.runBurstCycleForTest()) << "the plain reader reads nothing of it";
    {
        std::unique_lock<std::mutex> lk(mu);
        cv.wait(lk, [&] { return ran; });
    }
    auto reader = svc.oscarReaderForTest();
    ASSERT_TRUE(reader);
    reader->stop();   // joins the read that is finishing
    const auto st = svc.oscarStatus();
    EXPECT_EQ(st["state"].asString(), "imported");
    EXPECT_EQ(st["card_root"].asString(), card.string());
    EXPECT_TRUE(db_->sessionExists(svc.deviceIdForTest(), sessionClock(kT0)));
}

// ── the check on save (D7) ────────────────────────────────────────────────

TEST_F(OscarTest, TheCheckOnSaveNamesEachFieldsProblem) {
    const fs::path prog = dir_ / "OSCAR20";
    cardFile(dir_, "OSCAR20", "x");
    Settings s = settings_;
    s.program = prog.string();
    fake_->on_run = [](const std::vector<std::string>&) {
        return exited(0, "Usage: OSCAR20 [options]\n  --import <loader> <path>\n");
    };
    auto v = OscarCardReader::check(s, *fake_, false);
    EXPECT_TRUE(v["ok"].asBool()) << v.toStyledString();
    EXPECT_EQ(v["data_dir"]["version"].asString(), "2.1.1+00000000");
    EXPECT_EQ(v["profiles"].size(), 2u);

    s.profile = "nobody";
    v = OscarCardReader::check(s, *fake_, false);
    EXPECT_EQ(v["profile"]["state"].asString(), "missing");
    EXPECT_FALSE(v["ok"].asBool());

    fake_->on_run = [](const std::vector<std::string>&) { return exited(0, "Usage: OSCAR [options]\n"); };
    EXPECT_EQ(OscarCardReader::check(s, *fake_, false)["program"]["state"].asString(), "no_batch");

    s.program = (dir_ / "nothing").string();
    EXPECT_EQ(OscarCardReader::check(s, *fake_, false)["program"]["state"].asString(), "not_found");
    EXPECT_EQ(OscarCardReader::check(s, *fake_, true)["program"]["state"].asString(), "not_here")
        << "Docker / the HA add-on: OSCAR is not on this computer";
    s.data_dir = (dir_ / "nowhere").string();
    EXPECT_EQ(OscarCardReader::check(s, *fake_, false)["data_dir"]["state"].asString(), "no_db");
}

// ── D1: the Sleep Bridge on a DreamStation 2 is a Philips source ──────────

TEST_F(OscarTest, ABridgeFolderHoldingPSeriesIsAPhilipsSourceNotResMed) {
    Json::Value status(Json::objectValue);
    Json::Value b(Json::objectValue);
    b["sender"] = "TAP-S3-00000080";
    b["card_root"] = card_.string();
    b["card_format"] = "";
    status["bridges"].append(b);
    std::string why;
    const auto src = sleepbridge::bridgeSource(status, "TAP-S3-00000080", &why);
    ASSERT_TRUE(src) << why;
    EXPECT_EQ(src->format, "philips");
    EXPECT_TRUE(src->format_known);
    EXPECT_EQ(sleepbridge::formatForBrand("Philips Respironics"), "philips");
    EXPECT_FALSE(sleepbridge::holdsPhilipsCard((dir_ / "nothing").string()));
}

// ── D7: the tile of what carries the card shows the OSCAR line ────────────

TEST_F(OscarTest, TheBridgeTileCarriesTheOscarLine) {
    devices::DeviceInputs in;
    Json::Value b(Json::objectValue);
    b["sender"] = "TAP-S3-00000080";
    b["card_root"] = card_.string() + "/";
    in.sleepbridge["bridges"].append(b);
    in.oscar["state"] = "imported";
    in.oscar["card_root"] = card_.string();
    devices::DevicesStore store("");
    const auto out = devices::buildDevices(in, store);
    ASSERT_EQ(out["devices"].size(), 1u);
    EXPECT_EQ(out["devices"][0]["detail"]["oscar"]["state"].asString(), "imported");
}
