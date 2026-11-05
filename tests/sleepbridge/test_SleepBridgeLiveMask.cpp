// The Sleep Bridge's mask on MQTT: a Home Assistant binary sensor on the
// retained cpap/<device_id>/realtime/mask, announced at start and when a
// bridge first shows up this run, and a retained state that is right after a
// restart. The last test talks to a throwaway broker (TestBroker.h) and skips
// without one.

#include "TestBroker.h"
#include "mqtt_client.h"
#include "sleepbridge/LiveMask.h"
#include "sleepbridge/LiveNight.h"

#include <gtest/gtest.h>
#include <json/json.h>
#include <unistd.h>

#include <chrono>
#include <map>
#include <mutex>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

using hms_cpap::sleepbridge::LiveMask;
using hms_cpap::sleepbridge::LiveNight;
using hms_cpap::sleepbridge::LiveSink;

namespace {

struct Sent {
    std::string topic, payload;
    int         qos;
    bool        retain;
};

Json::Value parse(const std::string& s) {
    Json::Value v;
    Json::CharReaderBuilder rb;
    std::istringstream in(s);
    std::string errs;
    EXPECT_TRUE(Json::parseFromStream(rb, in, &v, &errs)) << errs;
    return v;
}

const auto kT0 = std::chrono::system_clock::time_point{} + std::chrono::hours(500000);

struct Recorder {
    std::vector<Sent> out;
    bool              up{true};
    LiveMask::Publish publish() {
        return [this](const std::string& t, const std::string& p, int q, bool r) {
            if (!up) return false;
            out.push_back({t, p, q, r});
            return true;
        };
    }
    std::vector<Sent> on(const std::string& topic) const {
        std::vector<Sent> v;
        for (const auto& s : out)
            if (s.topic == topic) v.push_back(s);
        return v;
    }
};

}  // namespace

TEST(SleepBridgeLiveMask, AnnouncesABinarySensorOnTheMaskTopicOfTheSameDevice) {
    Recorder rec;
    LiveMask mask(rec.publish(), "cpap_test", "Test CPAP", kT0);
    ASSERT_TRUE(mask.announce());

    const auto cfgs = rec.on("homeassistant/binary_sensor/cpap_test/mask/config");
    ASSERT_EQ(cfgs.size(), 1u);
    EXPECT_TRUE(cfgs[0].retain);
    EXPECT_EQ(cfgs[0].qos, 1);
    const auto c = parse(cfgs[0].payload);
    EXPECT_EQ(c["name"].asString(), "Mask");
    EXPECT_EQ(c["unique_id"].asString(), "cpap_test_mask");
    EXPECT_EQ(c["state_topic"].asString(), "cpap/cpap_test/realtime/mask");
    EXPECT_EQ(c["payload_on"].asString(), "ON");
    EXPECT_EQ(c["payload_off"].asString(), "OFF");
    EXPECT_EQ(c["device_class"].asString(), "running");
    // The device every other hms-cpap sensor belongs to: identifiers = the id.
    ASSERT_EQ(c["device"]["identifiers"].size(), 1u);
    EXPECT_EQ(c["device"]["identifiers"][0].asString(), "cpap_test");
    EXPECT_EQ(c["device"]["name"].asString(), "Test CPAP");
    // Announcing says nothing about the mask itself.
    EXPECT_TRUE(rec.on("cpap/cpap_test/realtime/mask").empty());
}

TEST(SleepBridgeLiveMask, ABridgeSeenForTheFirstTimeThisRunAnnouncesAgainOnce) {
    Recorder rec;
    LiveMask mask(rec.publish(), "cpap_test", "Test CPAP", kT0);
    const std::string cfg = "homeassistant/binary_sensor/cpap_test/mask/config";
    mask.sourceSeen("TAP-S3-00000123");
    EXPECT_EQ(rec.on(cfg).size(), 1u);
    mask.sourceSeen("TAP-S3-00000123");
    EXPECT_EQ(rec.on(cfg).size(), 1u) << "the same bridge again is not news";
    mask.sourceSeen("TAP-S3-00000456");
    EXPECT_EQ(rec.on(cfg).size(), 2u);
}

TEST(SleepBridgeLiveMask, OnAndOffAreRetainedOnTheRealtimeTopic) {
    Recorder rec;
    LiveMask mask(rec.publish(), "cpap_test", "Test CPAP", kT0);
    mask.set("TAP-S3-00000123", true);
    mask.set("TAP-S3-00000123", false);
    const auto s = rec.on("cpap/cpap_test/realtime/mask");
    ASSERT_EQ(s.size(), 2u);
    EXPECT_EQ(s[0].payload, "ON");
    EXPECT_EQ(s[1].payload, "OFF");
    EXPECT_TRUE(s[0].retain);
    EXPECT_TRUE(s[1].retain);
}

// After a restart the broker may still hold an ON from a night that ended
// while hms-cpap was away. Nothing said for the settle time: OFF, once.
TEST(SleepBridgeLiveMask, AfterARestartWithNoTherapyTheRetainedStateBecomesOff) {
    Recorder rec;
    LiveMask mask(rec.publish(), "cpap_test", "Test CPAP", kT0, std::chrono::seconds(300));
    const std::string state = "cpap/cpap_test/realtime/mask";
    mask.tick(kT0 + std::chrono::seconds(30));
    mask.tick(kT0 + std::chrono::seconds(299));
    EXPECT_TRUE(rec.on(state).empty()) << "nothing before the settle time";
    mask.tick(kT0 + std::chrono::seconds(300));
    ASSERT_EQ(rec.on(state).size(), 1u);
    EXPECT_EQ(rec.on(state)[0].payload, "OFF");
    EXPECT_TRUE(rec.on(state)[0].retain);
    mask.tick(kT0 + std::chrono::seconds(330));
    mask.tick(kT0 + std::chrono::hours(5));
    EXPECT_EQ(rec.on(state).size(), 1u) << "said once";
}

