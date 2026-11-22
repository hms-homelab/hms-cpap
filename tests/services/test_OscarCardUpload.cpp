// SDD-083: a DreamStation 2 card whose files are encrypted, uploaded on the
// upload page, is read by the OSCAR the user paired; with none paired it is
// refused and nothing is kept.
//
// No OSCAR here and no OSCAR-made file: the fake OSCAR and the synthetic
// oscar.db of OscarFakes.h, and the made-up card of
// tests/fixtures/philips (bytes of nothing, never a real card). Nothing is
// decrypted.

#include "database/SQLiteDatabase.h"
#include "services/BurstCollectorService.h"
#include "services/CardUpload.h"
#include "services/DataPublisherService.h"
#include "services/OscarCard.h"
#include "services/OscarCardReader.h"
#include "services/RemovedNights.h"
#include "OscarFakes.h"

#include <gtest/gtest.h>
#include <json/json.h>

#include <chrono>
#include <condition_variable>
#include <filesystem>
#include <fstream>
#include <mutex>

using namespace hms_cpap;
using namespace hms_cpap::oscar;
using namespace oscar_fakes;
namespace fs = std::filesystem;

namespace {

fs::path fixtures() { return fs::path(__FILE__).parent_path().parent_path() / "fixtures/philips"; }

/// A helper that is paired and answers with [outcome], never importing.
class FakeRemote : public IRemoteOscar {
public:
    Outcome outcome{Outcome::HelperOffline};
    std::string message{"waiting for HOME-PC"};
    int reads{0};
    std::string last_root;
    bool remembered() const override { return true; }
    Json::Value status() const override {
        Json::Value v(Json::objectValue);
        v["remembered"] = true;
        v["name"] = "HOME-PC";
        return v;
    }
    RemoteRead read(const std::string& card_root, const Want&, const std::string&) override {
        ++reads;
        last_root = card_root;
        RemoteRead r;
        r.outcome = outcome;
        r.message = message;
        return r;
    }
};

class OscarUploadTest : public ::testing::Test {
protected:
    fs::path dir_, data_, staging_, store_;
    std::shared_ptr<SQLiteDatabase> db_;
    std::unique_ptr<FakeOscarDb> oscar_;
    std::shared_ptr<FakeOscar> fake_ = std::make_shared<FakeOscar>();
    Settings settings_;

