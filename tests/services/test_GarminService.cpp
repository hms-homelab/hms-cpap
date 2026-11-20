// SDD-078 section 4: D2 (events, never a timer), D6 (one recording per sleep
// period) and the upload copy replaced, against a local fake Garmin and a
// temporary SQLite database. No real account is used.
#include "services/GarminService.h"

#include "FakeGarminServer.h"
#include "database/SQLiteDatabase.h"
#include "services/OximetryImport.h"
#include "services/OximetryOutbox.h"
#include "utils/AppConfig.h"
#include "utils/MachineClock.h"
#include "utils/OximetryDevice.h"

#include <gtest/gtest.h>

#include <chrono>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <thread>
#ifndef _WIN32
#include <unistd.h>
#else
#include <process.h>
#endif

using namespace hms_cpap;
using nlohmann::json;
namespace fs = std::filesystem;

namespace {

// The night of 2026-11-18 on a machine at UTC+10: asleep 23:00 to 06:00,
// 13:00 to 20:00 UTC. Garmin files it under 2026-11-19, the morning.
constexpr long long kStart = 1795006800LL;
constexpr long long kMorning = kStart + 9 * 3600;   // 08:00 on the machine

std::string gmt(long long secs) {
    const std::time_t t = static_cast<std::time_t>(secs);
    std::tm tm{};
    gmtime_r(&t, &tm);
    char buf[32];
    std::strftime(buf, sizeof buf, "%Y-%m-%dT%H:%M:%S.0", &tm);
    return buf;
}

std::string sleepJson(long long start, int hours = 7) {
    json j;
    j["dailySleepDTO"] = {{"sleepStartTimestampGMT", start * 1000},
                          {"sleepEndTimestampGMT", (start + hours * 3600) * 1000}};
    json s = json::array(), hr = json::array();
    for (int i = 0; i < hours * 60; ++i) {
        s.push_back({{"epochTimestamp", gmt(start + i * 60)}, {"spo2Reading", 95}});
        hr.push_back({{"startGMT", (start + i * 60) * 1000}, {"value", 60}});
    }
    j["wellnessEpochSPO2DataDTOList"] = s;
    j["sleepHeartRate"] = hr;
    return j.dump();
}

long long asNum(const Json::Value& v) {
    if (v.isNull()) return -1;
    if (v.isString()) return std::stoll(v.asString());
    return v.asInt64();
}

class GarminServiceTest : public ::testing::Test {
protected:
    void SetUp() override {
        path_ = (fs::temp_directory_path() / ("hms_garmin_" + std::to_string(::getpid()) + ".db")).string();
        fs::remove(path_);
        auto lite = std::make_shared<SQLiteDatabase>(path_);
        ASSERT_TRUE(lite->connect());
        db_ = lite;
        // The machine's clock is UTC+10, learned (SDD-074).
        MachineClock c;
        c.zone_s = 10 * 3600;
        c.zone_known = true;
        ASSERT_TRUE(saveMachineClock(*db_, "cpap", c, kMorning));

        config_.garmin.enabled  = true;
        config_.garmin.email    = "someone@example.com";
        config_.garmin.password = "right-password";
        now_ = kMorning;
    }

    void TearDown() override {
        svc_.reset();
        db_.reset();
        for (const auto* s : {"", "-wal", "-shm"}) {
            std::error_code ec;
            fs::remove(path_ + s, ec);
        }
    }

    GarminService& svc() {
        if (!svc_) {
            GarminService::Deps d;
            d.db = db_;
            d.config = &config_;
            d.cpap_device_id = "cpap";
            d.endpoints.sso = g_.base();
            d.endpoints.diauth = g_.base() + "/di-oauth2-service/oauth/token";
            d.endpoints.connectapi = g_.base();
            d.now = [this] { return now_; };
            svc_ = std::make_unique<GarminService>(std::move(d));
        }
        return *svc_;
    }

    Json::Value rows() {
        return db_->executeQuery(
            "SELECT filename, source, device_name, start_time, valid_samples FROM oximetry_sessions "
            "ORDER BY filename", {});
    }
    long long samplesOf(const std::string& filename) {
        const auto r = db_->executeQuery(
            "SELECT COUNT(*) AS n FROM oximetry_samples s JOIN oximetry_sessions o ON o.id = s.oximetry_session_id "
            "WHERE o.filename = ?", {filename});
        return asNum(r[0u]["n"]);
    }

