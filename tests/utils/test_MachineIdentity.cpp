// SDD-050: the machine says what it is. The identification file and the
// parsed session are the only sources of the model, serial and firmware
// published to Home Assistant; nothing is filled in when they are silent.
//
// The files below are synthetic, written in the two formats real cards use,
// with made-up serials.
#include <gtest/gtest.h>
#include "utils/MachineIdentity.h"
#include "services/DataPublisherService.h"
#include "utils/AppConfig.h"

#include <cstdlib>
#include <filesystem>
#include <fstream>

namespace fs = std::filesystem;
using namespace hms_cpap;

namespace {

class CardRoot {
public:
    CardRoot() {
        dir_ = fs::temp_directory_path() /
               ("machine_identity_" + std::to_string(std::rand()) + "_" +
                std::to_string(reinterpret_cast<uintptr_t>(this)));
        fs::create_directories(dir_);
    }
    ~CardRoot() {
        std::error_code ec;
        fs::remove_all(dir_, ec);
    }
    void write(const std::string& name, const std::string& body) const {
        std::ofstream(dir_ / name) << body;
    }
    std::string path() const { return dir_.string(); }

private:
    fs::path dir_;
};

// The shape of an AirSense 10's Identification.tgt: "#KEY value" with blank
// lines between, CRLF on the card.
const char* kTgt =
    "#IMF 0001\r\n\r\n#VIR 0068\r\n\r\n#SRN 99990000001\r\n\r\n#SID SX567-0401\r\n\r\n"
    "#PNA AirSense_10_AutoSet\r\n\r\n#PCD 37160\r\n\r\n#MID 0024\r\n\r\n#FGT 24_M36_V39\r\n";

// The shape of an 11 series' Identification.json.
const char* kJson = R"({
    "FlowGenerator": {
        "IdentificationProfiles": {
            "Product": {
                "SerialNumber": "99990000002",
                "ProductCode": "39494",
                "ProductName": "AirCurve11VAuto"
            },
            "Software": {
                "ApplicationIdentifier": "SW04600.18.8.7.0.2d0405660"
            }
        }
    }
})";

}  // namespace

// ── readableModel ────────────────────────────────────────────────────────────

TEST(ReadableModel, SplitsWhereLettersMeetDigits) {
    EXPECT_EQ(readableModel("AirCurve11VAuto"), "AirCurve 11 VAuto");
    EXPECT_EQ(readableModel("AirSense11AutoSet"), "AirSense 11 AutoSet");
}

TEST(ReadableModel, UnderscoresBecomeOneSpace) {
    EXPECT_EQ(readableModel("AirSense_10_AutoSet"), "AirSense 10 AutoSet");
    EXPECT_EQ(readableModel("AirSense_10_AutoSet_For_Her"), "AirSense 10 AutoSet For Her");
    EXPECT_EQ(readableModel("S.Box_AUTO "), "S.Box AUTO");
}

TEST(ReadableModel, EmptyStaysEmpty) {
    EXPECT_EQ(readableModel(""), "");
    EXPECT_EQ(readableModel("   "), "");
}

// ── readIdentification ───────────────────────────────────────────────────────

TEST(ReadIdentification, TgtGivesModelSerialAndSoftware) {
    CardRoot card;
    card.write("Identification.tgt", kTgt);
    const auto id = readIdentification(card.path());
    EXPECT_EQ(id.manufacturer, "ResMed");
    EXPECT_EQ(id.model, "AirSense 10 AutoSet");
    EXPECT_EQ(id.serial, "99990000001");
    EXPECT_EQ(id.firmware, "SX567-0401");
}

TEST(ReadIdentification, JsonGivesModelSerialAndSoftware) {
    CardRoot card;
    card.write("Identification.json", kJson);
    const auto id = readIdentification(card.path());
    EXPECT_EQ(id.manufacturer, "ResMed");
    EXPECT_EQ(id.model, "AirCurve 11 VAuto");
    EXPECT_EQ(id.serial, "99990000002");
    EXPECT_EQ(id.firmware, "SW04600.18.8.7.0.2d0405660");
}

TEST(ReadIdentification, JsonWinsWhenBothArePresent) {
    CardRoot card;
    card.write("Identification.tgt", kTgt);
    card.write("Identification.json", kJson);
    EXPECT_EQ(readIdentification(card.path()).model, "AirCurve 11 VAuto");
}

TEST(ReadIdentification, MalformedJsonFallsBackToTgt) {
    CardRoot card;
    card.write("Identification.json", "{ not json");
    card.write("Identification.tgt", kTgt);
    EXPECT_EQ(readIdentification(card.path()).model, "AirSense 10 AutoSet");
}

TEST(ReadIdentification, MalformedJsonAloneIsEmpty) {
    CardRoot card;
    card.write("Identification.json", "{ not json");
    EXPECT_TRUE(readIdentification(card.path()).empty());
}

TEST(ReadIdentification, TgtWithoutProductNameKeepsWhatItHas) {
    CardRoot card;
    card.write("Identification.tgt", "#SRN 99990000003\n#SID SX567-0401\n");
    const auto id = readIdentification(card.path());
    EXPECT_EQ(id.model, "");
    EXPECT_EQ(id.serial, "99990000003");
}

TEST(ReadIdentification, NoFileIsEmpty) {
    CardRoot card;
    EXPECT_TRUE(readIdentification(card.path()).empty());
    EXPECT_TRUE(readIdentification("").empty());
    EXPECT_TRUE(readIdentification("/nonexistent/card/root").empty());
}