    void SetUp() override {
        dir_ = fs::temp_directory_path() /
               ("oscar_sdd083_" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
        data_ = dir_ / "OSCAR_Data";
        staging_ = dir_ / "cpap_stage_1";
        store_ = dir_ / "uploads" / "philips";
        oscar_ = std::make_unique<FakeOscarDb>(data_);
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

    /// The encrypted fixture, extracted as a zip would be, under [wrapper].
    fs::path encryptedZip(const std::string& wrapper = "") {
        const fs::path to = wrapper.empty() ? staging_ : staging_ / wrapper;
        fs::create_directories(to);
        fs::copy(fixtures() / "ds2_encrypted", to, fs::copy_options::recursive);
        cardFile(to, "P-SERIES/PXENC001/P0/00000041.B01", "made-up closing bytes");
        return to;
    }

    std::unique_ptr<BurstCollectorService> collector() {
        auto svc = std::make_unique<BurstCollectorService>(60);
        svc->injectDependenciesForTest(db_, nullptr,
                                       std::make_unique<DataPublisherService>(std::shared_ptr<hms::MqttClient>{}, db_));
        svc->setOscarRunnerForTest(fake_);
        return svc;
    }
};

}  // namespace

// ── what the upload is ─────────────────────────────────────────────────────

TEST_F(OscarUploadTest, AnEncryptedDreamStationCardIsFoundEvenInsideAWrapperFolder) {
    if (!fs::exists(fixtures() / "ds2_encrypted")) GTEST_SKIP() << "fixture not found";
    encryptedZip("cpap files");
    const auto card = encryptedPhilipsCardIn(staging_.string());
    ASSERT_TRUE(card.has_value());
    EXPECT_EQ(fs::path(*card).filename().string(), "PXENC001");
}

TEST_F(OscarUploadTest, APlainDreamStationCardOrAResmedCardIsNotAnEncryptedOne) {
    if (!fs::exists(fixtures() / "ds2_live")) GTEST_SKIP() << "fixture not found";
    fs::create_directories(staging_);
    fs::copy(fixtures() / "ds2_live", staging_, fs::copy_options::recursive);
    EXPECT_FALSE(encryptedPhilipsCardIn(staging_.string()).has_value()) << "plain files keep today's path";

    const fs::path resmed = dir_ / "resmed";
    cardFile(resmed, "DATALOG/20300101/20300101_000000_BRP.edf");
    cardFile(resmed, "STR.edf");
    EXPECT_FALSE(encryptedPhilipsCardIn(resmed.string()).has_value());
}

// ── where it is kept ───────────────────────────────────────────────────────

TEST_F(OscarUploadTest, TheCardIsKeptAsOscarTakesACardWhicheverWayItWasZipped) {
    // A zip of the card: P-SERIES comes whole, LAST.TXT with it.
    cardFile(staging_, "P-SERIES/LAST.TXT", "AAAA0000");
    cardFile(staging_, "P-SERIES/AAAA0000/PROP.BIN");
    cardFile(staging_, "P-SERIES/AAAA0000/P0/00000001.B01");
    std::string err;
    ASSERT_TRUE(keepPhilipsUpload((staging_ / "P-SERIES/AAAA0000").string(), store_.string(), err)) << err;
    EXPECT_TRUE(fs::exists(store_ / "P-SERIES/LAST.TXT"));
    EXPECT_TRUE(fs::exists(store_ / "P-SERIES/AAAA0000/P0/00000001.B01"));

    // A zip of the machine folder alone keeps its name.
    const fs::path m = dir_ / "z2" / "BBBB0000";
    cardFile(m, "PROP.BIN");
    cardFile(m, "P0/00000002.B01");
    ASSERT_TRUE(keepPhilipsUpload(m.string(), store_.string(), err)) << err;
    EXPECT_TRUE(fs::exists(store_ / "P-SERIES/BBBB0000/P0/00000002.B01"));

    // Its contents at the zip's root have no name of their own.
    const fs::path c = dir_ / "cpap_stage_9";
    cardFile(c, "PROP.BIN");
    cardFile(c, "P0/00000003.B01");
    ASSERT_TRUE(keepPhilipsUpload(c.string(), store_.string(), err)) << err;
    EXPECT_TRUE(fs::exists(store_ / "P-SERIES/UPLOAD/P0/00000003.B01"));

    // A later upload merges; the earlier files stay.
    EXPECT_TRUE(fs::exists(store_ / "P-SERIES/AAAA0000/P0/00000001.B01"));
}

// ── the read, on the reader's own thread ───────────────────────────────────

TEST_F(OscarUploadTest, ReadCardReadsTheGivenRootOnTheWorkerAndSaysWhatLanded) {
    const fs::path live = dir_ / "live";   // the source's card: not this read's
    cardFile(live, "P-SERIES/1A2B3C4D/PROP.BIN");
    cardFile(store_, "P-SERIES/PXENC001/PROP.BIN");
    cardFile(store_, "P-SERIES/PXENC001/P0/00000041.B01");
    fake_->on_run = [&](const std::vector<std::string>& argv) {
        EXPECT_EQ(argv.back(), store_.string()) << "the uploaded card root";
        oscar_->session(oscar_->hms_, 0x41, kT0, 10);
        return exited(0);
    };
    OscarCardReader::Options o;
    o.settings = [this] { return settings_; };
    o.card_root = [&] { return live.string(); };
    o.db = db_;
    o.device_id = kDevice;
    o.device_name = "DreamStation 2";
    o.runner = fake_;
    OscarCardReader reader(o);

    // Not running: said at once, nothing run.
    EXPECT_EQ(reader.readCard(store_.string(), "upload").get().outcome, Outcome::Failed);
    EXPECT_TRUE(fake_->runs.empty());

    reader.start();
    const auto r = reader.readCard(store_.string(), "upload").get();
    reader.stop();
    EXPECT_EQ(r.outcome, Outcome::Imported) << r.message;
    EXPECT_EQ(r.stored, 1);
    ASSERT_EQ(r.landed.size(), 1u);
    EXPECT_EQ(fake_->runs.size(), 1u);
    EXPECT_TRUE(db_->sessionExists(kDevice, sessionClock(kT0)));
}

TEST_F(OscarUploadTest, AsksForTheSameCardWhileOscarRunsAreOneMoreReadNeverAQueue) {
    cardFile(store_, "P-SERIES/PXENC001/PROP.BIN");
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
    OscarCardReader::Options o;
    o.settings = [this] { return settings_; };
    o.card_root = [this] { return store_.string(); };
    o.db = db_;
    o.device_id = kDevice;
    o.runner = fake_;
    OscarCardReader reader(o);
    reader.start();
    auto first = reader.readCard(store_.string(), "upload");
    {
        std::unique_lock<std::mutex> lk(mu);
        cv.wait(lk, [&] { return entered; });
    }
    auto second = reader.readCard(store_.string(), "upload");
    auto third = reader.readCard(store_.string(), "upload");
    {
        std::lock_guard<std::mutex> lk(mu);
        release = true;
    }
    cv.notify_all();
    first.wait();
    second.wait();
    third.wait();
    reader.stop();
    EXPECT_EQ(runs, 2) << "one read after the one running covers both asks";
}

// ── paired or not ──────────────────────────────────────────────────────────

TEST_F(OscarUploadTest, WithNoOscarPairedNothingIsReadAndTheRefusalSaysHowToPairOne) {
    auto svc = collector();
    Settings none;
    none.program = "/opt/oscar/OSCAR20";   // a program alone is not a set-up OSCAR
    none.data_dir.clear();
    svc->setOscarSettings(none);
    EXPECT_FALSE(svc->oscarSourcePaired());
    cardFile(store_, "P-SERIES/PXENC001/PROP.BIN");
    EXPECT_FALSE(svc->readUploadedOscarCard(store_.string()).has_value());
    EXPECT_TRUE(fake_->runs.empty()) << "OSCAR never started";
    EXPECT_FALSE(svc->oscarReaderForTest()) << "no reader is built for a refusal";

    for (bool containerised : {false, true}) {
        const auto r = philipsUploadRefusal(containerised);
        const std::string e = r["error"].asString();
        EXPECT_NE(e.find("encrypted"), std::string::npos);
        EXPECT_NE(e.find("does not decrypt"), std::string::npos);
        EXPECT_NE(e.find("OSCAR helper"), std::string::npos);
        EXPECT_NE(e.find("Nothing was kept"), std::string::npos);
        EXPECT_EQ(r["oscar"].asString(), "not_paired");
        if (!containerised) EXPECT_NE(e.find("Settings, Data Source"), std::string::npos);
        if (!containerised) EXPECT_NE(e.find("hms-cpap"), std::string::npos);
    }
}

TEST_F(OscarUploadTest, OscarOnThisComputerReadsTheUploadedCardAndItsNightsLand) {
    if (!fs::exists(fixtures() / "ds2_encrypted")) GTEST_SKIP() << "fixture not found";
    encryptedZip();
    const auto card = encryptedPhilipsCardIn(staging_.string());
    ASSERT_TRUE(card);
    std::string err;
    ASSERT_TRUE(keepPhilipsUpload(*card, store_.string(), err)) << err;
    fake_->on_run = [&](const std::vector<std::string>& argv) {
        EXPECT_EQ(argv, importCommand(settings_, store_.string()));
        oscar_->session(oscar_->hms_, 0x41, kT0, 10);
        return exited(0);
    };
    auto svc = collector();
    svc->setOscarSettings(settings_);
    ASSERT_TRUE(svc->oscarSourcePaired());
    const auto read = svc->readUploadedOscarCard(store_.string());
    ASSERT_TRUE(read.has_value());
    const auto reply = philipsUploadReply(*read, svc->oscarHelperPaired());
    EXPECT_FALSE(reply.isMember("error")) << reply.toStyledString();
    EXPECT_EQ(reply["status"].asString(), "done");
    EXPECT_EQ(reply["read_by"].asString(), "oscar");
    EXPECT_EQ(reply["sessions_found"].asInt(), 1);
    EXPECT_EQ(reply["dates"].size(), 1u);
    EXPECT_TRUE(db_->sessionExists(svc->deviceIdForTest(), sessionClock(kT0)));
    // The card's own files are the session's (SDD-077 D1), under the upload.
    const auto rows = db_->executeQuery(
        "SELECT COUNT(*) AS v FROM cpap_session_files f JOIN cpap_sessions s ON s.id = f.session_id "
        "WHERE s.device_id = ? AND f.source = 'card'",
        {svc->deviceIdForTest()});
    const Json::Value& v = rows[0]["v"];
    EXPECT_GT(v.isString() ? std::stoi(v.asString()) : v.asInt(), 0);

    // The same card again: nothing new, and no error.
    const auto again = philipsUploadReply(*svc->readUploadedOscarCard(store_.string()), false);
    EXPECT_FALSE(again.isMember("error"));
    EXPECT_EQ(again["sessions_found"].asInt(), 0);
    svc->oscarReaderForTest()->stop();
}

TEST_F(OscarUploadTest, APairedHelperIsAnOscarSourceAndAnOfflineOneIsSaidSo) {
    auto svc = collector();
    auto remote = std::make_shared<FakeRemote>();
    svc->setOscarRemote(remote, (dir_ / "slice").string());
    Settings none;
    none.data_dir.clear();
    svc->setOscarSettings(none);
    ASSERT_TRUE(svc->oscarSourcePaired()) << "a remembered helper is paired, with no OSCAR here";
    EXPECT_TRUE(svc->oscarHelperPaired());
    cardFile(store_, "P-SERIES/PXENC001/PROP.BIN");
    const auto read = svc->readUploadedOscarCard(store_.string());
    ASSERT_TRUE(read.has_value());
    EXPECT_EQ(remote->reads, 1);
    EXPECT_EQ(remote->last_root, store_.string());
    EXPECT_TRUE(fake_->runs.empty()) << "the helper's read, not OSCAR here";
    const auto reply = philipsUploadReply(*read, true);
    EXPECT_EQ(reply["oscar"].asString(), "helper_offline");
    EXPECT_NE(reply["error"].asString().find("not connected"), std::string::npos);
    EXPECT_NE(reply["error"].asString().find("waiting for HOME-PC"), std::string::npos);

    remote->outcome = Outcome::NothingNew;
    const auto ok = philipsUploadReply(*svc->readUploadedOscarCard(store_.string()), true);
    EXPECT_FALSE(ok.isMember("error"));
    EXPECT_EQ(ok["read_by"].asString(), "oscar_helper");
    svc->oscarReaderForTest()->stop();
}

// ── removed nights come back with the card (SDD-036 D2) ───────────────────

namespace {

std::string nightOfMs(long long ms) { return strDayForSessionStart(sessionClock(ms)); }

int sessionsOn(SQLiteDatabase& db, const std::string& device) {
    const auto rows = db.executeQuery("SELECT COUNT(*) AS v FROM cpap_sessions WHERE device_id = ?", {device});
    const Json::Value& v = rows[0]["v"];
    return v.isString() ? std::stoi(v.asString()) : v.asInt();
}

}  // namespace

TEST_F(OscarUploadTest, AnUploadedCardBringsBackTheRemovedNightsItHoldsAndNoOther) {
    constexpr long long kDay = 86400000LL;
    // The upload holds session 0x41. OSCAR's profile also holds 0x50, from
    // another card it read once, on a night removed too.
    cardFile(store_, "P-SERIES/PXENC001/PROP.BIN");
    cardFile(store_, "P-SERIES/PXENC001/P0/00000041.B01");
    oscar_->session(oscar_->hms_, 0x41, kT0, 10);
    oscar_->session(oscar_->hms_, 0x50, kT0 + 3 * kDay, 10);
    const std::string mine = nightOfMs(kT0), other = nightOfMs(kT0 + 3 * kDay);
    ASSERT_NE(mine, other);
    ASSERT_TRUE(db_->removeNight(kDevice, mine).ok);
    ASSERT_TRUE(db_->removeNight(kDevice, other).ok);

    OscarCardReader::Options o;
    o.settings = [this] { return settings_; };
    o.card_root = [this] { return store_.string(); };
    o.db = db_;
    o.device_id = kDevice;
    o.device_name = "DreamStation 2";
    o.runner = fake_;
    o.removed = [this] { return removedNightSet(*db_, kDevice); };
    OscarCardReader reader(o);

    // A read that is not an upload (a session close, "Read now") restores nothing.
    auto r = reader.readOnce(store_.string());
    EXPECT_TRUE(r.restored.empty());
    EXPECT_EQ(sessionsOn(*db_, kDevice), 0);

    r = reader.readOnce(store_.string(), true);
    ASSERT_EQ(r.restored.size(), 1u);
    EXPECT_EQ(nightKeyOf(r.restored[0]), mine);
    EXPECT_EQ(r.stored, 1);
    EXPECT_TRUE(db_->sessionExists(kDevice, sessionClock(kT0)));
    EXPECT_FALSE(db_->sessionExists(kDevice, sessionClock(kT0 + 3 * kDay))) << "not on this card";
    const auto still = removedNightSet(*db_, kDevice);
    EXPECT_FALSE(still.count(mine));
    EXPECT_TRUE(still.count(other)) << "another card's removed night stays removed";

    const auto reply = philipsUploadReply(r, false);
    ASSERT_EQ(reply["restored_nights"].size(), 1u);
    EXPECT_EQ(reply["restored_nights"][0].asString(), r.restored[0]);
}

TEST_F(OscarUploadTest, ANightStoredThenRemovedIsStoredAgainWhenTheCardIsUploadedAgain) {
    cardFile(store_, "P-SERIES/PXENC001/PROP.BIN");
    cardFile(store_, "P-SERIES/PXENC001/P0/00000041.B01");
    oscar_->session(oscar_->hms_, 0x41, kT0, 10);
    OscarCardReader::Options o;
    o.settings = [this] { return settings_; };
    o.card_root = [this] { return store_.string(); };
    o.db = db_;
    o.device_id = kDevice;
    o.runner = fake_;
    o.removed = [this] { return removedNightSet(*db_, kDevice); };
    OscarCardReader reader(o);
    reader.start();
    EXPECT_EQ(reader.readCard(store_.string(), "upload").get().stored, 1);
    ASSERT_TRUE(db_->removeNight(kDevice, nightOfMs(kT0)).ok);
    EXPECT_EQ(sessionsOn(*db_, kDevice), 0);

    // The same process: what it noted as stored is stored again.
    const auto r = reader.readCard(store_.string(), "upload").get();
    reader.stop();
    EXPECT_EQ(r.restored.size(), 1u);
    EXPECT_EQ(r.stored, 1);
    EXPECT_EQ(sessionsOn(*db_, kDevice), 1);
}

TEST_F(OscarUploadTest, ThroughAHelperTheRemovedNightIsRestoredBeforeItsSessionsAreAskedFor) {
    // The helper returns the heads; the restore happens in the want, so the
    // restored session is among the ones asked back.
    class HeadsRemote : public FakeRemote {
    public:
        std::vector<std::string> keys;
        std::vector<std::pair<long long, long long>> spans;
        std::vector<std::string> asked;
        RemoteRead read(const std::string& card_root, const Want& want, const std::string& d) override {
            FakeRemote::read(card_root, want, d);
            asked = want(keys, spans);
            RemoteRead r;
            r.outcome = Outcome::NothingNew;   // no slice: the store half is SDD-081's, tested there
            return r;
        }
    };
    cardFile(store_, "P-SERIES/PXENC001/PROP.BIN");
    cardFile(store_, "P-SERIES/PXENC001/P0/00000041.B01");
    auto remote = std::make_shared<HeadsRemote>();
    remote->keys = {std::string(kSerial) + "/65", std::string(kSerial) + "/80"};   // 0x41 on the card, 0x50 not
    remote->spans = {{kT0, kT0 + 600000}, {kT0 + 3 * 86400000LL, kT0 + 3 * 86400000LL + 600000}};
    ASSERT_TRUE(db_->removeNight(kDevice, nightOfMs(kT0)).ok);
    ASSERT_TRUE(db_->removeNight(kDevice, nightOfMs(kT0 + 3 * 86400000LL)).ok);

    OscarCardReader::Options o;
    o.settings = [this] { return settings_; };
    o.card_root = [this] { return store_.string(); };
    o.db = db_;
    o.device_id = kDevice;
    o.runner = fake_;
    o.removed = [this] { return removedNightSet(*db_, kDevice); };
    o.remote = remote;
    OscarCardReader reader(o);
    const auto r = reader.readOnce(store_.string(), true);
    ASSERT_EQ(r.restored.size(), 1u);
    EXPECT_EQ(nightKeyOf(r.restored[0]), nightOfMs(kT0));
    ASSERT_EQ(remote->asked.size(), 1u);
    EXPECT_EQ(remote->asked[0], std::string(kSerial) + "/65");
    EXPECT_TRUE(removedNightSet(*db_, kDevice).count(nightOfMs(kT0 + 3 * 86400000LL)));
}

// ── D10: a DreamStation card whose files are plain ─────────────────────────

TEST_F(OscarUploadTest, APlainDreamStationCardIsReadAsAPlainCardInAFolderIs) {
    if (!fs::exists(fixtures() / "ds2_live")) GTEST_SKIP() << "fixture not found";
    fs::create_directories(staging_ / "wrapped");
    fs::copy(fixtures() / "ds2_live", staging_ / "wrapped", fs::copy_options::recursive);
    const auto card = plainPhilipsCardIn(staging_.string());
    ASSERT_TRUE(card.has_value());
    EXPECT_EQ(fs::path(*card).filename().string(), "PXLIVE01");
    EXPECT_EQ(classifyUploadedCard(staging_.string()), UploadedCard::Unknown)
        << "the installer's card read is not changed by this";
    EXPECT_STREQ(uploadedCardName(UploadedCard::Philips), "philips");

    const auto nights = cardNights(UploadedCard::Philips, *card);
    ASSERT_FALSE(nights.empty());
    std::string err;
    ASSERT_TRUE(keepPhilipsUpload(*card, store_.string(), err)) << err;
    auto c = importCardSessions(*db_, UploadedCard::Philips, store_.string(), kDevice, "DreamStation");
    EXPECT_GT(c.found, 0);
    EXPECT_EQ(c.imported, c.found);
    EXPECT_EQ(c.nights, nights);
    c = importCardSessions(*db_, UploadedCard::Philips, store_.string(), kDevice, "DreamStation");
    EXPECT_EQ(c.imported, 0);
    EXPECT_EQ(c.already_stored, c.found) << "uploaded again: nothing new";
}

TEST_F(OscarUploadTest, AnEncryptedCardIsNeverTakenForAPlainOne) {
    if (!fs::exists(fixtures() / "ds2_encrypted")) GTEST_SKIP() << "fixture not found";
    encryptedZip();
    EXPECT_FALSE(plainPhilipsCardIn(staging_.string()).has_value());
    EXPECT_TRUE(encryptedPhilipsCardIn(staging_.string()).has_value());
}

TEST(OscarUploadReply, EveryOutcomeButAnImportIsAnErrorThatSaysWhatToDo) {
    for (Outcome o : {Outcome::NotSetUp, Outcome::NoProgram, Outcome::NotSupported, Outcome::NoProfile,
                      Outcome::Busy, Outcome::Timeout, Outcome::ReadNothing, Outcome::Failed,
                      Outcome::HelperOffline, Outcome::HelperUpdate}) {
        OscarCardReader::ReadResult r;
        r.outcome = o;
        r.message = "why";
        const auto reply = philipsUploadReply(r, false);
        EXPECT_FALSE(reply["error"].asString().empty()) << outcomeName(o);
        EXPECT_EQ(reply["oscar"].asString(), outcomeName(o));
        EXPECT_FALSE(reply.isMember("dates")) << outcomeName(o);
    }
    OscarCardReader::ReadResult r;
    r.outcome = Outcome::NoProfile;
    EXPECT_NE(philipsUploadReply(r, false)["error"].asString().find("create a profile named hms-cpap"),
              std::string::npos);
}
