//
// test_SessionEndOnMqtt.cpp: issue #57, the session's end on MQTT.
//
// session_active and session_status (cpap/<id>/realtime/...) went ON /
// in_progress on every burst that saw the night's files grow, and back to
// OFF / completed only when the NIGHT was announced: an hour after its folder
// last grew (SDD-046, announceQuietNights). The session itself closed on the
// first burst that found its files unchanged, which is what the web UI shows,
// so a streamed night showed done in the UI and in_progress in Home Assistant
// for the hour after. Reported against a Sleep Bridge night: mask off at
// 8:07, the files stopped, the UI closed the night, MQTT said completed at
// 9:02.
//
// Now: a session whose files have stopped and whose mask a STREAMING source
// said off is published completed on that burst. A card read a burst later
// (ez Share, FlashAir, a folder) keeps the hour, as does the startup OFF.
//
// The decision runs with a publisher on no broker (each publish a guarded
// no-op that answers true). The broker test talks to a throwaway broker
// (TestBroker.h) and skips without one.
//
#include <gtest/gtest.h>

#include "TestBroker.h"
#include "database/SQLiteDatabase.h"
#include "mqtt/LiveState.h"
#include "mqtt_client.h"
#include "services/BurstCollectorService.h"
#include "services/DataPublisherService.h"
#include "utils/NightQuiet.h"

#include "PosixCompat.h"   // getpid, setenv/unsetenv on MSVC too

#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>

using namespace hms_cpap;
using namespace std::chrono;
namespace fs = std::filesystem;

namespace {

LiveState::Publish nowhere() {
    return [](const std::string&, const std::string&, int, bool) { return true; };
}

class SessionEndOnMqttTest : public ::testing::Test {
protected:
    void SetUp() override {
        device_ = "i57_" + std::to_string(::getpid()) + "_" +
                  ::testing::UnitTest::GetInstance()->current_test_info()->name();
        path_ = (fs::temp_directory_path() / (device_ + ".db")).string();
        fs::remove(path_);
        db_ = std::make_shared<SQLiteDatabase>(path_);
        ASSERT_TRUE(db_->connect());
        now_   = time_point_cast<seconds>(system_clock::now());
        start_ = now_ - hours(7);
    }
    void TearDown() override {
        db_.reset();
        fs::remove(path_);
    }

    /// A collector on this test's device, its publisher on `mqtt` (null: no
    /// broker), its live state `live`.
    std::unique_ptr<BurstCollectorService> collector(std::shared_ptr<hms::MqttClient> mqtt,
                                                     std::shared_ptr<LiveState> live) {
        setenv("CPAP_DEVICE_ID", device_.c_str(), 1);
        auto svc = std::make_unique<BurstCollectorService>(60);
        auto pub = std::make_unique<DataPublisherService>(mqtt, db_);
        unsetenv("CPAP_DEVICE_ID");
        svc->injectDependenciesForTest(db_, nullptr, std::move(pub));
        svc->setSourceForTest("local");
        svc->setLiveState(live);
        return svc;
    }

    std::shared_ptr<LiveState> live() {
        return std::make_shared<LiveState>(nowhere(), device_, "Test CPAP", now_ - hours(8));
    }

    void bridge(LiveState& l, bool on, const std::string& session = "20261105_010913") {
        l.mask({LiveState::kSleepBridge, "TAP-1", on, start_, session}, now_);
    }

    std::string device_, path_;
    std::shared_ptr<SQLiteDatabase> db_;
    system_clock::time_point now_, start_;
};

}  // namespace

// ── the decision ────────────────────────────────────────────────────────────

TEST_F(SessionEndOnMqttTest, WithNoLiveSourceTheEndWaitsForTheNightsHour) {
    auto svc = collector(nullptr, nullptr);
    EXPECT_FALSE(svc->publishStreamedSessionEndForTest(start_));
}

TEST_F(SessionEndOnMqttTest, NotWhileTheStreamSaysTheMaskIsOn) {
    auto l = live();
    auto svc = collector(nullptr, l);
    bridge(*l, true);
    EXPECT_FALSE(svc->publishStreamedSessionEndForTest(start_))
        << "files stopped with the mask still on is a pause, not an end";
}

TEST_F(SessionEndOnMqttTest, TheStreamsMaskOffEndsTheSessionOnceAndOnlyForward) {
    auto l = live();
    auto svc = collector(nullptr, l);
    bridge(*l, true);
    bridge(*l, false);
    EXPECT_TRUE(l->streamSaidOff());
    EXPECT_TRUE(svc->publishStreamedSessionEndForTest(start_));
    EXPECT_FALSE(svc->publishStreamedSessionEndForTest(start_)) << "once per end";
    EXPECT_FALSE(svc->publishStreamedSessionEndForTest(start_ - hours(2)))
        << "an earlier session of the same night, listed on the same burst";
    EXPECT_TRUE(svc->publishStreamedSessionEndForTest(start_ + hours(1)))
        << "a later session's end is its own";
}