    garminfake::FakeGarmin g_;
    std::string path_;
    std::shared_ptr<IDatabase> db_;
    AppConfig config_;
    long long now_ = 0;
    std::unique_ptr<GarminService> svc_;
};

}  // namespace

TEST_F(GarminServiceTest, NoFetchHappensWithoutAnEvent) {
    g_.sleep_by_date["2026-11-19"] = sleepJson(kStart);
    svc().start();   // the worker, waiting on its queue
    (void)svc().status();
    std::this_thread::sleep_for(std::chrono::milliseconds(300));
    svc().stop();
    EXPECT_EQ(g_.server.total(), 0u) << "no event, no request";
    EXPECT_EQ(rows().size(), 0u);
}

TEST_F(GarminServiceTest, ANightCloseFetchesThatNight) {
    g_.sleep_by_date["2026-11-19"] = sleepJson(kStart);
    svc().handle(GarminService::Event::NightClosed, "20261118");

    const auto r = rows();
    ASSERT_EQ(r.size(), 1u);
    EXPECT_EQ(r[0u]["filename"].asString(), "garmin-" + std::to_string(kStart * 1000));
    EXPECT_EQ(r[0u]["source"].asString(), "garmin");
    EXPECT_EQ(r[0u]["device_name"].asString(), "Garmin fenix 7X");
    EXPECT_EQ(r[0u]["start_time"].asString(), "2026-11-18 23:00:00") << "on the machine's clock (D5)";
    EXPECT_EQ(samplesOf(r[0u]["filename"].asString()), 420);

    // D1: Garmin's tokens are settings now.
    EXPECT_FALSE(config_.garmin.refresh_token.empty());
    EXPECT_EQ(config_.garmin.display_name, "runner-42");
    const auto st = svc().status();
    EXPECT_EQ(st["state"].asString(), "signed_in");
    EXPECT_EQ(st["last_fetch"]["stored"].asInt(), 1);
    EXPECT_TRUE(st["last_fetch"]["ok"].asBool());
}

// SDD-079 D4: the stored night goes to CpapDash as hms-json on Garmin's own
// instants (clock "utc"), never the machine's wall clock it is stored on.
TEST_F(GarminServiceTest, AStoredNightIsWrittenToTheOximetryOutboxOnItsUtcInstants) {
    const fs::path dir = fs::temp_directory_path() / ("hms_garmin_outbox_" + std::to_string(::getpid()));
    std::error_code gone;
    fs::remove_all(dir, gone);
    oximetry_outbox::setRoot(oximetry_outbox::rootFor(dir.string()));
    g_.sleep_by_date["2026-11-19"] = sleepJson(kStart);
    svc().handle(GarminService::Event::NightClosed, "20261118");
    oximetry_outbox::setRoot("");

    const std::string name = "garmin-" + std::to_string(kStart * 1000);
    const fs::path file = fs::path(oximetry_outbox::rootFor(dir.string())) / (name + ".json");
    Json::Value body;
    {
        // Closed before the folder is removed: Windows will not delete a
        // folder with a file in it still open.
        std::ifstream in(file, std::ios::binary);
        ASSERT_TRUE(in) << file;
        Json::CharReaderBuilder rb;
        ASSERT_TRUE(Json::parseFromStream(rb, in, &body, nullptr));
    }
    EXPECT_EQ(body["clock"].asString(), "utc");
    EXPECT_EQ(body["samples"].size(), 420u);
    EXPECT_EQ(body["samples"][0u][0u].asInt64(), kStart) << "the instant; stored as 23:00 on the machine";
    EXPECT_EQ(body["samples"][0u][2u].asInt(), 60) << "the pulse Garmin has goes";
    const auto meta = oximetry_outbox::metaFor(oximetry_outbox::rootFor(dir.string()), name + ".json");
    ASSERT_TRUE(meta);
    EXPECT_EQ(meta->source, "garmin");
    EXPECT_EQ(meta->device_name, "Garmin fenix 7X");
    fs::remove_all(dir, gone);
}

