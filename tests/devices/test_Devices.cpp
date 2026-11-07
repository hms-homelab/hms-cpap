// The Devices page's one read (SDD-062 D8), its local names and forgotten
// devices (D12), and the update job object every page reads (platform job
// shape, kept as a file per unit). Everything here is driven by fakes: the
// aggregate is a pure function of what each service reports.

#include <gtest/gtest.h>

#include "devices/Devices.h"
#include "devices/DevicesStore.h"
#include "devices/OtaJob.h"
#include "devices/Recording.h"

#include <json/json.h>

#include <chrono>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>

using namespace hms_cpap::devices;
namespace fs = std::filesystem;

namespace {

constexpr std::time_t kNow = 1762300000;

struct Dir {
    fs::path path;
    Dir() {
        path = fs::temp_directory_path() /
               ("hms_devices_" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
        fs::create_directories(path);
    }
    ~Dir() {
        std::error_code ec;
        fs::remove_all(path, ec);
    }
};

Json::Value parse(const std::string& s) {
    Json::Value v;
    Json::CharReaderBuilder rb;
    std::istringstream ss(s);
    Json::parseFromStream(rb, ss, &v, nullptr);
    return v;
}

Json::Value muleUnit(const std::string& serial, bool online, long long heard, bool streaming = false,
                     bool landing = false) {
    Json::Value u(Json::objectValue);
    u["serial"]        = serial;
    u["online"]        = online;
    u["last_heard"]    = static_cast<Json::Int64>(heard);
    u["streaming"]     = streaming;
    u["night_landing"] = landing;
    u["fw"]            = "6.0.0";
    u["miner_fw"]      = "6.0.0";
    u["commands"]      = Json::Value(Json::arrayValue);
    return u;
}

Json::Value bridge(const std::string& sender, bool online, long long heard, const std::string& rec = "no") {
    Json::Value b(Json::objectValue);
    b["sender"]           = sender;
    b["online"]           = online;
    b["last_heard"]       = static_cast<Json::Int64>(heard);
    b["recording"]        = rec;
    b["recording_reason"] = rec == "yes" ? "the machine wrote to its card 2 min ago" : "";
    b["fw_version"]       = "1.0.1";
    b["commands"]         = Json::Value(Json::arrayValue);
    return b;
}

DeviceInputs none() {
    DeviceInputs in;
    in.now       = kNow;
    in.transport = "local";
    return in;
}

const Json::Value* find(const Json::Value& out, const std::string& kind) {
    for (const auto& d : out["devices"])
        if (d["kind"].asString() == kind) return &d;
    return nullptr;
}

}  // namespace

// ── the aggregate ──

TEST(Devices, NothingConfiguredListsNothing) {
    DevicesStore store("");
    const auto out = buildDevices(none(), store);
    EXPECT_TRUE(out["devices"].empty());
    EXPECT_EQ(out["stream_port"].asInt(), 8894);
}

TEST(Devices, AMuleAndMinerHereAwayStreamingLandingNeverHeard) {
    DevicesStore store("");
    auto in = none();
    in.mule["units"] = Json::Value(Json::arrayValue);
    in.mule["units"].append(muleUnit("FE-1", true, kNow - 5, true));
    in.mule["units"].append(muleUnit("FE-2", false, kNow - 600, false, true));
    in.mule["units"].append(muleUnit("FE-3", false, kNow - 900));
    in.mule["units"].append(muleUnit("FE-4", false, 0));
    const auto out = buildDevices(in, store);
    ASSERT_EQ(out["devices"].size(), 4u);
    const auto& d = out["devices"];
    EXPECT_EQ(d[0]["id"].asString(), "FE-1");
    EXPECT_EQ(d[0]["present"].asString(), "yes");
    EXPECT_EQ(d[0]["recording"].asString(), "yes");
    EXPECT_NE(d[0]["recording_reason"].asString().find("streaming"), std::string::npos);
    EXPECT_EQ(d[1]["id"].asString(), "FE-2");
    EXPECT_EQ(d[1]["present"].asString(), "no");
    EXPECT_EQ(d[1]["recording"].asString(), "yes") << "a night not landed yet counts";
    EXPECT_EQ(d[2]["recording"].asString(), "no");
    EXPECT_EQ(d[3]["id"].asString(), "FE-4") << "never heard: last";
    EXPECT_EQ(d[3]["recording"].asString(), "unknown");
    EXPECT_TRUE(d[0]["forgettable"].asBool());
    EXPECT_EQ(d[0]["key"].asString(), "mule/FE-1");
    EXPECT_EQ(d[0]["version"].asString(), "6.0.0");
}

TEST(Devices, AMulesFirmwareAndItsJobRideAlong) {
    DevicesStore store("");
    auto in = none();
    in.mule["units"].append(muleUnit("FE-1", true, kNow));
    in.mule_firmware["FE-1"] = parse(
        R"({"latest":{"mule":"6.1.0","miner":"6.1.0"},"update_available":{"mule":true,"miner":true},
            "manifest":{"error":""},"ota":{"phase":"transferring","step":1,"steps":2}})");
    const auto out = buildDevices(in, store);
    const auto& det = out["devices"][0]["detail"];
    EXPECT_TRUE(det["firmware"]["update_available"]["mule"].asBool());
    EXPECT_EQ(det["firmware"]["latest"]["miner"].asString(), "6.1.0");
    EXPECT_EQ(det["ota"]["phase"].asString(), "transferring");
}

