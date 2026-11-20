// SDD-081: the desktop app as an OSCAR helper. The role, what the window
// writes, the installer's look (D3) and the tray's line, with no Qt.

#include "HelperSetup.h"

#include <gtest/gtest.h>

using namespace cpapdash::supervisor;

TEST(HelperSetup, TheRoleIsReadFromConfigJsonAndHmsCpapIsTheDefault) {
    EXPECT_EQ(roleOf(std::nullopt), "");
    EXPECT_EQ(roleOf(nlohmann::json::object()), "");
    EXPECT_EQ(roleOf(nlohmann::json{{"role", "oscar_helper"}}), "oscar_helper");
    EXPECT_EQ(roleOf(nlohmann::json{{"role", "something"}}), "");
}

TEST(HelperSetup, TheHelperStartsOnceItHasAnAddressAndOscarsFolder) {
    nlohmann::json j{{"role", "oscar_helper"}};
    EXPECT_FALSE(helperReady(j));
    HelperInputs in;
    in.host = "192.168.2.15";
    in.data_dir = "/home/u/Documents/OSCAR_Data";
    applyHelperInputs(j, in);
    EXPECT_TRUE(helperReady(j));
    EXPECT_EQ(j["oscar"]["profile"], "hms-cpap");
}

TEST(HelperSetup, TheTokenIsKeptForTheSameHmsCpapOnlyAndConnectAgainDropsIt) {
    nlohmann::json j{{"web_port", 8893}, {"oscar_helper", {{"host", "10.0.0.2"}, {"port", 8894}, {"token", "t"}}}};
    HelperInputs in;
    in.host = "10.0.0.2";
    in.data_dir = "/d";
    applyHelperInputs(j, in);
    EXPECT_EQ(j["oscar_helper"]["token"], "t") << "the same hms-cpap: still paired";
    EXPECT_EQ(j["web_port"], 8893) << "every other key kept";
    EXPECT_EQ(j["role"], "oscar_helper");
    in.host = "10.0.0.3";
    applyHelperInputs(j, in);
    EXPECT_EQ(j["oscar_helper"]["token"], "") << "another hms-cpap is a new pairing";
    j["oscar_helper"]["token"] = "u";
    dropHelperToken(j);
    EXPECT_EQ(j["oscar_helper"]["token"], "");
}

TEST(HelperSetup, TheLookFindsWhatAnswered) {
    const auto f = parseDiscover(
        R"({"found":[{"address":"192.168.2.15","port":8894,"name":"homeassistant","helper":"ALBIN-PC"},)"
        R"({"address":"192.168.2.40","name":"hub"}],"pc_name":"X"})");
    ASSERT_EQ(f.size(), 2u);
    EXPECT_EQ(f[0].address, "192.168.2.15");
    EXPECT_EQ(f[0].helper, "ALBIN-PC");
    EXPECT_EQ(f[1].port, 8894);
    EXPECT_TRUE(parseDiscover("not json").empty());
    EXPECT_TRUE(parseDiscover(R"({"found":[]})").empty());
}

TEST(HelperSetup, TheTrayLineFollowsTheDaemonsState) {
    auto t = trayLine({{"state", "connected"}, {"host", "192.168.2.15"}}, "06:12");
    EXPECT_EQ(t.key, HTxt::TrayConnectedLast);
    EXPECT_EQ(t.arg1, "192.168.2.15");
    EXPECT_EQ(t.arg2, "06:12");
    EXPECT_FALSE(t.connect_again);
    EXPECT_EQ(trayLine({{"state", "connected"}}, "").key, HTxt::TrayConnected);
    EXPECT_EQ(trayLine({{"state", "connecting"}}, "").key, HTxt::TrayConnecting);

    t = trayLine({{"state", "forgotten"}, {"host", "h"}}, "");
    EXPECT_EQ(t.key, HTxt::TrayForgotten);
    EXPECT_TRUE(t.connect_again) << "D4: Connect again, never by itself";
    t = trayLine({{"state", "refused"}, {"helper", "ALBIN-PC"}}, "");
    EXPECT_EQ(t.key, HTxt::TrayRefused);
    EXPECT_EQ(t.arg1, "ALBIN-PC");
    EXPECT_TRUE(t.connect_again);
    EXPECT_TRUE(trayLine({{"state", "update"}}, "").problem);
    EXPECT_FALSE(trayLine({{"state", "unreachable"}}, "").connect_again);
}

TEST(HelperSetup, EveryWordIsInFiveLanguagesAndInOrder) {
    for (int i = 0; i < static_cast<int>(HTxt::Count_); ++i)
        for (Lang l : {Lang::En, Lang::Es, Lang::Fr, Lang::Hu, Lang::Pt})
            EXPECT_STRNE(htext(static_cast<HTxt>(i), l), "") << "word " << i << " language " << static_cast<int>(l);
    EXPECT_EQ(checkWord("missing"), HTxt::StateMissing);
    EXPECT_EQ(checkWord("whatever"), HTxt::StateUnknown);
}