// ── identityFromSession ──────────────────────────────────────────────────────

TEST(IdentityFromSession, SefamNamesModelSerialAndFirmware) {
    CPAPSession s;
    s.manufacturer = DeviceManufacturer::SEFAM;
    s.product_name = "S.Box_AUTO";
    s.serial_number = "1263R00000000";
    s.firmware = "VER :A020400";
    const auto id = identityFromSession(s);
    EXPECT_EQ(id.manufacturer, "Sefam");
    EXPECT_EQ(id.model, "S.Box AUTO");
    EXPECT_EQ(id.serial, "1263R00000000");
    EXPECT_EQ(id.firmware, "VER :A020400");
}

TEST(IdentityFromSession, PrismaHasNoModel) {
    CPAPSession s;
    s.manufacturer = DeviceManufacturer::LOWENSTEIN;
    s.serial_number = "TESTSN00";
    s.firmware = "5.05";
    const auto id = identityFromSession(s);
    EXPECT_EQ(id.manufacturer, "Löwenstein");
    EXPECT_EQ(id.model, "");
    EXPECT_EQ(id.firmware, "5.05");
}

TEST(IdentityFromSession, LunaHasOnlyItsSerial) {
    CPAPSession s;
    s.manufacturer = DeviceManufacturer::BMC;
    s.serial_number = "00000001";
    const auto id = identityFromSession(s);
    EXPECT_EQ(id.manufacturer, "BMC");
    EXPECT_EQ(id.model, "");
    EXPECT_EQ(id.serial, "00000001");
    EXPECT_EQ(id.firmware, "");
}

TEST(IdentityFromSession, UnknownManufacturerIsLeftOut) {
    CPAPSession s;
    EXPECT_EQ(identityFromSession(s).manufacturer, "");
}

// ── The Home Assistant device block ──────────────────────────────────────────

// Every field is SENT, empty when unknown: Home Assistant keeps a value whose
// key is absent, so an upgraded install would go on showing "AirSense 10".
TEST(DeviceBlock, WithoutAnIdentityBlanksEveryField) {
    DataPublisherService pub(nullptr, nullptr);
    const auto d = pub.deviceInfo();
    for (const char* key : {"manufacturer", "model", "serial_number", "sw_version"}) {
        ASSERT_TRUE(d.isMember(key)) << key << " must be sent to clear an old value";
        EXPECT_TRUE(d[key].isString()) << key << " must never be null";
        EXPECT_EQ(d[key].asString(), "") << key;
    }
    EXPECT_EQ(d.toStyledString().find("AirSense"), std::string::npos);
}

TEST(DeviceBlock, APartialIdentityBlanksWhatItDoesNotSay) {
    DataPublisherService pub(nullptr, nullptr);
    pub.setIdentity({"BMC", "", "00000001", ""});
    const auto d = pub.deviceInfo();
    EXPECT_EQ(d["manufacturer"].asString(), "BMC");
    EXPECT_EQ(d["model"].asString(), "");
    EXPECT_EQ(d["sw_version"].asString(), "");
}

TEST(DeviceBlock, CarriesWhatTheCardSaid) {
    DataPublisherService pub(nullptr, nullptr);
    pub.setIdentity({"ResMed", "AirCurve 11 VAuto", "99990000002", "SW04600.18.8.7.0"});
    const auto d = pub.deviceInfo();
    EXPECT_EQ(d["manufacturer"].asString(), "ResMed");
    EXPECT_EQ(d["model"].asString(), "AirCurve 11 VAuto");
    EXPECT_EQ(d["serial_number"].asString(), "99990000002");
    EXPECT_EQ(d["sw_version"].asString(), "SW04600.18.8.7.0");
}

TEST(DeviceBlock, AnEmptyIdentityDoesNotEraseAKnownOne) {
    DataPublisherService pub(nullptr, nullptr);
    pub.setIdentity({"ResMed", "AirSense 10 AutoSet", "", ""});
    pub.setIdentity({});
    EXPECT_EQ(pub.deviceInfo()["model"].asString(), "AirSense 10 AutoSet");
}

// ── Defaults ─────────────────────────────────────────────────────────────────

TEST(DeviceDefaults, ANewInstallNamesNoMachine) {
    AppConfig c;
    EXPECT_EQ(c.device_id, "cpapdash");
    EXPECT_EQ(c.device_name, "CPAP");
}

TEST(DeviceDefaults, AnExistingFileWithoutTheKeysKeepsWhatTheyMeant) {
    CardRoot dir;
    dir.write("config.json", R"({"source": "local"})");
    AppConfig c;
    ASSERT_EQ(AppConfig::loadFile(dir.path() + "/config.json", c), AppConfig::LoadStatus::Ok);
    EXPECT_EQ(c.device_id, "cpap_resmed_23243570851");
    EXPECT_EQ(c.device_name, "ResMed AirSense 10");
}

TEST(DeviceDefaults, AnExistingIdIsNeverRewritten) {
    CardRoot dir;
    dir.write("config.json", R"({"device_id": "cpap_resmed_23243570851", "device_name": "Mine"})");
    AppConfig c;
    ASSERT_EQ(AppConfig::loadFile(dir.path() + "/config.json", c), AppConfig::LoadStatus::Ok);
    EXPECT_EQ(c.device_id, "cpap_resmed_23243570851");
    EXPECT_EQ(c.device_name, "Mine");
}
