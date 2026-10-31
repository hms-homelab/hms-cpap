#include <gtest/gtest.h>

#include "clients/LocalDataSource.h"
#include "mule/MuleService.h"
#include "mule/PosixTz.h"
#include "services/As11NightWriter.h"

#include <json/json.h>

#include <chrono>
#include <cstdio>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <functional>
#include <map>
#include <memory>
#include <sstream>
#include <string>
#include <vector>

// The Mule & Miner local stream: a fake unit drives the receiver frame by
// frame, as the unit's socket would, and what lands is read back as a card.
// Every value is made up; no real night, no real unit.

using namespace hms_cpap;
using namespace hms_cpap::mule;
namespace fs = std::filesystem;

namespace {

constexpr const char* kUnit    = "FE-C3-00000001";
constexpr const char* kMachine = "23000000001";
constexpr const char* kTz      = "EST5EDT,M3.2.0,M11.1.0";
// 2026-10-17 03:00:00 UTC: 23:00 on the 16th in New York (daylight time),
// the night of the 16th.
constexpr int64_t kT0 = 1792206000000LL;

Json::Value parse(const std::string& text) {
    Json::Value v;
    Json::CharReaderBuilder rb;
    std::istringstream ss(text);
    Json::parseFromStream(rb, ss, &v, nullptr);
    return v;
}

std::string compact(const Json::Value& v) {
    Json::StreamWriterBuilder w;
    w["indentation"] = "";
    return Json::writeString(w, v);
}

// One block as the unit sends it: 10 s, flow and mask pressure at 25 Hz, and
// the leak and respiratory rate every 2 s.
as11::StreamBlock tenSeconds(int64_t start) {
    as11::StreamBlock b;
    b.start_ms = start;
    b.serial   = kMachine;
    b.channels.push_back({as11::kFlow, 0, std::vector<int16_t>(250, 300)});
    b.channels.push_back({as11::kMaskPressure, 0, std::vector<int16_t>(250, 900)});
    b.channels.push_back({as11::kLeak, 1, std::vector<int16_t>(5, 120)});
    b.channels.push_back({as11::kRespRate, 1, std::vector<int16_t>(5, 150)});
    return b;
}

// The unit's end of one connection. What the receiver sends down lands in
// `down`; `on_down` sees each frame as it is sent.
class FakeUnit {
public:
    FakeUnit(MuleService& svc, std::string serial = kUnit) : svc_(svc), serial_(std::move(serial)) {
        conn_ = svc_.open(serial_, [this](const std::string& t) {
            down.push_back(parse(t));
            if (on_down) on_down(down.back());
            return true;
        });
    }
    ~FakeUnit() { svc_.closed(conn_); }

    void text(const Json::Value& v) { svc_.onText(conn_, compact(v)); }

    void hello(bool streaming = true, const std::string& mode = "as11_ble") {
        Json::Value h(Json::objectValue);
        h["t"]           = "hello";
        h["fw"]          = "9.9.9";
        h["miner_fw"]    = "9.9.9";
        h["boot_reason"] = "poweron";
        h["mode"]        = mode;
        h["result"]      = Json::nullValue;
        h["state"]       = state(streaming, 0);
        text(h);
    }

    static Json::Value state(bool streaming, int buffered, const Json::Value& events = Json::Value()) {
        Json::Value s(Json::objectValue);
        s["tz"]                     = kTz;
        s["as11"]["enabled"]        = true;
        s["as11"]["paired"]         = true;
        s["as11"]["streaming"]      = streaming;
        s["as11"]["stream_buffered"] = buffered;
        s["as11"]["stream_dropped"] = 0;
        if (events.isArray()) s["resp_events"] = events;
        return s;
    }

    void sendState(bool streaming, int buffered, const Json::Value& events = Json::Value()) {
        Json::Value f(Json::objectValue);
        f["t"]      = "state";
        f["state"]  = state(streaming, buffered, events);
        f["result"] = Json::nullValue;
        text(f);
    }

    void block(const as11::StreamBlock& b) {
        const auto body = as11::encodeCds1(b);
        std::string frame = encodeHeader({0, 0, 1, kKindBlock});
        frame.append(reinterpret_cast<const char*>(body.data()), body.size());
        svc_.onBinary(conn_, frame);
    }