TEST_F(SessionEndOnMqttTest, ACardsMaskOffOrTheStartupOffKeepTheHour) {
    auto card = live();
    card->mask({LiveState::kLocal, "card", true, start_, "20261105_010913"}, now_);
    card->mask({LiveState::kLocal, "card", false, start_, "20261105_010913"}, now_);
    EXPECT_FALSE(card->streamSaidOff());
    EXPECT_FALSE(collector(nullptr, card)->publishStreamedSessionEndForTest(start_));

    auto startup = live();
    startup->tick(now_);   // nobody said anything for the settle: OFF, source "startup"
    EXPECT_FALSE(startup->maskOn());
    EXPECT_FALSE(startup->streamSaidOff());
    EXPECT_FALSE(collector(nullptr, startup)->publishStreamedSessionEndForTest(start_));
}

TEST_F(SessionEndOnMqttTest, EveryStreamingSourceCounts) {
    for (const char* src : {LiveState::kSleepBridge, LiveState::kMuleBle, LiveState::kAs11Ble})
        EXPECT_TRUE(LiveState::isStreamingSource(src)) << src;
    for (const char* src : {LiveState::kEzShare, LiveState::kFlashAir, LiveState::kLocal, "startup"})
        EXPECT_FALSE(LiveState::isStreamingSource(src)) << src;
}

// ── on a broker: what Home Assistant reads ──────────────────────────────────

namespace {

/// The retained value of each topic under `prefix`, read by a fresh client.
std::map<std::string, std::string> retained(const std::string& prefix, const std::string& id) {
    hms::MqttClient reader(testMqttConfig(id));
    std::map<std::string, std::string> seen;
    std::mutex mu;
    if (!reader.connect()) return seen;
    reader.subscribe(prefix + "#",
                     [&](const std::string& t, const std::string& p) {
                         std::lock_guard<std::mutex> lk(mu);
                         seen[t] = p;
                     },
                     1);
    std::this_thread::sleep_for(milliseconds(700));
    reader.disconnect();
    std::lock_guard<std::mutex> lk(mu);
    return seen;
}

}  // namespace

// The reported night, replayed: in progress while the files grew, the files
// stopped, the bridge's mask off. Before the fix nothing changed on MQTT until
// the night's hour; now the burst that sees it says completed / OFF.
TEST_F(SessionEndOnMqttTest, OnABrokerTheStreamedNightEndsWhenTheMaskComesOff) {
    auto mqtt = std::make_shared<hms::MqttClient>(testMqttConfig(device_ + "_pub"));
    if (!mqtt->connect()) GTEST_SKIP() << "no test broker (HMS_TEST_MQTT)";
    auto l   = live();
    auto svc = collector(mqtt, l);
    const std::string rt = "cpap/" + device_ + "/realtime/";

    // In progress: what every growing burst publishes.
    CPAPSession s;
    s.device_id        = device_;
    s.device_name      = "AirSense 11";
    s.session_start    = start_;
    s.duration_seconds = 7 * 3600;
    ASSERT_TRUE(db_->saveSession(s));
    setenv("CPAP_DEVICE_ID", device_.c_str(), 1);
    DataPublisherService pub(mqtt, db_);
    unsetenv("CPAP_DEVICE_ID");
    pub.publishSession(s);
    bridge(*l, true);
    auto r = retained(rt, device_ + "_r1");
    ASSERT_EQ(r[rt + "session_status"], "in_progress");
    ASSERT_EQ(r[rt + "session_active"], "ON");

    // The files stop; the night's last growth was just now, so the hour has
    // not passed and the night is not announced. This was the whole story
    // before: in_progress for the next hour.
    ASSERT_TRUE(noteNightGrowth(*db_, device_, "20261104", start_, now_ - minutes(5)));
    EXPECT_EQ(svc->announceQuietNightsForTest(now_), 0);
    r = retained(rt, device_ + "_r2");
    EXPECT_EQ(r[rt + "session_status"], "in_progress") << "the hour has not passed";

    // The bridge saw the mask come off: the burst that finds the files
    // stopped ends the session on MQTT now.
    bridge(*l, false);
    EXPECT_TRUE(svc->publishStreamedSessionEndForTest(start_));
    r = retained(rt, device_ + "_r3");
    EXPECT_EQ(r[rt + "session_status"], "completed");
    EXPECT_EQ(r[rt + "session_active"], "OFF");

    // Clean up the retained topics this test left.
    for (const auto& t : {"session_status", "session_active", "session_duration",
                          "last_session_time"})
        mqtt->publish(rt + t, "", 1, true);
    std::this_thread::sleep_for(milliseconds(200));
    mqtt->disconnect();
}