TEST(Devices, ALogPullsTextIsCarriedOnceNotInEveryCommand) {
    DevicesStore store("");
    auto in = none();
    Json::Value b = bridge("TAP-1", true, kNow);
    for (int i = 0; i < 15; i++) {
        Json::Value c(Json::objectValue);
        c["req_id"] = 100 - i;
        c["cmd"]    = "logs";
        c["logs"]   = "line " + std::to_string(i) + "\n";
        b["commands"].append(c);
    }
    in.sleepbridge["bridges"].append(b);
    const auto out = buildDevices(in, store);
    const auto& det = out["devices"][0]["detail"];
    EXPECT_EQ(det["commands"].size(), 10u);
    for (const auto& c : det["commands"]) EXPECT_FALSE(c.isMember("logs"));
    EXPECT_EQ(det["last_logs"]["text"].asString(), "line 0\n") << "the newest";
    EXPECT_EQ(det["last_logs"]["req_id"].asUInt64(), 100u);
}

TEST(Devices, ASleepBridgeCarriesItsOwnRecordingJudgement) {
    DevicesStore store("");
    auto in = none();
    in.sleepbridge["bridges"].append(bridge("TAP-1", true, kNow, "yes"));
    in.sleepbridge["bridges"].append(bridge("TAP-2", false, 0, "unknown"));
    const auto out = buildDevices(in, store);
    const auto* d = find(out, "sleepbridge");
    ASSERT_TRUE(d);
    EXPECT_EQ((*d)["recording"].asString(), "yes");
    EXPECT_EQ(out["devices"][1]["recording"].asString(), "unknown");
    EXPECT_EQ(out["devices"][1]["present"].asString(), "no");
}

TEST(Devices, TheRingThroughABridgeInABuildWithoutBluetoothAndDirect) {
    DevicesStore store("");
    auto in = none();
    in.o2_enabled = true;
    in.o2_mode    = "http";
    in.o2_url     = "http://bridge.local";
    auto out = buildDevices(in, store);
    const auto* d = find(out, "o2ring");
    ASSERT_TRUE(d);
    EXPECT_TRUE((*d)["detail"]["through_bridge"].asBool()) << "D10: one line, no Bluetooth facts";
    EXPECT_EQ((*d)["present"].asString(), "unknown");

    in.o2_mode      = "ble";
    in.ble_compiled = false;
    out = buildDevices(in, store);
    EXPECT_TRUE((*find(out, "o2ring"))["detail"]["not_compiled"].asBool());

    in.ble_compiled = true;
    out = buildDevices(in, store);
    EXPECT_EQ((*find(out, "o2ring"))["present"].asString(), "unknown") << "not asked yet";

    in.o2 = parse(R"({"checked_at":1762299990,"connected":true,"battery":81,"last_read_at":1762290000,
                      "last_file":"20261105.vld","live":{"active":true,"spo2":96,"hr":61}})");
    out = buildDevices(in, store);
    d = find(out, "o2ring");
    EXPECT_EQ((*d)["present"].asString(), "yes");
    EXPECT_EQ((*d)["detail"]["battery"].asInt(), 81);
    EXPECT_EQ((*d)["recording"].asString(), "yes");
    EXPECT_NE((*d)["recording_reason"].asString().find("ring"), std::string::npos);
    EXPECT_EQ((*d)["last_heard"].asInt64(), 1762299990);
}