    // An as11_download's answer: the bytes as kind 6 frames, the empty last
    // frame, then the result.
    void answerPull(uint32_t req_id, const std::string& bytes, size_t chunk = 700) {
        uint32_t seq = 0;
        for (size_t at = 0; at < bytes.size(); at += chunk) {
            std::string f = encodeHeader({req_id, seq++, 0, kKindAs11});
            f += bytes.substr(at, chunk);
            svc_.onBinary(conn_, f);
        }
        svc_.onBinary(conn_, encodeHeader({req_id, seq, 1, kKindAs11}));
        Json::Value r(Json::objectValue);
        r["t"]      = "result";
        r["req_id"] = req_id;
        r["ok"]     = true;
        r["result"] = Json::Value(Json::objectValue);
        text(r);
    }

    std::vector<Json::Value> of(const std::string& t) const {
        std::vector<Json::Value> out;
        for (const auto& f : down)
            if (f["t"].asString() == t) out.push_back(f);
        return out;
    }

    std::vector<Json::Value>               down;
    std::function<void(const Json::Value&)> on_down;

private:
    MuleService& svc_;
    std::string  serial_;
    uint64_t     conn_{0};
};

// ── a Summary and a layout, enough for STR.edf (made up) ──

using Bytes = std::vector<uint8_t>;
Bytes varint(uint64_t n) {
    Bytes out;
    do { uint8_t b = n & 0x7F; n >>= 7; out.push_back(n ? (b | 0x80) : b); } while (n);
    return out;
}
void append(Bytes& to, const Bytes& b) { to.insert(to.end(), b.begin(), b.end()); }
Bytes fieldVarint(uint32_t num, uint64_t v) { Bytes o = varint(uint64_t(num) << 3); append(o, varint(v)); return o; }
Bytes fieldBytes(uint32_t num, const Bytes& v) {
    Bytes o = varint((uint64_t(num) << 3) | 2); append(o, varint(v.size())); append(o, v); return o;
}
constexpr uint64_t kDay = 86'400'000ULL;

Bytes summaryDay(uint64_t start_ms, int usage) {
    Bytes rec;
    append(rec, fieldVarint(2, start_ms));
    append(rec, fieldVarint(3, start_ms + kDay));
    append(rec, fieldVarint(5, static_cast<uint64_t>(usage)));
    Bytes sess, s;
    append(s, fieldVarint(1, start_ms + 36'000'000));
    append(s, fieldVarint(2, static_cast<uint64_t>(usage)));
    append(sess, fieldBytes(1, s));
    append(rec, fieldBytes(6, sess));
    append(rec, fieldVarint(7, 150));
    append(rec, fieldVarint(39, 1));
    return fieldBytes(2, rec);
}

struct Sig { const char* label; int dmin, dmax; double pmin, pmax; int spr; };

Bytes layout() {
    const std::vector<Sig> sigs = {
        {"Date", 0, 24836, 0, 24836, 1},   {"MaskOn", 0, 1440, 0, 1440, 10},
        {"MaskOff", 0, 1440, 0, 1440, 10}, {"MaskEvents", 0, 255, 0, 255, 1},
        {"Duration", 0, 1440, 0, 1440, 1}, {"AHI", 0, 32767, 0, 32.767, 1},
        {"Crc16", -32768, 32767, -32768, 32767, 1},
    };
    const size_t ns = sigs.size();
    std::string h(256 + 256 * ns, ' ');
    auto put = [&](size_t at, size_t w, const std::string& v) { h.replace(at, std::min(w, v.size()), v.substr(0, w)); };
    put(0, 8, "0");
    put(8, 80, "X X X X");
    put(168, 8, "01.01.00");
    put(176, 8, "12.00.00");
    put(184, 8, std::to_string(h.size()));
    put(236, 8, "0");
    put(244, 8, "86400");
    put(252, 4, std::to_string(ns));
    size_t at = 256;
    auto each = [&](size_t w, auto f) { for (size_t i = 0; i < ns; i++) put(at + i * w, w, f(sigs[i])); at += w * ns; };
    each(16, [](const Sig& s) { return std::string(s.label); });
    each(80, [](const Sig&) { return std::string(); });
    each(8, [](const Sig&) { return std::string(); });
    each(8, [](const Sig& s) { return std::to_string(s.pmin); });
    each(8, [](const Sig& s) { return std::to_string(s.pmax); });
    each(8, [](const Sig& s) { return std::to_string(s.dmin); });
    each(8, [](const Sig& s) { return std::to_string(s.dmax); });
    each(80, [](const Sig&) { return std::string(); });
    each(8, [](const Sig& s) { return std::to_string(s.spr); });
    each(32, [](const Sig&) { return std::string(); });
    return Bytes(h.begin(), h.end());
}

long records(const fs::path& p) {
    std::ifstream in(p, std::ios::binary);
    char f[9] = {};
    in.seekg(236);
    in.read(f, 8);
    return std::atol(f);
}

std::string slurp(const fs::path& p) {
    std::ifstream in(p, std::ios::binary);
    std::stringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

class MuleReceiverTest : public ::testing::Test {
protected:
    fs::path dir_;
    std::shared_ptr<As11NightWriter> writer_;
    std::unique_ptr<MuleService> svc_;

    void SetUp() override {
        dir_ = fs::temp_directory_path() /
               ("mule_rx_" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
        fs::create_directories(dir_);
        restart();
    }
    void TearDown() override {
        svc_.reset();
        fs::remove_all(dir_);
    }
    // A fresh receiver on the same disk: what an hms-cpap restart is.
    void restart() {
        svc_.reset();
        writer_ = std::make_shared<As11NightWriter>(dir_.string(), fixedOffsetClock(0));
        MuleService::Options o;
        o.root     = (dir_ / "mule").string();
        o.data_dir = dir_.string();
        svc_       = std::make_unique<MuleService>(o, writer_);
    }
    fs::path machineRoot() const { return dir_ / "as11" / kMachine; }
    fs::path blockFile(int64_t start) const {
        return dir_ / "mule" / kUnit / "blocks" / (std::to_string(start) + ".cds1");
    }
    Json::Value unitStatus() const {
        const Json::Value all = svc_->status();   // held: the loop reads into it
        for (const auto& u : all["units"])
            if (u["serial"].asString() == kUnit) return u;
        return Json::Value();
    }
};

Json::Value event(int seq, const std::string& name, int64_t at_ms, int dur) {
    const auto wc = fixedOffsetClock(0)(at_ms);
    char buf[40];
    std::snprintf(buf, sizeof(buf), "%04d-%02d-%02dT%02d:%02d:%02d.000Z", wc.year, wc.month, wc.day,
                  wc.hour, wc.minute, wc.second);
    Json::Value e(Json::objectValue);
    e["seq"]             = seq;
    e["event"]           = name;
    e["reportTime"]      = buf;
    e["durationSeconds"] = dur;
    e["backdateSeconds"] = dur;
    return e;
}

}  // namespace

// ── the night, end to end ──

TEST_F(MuleReceiverTest, AShortNightLandsAsACardWithItsEventsStrAndIdentification) {
    fs::create_directories(dir_ / "str-layouts");
    {
        const auto l = layout();
        std::ofstream(dir_ / "str-layouts" / "39420.edfh", std::ios::binary)
            .write(reinterpret_cast<const char*>(l.data()), static_cast<std::streamsize>(l.size()));
    }
    FakeUnit unit(*svc_);
    // Every block_ack must find its block already on disk.
    int acks_checked = 0;
    unit.on_down = [&](const Json::Value& f) {
        if (f["t"].asString() != "block_ack") return;
        EXPECT_TRUE(fs::exists(blockFile(f["start_ms"].asInt64()))) << f["start_ms"].asInt64();
        ++acks_checked;
    };

    unit.hello();
    ASSERT_EQ(unit.of("ack").size(), 1u);
    EXPECT_EQ(unit.of("ack")[0]["ping_s"].asInt(), kPingSec);

    // A minute of therapy, the machine's events among it.
    for (int i = 0; i < 6; i++) unit.block(tenSeconds(kT0 + i * 10000));
    Json::Value evs(Json::arrayValue);
    evs.append(event(1, "ObstructiveApneaEnd", kT0 + 20000, 12));
    evs.append(event(2, "HypopneaEnd", kT0 + 45000, 15));
    unit.sendState(true, 0, evs);
    unit.sendState(true, 0, evs);   // a state sent again: each event kept once
    EXPECT_EQ(acks_checked, 6);
    EXPECT_EQ(unit.of("block_ack").size(), 6u);
    EXPECT_TRUE(unit.of("cmd").empty()) << "nothing is asked of the machine during the night";

    // Mask off, with two blocks still held on the unit: not landed yet.
    unit.sendState(false, 2);
    EXPECT_TRUE(unit.of("cmd").empty());
    EXPECT_TRUE(unitStatus()["night_landing"].asBool());
    for (int i = 6; i < 8; i++) unit.block(tenSeconds(kT0 + i * 10000));
    unit.sendState(false, 0);

    // The night has landed: the machine's records are asked for, one at a
    // time, the Identification first.
    const std::string ident =
        R"({"IdentificationProfiles":{"Product":{"SerialNumber":"23000000001","ProductCode":"39420"},)"
        R"("Software":{"PlatformIdentifier":"36","VariantIdentifier":"1"}}})";
    Bytes spool = summaryDay(static_cast<uint64_t>(kT0) - 11 * 3'600'000ULL, 420);   // the 16th, noon EDT
    const std::string summary(spool.begin(), spool.end());
    const std::string settings(300, 'S');

    ASSERT_EQ(unit.of("cmd").size(), 1u);
    auto c = unit.of("cmd").back();
    EXPECT_EQ(c["cmd"].asString(), "as11_download");
    EXPECT_EQ(c["spool"].asString(), "Identification");
    EXPECT_EQ(c["from"].asString(), "2000-01-01");
    unit.answerPull(c["req_id"].asUInt(), ident);

    ASSERT_EQ(unit.of("cmd").size(), 2u);
    c = unit.of("cmd").back();
    EXPECT_EQ(c["spool"].asString(), "Summary");
    unit.answerPull(c["req_id"].asUInt(), summary);

    ASSERT_EQ(unit.of("cmd").size(), 3u);
    c = unit.of("cmd").back();
    EXPECT_EQ(c["spool"].asString(), "Settings");
    unit.answerPull(c["req_id"].asUInt(), settings);
    EXPECT_EQ(unit.of("cmd").size(), 3u);

    // Read back as a card.
    LocalDataSource card(machineRoot().string());
    const auto nights = card.listDateFolders();
    ASSERT_EQ(nights.size(), 1u) << ::testing::PrintToString(nights);
    EXPECT_EQ(nights[0], "20261016");
    std::map<std::string, size_t> names;
    for (const auto& f : card.listFiles("20261016")) names[f.name] = f.size_kb;
    EXPECT_TRUE(names.count("20261016_230000_BRP.edf")) << ::testing::PrintToString(names);
    EXPECT_TRUE(names.count("20261016_230000_PLD.edf"));
    EXPECT_TRUE(names.count("20261016_230000_EVE.edf"));
    EXPECT_TRUE(names.count("20261016_230000_CSL.edf"));
    EXPECT_FALSE(names.count("20261016_230000_SAD.edf")) << "no ring rode";

    const fs::path got = dir_ / "got";
    fs::create_directories(got);
    ASSERT_TRUE(card.downloadFile("20261016", "20261016_230000_BRP.edf", (got / "brp").string()));
    EXPECT_EQ(records(got / "brp"), 80);   // 80 seconds, one record each
    ASSERT_TRUE(card.downloadFile("20261016", "20261016_230000_EVE.edf", (got / "eve").string()));
    const std::string eve = slurp(got / "eve");
    EXPECT_NE(eve.find("Obstructive Apnea"), std::string::npos);
    EXPECT_NE(eve.find("Hypopnea"), std::string::npos);
    EXPECT_EQ(eve.find("Hypopnea"), eve.rfind("Hypopnea")) << "an event sent twice is kept once";

    ASSERT_TRUE(card.downloadRootFile("Identification.json", (got / "ident").string()));
    EXPECT_NE(slurp(got / "ident").find("\"FlowGenerator\""), std::string::npos);
    ASSERT_TRUE(card.downloadRootFile("STR.edf", (got / "str").string()));
    EXPECT_GE(records(got / "str"), 1);
    EXPECT_TRUE(fs::exists(machineRoot() / "AS11" / "summary-2000-01-01.as11"));
    EXPECT_EQ(fs::file_size(machineRoot() / "AS11" / "settings-2000-01-01.as11set"), settings.size());

    // What the status shows, and the next night's Summary asks from two
    // days before the newest one this brought.
    const auto st = unitStatus();
    EXPECT_TRUE(st["online"].asBool());
    EXPECT_EQ(st["machine"].asString(), kMachine);
    EXPECT_EQ(st["blocks_stored"].asUInt64(), 8u);
    EXPECT_EQ(st["summary_from"].asString(), "2026-10-14");
    for (const auto& k : st["commands"]) {
        EXPECT_EQ(k["state"].asString(), "answered");
        EXPECT_TRUE(k["automatic"].asBool());
    }
    EXPECT_NE(st["commands"][1]["outcome"].asString().find("STR.edf 1 day"), std::string::npos)
        << st["commands"][1]["outcome"].asString();
}

TEST_F(MuleReceiverTest, TheNightGrowsLiveBlockByBlockAndEventsLandAtOnce) {
    FakeUnit unit(*svc_);
    unit.hello();
    LocalDataSource card(machineRoot().string());
    const fs::path brp = machineRoot() / "DATALOG" / "20261016" / "20261016_230000_BRP.edf";
    const fs::path eve = machineRoot() / "DATALOG" / "20261016" / "20261016_230000_EVE.edf";

    // After each block the BRP is a readable EDF with that many seconds, and
    // the card lists the session while it is still growing.
    for (int i = 0; i < 3; i++) {
        unit.block(tenSeconds(kT0 + i * 10000));
        ASSERT_TRUE(fs::exists(brp)) << "block " << i;
        EXPECT_EQ(records(brp), 10 * (i + 1));
        const auto nights = card.listDateFolders();
        ASSERT_EQ(nights.size(), 1u);
        bool listed = false;
        for (const auto& f : card.listFiles("20261016")) listed = listed || f.name == "20261016_230000_BRP.edf";
        EXPECT_TRUE(listed) << "block " << i;
    }
    ASSERT_TRUE(fs::exists(eve)) << "the EVE is there from the first block";
    EXPECT_EQ(slurp(eve).find("Hypopnea"), std::string::npos);

    // An event in a state frame is in the EVE at once, the stream still open.
    Json::Value evs(Json::arrayValue);
    evs.append(event(1, "HypopneaEnd", kT0 + 25000, 14));
    unit.sendState(true, 0, evs);
    EXPECT_NE(slurp(eve).find("Hypopnea"), std::string::npos);
    EXPECT_FALSE(unitStatus()["night_landing"].asBool());
    EXPECT_TRUE(unit.of("cmd").empty()) << "nothing is pulled while the night runs";

    // The next block keeps the event and grows the BRP.
    unit.block(tenSeconds(kT0 + 30000));
    EXPECT_EQ(records(brp), 40);
    EXPECT_NE(slurp(eve).find("Hypopnea"), std::string::npos);
}

TEST_F(MuleReceiverTest, ARedeliveredBlockIsAckedAgainAndKeptOnceAcrossARestart) {
    {
        FakeUnit unit(*svc_);
        unit.hello();
        for (int i = 0; i < 3; i++) unit.block(tenSeconds(kT0 + i * 10000));
        unit.block(tenSeconds(kT0 + 10000));   // the ack was lost: sent again
        EXPECT_EQ(unit.of("block_ack").size(), 4u);
        EXPECT_EQ(unitStatus()["blocks_stored"].asUInt64(), 3u);
        EXPECT_EQ(unitStatus()["blocks_resent"].asUInt64(), 1u);
    }
    // hms-cpap restarts between a write and its ack: the unit sends the block
    // again on its next connection.
    restart();
    FakeUnit unit(*svc_);
    unit.hello();
    unit.block(tenSeconds(kT0 + 20000));
    ASSERT_EQ(unit.of("block_ack").size(), 1u);
    EXPECT_EQ(unit.of("block_ack")[0]["start_ms"].asInt64(), kT0 + 20000);
    unit.sendState(false, 0);

    size_t held = 0;
    for (const auto& f : fs::directory_iterator(dir_ / "mule" / kUnit / "blocks"))
        if (f.path().extension() == ".cds1") ++held;
    EXPECT_EQ(held, 3u);
    LocalDataSource card(machineRoot().string());
    const fs::path got = dir_ / "brp";
    ASSERT_TRUE(card.downloadFile("20261016", "20261016_230000_BRP.edf", got.string()));
    EXPECT_EQ(records(got), 30);
}

TEST_F(MuleReceiverTest, ATestHelloIsAckedAndSavesNothing) {
    FakeUnit unit(*svc_);
    Json::Value h(Json::objectValue);
    h["t"]        = "hello";
    h["test"]     = true;
    h["fw"]       = "9.9.9";
    h["miner_fw"] = "9.9.9";
    h["mode"]     = "as11_ble";
    unit.text(h);
    ASSERT_EQ(unit.down.size(), 1u);
    EXPECT_EQ(unit.down[0]["t"].asString(), "ack");
    EXPECT_FALSE(fs::exists(dir_ / "mule"));
    EXPECT_EQ(svc_->status()["units"].size(), 0u);
    // Not a session: a command for it has no unit to go to.
    Json::Value cmd(Json::objectValue);
    cmd["cmd"] = "restate";
    EXPECT_EQ(svc_->queueCommand(kUnit, cmd).status, 404);
}

TEST_F(MuleReceiverTest, ACardModeHelloIsRefusedWithItsReason) {
    FakeUnit unit(*svc_);
    unit.hello(false, "card");
    ASSERT_EQ(unit.of("error").size(), 1u);
    EXPECT_NE(unit.of("error")[0]["error"].asString().find("Home network mode"), std::string::npos);
    EXPECT_TRUE(unit.of("ack").empty());
    EXPECT_FALSE(fs::exists(dir_ / "mule"));
}

TEST_F(MuleReceiverTest, EventsBeforeAnyBlockWaitForTheMachinesName) {
    FakeUnit unit(*svc_);
    unit.hello();
    Json::Value evs(Json::arrayValue);
    evs.append(event(1, "CentralApneaEnd", kT0 + 5000, 10));
    unit.sendState(true, 0, evs);
    EXPECT_FALSE(fs::exists(machineRoot() / "stream" / "events.jsonl"));
    unit.block(tenSeconds(kT0));
    EXPECT_NE(slurp(machineRoot() / "stream" / "events.jsonl").find("CentralApneaEnd"), std::string::npos);
}

// ── the commands ──

TEST_F(MuleReceiverTest, CommandsGoDownOneAtATimeAndTheirResultsComeBack) {
    FakeUnit unit(*svc_);
    unit.hello();

    Json::Value logs(Json::objectValue);
    logs["cmd"] = "logs";
    logs["n"]   = 50;
    const auto a = svc_->queueCommand(kUnit, logs);
    ASSERT_EQ(a.status, 200) << compact(a.body);
    Json::Value back(Json::objectValue);
    back["cmd"]     = "config";
    back["hms_url"] = "";
    const auto b = svc_->queueCommand(kUnit, back);
    ASSERT_EQ(b.status, 200) << compact(b.body);
    EXPECT_EQ(b.body["command"]["state"].asString(), "queued") << "one command in flight";

    ASSERT_EQ(unit.of("cmd").size(), 1u);
    const auto sent = unit.of("cmd")[0];
    EXPECT_EQ(sent["cmd"].asString(), "logs");
    EXPECT_EQ(sent["n"].asInt(), 50);
    EXPECT_EQ(sent["req_id"].asUInt64(), a.body["req_id"].asUInt64());

    Json::Value r(Json::objectValue);
    r["t"]              = "result";
    r["req_id"]         = sent["req_id"];
    r["ok"]             = true;
    r["result"]["logs"] = "I (1) boot\nI (2) ws: connected\n";
    unit.text(r);

    ASSERT_EQ(unit.of("cmd").size(), 2u);
    const auto sw = unit.of("cmd")[1];
    EXPECT_EQ(sw["cmd"].asString(), "config");
    EXPECT_EQ(compact(sw["config"]), R"({"hms_url":""})");
    Json::Value r2(Json::objectValue);
    r2["t"]                           = "result";
    r2["req_id"]                      = sw["req_id"];
    r2["ok"]                          = true;
    r2["result"]["ok"]                = true;
    r2["result"]["hms_url"]["ok"]     = true;
    r2["result"]["hms_url"]["switched"] = true;
    r2["result"]["hms_url"]["url"]    = "";
    unit.text(r2);

    std::map<std::string, Json::Value> by;
    const Json::Value st = unitStatus();
    for (const auto& k : st["commands"]) by[k["cmd"].asString()] = k;
    ASSERT_EQ(by.size(), 2u);
    EXPECT_EQ(by["logs"]["state"].asString(), "answered");
    EXPECT_NE(by["logs"]["result"]["result"]["logs"].asString().find("ws: connected"), std::string::npos);
    EXPECT_EQ(by["config"]["state"].asString(), "answered");
    EXPECT_TRUE(by["config"]["result"]["result"]["hms_url"]["switched"].asBool());
    EXPECT_FALSE(by["config"]["automatic"].asBool());

    // A refusal comes back as the unit sent it.
    Json::Value pair(Json::objectValue);
    pair["cmd"] = "as11_pair";
    ASSERT_EQ(svc_->queueCommand(kUnit, pair).status, 200);
    const auto p = unit.of("cmd").back();
    EXPECT_EQ(p["cmd"].asString(), "as11_pair");
    EXPECT_TRUE(p["start"].asBool());
    Json::Value no(Json::objectValue);
    no["t"]      = "result";
    no["req_id"] = p["req_id"];
    no["ok"]     = false;
    no["error"]  = "no answer from the card reader";
    unit.text(no);
    EXPECT_EQ(unitStatus()["commands"][0]["result"]["error"].asString(), "no answer from the card reader");
}

TEST_F(MuleReceiverTest, RebootDoesNotHoldTheNextCommandAndOfflineCommandsWait) {
    FakeUnit unit(*svc_);
    unit.hello();
    Json::Value reboot(Json::objectValue);
    reboot["cmd"]    = "reboot";
    reboot["target"] = "miner";
    ASSERT_EQ(svc_->queueCommand(kUnit, reboot).status, 200);
    Json::Value restate(Json::objectValue);
    restate["cmd"] = "restate";
    ASSERT_EQ(svc_->queueCommand(kUnit, restate).status, 200);
    ASSERT_EQ(unit.of("cmd").size(), 2u) << "a reboot is never answered, so it holds nothing";
    EXPECT_EQ(unit.of("cmd")[0]["target"].asString(), "miner");
    EXPECT_EQ(unit.of("cmd")[1]["cmd"].asString(), "restate");
}

TEST_F(MuleReceiverTest, AQueuedCommandGoesOnTheNextSession) {
    { FakeUnit unit(*svc_); unit.hello(); }   // seen, then gone
    Json::Value reset(Json::objectValue);
    reset["cmd"] = "reset";
    const auto a = svc_->queueCommand(kUnit, reset);
    ASSERT_EQ(a.status, 200);
    EXPECT_EQ(a.body["command"]["state"].asString(), "queued");
    FakeUnit unit(*svc_);
    unit.hello();
    ASSERT_EQ(unit.of("cmd").size(), 1u);
    EXPECT_EQ(unit.of("cmd")[0]["cmd"].asString(), "reset");
    EXPECT_GT(unit.down.size(), 1u);
    EXPECT_EQ(unit.down[0]["t"].asString(), "ack") << "the ack goes ahead of any command";
}

TEST_F(MuleReceiverTest, OnlyTheAllowedCommandsAndArgumentsAreTaken) {
    FakeUnit unit(*svc_);
    unit.hello();
    auto status = [&](const std::string& json) { return svc_->queueCommand(kUnit, parse(json)).status; };
    EXPECT_EQ(status(R"({"cmd":"ota","url":"http://x"})"), 400);
    EXPECT_EQ(status(R"({"cmd":"download","path":"STR.edf"})"), 400);
    EXPECT_EQ(status(R"({"cmd":"config","config":{"wifi_ssid":"x"}})"), 400);
    EXPECT_EQ(status(R"({"cmd":"config","config":{"hms_url":"","tz":"UTC0"}})"), 400);
    EXPECT_EQ(status(R"({"cmd":"config","hms_url":"ws://192.168.1.20"})"), 400);
    EXPECT_EQ(status(R"({"cmd":"config","hms_url":"192.168.1.20:8894"})"), 400);
    EXPECT_EQ(status(R"({"cmd":"config","hms_url":"hmscpap.local/mule"})"), 400);
    EXPECT_EQ(status(R"({"cmd":"reboot","target":"everything"})"), 400);
    EXPECT_EQ(status(R"({"cmd":"logs","n":0})"), 400);
    EXPECT_EQ(status(R"({"cmd":"as11_pair","code":"12a4"})"), 400);
    EXPECT_EQ(status(R"({"cmd":"as11_download","from":"yesterday"})"), 400);
    EXPECT_EQ(status(R"({"cmd":"as11_download","from":"2026-10-01","spool":"Everything"})"), 400);
    EXPECT_TRUE(unit.of("cmd").empty());
    EXPECT_EQ(svc_->queueCommand("FE-C3-99999999", parse(R"({"cmd":"restate"})")).status, 404);

    EXPECT_EQ(status(R"({"cmd":"config","config":{"hms_url":"hmscpap.local"}})"), 200);
    EXPECT_EQ(compact(unit.of("cmd").back()["config"]), R"({"hms_url":"hmscpap.local"})");
}

// ── the wire's pieces ──

TEST(MuleWire, HeaderRoundTripsLittleEndian) {
    const FrameHeader h{0x01020304u, 7u, 1u, kKindAs11};
    const std::string s = encodeHeader(h);
    ASSERT_EQ(s.size(), kHeaderSize);
    EXPECT_EQ(static_cast<uint8_t>(s[0]), 0x04);
    EXPECT_EQ(static_cast<uint8_t>(s[10]), kKindAs11);
    const auto back = parseHeader(s);
    ASSERT_TRUE(back.has_value());
    EXPECT_EQ(back->req_id, h.req_id);
    EXPECT_EQ(back->seq, 7u);
    EXPECT_EQ(back->flags, 1u);
    EXPECT_FALSE(parseHeader("short").has_value());
}

TEST(MuleWire, SerialsAndHostsAreHeldToTheirShape) {
    EXPECT_TRUE(validSerial("FE-C3-00000001"));
    EXPECT_FALSE(validSerial(""));
    EXPECT_FALSE(validSerial(".hidden"));
    EXPECT_FALSE(validSerial("a/b"));
    EXPECT_TRUE(validUnitHost(""));
    EXPECT_TRUE(validUnitHost("192.168.1.20"));
    EXPECT_TRUE(validUnitHost("hmscpap.local"));
    EXPECT_FALSE(validUnitHost("-x"));
    EXPECT_FALSE(validUnitHost(std::string(64, 'a')));
}

TEST(MulePosixTz, DaylightRulesAndFixedZones) {
    const auto ny = parsePosixTz("EST5EDT,M3.2.0,M11.1.0");
    ASSERT_TRUE(ny.has_value());
    // 2026-03-08 06:59:59 UTC is 01:59:59 EST; a second later it is 03:00 EDT.
    EXPECT_EQ(ny->offsetAt(1772953199), -5 * 3600);
    EXPECT_EQ(ny->offsetAt(1772953200), -4 * 3600);
    // 2026-11-01 05:59:59 UTC is 01:59:59 EDT; a second later 01:00 EST.
    EXPECT_EQ(ny->offsetAt(1793512799), -4 * 3600);
    EXPECT_EQ(ny->offsetAt(1793512800), -5 * 3600);

    const auto cet = parsePosixTz("CET-1CEST,M3.5.0,M10.5.0/3");
    ASSERT_TRUE(cet.has_value());
    EXPECT_EQ(cet->offsetAt(1784000000), 2 * 3600);   // July
    EXPECT_EQ(cet->offsetAt(1767225600), 3600);       // 2026-01-01

    // Southern hemisphere: daylight across the new year.
    const auto syd = parsePosixTz("AEST-10AEDT,M10.1.0,M4.1.0/3");
    ASSERT_TRUE(syd.has_value());
    EXPECT_EQ(syd->offsetAt(1767225600), 11 * 3600);   // January
    EXPECT_EQ(syd->offsetAt(1784000000), 10 * 3600);   // July

    EXPECT_EQ(parsePosixTz("<+0530>-5:30")->offsetAt(0), 5 * 3600 + 1800);
    EXPECT_EQ(parsePosixTz("UTC0")->offsetAt(0), 0);
    EXPECT_FALSE(parsePosixTz("").has_value());
    EXPECT_FALSE(parsePosixTz("America/New_York").has_value());
    EXPECT_FALSE(parsePosixTz("EST5EDT,M13.1.0,M11.1.0").has_value());

    const auto clock = posixTzClock("EST5EDT,M3.2.0,M11.1.0");
    ASSERT_TRUE(clock.has_value());
    const auto wc = (*clock)(kT0);
    EXPECT_EQ(wc.day, 16);
    EXPECT_EQ(wc.hour, 23);
}
