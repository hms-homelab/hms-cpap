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

#include <chrono>
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

// The folder an AirSense 11's own Bluetooth fills: <data_dir>/as11/<serial>/,
// the identification in the card's shape.
TEST(ReadBluetoothIdentification, NamesTheMachineFromItsBluetoothFolder) {
    CardRoot data;
    fs::create_directories(fs::path(data.path()) / "as11" / "99990000003");
    std::ofstream(fs::path(data.path()) / "as11" / "99990000003" / "Identification.json") << R"({
        "FlowGenerator": {"IdentificationProfiles": {
            "Product": {"SerialNumber": "99990000003", "ProductCode": "39485",
                        "ProductName": "AirSense 11 AutoSet"},
            "Software": {"ApplicationIdentifier": "SW04600.17.8.6.0.0000000"}}}})";
    const auto id = readBluetoothIdentification(data.path());
    EXPECT_EQ(id.manufacturer, "ResMed");
    EXPECT_EQ(id.model, "AirSense 11 AutoSet");
    EXPECT_EQ(id.serial, "99990000003");
    EXPECT_EQ(id.firmware, "SW04600.17.8.6.0.0000000");
}

TEST(ReadBluetoothIdentification, TheNewestMachineWins) {
    CardRoot data;
    const auto as11 = fs::path(data.path()) / "as11";
    for (const char* serial : {"99990000004", "99990000005"}) {
        fs::create_directories(as11 / serial);
        std::ofstream(as11 / serial / "Identification.json")
            << R"({"FlowGenerator":{"IdentificationProfiles":{"Product":{"SerialNumber":")" << serial
            << R"(","ProductName":"AirSense11AutoSet"}}}})";
    }
    fs::last_write_time(as11 / "99990000004" / "Identification.json",
                        fs::file_time_type::clock::now() - std::chrono::hours(24));
    EXPECT_EQ(readBluetoothIdentification(data.path()).serial, "99990000005");
}

TEST(ReadBluetoothIdentification, NoBluetoothFolderIsEmpty) {
    CardRoot data;
    EXPECT_TRUE(readBluetoothIdentification(data.path()).empty());
    fs::create_directories(fs::path(data.path()) / "as11" / "99990000006");   // a folder, no file yet
    EXPECT_TRUE(readBluetoothIdentification(data.path()).empty());
    EXPECT_TRUE(readBluetoothIdentification("").empty());
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

namespace {

// A ResMed EDF's first 256 bytes, with [recording] in its 80-byte recording
// field at offset 88, as DATALOG/<night>/<name> under [root].
void writeEdfHeader(const std::string& root, const std::string& night, const std::string& name,
                    const std::string& recording) {
    const fs::path dir = fs::path(root) / "DATALOG" / night;
    fs::create_directories(dir);
    std::string header(256, ' ');
    header.replace(0, 8, "0       ");
    header.replace(88, std::min<size_t>(80, recording.size()), recording.substr(0, 80));
    std::ofstream(dir / name, std::ios::binary) << header;
}

}  // namespace

// hmscpap-tapper41, 2026-11-18: a Sleep Bridge's card is rebuilt from what the
// machine writes during therapy, and an AirSense writes Identification.tgt only
// when the card is set up, so the rebuilt card had none and the CpapDash
// uploader held every file of the night. Every ResMed EDF names the serial in
// its header.
TEST(ReadIdentification, NoFileTakesTheSerialFromAnEdfHeader) {
    CardRoot card;
    writeEdfHeader(card.path(), "20261117", "20261118_013236_BRP.edf",
                   "Startdate 18-NOV-2026 X X X SRN=99990000004 MID=36 VID=39");
    const auto id = readIdentification(card.path());
    EXPECT_EQ(id.serial, "99990000004");
    EXPECT_EQ(id.manufacturer, "ResMed");
    EXPECT_EQ(id.model, "") << "the header names no model: nothing is made up";
}

TEST(ReadIdentification, TheNewestNightsHeaderWins) {
    CardRoot card;
    writeEdfHeader(card.path(), "20261101", "20261101_220000_BRP.edf",
                   "Startdate 01-NOV-2026 X X X SRN=99990000005");
    writeEdfHeader(card.path(), "20261117", "20261117_223407_BRP.edf",
                   "Startdate 17-NOV-2026 X X X SRN=99990000006");
    EXPECT_EQ(readIdentification(card.path()).serial, "99990000006");
}

TEST(ReadIdentification, AnIdentificationFileWinsOverTheHeaders) {
    CardRoot card;
    card.write("Identification.tgt", kTgt);
    writeEdfHeader(card.path(), "20261117", "20261118_013236_BRP.edf",
                   "Startdate 18-NOV-2026 X X X SRN=99990000007");
    EXPECT_EQ(readIdentification(card.path()).serial, "99990000001");
}

// #94: an AirSense 10's card moved into an AirSense 11. The 10's
// Identification.tgt stays on the card beside the 11's Identification.json,
// each naming its own machine; the nights are the 11's.
TEST(ReadIdentification, ACardMovedToAnAirSense11NamesThe11) {
    CardRoot card;
    card.write("Identification.tgt", kTgt);                       // AirSense 10, 99990000001
    card.write("Identification.json", R"({"FlowGenerator":{"IdentificationProfiles":{
        "Product":{"SerialNumber":"99990000008","ProductCode":"39485","ProductName":"AirSense 11 AutoSet"}}}})");
    writeEdfHeader(card.path(), "20261120", "20261120_223000_BRP.edf",
                   "Startdate 20-NOV-2026 X X X SRN=99990000008 MID=46");
    const auto id = readIdentification(card.path());
    EXPECT_EQ(id.model, "AirSense 11 AutoSet");
    EXPECT_EQ(id.serial, "99990000008");
}