TEST_F(GarminServiceTest, ALaterEventFetchesANightStillMissing) {
    // At 06:30 on the machine the watch has not synced the end of the night.
    now_ = kStart + 7 * 3600 - 1800;
    g_.sleep_by_date["2026-11-19"] = sleepJson(kStart);
    svc().handle(GarminService::Event::NightClosed, "20261118");
    EXPECT_EQ(rows().size(), 0u) << "an incomplete sleep period is left for the next event";

    // The dashboard is opened later: the night is asked for again and lands.
    now_ = kMorning;
    svc().handle(GarminService::Event::NightListOpened);
    EXPECT_EQ(rows().size(), 1u);

    // And no further asks for it once it is in.
    const auto asked = g_.server.count("/wellness-service/wellness/dailySleepData/runner-42");
    svc().handle(GarminService::Event::FetchNow);
    const auto after = g_.server.count("/wellness-service/wellness/dailySleepData/runner-42");
    EXPECT_EQ(after - asked, 3u) << "only the three other days of the window, still empty, are asked";
}

TEST_F(GarminServiceTest, TheButtonFetches) {
    g_.sleep_by_date["2026-11-19"] = sleepJson(kStart);
    EXPECT_TRUE(svc().fetchNow());
    svc().handle(GarminService::Event::FetchNow);
    EXPECT_EQ(rows().size(), 1u);
}

TEST_F(GarminServiceTest, TheButtonIsRefusedWhenGarminIsNotSetUp) {
    config_.garmin.password.clear();
    EXPECT_FALSE(svc().fetchNow());
    config_.garmin.enabled = false;
    config_.garmin.password = "right-password";
    EXPECT_FALSE(svc().fetchNow());
    svc().handle(GarminService::Event::FetchNow);
    EXPECT_EQ(g_.server.total(), 0u);
    EXPECT_EQ(svc().status()["state"].asString(), "off");
}

TEST_F(GarminServiceTest, TheSameNightFetchedTwiceIsOneRecording) {
    g_.sleep_by_date["2026-11-19"] = sleepJson(kStart);
    svc().handle(GarminService::Event::NightClosed, "20261118");
    // Fetched again regardless of the have-check: the same row, replaced.
    GarminConnectClient c([this] {
        GarminEndpoints e;
        e.sso = g_.base();
        e.diauth = g_.base() + "/di-oauth2-service/oauth/token";
        e.connectapi = g_.base();
        return e;
    }());
    ASSERT_TRUE(c.signIn("someone@example.com", "right-password").ok());
    const auto again = convertGarminSleep(c.sleepData("runner-42", "2026-11-19").body, now_,
                                          [](long long) { return 36000LL; });
    ASSERT_EQ(again.verdict, GarminNight::Verdict::Ready);
    ASSERT_TRUE(db_->saveOximetrySessionFrom(kOximetryDeviceId, again.session, {"garmin", "Garmin fenix 7X"}));
    EXPECT_EQ(rows().size(), 1u);
    EXPECT_EQ(samplesOf(again.session.filename), 420) << "replaced, not doubled";
}

TEST_F(GarminServiceTest, TheNativeFetchReplacesTheUploadedCopy) {
    // The reference importer's CSV for the same night, through the upload API.
    std::string csv = "Time,Oxygen Level,Pulse Rate,Motion,O2 Reminder,PR Reminder\n";
    for (int i = 0; i < 60; ++i) {
        char line[64];
        std::snprintf(line, sizeof line, "23:%02d:00 Nov 18 2026,96,61,0,,\n", i);
        csv += line;
    }
    const auto up = importOximetryUpload(*db_, csv, "garmin-watch-2026-11-19.csv", uploadOrigin("", ""));
    ASSERT_FALSE(up.isMember("error")) << up["error"].asString();
    // And a ring's upload the same night, which stays.
    std::string ring = "Time,Oxygen Level,Pulse Rate,Motion,O2 Reminder,PR Reminder\n";
    ring += "23:30:00 Nov 18 2026,93,58,0,,\n23:30:04 Nov 18 2026,93,58,0,,\n";
    ASSERT_FALSE(importOximetryUpload(*db_, ring, "O2Ring_20261118233000.csv", uploadOrigin("", "")).isMember("error"));
    ASSERT_EQ(rows().size(), 2u);

    g_.sleep_by_date["2026-11-19"] = sleepJson(kStart);
    svc().handle(GarminService::Event::NightClosed, "20261118");
    const auto r = rows();
    ASSERT_EQ(r.size(), 2u);
    EXPECT_EQ(r[0u]["filename"].asString(), "O2Ring_20261118233000.csv") << "the ring's upload is kept";
    EXPECT_EQ(r[1u]["source"].asString(), "garmin") << "the uploaded Garmin copy is gone";
    EXPECT_EQ(samplesOf("garmin-watch-2026-11-19.csv"), 0);
}

