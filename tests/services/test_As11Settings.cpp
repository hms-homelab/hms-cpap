#include <gtest/gtest.h>

#include "services/As11Settings.h"

#include <chrono>
#include <filesystem>
#include <fstream>
#include <map>
#include <sstream>
#include <string>

using namespace hms_cpap;
namespace fs = std::filesystem;

// The machine's SettingProfiles to STR codes. Every value is made up; the
// layout of the object and the codes are those of a real card's files.

namespace {

// What Get ["SettingProfiles"] returns, with the parts a test changes.
std::string profiles(const std::string& applied = "2026-10-30T19:02:36.785Z",
                     const std::string& mode = "AutoSet", const std::string& mask = "Pillows",
                     const std::string& tube = "15mmNonHeated", const std::string& epr_type = "FullTime") {
    return R"({"SettingProfiles":{"Attributes":{"AppliedDateTime":")" + applied + R"(","Source":""},)"
           R"("ActiveProfiles":{"TherapyProfile":"AutoSetProfile"},)"
           R"("TherapyProfiles":{)"
           R"("AutoSetProfile":{"TherapyMode":")" + mode + R"(","MaxPressure":16.0,"MinPressure":9.0,"StartPressure":4.0},)"
           R"("AutoSetForHerProfile":{"TherapyMode":"HerAuto","MaxPressure":16.0,"MinPressure":5.0,"StartPressure":4.0},)"
           R"("CpapProfile":{"TherapyMode":"CPAP","SetPressure":11.0,"StartPressure":4.0}},)"
           R"("FeatureProfiles":{)"
           R"("ComfortFeature":{"AutoSetComfort":"Off"},)"
           R"("EprFeature":{"EprEnablePatientAccess":"On","EprEnable":"On","EprType":")" + epr_type + R"(","EprPressure":2},)"
           R"("AutoRampFeature":{"RampTime":15,"RampEnable":"Off"},)"
           R"("SmartStartStopFeature":{"SmartStart":"On","SmartStop":"On"},)"
           R"("CircuitFeature":{"MaskType":")" + mask + R"(","TubeType":")" + tube + R"(","AntiBacterialFilter":"No"},)"
           R"("ClimateFeature":{"ClimateControl":"Auto","HumidifierSettingEnable":"On","HumidifierLevel":5,)"
           R"("HeatedTubeSettingEnable":"Off","HeatedTubeTemperature":27.0},)"
           R"("TimeZoneFeature":{"TimeZoneOffset":"-07:00"}}}})";
}

std::map<std::string, double> valuesOf(const As11Settings& s) {
    std::map<std::string, double> m;
    for (const auto& kv : s.values) m[kv.first] = kv.second;
    return m;
}

class SettingsFile : public ::testing::Test {
protected:
    fs::path root_;
    void SetUp() override {
        root_ = fs::temp_directory_path() /
                ("as11_settings_" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
        fs::create_directories(root_);
    }
    void TearDown() override { fs::remove_all(root_); }
    std::string text() const {
        std::ifstream in(settingsPath(root_.string()));
        std::stringstream ss; ss << in.rdbuf();
        return ss.str();
    }
};

}  // namespace

TEST(As11Settings, EveryMatchedFieldGetsItsStrCode) {
    const auto s = parseSettings(profiles());
    ASSERT_TRUE(s.has_value());
    const auto v = valuesOf(*s);
    EXPECT_EQ(v.at("Mode"), 1.0);
    EXPECT_EQ(v.at("S.A.MaxPress"), 16.0);
    EXPECT_EQ(v.at("S.A.MinPress"), 9.0);
    EXPECT_EQ(v.at("S.A.StartPress"), 4.0);
    EXPECT_EQ(v.at("S.AFH.MinPress"), 5.0);
    EXPECT_EQ(v.at("S.AFH.MaxPress"), 16.0);
    EXPECT_EQ(v.at("S.C.Press"), 11.0);
    EXPECT_EQ(v.at("S.C.StartPress"), 4.0);
    EXPECT_EQ(v.at("S.AS.Comfort"), 1.0);       // Off
    EXPECT_EQ(v.at("S.RampEnable"), 1.0);       // Off
    EXPECT_EQ(v.at("S.RampTime"), 15.0);
    EXPECT_EQ(v.at("S.EPR.ClinEnable"), 2.0);   // On
    EXPECT_EQ(v.at("S.EPR.EPREnable"), 2.0);
    EXPECT_EQ(v.at("S.EPR.EPRType"), 2.0);      // FullTime
    EXPECT_EQ(v.at("S.EPR.Level"), 2.0);
    EXPECT_EQ(v.at("S.SmartStart"), 2.0);       // On
    EXPECT_EQ(v.at("S.ABFilter"), 1.0);         // No
    EXPECT_EQ(v.at("S.Mask"), 2.0);             // Pillows
    EXPECT_EQ(v.at("S.Tube"), 3.0);             // 15mmNonHeated
    EXPECT_EQ(v.at("S.ClimateControl"), 1.0);   // Auto
    EXPECT_EQ(v.at("S.HumEnable"), 2.0);
    EXPECT_EQ(v.at("S.HumLevel"), 5.0);
    EXPECT_EQ(v.at("S.TempEnable"), 1.0);       // Off
    EXPECT_EQ(v.at("S.Temp"), 27.0);
}

TEST(As11Settings, ACardsFileAndTheBluetoothResultParseTheSame) {
    const std::string bare = profiles();
    const std::string card = "{\"FlowGenerator\":" + bare + "}";
    const auto a = parseSettings(bare), b = parseSettings(card);
    ASSERT_TRUE(a && b);
    EXPECT_EQ(a->values, b->values);
    EXPECT_EQ(a->applied_ms, b->applied_ms);
}

TEST(As11Settings, AValueNeverSeenOnACardIsLeftOutNotGuessed) {
    const auto s = parseSettings(profiles("2026-10-30T19:02:36.785Z", "CPAP", "FullFace", "HeatedTube", "RampOnly"));
    ASSERT_TRUE(s.has_value());
    const auto v = valuesOf(*s);
    EXPECT_EQ(v.count("Mode"), 0u);
    EXPECT_EQ(v.count("S.Mask"), 0u);
    EXPECT_EQ(v.count("S.Tube"), 0u);
    EXPECT_EQ(v.count("S.EPR.EPRType"), 0u);
    EXPECT_EQ(v.at("S.A.MinPress"), 9.0);   // the numbers do not depend on the mode
}

TEST(As11Settings, TheyApplyFromTheSleepDayTheyWereAppliedOn) {
    // 19:02 UTC on 30 October is 12:02 at UTC-7: the sleep day of the 30th.
    const auto s = parseSettings(profiles("2026-10-30T19:02:36.785Z"));
    ASSERT_TRUE(s);
    EXPECT_EQ(s->offset_min, -420);
    EXPECT_EQ(s->firstDay(), 20756);   // 2026-10-30
    // 09:00 UTC on the 31st is 02:00 that night: still the sleep day of the 30th.
    EXPECT_EQ(parseSettings(profiles("2026-10-31T09:00:00.000Z"))->firstDay(), 20756);
    // 20:00 UTC on the 31st is 13:00: the 31st.
    EXPECT_EQ(parseSettings(profiles("2026-10-31T20:00:00.000Z"))->firstDay(), 20757);
}

TEST(As11Settings, WhatIsNotSettingsIsRefused) {
    EXPECT_FALSE(parseSettings(""));
    EXPECT_FALSE(parseSettings("not json"));
    EXPECT_FALSE(parseSettings("[]"));
    EXPECT_FALSE(parseSettings(R"({"IdentificationProfiles":{}})"));
    EXPECT_FALSE(parseSettings(R"({"SettingProfiles":{}})"));   // names nothing we know
}

TEST_F(SettingsFile, TheResultIsKeptAsACardsFile) {
    ASSERT_TRUE(writeSettings(root_.string(), profiles()));
    EXPECT_EQ(text(), "{\"FlowGenerator\":" + profiles() + "}");
    const auto back = readSettings(root_.string());
    ASSERT_TRUE(back);
    EXPECT_EQ(valuesOf(*back).at("S.A.MinPress"), 9.0);
}

TEST_F(SettingsFile, ANewerFileReplacesAnOlderAndNeverTheOtherWayRound) {
    ASSERT_TRUE(writeSettings(root_.string(), profiles("2026-10-30T19:00:00.000Z")));
    const std::string first = text();
    // Older: kept out, the file is as it was.
    EXPECT_FALSE(writeSettings(root_.string(), profiles("2026-10-29T19:00:00.000Z")));
    EXPECT_EQ(text(), first);
    // The same moment: the file there stays (a card's copy over the machine's).
    EXPECT_FALSE(writeSettings(root_.string(), profiles("2026-10-30T19:00:00.000Z", "AutoSet", "Pillows", "15mmNonHeated", "FullTime")));
    EXPECT_EQ(text(), first);
    // Newer: replaces it.
    ASSERT_TRUE(writeSettings(root_.string(), profiles("2026-10-31T19:00:00.000Z")));
    EXPECT_NE(text(), first);
    EXPECT_EQ(readSettings(root_.string())->firstDay(), 20757);
}

TEST_F(SettingsFile, ACardsFileAlreadyThereIsKeptWhenItIsAsNewAsTheMachinesOwn) {
    fs::create_directories(fs::path(settingsPath(root_.string())).parent_path());
    const std::string card = "{\"FlowGenerator\":" + profiles("2026-10-30T19:02:36.785Z") + "}";
    std::ofstream(settingsPath(root_.string())) << card;
    EXPECT_FALSE(writeSettings(root_.string(), profiles("2026-10-30T19:02:36.785Z")));
    EXPECT_EQ(text(), card);
}

TEST_F(SettingsFile, NothingIsWrittenForAResultThatIsNotSettings) {
    EXPECT_FALSE(writeSettings(root_.string(), "{}"));
    EXPECT_FALSE(fs::exists(settingsPath(root_.string())));
    EXPECT_FALSE(readSettings(root_.string()));
}

namespace {
// The .crc beside the settings file: four bytes, the file's CRC-32 little-endian.
std::string crcFileOf(const std::string& settings_path) {
    std::string p = settings_path;
    p.replace(p.size() - 5, 5, ".crc");
    std::ifstream in(p, std::ios::binary);
    return std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
}
// CRC-32 by the book, to check the writer's against.
uint32_t refCrc32(const std::string& s) {
    uint32_t c = 0xFFFFFFFFu;
    for (unsigned char ch : s) {
        c ^= ch;
        for (int i = 0; i < 8; i++) c = (c & 1u) ? ((c >> 1) ^ 0xEDB88320u) : (c >> 1);
    }
    return ~c;
}
}  // namespace

TEST_F(SettingsFile, TheCrcOfTheFileIsKeptBesideItAsACardKeepsIt) {
    ASSERT_TRUE(writeSettings(root_.string(), profiles()));
    const std::string crc = crcFileOf(settingsPath(root_.string()));
    ASSERT_EQ(crc.size(), 4u);
    const uint32_t want = refCrc32(text());
    EXPECT_EQ(static_cast<uint8_t>(crc[0]), want & 0xFF);
    EXPECT_EQ(static_cast<uint8_t>(crc[1]), (want >> 8) & 0xFF);
    EXPECT_EQ(static_cast<uint8_t>(crc[2]), (want >> 16) & 0xFF);
    EXPECT_EQ(static_cast<uint8_t>(crc[3]), (want >> 24) & 0xFF);

    // A newer file replaces it, and the CRC follows.
    ASSERT_TRUE(writeSettings(root_.string(), profiles("2026-10-31T19:00:00.000Z")));
    EXPECT_NE(crcFileOf(settingsPath(root_.string())), crc);
    EXPECT_EQ(static_cast<uint8_t>(crcFileOf(settingsPath(root_.string()))[0]), refCrc32(text()) & 0xFF);
}

TEST_F(SettingsFile, ACardsOwnCrcIsKeptAndAFileWithoutOneGetsOne) {
    fs::create_directories(fs::path(settingsPath(root_.string())).parent_path());
    const std::string card = "{\"FlowGenerator\":" + profiles("2026-10-30T19:02:36.785Z") + "}";
    std::ofstream(settingsPath(root_.string())) << card;
    // No .crc yet: the file stays, and gets one.
    EXPECT_FALSE(writeSettings(root_.string(), profiles("2026-10-30T19:02:36.785Z")));
    EXPECT_EQ(text(), card);
    EXPECT_EQ(crcFileOf(settingsPath(root_.string())).size(), 4u);
    // A card's own is not replaced.
    {
        std::string p = settingsPath(root_.string());
        p.replace(p.size() - 5, 5, ".crc");
        std::ofstream(p, std::ios::binary | std::ios::trunc) << "card";
    }
    EXPECT_FALSE(writeSettings(root_.string(), profiles("2026-10-30T19:02:36.785Z")));
    EXPECT_EQ(crcFileOf(settingsPath(root_.string())), "card");
}