TEST(ReadIdentification, ACardMovedBackToAnAirSense10NamesThe10) {
    CardRoot card;
    card.write("Identification.tgt", kTgt);                       // AirSense 10, 99990000001
    card.write("Identification.json", kJson);                     // AirCurve 11, 99990000002
    writeEdfHeader(card.path(), "20261120", "20261120_223000_BRP.edf",
                   "Startdate 20-NOV-2026 X X X SRN=99990000001 MID=36 VID=39");
    const auto id = readIdentification(card.path());
    EXPECT_EQ(id.model, "AirSense 10 AutoSet");
    EXPECT_EQ(id.serial, "99990000001");
}

TEST(ReadIdentification, TwoMachinesAndNoNightsTheJsonWins) {
    CardRoot card;
    card.write("Identification.tgt", kTgt);
    card.write("Identification.json", kJson);
    EXPECT_EQ(readIdentification(card.path()).model, "AirCurve 11 VAuto");
}

TEST(ReadIdentification, AnEdfWithoutASerialGivesNothing) {
    CardRoot card;
    writeEdfHeader(card.path(), "20261117", "20261118_013236_BRP.edf", "Startdate 18-NOV-2026 X X X");
    EXPECT_TRUE(readIdentification(card.path()).empty());
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

// #94 follow-up: Home Assistant's name for the device, by the dashboard
// title's rule. CPAP_DEVICE_NAME is what the service bridges device_name into.
namespace {
class DeviceNameEnv {
public:
    explicit DeviceNameEnv(const char* value) {
        if (const char* v = std::getenv("CPAP_DEVICE_NAME")) {
            saved_ = v;
            had_ = true;
        }
        set(value);
    }
    ~DeviceNameEnv() { set(had_ ? saved_.c_str() : nullptr); }

private:
    static void set(const char* v) {
#ifdef _WIN32
        _putenv_s("CPAP_DEVICE_NAME", v ? v : "");
#else
        if (v) setenv("CPAP_DEVICE_NAME", v, 1); else unsetenv("CPAP_DEVICE_NAME");
#endif
    }
    std::string saved_;
    bool had_ = false;
};
}  // namespace

TEST(DeviceBlock, TheLegacyNameGivesWayToTheMachine) {
    DeviceNameEnv env("ResMed AirSense 10");
    DataPublisherService pub(nullptr, nullptr);
    EXPECT_EQ(pub.deviceInfo()["name"].asString(), "CPAP") << "no model known: no machine named";
    pub.setIdentity({"ResMed", "AirSense 11 AutoSet", "99990000009", ""});
    EXPECT_EQ(pub.deviceInfo()["name"].asString(), "ResMed AirSense 11 AutoSet");
    EXPECT_EQ(pub.deviceName(), "ResMed AirSense 11 AutoSet");
}

TEST(DeviceBlock, TheNewInstallNameGivesWayToTheMachine) {
    DeviceNameEnv env("CPAP");
    DataPublisherService pub(nullptr, nullptr);
    pub.setIdentity({"ResMed", "AirCurve 11 VAuto", "", ""});
    EXPECT_EQ(pub.deviceInfo()["name"].asString(), "ResMed AirCurve 11 VAuto");
}

TEST(DeviceBlock, ANameSomeoneChoseIsKept) {
    DeviceNameEnv env("Bedroom AirSense");
    DataPublisherService pub(nullptr, nullptr);
    pub.setIdentity({"ResMed", "AirSense 11 AutoSet", "", ""});
    EXPECT_EQ(pub.deviceInfo()["name"].asString(), "Bedroom AirSense");
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

// ── installIdentity ──────────────────────────────────────────────────────────

namespace {
const char* kAs11Json = R"({"FlowGenerator":{"IdentificationProfiles":{
    "Product":{"SerialNumber":"99990000009","ProductCode":"39485","ProductName":"AirSense 11 AutoSet"}}}})";
}