TEST(Devices, TheCardSourceReachableNotAnsweringNotYetAndTheMuleInCardMode) {
    DevicesStore store("");
    auto in = none();
    in.transport   = "ezshare";
    in.source_url  = "http://192.168.4.1";
    in.format      = "resmed";
    in.ledger_live = "no";
    auto out = buildDevices(in, store);
    const auto* d = find(out, "ezshare");
    ASSERT_TRUE(d);
    EXPECT_EQ((*d)["present"].asString(), "unknown") << "no burst has tried yet";
    EXPECT_EQ((*d)["id"].asString(), "source");
    EXPECT_FALSE((*d)["forgettable"].asBool());

    in.source = parse(R"({"last_attempt":1762299900,"answering":false,"last_answered":1762290000,
                          "health":"Not answering for 1 h","consecutive_failures":12})");
    out = buildDevices(in, store);
    d = find(out, "ezshare");
    EXPECT_EQ((*d)["present"].asString(), "no");
    EXPECT_EQ((*d)["detail"]["health"].asString(), "Not answering for 1 h");
    EXPECT_EQ((*d)["detail"]["consecutive_failures"].asInt(), 12);
    EXPECT_EQ((*d)["last_heard"].asInt64(), 1762290000);

    in.source["answering"] = true;
    in.source["health"]    = "OK";
    in.source_probe        = parse(R"({"bridge_serial":"FE-9","bridge_fw":"6.0.6"})");
    in.ledger_live         = "yes";
    out = buildDevices(in, store);
    d = find(out, "ezshare");
    EXPECT_EQ((*d)["present"].asString(), "yes");
    EXPECT_EQ((*d)["detail"]["bridge"]["serial"].asString(), "FE-9") << "D11";
    EXPECT_EQ((*d)["recording"].asString(), "yes") << "the ledger's live night";

    in.transport = "flashair";
    out = buildDevices(in, store);
    EXPECT_FALSE(find(out, "ezshare"));
    ASSERT_TRUE(find(out, "flashair"));
    EXPECT_FALSE((*find(out, "flashair"))["detail"].isMember("bridge"));
    EXPECT_EQ((*find(out, "flashair"))["default_name"].asString(), "FlashAir card");
}

TEST(Devices, TheAirSense11OverBluetooth) {
    DevicesStore store("");
    auto in = none();
    in.as11_enabled    = true;
    in.as11_pull_hours = 6;
    in.ledger_live     = "no";
    in.ble_compiled    = false;
    auto out = buildDevices(in, store);
    const auto* d = find(out, "as11_ble");
    ASSERT_TRUE(d);
    EXPECT_TRUE((*d)["detail"]["not_compiled"].asBool());
    EXPECT_EQ((*d)["id"].asString(), "as11") << "no address known";

    in.ble_compiled = true;
    in.as11_address = "AA:BB:CC:DD:EE:FF";
    in.as11 = parse(R"({"paired":true,"connected":true,"therapy":true,"streaming":true,"last_contact":1762299999})");
    out = buildDevices(in, store);
    d = find(out, "as11_ble");
    EXPECT_EQ((*d)["id"].asString(), "AA:BB:CC:DD:EE:FF");
    EXPECT_EQ((*d)["present"].asString(), "yes");
    EXPECT_EQ((*d)["recording"].asString(), "yes");

    // Not connected, but the last pull within pull_hours answered: here.
    in.as11 = parse(R"({"paired":true,"connected":false,"therapy":null,"last_contact":1762280000,
                        "last_summary_at":1762290000,"last_summary_day":"2026-11-04","last_summary_error":""})");
    out = buildDevices(in, store);
    d = find(out, "as11_ble");
    EXPECT_EQ((*d)["present"].asString(), "yes");
    EXPECT_EQ((*d)["recording"].asString(), "no") << "the ledger, when not connected";
    EXPECT_EQ((*d)["last_heard"].asInt64(), 1762290000);

    // A pull that failed long ago, no link: away.
    in.as11 = parse(R"({"connected":false,"last_contact":1762000000,"last_summary_at":1762000000,
                        "last_summary_error":"no summary"})");
    out = buildDevices(in, store);
    EXPECT_EQ((*find(out, "as11_ble"))["present"].asString(), "no");
}