// A night still running when hms-cpap restarts writes therapy within a
// minute: ON, and no OFF in between for an automation to act on.
TEST(SleepBridgeLiveMask, AfterARestartMidNightTheMaskStaysOnWithoutAnOffBlip) {
    Recorder rec;
    LiveMask mask(rec.publish(), "cpap_test", "Test CPAP", kT0, std::chrono::seconds(300));
    mask.tick(kT0 + std::chrono::seconds(30));
    mask.set("TAP-S3-00000123", true);   // the first therapy write after the start
    mask.tick(kT0 + std::chrono::seconds(300));
    mask.tick(kT0 + std::chrono::seconds(600));
    const auto s = rec.on("cpap/cpap_test/realtime/mask");
    ASSERT_EQ(s.size(), 1u);
    EXPECT_EQ(s[0].payload, "ON");
}

TEST(SleepBridgeLiveMask, ABrokerAwayAtTheSettleTimeIsToldAtTheNextTick) {
    Recorder rec;
    rec.up = false;
    LiveMask mask(rec.publish(), "cpap_test", "Test CPAP", kT0, std::chrono::seconds(300));
    mask.tick(kT0 + std::chrono::seconds(300));
    EXPECT_TRUE(rec.out.empty());
    rec.up = true;
    mask.tick(kT0 + std::chrono::seconds(330));
    const auto s = rec.on("cpap/cpap_test/realtime/mask");
    ASSERT_EQ(s.size(), 1u);
    EXPECT_EQ(s[0].payload, "OFF");
}

// The live night drives it as main.cpp wires it: a therapy write is ON at
// once, the STR after therapy is OFF at once, and five quiet minutes is OFF.
TEST(SleepBridgeLiveMask, TheLiveNightDrivesItOnTheWriteThatChangesIt) {
    Recorder rec;
    LiveMask mask(rec.publish(), "cpap_test", "Test CPAP", kT0, std::chrono::seconds(300));
    LiveSink sink;
    sink.mask = [&](bool on) { mask.set("TAP-S3-00000123", on); };
    LiveNight night("/nonexistent", std::move(sink), std::chrono::seconds(300));
    const std::string state = "cpap/cpap_test/realtime/mask";

    night.onWritten({"DATALOG/20261030/20261030_223000_BRP.edf"}, kT0 + std::chrono::seconds(60));
    ASSERT_EQ(rec.on(state).size(), 1u);
    EXPECT_EQ(rec.on(state).back().payload, "ON");
    night.onWritten({"DATALOG/20261030/20261030_223000_PLD.edf"}, kT0 + std::chrono::seconds(120));
    EXPECT_EQ(rec.on(state).size(), 1u) << "still on, nothing new said";
    night.onWritten({"STR.edf"}, kT0 + std::chrono::seconds(150));
    ASSERT_EQ(rec.on(state).size(), 2u);
    EXPECT_EQ(rec.on(state).back().payload, "OFF");

    // On again, then silence: OFF at five minutes after the last write.
    night.onWritten({"DATALOG/20261030/20261030_233000_BRP.edf"}, kT0 + std::chrono::seconds(1000));
    night.tick(kT0 + std::chrono::seconds(1299));
    EXPECT_EQ(rec.on(state).back().payload, "ON");
    night.tick(kT0 + std::chrono::seconds(1300));
    EXPECT_EQ(rec.on(state).back().payload, "OFF");
}

// Against a broker: a client that subscribes after the fact (Home Assistant
// starting later) reads the entity's config and the mask's state, retained.
TEST(SleepBridgeLiveMaskBroker, DiscoveryAndStateAreRetainedOnTheBroker) {
    auto pub = std::make_shared<hms::MqttClient>(testMqttConfig("maskdisc_pub"));
    if (!pub->connect()) GTEST_SKIP() << "no MQTT broker";
    const std::string id = "maskdisc_" + std::to_string(::getpid());
    LiveMask mask(
        [pub](const std::string& t, const std::string& p, int q, bool r) {
            return pub->isConnected() && pub->publish(t, p, q, r);
        },
        id, "Mask test", std::chrono::system_clock::now());
    ASSERT_TRUE(mask.announce());
    mask.set("TAP-S3-00000123", true);
    std::this_thread::sleep_for(std::chrono::milliseconds(300));

    hms::MqttClient probe(testMqttConfig("maskdisc_probe"));
    ASSERT_TRUE(probe.connect());
    std::map<std::string, std::string> got;
    std::mutex mu;
    probe.subscribe(std::vector<std::string>{mask.discoveryTopic(), mask.stateTopic()},
                    [&](const std::string& t, const std::string& p) {
                        std::lock_guard<std::mutex> lk(mu);
                        got[t] = p;
                    },
                    1);
    std::this_thread::sleep_for(std::chrono::seconds(1));
    {
        std::lock_guard<std::mutex> lk(mu);
        ASSERT_EQ(got.count(mask.discoveryTopic()), 1u);
        const auto c = parse(got[mask.discoveryTopic()]);
        EXPECT_EQ(c["unique_id"].asString(), id + "_mask");
        EXPECT_EQ(c["state_topic"].asString(), "cpap/" + id + "/realtime/mask");
        EXPECT_EQ(got[mask.stateTopic()], "ON");
    }
    // Tidy up: empty retained payloads delete them.
    pub->publish(mask.discoveryTopic(), "", 1, true);
    pub->publish(mask.stateTopic(), "", 1, true);
    std::this_thread::sleep_for(std::chrono::milliseconds(200));
    probe.disconnect();
    pub->disconnect();
}
