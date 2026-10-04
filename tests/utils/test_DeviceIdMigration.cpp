// SDD-051: every install its own device id. The pure steps: what a generated
// id looks like, which ids count as an old default, and when a move begins.
// The database half is test_DeviceIdMoveBackends; the broker half needs one
// (HMS_TEST_MQTT) and skips without it.
#include <gtest/gtest.h>
#include "utils/DeviceIdMigration.h"
#include "mqtt_client.h"
#include "TestBroker.h"

#include <chrono>
#include <map>
#include <mutex>
#include <regex>
#include <thread>
#include <unistd.h>
#include <vector>

using namespace hms_cpap;

TEST(GenerateDeviceId, IsCpapdashAndEightHex) {
    const std::regex shape("^cpapdash_[0-9a-f]{8}$");
    for (int i = 0; i < 20; ++i) EXPECT_TRUE(std::regex_match(generateDeviceId(), shape));
}

TEST(GenerateDeviceId, TwoInstallsDiffer) {
    EXPECT_NE(generateDeviceId(), generateDeviceId());
}

TEST(OldDefaultDeviceId, BothFixedDefaultsAndNothingElse) {
    EXPECT_TRUE(isOldDefaultDeviceId("cpap_resmed_23243570851"));
    EXPECT_TRUE(isOldDefaultDeviceId("cpapdash"));
    EXPECT_FALSE(isOldDefaultDeviceId("cpapdash_addon"));   // the add-on's, left alone
    EXPECT_FALSE(isOldDefaultDeviceId("cpapdash_3f9a1c07"));
    EXPECT_FALSE(isOldDefaultDeviceId("my_airsense"));
}

TEST(BeginDeviceIdBackfill, MovesAnOldDefault) {
    AppConfig c;
    c.device_id = "cpap_resmed_23243570851";
    ASSERT_TRUE(beginDeviceIdBackfill(c));
    EXPECT_EQ(c.device_id_previous, "cpap_resmed_23243570851");
    EXPECT_TRUE(std::regex_match(c.device_id, std::regex("^cpapdash_[0-9a-f]{8}$")));
}

TEST(BeginDeviceIdBackfill, NeverMovesAPinnedId) {
    AppConfig c;
    c.device_id = "cpap_resmed_23243570851";
    c.device_id_pinned = true;
    EXPECT_FALSE(beginDeviceIdBackfill(c));
    EXPECT_EQ(c.device_id, "cpap_resmed_23243570851");
    EXPECT_TRUE(c.device_id_previous.empty());
}

TEST(BeginDeviceIdBackfill, LeavesAChosenIdAlone) {
    AppConfig c;
    c.device_id = "bedroom_cpap";
    EXPECT_FALSE(beginDeviceIdBackfill(c));
    EXPECT_EQ(c.device_id, "bedroom_cpap");
}

// A crash after the config was written: the next start finishes THAT move,
// with the same new id, instead of minting another and stranding the rows.
TEST(BeginDeviceIdBackfill, ResumesAnUnfinishedMoveWithTheSameId) {
    AppConfig c;
    c.device_id = "cpapdash_0badc0de";
    c.device_id_previous = "cpapdash";
    ASSERT_TRUE(beginDeviceIdBackfill(c));
    EXPECT_EQ(c.device_id, "cpapdash_0badc0de");
    EXPECT_EQ(c.device_id_previous, "cpapdash");
}

TEST(DeviceIdConfig, PinAndPreviousSurviveASaveAndLoad) {
    const auto path = (std::filesystem::temp_directory_path() /
                       ("sdd051_" + std::to_string(::getpid()) + ".json")).string();
    AppConfig c;
    c.device_id = "cpapdash_0badc0de";
    c.device_id_pinned = true;
    c.device_id_previous = "cpapdash";
    ASSERT_TRUE(c.save(path));
    AppConfig back;
    ASSERT_EQ(AppConfig::loadFile(path, back), AppConfig::LoadStatus::Ok);
    EXPECT_TRUE(back.device_id_pinned);
    EXPECT_EQ(back.device_id_previous, "cpapdash");
    std::filesystem::remove(path);
}

// The broker half: everything retained under the old id is emptied, and a
// neighbour's topics are not touched.
TEST(ClearRetainedDevice, EmptiesTheOldDeviceOnly) {
    hms::MqttClient probe(testMqttConfig("sdd051_probe"));
    if (!probe.connect()) GTEST_SKIP() << "no MQTT broker";
    const std::string pid = std::to_string(::getpid());
    const std::string old_id = "sdd051_old_" + pid;
    const std::string other = "sdd051_other_" + pid;

    probe.publish("homeassistant/sensor/" + old_id + "/x/config", "{\"a\":1}", 1, true);
    probe.publish("cpap/" + old_id + "/historical/ahi", "1.30", 1, true);
    probe.publish("cpap/" + other + "/historical/ahi", "2.00", 1, true);
    std::this_thread::sleep_for(std::chrono::milliseconds(300));

    ASSERT_TRUE(clearRetainedDevice(testMqttConfig("sdd051_clear"), old_id));

    std::map<std::string, std::string> left;
    std::mutex mu;
    auto keep = [&](const std::string& t, const std::string& p) {
        std::lock_guard<std::mutex> lk(mu);
        left[t] = p;
    };
    // One subscribe, for the reason clearRetainedDevice gives.
    probe.subscribe(std::vector<std::string>{"homeassistant/sensor/" + old_id + "/#",
                                             "cpap/" + old_id + "/#", "cpap/" + other + "/#"},
                    keep, 1);
    std::this_thread::sleep_for(std::chrono::seconds(1));
    {
        std::lock_guard<std::mutex> lk(mu);
        EXPECT_EQ(left.count("homeassistant/sensor/" + old_id + "/x/config"), 0u);
        EXPECT_EQ(left.count("cpap/" + old_id + "/historical/ahi"), 0u);
        EXPECT_EQ(left["cpap/" + other + "/historical/ahi"], "2.00");
    }
    probe.publish("cpap/" + other + "/historical/ahi", "", 1, true);   // tidy up
    probe.disconnect();
}