// SDD-064: read after the night, so never "recording" from here.
TEST(Devices, TheDreamStation2OverBluetooth) {
    DevicesStore store("");
    auto in = none();
    in.ds2_enabled  = true;
    in.ble_compiled = false;
    auto out = buildDevices(in, store);
    const auto* d = find(out, "ds2_ble");
    ASSERT_TRUE(d);
    EXPECT_TRUE((*d)["detail"]["not_compiled"].asBool());
    EXPECT_EQ((*d)["id"].asString(), "ds2");
    EXPECT_EQ((*d)["default_name"].asString(), "DreamStation 2");

    in.ble_compiled = true;
    in.ds2 = parse(R"({"paired":true,"machine":{"name":"PR BT 0001","serial":"TEST0000000002","model":"520X110C",
                        "firmware":"V1"},"last_read":{"at":)" + std::to_string(kNow - 3600) +
                   R"(,"attempt_at":)" + std::to_string(kNow - 3600) + R"(,"records":2,"error":""}})");
    out = buildDevices(in, store);
    d = find(out, "ds2_ble");
    EXPECT_EQ((*d)["present"].asString(), "yes");
    EXPECT_EQ((*d)["recording"].asString(), "unknown");
    EXPECT_EQ((*d)["version"].asString(), "V1");
    EXPECT_EQ((*d)["last_heard"].asInt64(), kNow - 3600);

    // Tried, never finished: away.
    in.ds2 = parse(R"({"paired":true,"last_read":{"at":0,"attempt_at":)" + std::to_string(kNow - 60) +
                   R"(,"records":0,"error":"no answer"}})");
    out = buildDevices(in, store);
    EXPECT_EQ((*find(out, "ds2_ble"))["present"].asString(), "no");

    // Paired, never read: unknown.
    in.ds2 = parse(R"({"paired":true,"last_read":null})");
    out = buildDevices(in, store);
    EXPECT_EQ((*find(out, "ds2_ble"))["present"].asString(), "unknown");
    EXPECT_TRUE(knownKind("ds2_ble"));
}

TEST(Devices, OrderIsHereFirstThenLastHeardThenNeverHeard) {
    DevicesStore store("");
    auto in = none();
    in.mule["units"].append(muleUnit("A", false, 0));
    in.mule["units"].append(muleUnit("B", false, kNow - 100));
    in.mule["units"].append(muleUnit("C", true, kNow - 50));
    in.sleepbridge["bridges"].append(bridge("D", false, kNow - 10));
    const auto out = buildDevices(in, store);
    std::string order;
    for (const auto& d : out["devices"]) order += d["id"].asString();
    EXPECT_EQ(order, "CDBA");
}

// ── names and forgetting (D12) ──

TEST(DevicesStore, NamesAreTrimmedLimitedAndCleared) {
    std::string t, why;
    EXPECT_TRUE(validName("  Bedroom bridge  ", &t, &why));
    EXPECT_EQ(t, "Bedroom bridge");
    EXPECT_TRUE(validName(std::string(40, 'x'), &t, &why));
    EXPECT_FALSE(validName(std::string(41, 'x'), &t, &why));
    EXPECT_NE(why.find("40"), std::string::npos);
    // 40 characters of two bytes each is still 40 characters.
    std::string hu;
    for (int i = 0; i < 40; i++) hu += "\xC3\xA9";   // é
    EXPECT_TRUE(validName(hu, &t, &why));
    EXPECT_FALSE(validName(hu + "\xC3\xA9", &t, &why));
    EXPECT_FALSE(validName("a\x01b", &t, &why));
    EXPECT_FALSE(validName("\xC3", &t, &why)) << "broken UTF-8";
    EXPECT_TRUE(validName("   ", &t, &why));
    EXPECT_EQ(t, "") << "empty clears the name";
}