// A Sleep Bridge's folder is a local source: its rebuilt card is the root.
TEST(InstallIdentity, ALocalFolderIsTheCard) {
    CardRoot local, archive, data;
    local.write("Identification.json", kAs11Json);
    archive.write("Identification.tgt", kTgt);
    EXPECT_EQ(installIdentity(local.path(), archive.path(), data.path()).model, "AirSense 11 AutoSet");
}

// ez Share and FlashAir land the card's root files in the archive.
TEST(InstallIdentity, NoLocalFolderReadsTheArchive) {
    CardRoot archive, data;
    archive.write("Identification.json", kJson);
    EXPECT_EQ(installIdentity("", archive.path(), data.path()).model, "AirCurve 11 VAuto");
}

TEST(InstallIdentity, NoArchiveSetReadsTheDefaultArchive) {
    CardRoot data;
    fs::create_directories(fs::path(data.path()) / "cpap_data");
    std::ofstream(fs::path(data.path()) / "cpap_data" / "Identification.json") << kAs11Json;
    EXPECT_EQ(installIdentity("", "", data.path()).model, "AirSense 11 AutoSet");
}

TEST(InstallIdentity, NoCardFallsBackToBluetooth) {
    CardRoot archive, data;
    fs::create_directories(fs::path(data.path()) / "as11" / "99990000009");
    std::ofstream(fs::path(data.path()) / "as11" / "99990000009" / "Identification.json") << kAs11Json;
    EXPECT_EQ(installIdentity("", archive.path(), data.path()).model, "AirSense 11 AutoSet");
}

// ── machineName ──────────────────────────────────────────────────────────────

// #94: the desktop wizard's config.json has no device_name, so it loads as
// "ResMed AirSense 10", and an AirSense 11 install's dashboard said so.
TEST(MachineName, TheLegacyDefaultGivesWayToTheMachine) {
    EXPECT_EQ(machineName("ResMed AirSense 10", {"ResMed", "AirSense 11 AutoSet", "", ""}),
              "ResMed AirSense 11 AutoSet");
    EXPECT_EQ(machineName("ResMed AirSense 10", {"ResMed", "AirCurve 11 VAuto", "", ""}),
              "ResMed AirCurve 11 VAuto");
}

TEST(MachineName, TheNewInstallDefaultGivesWayToTheMachine) {
    EXPECT_EQ(machineName("CPAP", {"ResMed", "AirSense 11 AutoSet", "", ""}), "ResMed AirSense 11 AutoSet");
    EXPECT_EQ(machineName("", {"Sefam", "S.Box AUTO", "", ""}), "Sefam S.Box AUTO");
}

TEST(MachineName, ANameSomeoneChoseIsKept) {
    EXPECT_EQ(machineName("Bedroom AirSense", {"ResMed", "AirSense 11 AutoSet", "", ""}), "Bedroom AirSense");
}

TEST(MachineName, AMachineWithoutAModelNamesNoMachine) {
    EXPECT_EQ(machineName("ResMed AirSense 10", {"ResMed", "", "99990000004", ""}), "CPAP");
    EXPECT_EQ(machineName("ResMed AirSense 10", {}), "CPAP");
    EXPECT_EQ(machineName("", {}), "CPAP");
}

// The whole #94 path: a config.json the desktop wizard wrote (no device_name)
// and a Sleep Bridge folder rebuilt from an AirSense 11's card.
TEST(MachineName, AWizardConfigAndAnAirSense11BridgeFolder) {
    CardRoot dir, bridge;
    dir.write("config.json", R"({"transport": "local", "format": "resmed", "setup_complete": true})");
    bridge.write("Identification.json", kAs11Json);
    AppConfig c;
    ASSERT_EQ(AppConfig::loadFile(dir.path() + "/config.json", c), AppConfig::LoadStatus::Ok);
    c.local_dir = bridge.path();
    const auto id = installIdentity(c.legacySource() == "local" ? c.local_dir : "", c.archive_dir, dir.path());
    EXPECT_EQ(machineName(c.device_name, id), "ResMed AirSense 11 AutoSet");
}

TEST(DeviceDefaults, AnExistingIdIsNeverRewritten) {
    CardRoot dir;
    dir.write("config.json", R"({"device_id": "cpap_resmed_23243570851", "device_name": "Mine"})");
    AppConfig c;
    ASSERT_EQ(AppConfig::loadFile(dir.path() + "/config.json", c), AppConfig::LoadStatus::Ok);
    EXPECT_EQ(c.device_id, "cpap_resmed_23243570851");
    EXPECT_EQ(c.device_name, "Mine");
}