TEST_F(GarminServiceTest, AGarminNightLeavesTheRingsLiveSession) {
    const auto wall = std::chrono::system_clock::from_time_t(kStart + 10 * 3600 + 600);   // 23:10 machine
    ASSERT_TRUE(db_->saveLiveOximetrySample(kOximetryDeviceId, wall, 95, 60, 0));
    g_.sleep_by_date["2026-11-19"] = sleepJson(kStart);
    svc().handle(GarminService::Event::NightClosed, "20261118");
    const auto r = db_->executeQuery("SELECT filename FROM oximetry_sessions WHERE filename LIKE 'live_%'", {});
    EXPECT_EQ(r.size(), 1u) << "only the ring's own file supersedes its live session";
}

TEST_F(GarminServiceTest, MfaIsAskedInlineAndTheCodeFinishesTheSignIn) {
    g_.mfa = true;
    g_.sleep_by_date["2026-11-19"] = sleepJson(kStart);
    svc().handle(GarminService::Event::FetchNow);
    EXPECT_EQ(svc().status()["state"].asString(), "mfa_required");
    EXPECT_EQ(rows().size(), 0u);

    // Another event while the code is awaited does not sign in again (that
    // would send another code).
    const auto logins = g_.server.count("/mobile/api/login");
    svc().handle(GarminService::Event::NightListOpened);
    EXPECT_EQ(g_.server.count("/mobile/api/login"), logins);

    EXPECT_FALSE(svc().submitMfaCode(""));
    ASSERT_TRUE(svc().submitMfaCode("123456"));
    svc().handle(GarminService::Event::MfaCode, "", "123456");
    EXPECT_EQ(svc().status()["state"].asString(), "signed_in");
    EXPECT_EQ(rows().size(), 1u) << "the code is an event too: the night is fetched";
}

TEST_F(GarminServiceTest, AChangedSignInIsTheClearErrorOnTheCard) {
    g_.changed_sign_in = true;
    svc().handle(GarminService::Event::FetchNow);
    const auto st = svc().status();
    EXPECT_EQ(st["state"].asString(), "error");
    EXPECT_EQ(st["error_kind"].asString(), "changed");
    EXPECT_EQ(st["error"].asString(), "Garmin changed its sign-in; update hms-cpap");
}

TEST_F(GarminServiceTest, ARefusedSessionSignsInAgainWithTheSavedPassword) {
    g_.sleep_by_date["2026-11-19"] = sleepJson(kStart);
    // Saved from an earlier run: a session Garmin no longer takes.
    config_.garmin.access_token = "stale";
    config_.garmin.refresh_token = "stale-refresh";
    config_.garmin.client_id = "GARMIN_CONNECT_MOBILE_ANDROID_DI";
    g_.refuse_refresh = true;
    svc().handle(GarminService::Event::NightClosed, "20261118");
    EXPECT_EQ(g_.server.count("/mobile/api/login"), 1u) << "nobody at the keyboard (D1)";
    EXPECT_EQ(rows().size(), 1u);
}

TEST_F(GarminServiceTest, TheDatesAreTheClosedNightAndTheLastThreeDays) {
    // A night closed long ago is asked for besides the window.
    const auto d = svc().datesToFetch("20261101");
    EXPECT_EQ(d, (std::set<std::string>{"2026-11-02", "2026-11-16", "2026-11-17", "2026-11-18", "2026-11-19"}));
}