TEST(DevicesStore, ANameSurvivesARestartAndAppliesToItsCard) {
    Dir dir;
    const std::string path = (dir.path / "devices.json").string();
    {
        DevicesStore s(path);
        std::string why;
        ASSERT_TRUE(s.setName("sleepbridge", "TAP-1", "Bedroom bridge", &why)) << why;
        EXPECT_FALSE(s.setName("sleepbridge", "TAP-1", std::string(41, 'x'), &why));
    }
    DevicesStore s(path);
    EXPECT_EQ(s.name("sleepbridge", "TAP-1"), "Bedroom bridge");
    auto in = none();
    in.sleepbridge["bridges"].append(bridge("TAP-1", true, kNow));
    const auto out = buildDevices(in, s);
    EXPECT_EQ(out["devices"][0]["name"].asString(), "Bedroom bridge");
    EXPECT_EQ(out["devices"][0]["own_name"].asString(), "Bedroom bridge");
    EXPECT_EQ(out["devices"][0]["default_name"].asString(), "TAP-1");
    std::string why;
    ASSERT_TRUE(s.setName("sleepbridge", "TAP-1", "", &why));
    EXPECT_EQ(buildDevices(in, s)["devices"][0]["name"].asString(), "TAP-1");
    EXPECT_TRUE(buildDevices(in, s)["devices"][0]["own_name"].isNull());
}

TEST(DevicesStore, AMissingOrCorruptFileReadsAsEmpty) {
    Dir dir;
    DevicesStore missing((dir.path / "none.json").string());
    EXPECT_EQ(missing.name("mule", "FE-1"), "");
    const auto bad = dir.path / "devices.json";
    { std::ofstream(bad) << "{not json"; }
    DevicesStore corrupt(bad.string());
    EXPECT_EQ(corrupt.name("mule", "FE-1"), "");
    EXPECT_FALSE(corrupt.forgottenAt("mule", "FE-1"));
    // Nothing is hidden.
    auto in = none();
    in.mule["units"].append(muleUnit("FE-1", false, 100));
    EXPECT_EQ(buildDevices(in, corrupt)["devices"].size(), 1u);
}

TEST(DevicesStore, AForgottenDeviceIsHiddenUntilHeardFromAgain) {
    Dir dir;
    const std::string path = (dir.path / "devices.json").string();
    DevicesStore s(path);
    std::string why;
    ASSERT_TRUE(s.setName("mule", "FE-1", "Old unit", &why));
    s.forget("mule", "FE-1", kNow - 10);
    EXPECT_EQ(s.name("mule", "FE-1"), "") << "its name goes with it";
    auto in = none();
    in.mule["units"].append(muleUnit("FE-1", false, kNow - 500));
    EXPECT_TRUE(buildDevices(in, s)["devices"].empty());
    // Survives a restart.
    DevicesStore again(path);
    EXPECT_TRUE(buildDevices(in, again)["devices"].empty());
    // Heard from after it was forgotten: back, as new, and the mark is gone.
    in.mule["units"][0] = muleUnit("FE-1", true, kNow);
    const auto out = buildDevices(in, again);
    ASSERT_EQ(out["devices"].size(), 1u);
    EXPECT_EQ(out["devices"][0]["name"].asString(), "FE-1");
    EXPECT_FALSE(again.forgottenAt("mule", "FE-1"));
    EXPECT_FALSE(DevicesStore(path).forgottenAt("mule", "FE-1"));
}

TEST(DevicesStore, OnlyTheTwoStreamingKindsAreForgotten) {
    EXPECT_TRUE(forgettableKind("mule"));
    EXPECT_TRUE(forgettableKind("sleepbridge"));
    for (const char* k : {"o2ring", "ezshare", "flashair", "as11_ble"}) {
        EXPECT_FALSE(forgettableKind(k)) << k;
        EXPECT_TRUE(knownKind(k)) << k;
    }
    EXPECT_FALSE(knownKind("coyote"));
}

// ── the recording warning ──

TEST(DevicesRecording, UnknownWarnsAndTheBodyIsTheCloudsShape) {
    Recording no;
    EXPECT_FALSE(no.warns());
    Recording maybe{"unknown", "not heard from since hms-cpap started"};
    EXPECT_TRUE(maybe.warns());
    const auto w = recordingWarning(Recording{"yes", "the machine wrote to its card 1 min ago"});
    EXPECT_FALSE(w["ok"].asBool());
    EXPECT_EQ(w["warning"].asString(), "recording");
    EXPECT_TRUE(w["confirm"].asBool());
    EXPECT_EQ(w["reason"].asString(), "the machine wrote to its card 1 min ago");
    EXPECT_TRUE(confirmed(parse(R"({"confirm":true})")));
    EXPECT_FALSE(confirmed(parse(R"({"confirm":"true"})"))) << "a boolean, exactly";
    EXPECT_FALSE(confirmed(parse(R"({})")));
}

// ── the job object ──

TEST(DevicesOtaJob, RoundTripsThroughItsFile) {
    Dir dir;
    OtaJob j;
    j.job_id                    = "17";
    j.serial                    = "FE-1";
    j.kind                      = "mule_miner";
    j.source                    = "manifest 6.1.0";
    j.confirmed_while_recording = true;
    OtaStep m;
    m.target         = "mule";
    m.phase          = "done";
    m.version_before = "6.0.0";
    m.version_target = "6.1.0";
    m.version_after  = "6.1.0";
    OtaStep n;
    n.target = "miner";
    n.phase  = "transferring";
    n.bytes  = 412000;
    n.total  = 1048576;
    j.steps  = {m, n};
    j.step   = 1;
    j.started_at = kNow;
    ASSERT_TRUE(otaSave(dir.path.string(), "FE-1", j));
    const auto all = otaLoadAll(dir.path.string());
    ASSERT_EQ(all.size(), 1u);
    EXPECT_EQ(all[0].first, "FE-1");
    const auto v = otaJson(all[0].second);
    EXPECT_EQ(v["phase"].asString(), "transferring");
    EXPECT_EQ(v["step"].asUInt64(), 2u);
    EXPECT_EQ(v["steps"].asUInt64(), 2u);
    EXPECT_EQ(v["steps_detail"][1]["bytes"].asUInt64(), 412000u);
    EXPECT_EQ(v["steps_detail"][0]["version_after"].asString(), "6.1.0");
    EXPECT_TRUE(v["confirmed_while_recording"].asBool());
    EXPECT_TRUE(v["hold_reason"].isNull());
    EXPECT_TRUE(v["reason"].isNull());
    EXPECT_TRUE(v["ended_at"].isNull());
    EXPECT_EQ(v["path"].asString(), "local");
}

TEST(DevicesOtaJob, ARestartFailsWhatWasCutAndKeepsWhatIsRestarting) {
    for (const char* p : {"queued", "preparing", "transferring", "verifying"}) {
        OtaJob j;
        j.job_id = "1";
        OtaStep s;
        s.phase = p;
        j.steps = {s};
        otaAfterRestart(j, kNow);
        EXPECT_EQ(j.phase(), "failed") << p;
        EXPECT_EQ(j.reason, "interrupted") << p;
        EXPECT_TRUE(otaJson(j)["retryable"].asBool());
    }
    OtaJob r;
    r.job_id = "2";
    OtaStep s;
    s.phase = "restarting";
    r.steps = {s};
    otaAfterRestart(r, kNow);
    EXPECT_EQ(r.phase(), "restarting");
    EXPECT_FALSE(r.ended());
    OtaJob d;
    d.job_id = "3";
    s.phase  = "done";
    d.steps  = {s};
    otaAfterRestart(d, kNow);
    EXPECT_EQ(d.phase(), "done");
}

TEST(DevicesOtaJob, OnlyTheRetryableReasonsOfferTryAgain) {
    for (const char* r : {"refused_image", "connection_lost", "back_on_old_version", "image_unavailable", "interrupted"})
        EXPECT_TRUE(otaRetryable(r)) << r;
    for (const char* r : {"offline", "superseded", "bake_failed", ""}) EXPECT_FALSE(otaRetryable(r)) << r;
    EXPECT_FALSE(otaFromJson(parse("[]")));
    EXPECT_FALSE(otaFromJson(parse(R"({"job_id":"1"})")));
}
