#include <gtest/gtest.h>

#include "clients/AirSense11BleClient.h"
#include "clients/As11Protocol.h"
#include "clients/O2RingBleClient.h"
#include "clients/OxyIIProtocol.h"
#include "clients/ble/BleTransport.h"
#include "clients/ble/LinkRecovery.h"
#include "web/As11PairingApi.h"

#include <atomic>
#include <algorithm>
#include <chrono>
#include <ctime>
#include <filesystem>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

// The two direct Bluetooth clients over a fake ble::ITransport: every decision
// they make about the link (which device, cached or advertising, the radio,
// writes split to the MTU, with or without response, the drop, the T8520
// skip) runs here with no Bluetooth, on every OS. The conversations on top
// have their own suites (test_As11Session, test_OxyIIProtocol).

using namespace hms_cpap;
using namespace std::chrono_literals;

namespace {

struct Write {
    std::vector<uint8_t> bytes;
    bool with_response;
};

struct FakeLink;

/// What the fake does when a piece is written: may answer through notify().
using Responder = std::function<void(FakeLink&, const std::vector<uint8_t>&)>;

struct LinkLog {
    std::mutex m;
    std::vector<Write> writes;
    int closes = 0;
};

struct FakeLink : ble::ILink {
    std::shared_ptr<LinkLog> log;
    ble::LinkHandlers handlers;
    uint16_t mtu_ = 0;
    Responder responder;
    ble::WriteResult fail;   // returned instead of ok when fail.error or fail.timed_out is set
    bool up = true;

    ble::WriteResult write(const std::vector<uint8_t>& piece, bool with_response) override {
        {
            std::lock_guard<std::mutex> lk(log->m);
            log->writes.push_back({piece, with_response});
        }
        if (fail.timed_out || !fail.error.empty()) return fail;
        if (responder) responder(*this, piece);
        ble::WriteResult ok;
        ok.ok = true;
        return ok;
    }
    uint16_t mtu() const override { return mtu_; }
    bool connected() const override { return up; }
    void close() override {
        std::lock_guard<std::mutex> lk(log->m);
        log->closes++;
        up = false;
    }
    void notify(const std::vector<uint8_t>& v) {
        if (handlers.notify) handlers.notify(v);
    }
};

struct FakeTransport : ble::ITransport {
    bool available_ = true;
    ble::RadioStatus radio_{ble::Radio::Ready, "", "fake0"};
    std::vector<ble::Device> known_;
    std::vector<ble::Device> adverts_;
    bool discover_starts = true;

    // connect()
    ble::LinkError connect_error = ble::LinkError::None;
    // #67: one outcome per connect, in order; connect_error once it runs out.
    std::vector<ble::LinkError> connect_script;
    std::vector<std::string> unreachable_ids;   // these ids refuse with Connect
    uint16_t mtu = 0;
    Responder responder;
    ble::WriteResult write_fail;
    std::function<void(ble::LinkHandlers&)> after_connect;

    std::shared_ptr<LinkLog> log = std::make_shared<LinkLog>();
    std::vector<std::string> connected_ids;
    std::vector<ble::GattSpec> specs;
    std::vector<std::string> forgotten;
    int radio_calls = 0;
    int discover_calls = 0;
    // #67: resetAdapter(), and every connect, forget and reset in order.
    ble::AdapterReset reset_result{true, ""};
    int resets = 0;
    std::vector<std::string> events;

    const char* backend() const override { return "fake"; }
    bool available() const override { return available_; }
    ble::RadioStatus radio(bool) override {
        radio_calls++;
        return radio_;
    }
    std::vector<ble::Device> known() override { return known_; }
    ble::DiscoverResult discover(const std::function<bool(const ble::Device&)>& match, int) override {
        discover_calls++;
        ble::DiscoverResult r;
        if (!discover_starts) {
            r.started = false;
            r.error = "adapter busy";
            return r;
        }
        for (const auto& d : adverts_)
            if (match(d)) {
                r.device = d;
                break;
            }
        return r;
    }
    ble::ConnectResult connect(const ble::Device& d, const ble::GattSpec& spec, ble::LinkHandlers h,
                               const ble::ConnectOptions&) override {
        connected_ids.push_back(d.id);
        specs.push_back(spec);
        events.push_back("connect:" + d.id);
        ble::ConnectResult r;
        ble::LinkError err = connect_error;
        if (!connect_script.empty()) {
            err = connect_script.front();
            connect_script.erase(connect_script.begin());
        }
        const bool refuse = std::find(unreachable_ids.begin(), unreachable_ids.end(), d.id) != unreachable_ids.end();
        if (err != ble::LinkError::None || refuse) {
            r.error = refuse ? ble::LinkError::Connect : err;
            r.detail = "fake refusal";
            return r;
        }
        auto link = std::make_unique<FakeLink>();
        link->log = log;
        link->mtu_ = mtu;
        link->responder = responder;
        link->fail = write_fail;
        if (h.ready) h.ready();
        link->handlers = std::move(h);
        if (after_connect) after_connect(link->handlers);
        r.link = std::move(link);
        return r;
    }
    bool forget(const ble::Device& d) override {
        forgotten.push_back(d.id);
        events.push_back("forget:" + d.id);
        return true;
    }
    ble::AdapterReset resetAdapter() override {
        resets++;
        events.push_back("reset");
        return reset_result;
    }

    std::vector<Write> writes() {
        std::lock_guard<std::mutex> lk(log->m);
        return log->writes;
    }
    int closes() {
        std::lock_guard<std::mutex> lk(log->m);
        return log->closes;
    }
};

ble::Device dev(const std::string& id, const std::string& name, std::vector<std::string> uuids = {},
                bool advertising = true, const std::string& address = "") {
    ble::Device d;
    d.id = id;
    d.name = name;
    d.address = address.empty() ? id : address;
    d.uuids = std::move(uuids);
    d.advertising = advertising;
    if (advertising) d.rssi = -50;
    return d;
}

std::string tempDir() {
    static int n = 0;
    auto p = std::filesystem::temp_directory_path() /
             ("hms_ble_fake_" + std::to_string(::time(nullptr)) + "_" + std::to_string(n++));
    std::filesystem::create_directories(p);
    return p.string();
}

as11::Timeouts quick() {
    as11::Timeouts t;
    t.call_ms = 300;
    t.frag_idle_ms = 300;
    return t;
}

// The original O2Ring's frame, as the ring sends it.
uint8_t crc8(const uint8_t* data, size_t len) {
    uint8_t crc = 0;
    for (size_t i = 0; i < len; i++) {
        uint8_t chk = crc ^ data[i];
        crc = 0;
        if (chk & 0x01) crc = 0x07;
        if (chk & 0x02) crc ^= 0x0e;
        if (chk & 0x04) crc ^= 0x1c;
        if (chk & 0x08) crc ^= 0x38;
        if (chk & 0x10) crc ^= 0x70;
        if (chk & 0x20) crc ^= 0xe0;
        if (chk & 0x40) crc ^= 0xc7;
        if (chk & 0x80) crc ^= 0x89;
    }
    return crc;
}

std::vector<uint8_t> ringReply(const std::string& payload) {
    std::vector<uint8_t> f{0x55, 0x00, 0xFF, 0x00, 0x00, static_cast<uint8_t>(payload.size() & 0xFF),
                           static_cast<uint8_t>(payload.size() >> 8)};
    f.insert(f.end(), payload.begin(), payload.end());
    f.push_back(crc8(f.data(), f.size()));
    return f;
}

} // namespace

// ── UUIDs as every backend hands them up ──

TEST(BleUuid, ShortAndLongFormsMeetInOneForm) {
    const std::string full = "0000fd56-0000-1000-8000-00805f9b34fb";
    EXPECT_EQ(ble::normalizeUuid("FD56"), full);            // CoreBluetooth's UUIDString
    EXPECT_EQ(ble::normalizeUuid("0xfd56"), full);
    EXPECT_EQ(ble::normalizeUuid("{0000FD56-0000-1000-8000-00805F9B34FB}"), full);   // a Windows GUID
    EXPECT_EQ(ble::normalizeUuid(full), full);              // BlueZ, unchanged
    EXPECT_EQ(ble::normalizeUuid("0000FD56"), full);        // 32-bit form
    EXPECT_EQ(ble::normalizeUuid("E8FB0001-A14B-98F9-831B-4E2941D01248"), oxyii::kServiceUuid);
}

TEST(BleUuid, AShortServiceUuidStillIdentifiesTheMachine) {
    EXPECT_TRUE(as11::advertIsAirSense("", {ble::normalizeUuid("FD56")}));
    EXPECT_FALSE(as11::advertIsAirSense("", {ble::normalizeUuid("180D")}));
}

// ── switching a radio that is off on (Windows; BlueZ has its own) ──

TEST(BleRadioSwitch, ARadioThatIsOnOrAbsentIsLeftAlone) {
    int asked = 0;
    ble::RadioSwitchOps ops;
    ops.request_access = [&] { asked++; return std::string(); };
    for (auto st : {ble::Radio::Ready, ble::Radio::NoAdapter, ble::Radio::Failed}) {
        ops.state = [st] { return st; };
        EXPECT_EQ(ble::switchOn(ops, "advice").state, st);
    }
    EXPECT_EQ(asked, 0);
}

TEST(BleRadioSwitch, OffIsSwitchedOnWhenTheSystemAllows) {
    ble::Radio now = ble::Radio::Off;
    ble::RadioSwitchOps ops;
    ops.state = [&] { return now; };
    ops.request_access = [] { return std::string(); };
    ops.switch_on = [&] { now = ble::Radio::Ready; return std::string(); };
    const auto r = ble::switchOn(ops, "switch it on");
    EXPECT_EQ(r.state, ble::Radio::Ready);
    EXPECT_TRUE(r.switched_on);
    EXPECT_TRUE(r.detail.empty());
}

TEST(BleRadioSwitch, ARefusalKeepsTheAdviceAndSaysWhy) {
    bool switched = false;
    ble::RadioSwitchOps ops;
    ops.state = [] { return ble::Radio::Off; };
    ops.request_access = [] { return std::string("access status 2"); };
    ops.switch_on = [&] { switched = true; return std::string(); };
    const auto r = ble::switchOn(ops, "switch it on");
    EXPECT_EQ(r.state, ble::Radio::Off);
    EXPECT_FALSE(switched);
    EXPECT_NE(r.detail.find("switch it on"), std::string::npos);
    EXPECT_NE(r.detail.find("access status 2"), std::string::npos);
}

TEST(BleRadioSwitch, ARadioThatStaysOffIsReportedOff) {
    ble::RadioSwitchOps ops;
    ops.state = [] { return ble::Radio::Off; };
    ops.request_access = [] { return std::string(); };
    ops.switch_on = [] { return std::string(); };
    const auto r = ble::switchOn(ops, "switch it on");
    EXPECT_EQ(r.state, ble::Radio::Off);
    EXPECT_FALSE(r.switched_on);
    EXPECT_NE(r.detail.find("still off"), std::string::npos);
}

TEST(BleRadioSwitch, WithoutTheMeansItOnlyAdvises) {
    ble::RadioSwitchOps ops;
    ops.state = [] { return ble::Radio::Off; };
    const auto r = ble::switchOn(ops, "switch it on");
    EXPECT_EQ(r.state, ble::Radio::Off);
    EXPECT_NE(r.detail.find("switch it on"), std::string::npos);
}

// ── the AirSense 11 ──

TEST(As11BleLink, TheRadioOffIsRadioOffWhateverTheOsCallsIt) {
    for (auto state : {ble::Radio::RfKilled, ble::Radio::Off, ble::Radio::Unauthorized}) {
        auto t = std::make_shared<FakeTransport>();
        t->radio_ = {state, "switch Bluetooth on", ""};
        AirSense11BleClient c(tempDir(), "", t, quick());
        EXPECT_EQ(c.pairStart(), as11::Result::RadioOff);
        EXPECT_EQ(c.lastError(), "switch Bluetooth on");
        EXPECT_TRUE(t->connected_ids.empty());
    }
}

TEST(As11BleLink, ARadioThatFailsOtherwiseIsAConnectFailure) {
    auto t = std::make_shared<FakeTransport>();
    t->radio_ = {ble::Radio::Failed, "the adapter would not power on", ""};
    AirSense11BleClient c(tempDir(), "", t, quick());
    EXPECT_EQ(c.pairStart(), as11::Result::Connect);
}

TEST(As11BleLink, NoAdapterNeitherScansNorConnects) {
    auto t = std::make_shared<FakeTransport>();
    t->available_ = false;
    AirSense11BleClient c(tempDir(), "", t, quick());
    EXPECT_EQ(c.pairStart(), as11::Result::Connect);
    EXPECT_EQ(c.lastError(), "no Bluetooth adapter");
    EXPECT_EQ(t->radio_calls, 0);
    EXPECT_EQ(t->discover_calls, 0);
}

TEST(As11BleLink, AMachineTheOsKnowsIsUsedWithoutAScan) {
    auto t = std::make_shared<FakeTransport>();
    t->known_ = {dev("phone", "Somebody's phone", {}, false), dev("m1", "ResMed 123456", {}, false)};
    t->write_fail.error = "refused";
    AirSense11BleClient c(tempDir(), "", t, quick());
    c.pairStart();
    ASSERT_EQ(t->connected_ids.size(), 1u);
    EXPECT_EQ(t->connected_ids[0], "m1");
    EXPECT_EQ(t->discover_calls, 0);
    EXPECT_EQ(t->specs[0].write_uuid, as11::kWriteUuid);
    EXPECT_EQ(t->specs[0].notify_uuid, as11::kNotifyUuid);
}

TEST(As11BleLink, DiscoveryTakesOnlyAMachineAdvertisingNow) {
    auto t = std::make_shared<FakeTransport>();
    t->adverts_ = {dev("stale", "ResMed 111111", {}, false), dev("live", "ResMed 222222", {}, true)};
    t->write_fail.error = "refused";
    AirSense11BleClient c(tempDir(), "", t, quick());
    c.pairStart();
    ASSERT_EQ(t->connected_ids.size(), 1u);
    EXPECT_EQ(t->connected_ids[0], "live");
}

TEST(As11BleLink, TheAddressPicksTheMachine) {
    auto t = std::make_shared<FakeTransport>();
    t->adverts_ = {dev("a", "ResMed 111111", {}, true, "aa:aa:aa:aa:aa:aa"),
                   dev("b", "ResMed 222222", {}, true, "bb:bb:bb:bb:bb:bb")};
    t->write_fail.error = "refused";
    AirSense11BleClient c(tempDir(), "BB:BB:BB:BB:BB:BB", t, quick());
    c.pairStart();
    ASSERT_EQ(t->connected_ids.size(), 1u);
    EXPECT_EQ(t->connected_ids[0], "b");
}

TEST(As11BleLink, NothingAdvertisingSaysSo) {
    auto t = std::make_shared<FakeTransport>();
    t->adverts_ = {dev("x", "Somebody's watch")};
    AirSense11BleClient c(tempDir(), "", t, quick());
    EXPECT_EQ(c.pairStart(), as11::Result::Connect);
    EXPECT_NE(c.lastError().find("not advertising"), std::string::npos);
    EXPECT_TRUE(t->connected_ids.empty());
}

TEST(As11BleLink, AScanThatCannotStartSaysWhy) {
    auto t = std::make_shared<FakeTransport>();
    t->discover_starts = false;
    AirSense11BleClient c(tempDir(), "", t, quick());
    EXPECT_EQ(c.pairStart(), as11::Result::Connect);
    EXPECT_EQ(c.lastError(), "scan start failed: adapter busy");
}

TEST(As11BleLink, EachLinkFailureNamesItsStep) {
    const std::pair<ble::LinkError, std::string> cases[] = {
        {ble::LinkError::Connect, "connect failed"},
        {ble::LinkError::Resolve, "services not resolved in time"},
        {ble::LinkError::NoCharacteristics, "the machine's characteristics were not found (fake refusal)"},
        {ble::LinkError::Notify, "notifications could not be enabled: fake refusal"},
    };
    for (const auto& [err, text] : cases) {
        auto t = std::make_shared<FakeTransport>();
        t->adverts_ = {dev("m", "ResMed 123456")};
        t->connect_error = err;
        AirSense11BleClient c(tempDir(), "", t, quick());
        EXPECT_EQ(c.pairStart(), as11::Result::Connect);
        EXPECT_EQ(c.lastError(), text);
    }
}

TEST(As11BleLink, FramesGoWithResponseSplitAtTheMtu) {
    for (const auto& [mtu, chunk] : std::vector<std::pair<uint16_t, size_t>>{{0, 20}, {23, 20}, {185, 182}}) {
        auto t = std::make_shared<FakeTransport>();
        t->adverts_ = {dev("m", "ResMed 123456")};
        t->mtu = mtu;
        AirSense11BleClient c(tempDir(), "", t, quick());
        // No answer comes: the exchange times out after the one frame.
        EXPECT_NE(c.pairStart(), as11::Result::WaitCode);
        const auto w = t->writes();
        ASSERT_GE(w.size(), 2u) << "StartKeyExchange is longer than one piece";
        size_t total = 0;
        for (size_t i = 0; i < w.size(); i++) {
            EXPECT_TRUE(w[i].with_response);
            EXPECT_LE(w[i].bytes.size(), chunk);
            if (i + 1 < w.size()) EXPECT_EQ(w[i].bytes.size(), chunk);
            total += w[i].bytes.size();
        }
        EXPECT_GT(total, chunk);
        EXPECT_GE(t->closes(), 1);   // the link is down after a failed start
    }
}

TEST(As11BleLink, AWriteThatTimesOutAbandonsThePull) {
    auto t = std::make_shared<FakeTransport>();
    t->adverts_ = {dev("m", "ResMed 123456")};
    t->write_fail.timed_out = true;
    AirSense11BleClient c(tempDir(), "", t, quick());
    const auto start = std::chrono::steady_clock::now();
    EXPECT_NE(c.pairStart(), as11::Result::WaitCode);
    EXPECT_LT(std::chrono::steady_clock::now() - start, 250ms) << "no wait for a reply after a dead write";
    EXPECT_NE(c.lastError().find("pairing could not start"), std::string::npos);
    EXPECT_EQ(t->writes().size(), 1u);
}

TEST(As11BleLink, TheLinkDroppingWakesTheConversationAtOnce) {
    auto t = std::make_shared<FakeTransport>();
    t->adverts_ = {dev("m", "ResMed 123456")};
    // The machine lets go the moment the request is written.
    t->responder = [](FakeLink& l, const std::vector<uint8_t>&) {
        if (l.handlers.disconnected) l.handlers.disconnected();
    };
    as11::Timeouts slow;
    slow.call_ms = 5000;
    AirSense11BleClient c(tempDir(), "", t, slow);
    const auto start = std::chrono::steady_clock::now();
    EXPECT_NE(c.pairStart(), as11::Result::WaitCode);
    EXPECT_LT(std::chrono::steady_clock::now() - start, 2000ms);
}

TEST(As11BleLink, NoPairingNoRadio) {
    auto t = std::make_shared<FakeTransport>();
    AirSense11BleClient c(tempDir(), "", t, quick());
    const int before = t->radio_calls;
    EXPECT_TRUE(c.fetchSummary("2026-11-01").empty());
    EXPECT_EQ(c.lastError(), "not paired with the machine yet");
    EXPECT_EQ(t->radio_calls, before);
    EXPECT_FALSE(c.paired());
}

TEST(As11BleLink, NoNightLoopWithoutAnAdapter) {
    auto t = std::make_shared<FakeTransport>();
    t->available_ = false;
    AirSense11BleClient c(tempDir(), "", t, quick());
    auto writer = std::make_shared<As11NightWriter>(tempDir(), hostLocalClock());
    c.startNightLoop(writer, [] {});
    c.stopNightLoop();   // returns at once: nothing was started
    EXPECT_EQ(t->discover_calls, 0);
}

// ── the ring ──

TEST(O2RingBleMatch, ProtocolByNameOrService) {
    using P = O2RingBleClient::Proto;
    bool t8520 = false;
    EXPECT_EQ(O2RingBleClient::classifyDevice("O2Ring 1234", {}), P::Legacy);
    EXPECT_EQ(O2RingBleClient::classifyDevice("Checkme O2", {}), P::Legacy);
    EXPECT_EQ(O2RingBleClient::classifyDevice("", {O2RingBleClient::SVC_UUID}), P::Legacy);
    EXPECT_EQ(O2RingBleClient::classifyDevice("S8-AW 0042", {}, &t8520), P::OxyII);
    EXPECT_FALSE(t8520);
    EXPECT_EQ(O2RingBleClient::classifyDevice("T8520_0042", {}, &t8520), P::OxyII);
    EXPECT_TRUE(t8520);
    // The service as CoreBluetooth spells it.
    EXPECT_EQ(O2RingBleClient::classifyDevice("", {"E8FB0001-A14B-98F9-831B-4E2941D01248"}, &t8520), P::OxyII);
    EXPECT_FALSE(t8520);
    EXPECT_FALSE(O2RingBleClient::classifyDevice("Fitness band", {"0000180d-0000-1000-8000-00805f9b34fb"}));
}

TEST(O2RingBleLink, TheOriginalRingListsItsFilesOverTheTransport) {
    auto t = std::make_shared<FakeTransport>();
    t->adverts_ = {dev("r", "O2Ring 1234")};
    t->mtu = 247;
    t->responder = [](FakeLink& l, const std::vector<uint8_t>& piece) {
        if (piece.size() >= 2 && piece[0] == 0xAA && piece[1] == 0x14)
            l.notify(ringReply(R"({"CurBAT":"87","FileList":"20261101010101,20261102010101"})"));
    };
    O2RingBleClient c(t);
    const auto files = c.listFiles();
    ASSERT_EQ(files.size(), 2u);
    EXPECT_EQ(files[0], "20261101010101");
    EXPECT_EQ(c.getBattery(), 87);
    EXPECT_EQ(t->specs[0].write_uuid, O2RingBleClient::WRITE_UUID);
    for (const auto& w : t->writes()) {
        EXPECT_FALSE(w.with_response);
        EXPECT_LE(w.bytes.size(), 20u) << "the original ring is written 20 bytes at a time";
    }
    EXPECT_EQ(t->closes(), 1);
    EXPECT_FALSE(c.isConnected());
}

TEST(O2RingBleLink, ANotificationSplitAcrossPiecesIsReassembled) {
    auto t = std::make_shared<FakeTransport>();
    t->adverts_ = {dev("r", "O2Ring 1234")};
    t->responder = [](FakeLink& l, const std::vector<uint8_t>& piece) {
        if (piece.size() < 2 || piece[1] != 0x14) return;
        const auto f = ringReply(R"({"CurBAT":"50","FileList":"20261101010101"})");
        for (size_t at = 0; at < f.size(); at += 20)
            l.notify(std::vector<uint8_t>(f.begin() + at, f.begin() + std::min(f.size(), at + 20)));
    };
    O2RingBleClient c(t);
    EXPECT_EQ(c.listFiles().size(), 1u);
    EXPECT_EQ(c.getBattery(), 50);
}

TEST(O2RingBleLink, RadioOffMeansNoConnect) {
    auto t = std::make_shared<FakeTransport>();
    t->radio_ = {ble::Radio::Off, "switch Bluetooth on", ""};
    t->adverts_ = {dev("r", "O2Ring 1234")};
    O2RingBleClient c(t);
    EXPECT_TRUE(c.listFiles().empty());
    EXPECT_TRUE(t->connected_ids.empty());
}

TEST(O2RingBleLink, AWornIdentityWithoutTheServiceIsPassedOver) {
    auto t = std::make_shared<FakeTransport>();
    t->adverts_ = {dev("worn", "T8520_0042")};
    t->connect_error = ble::LinkError::NoCharacteristics;
    O2RingBleClient c(t);
    EXPECT_TRUE(c.listFiles().empty());
    ASSERT_EQ(t->connected_ids.size(), 1u);
    EXPECT_EQ(t->specs[0].write_uuid, oxyii::kWriteUuid);
    // Next time the same identity is skipped; nothing else is around.
    EXPECT_TRUE(c.listFiles().empty());
    EXPECT_EQ(t->connected_ids.size(), 1u);
}

TEST(O2RingBleLink, ALivePollAndAFilePullNeverShareTheRing) {
    // The burst loop's live poll and a pull asked from the web page come from
    // two threads. Overlapped, the second connect tore down the first.
    auto t = std::make_shared<FakeTransport>();
    t->adverts_ = {dev("r", "O2Ring 1234")};
    // Links open at each connect: connects so far minus closes so far.
    std::atomic<int> most{0};
    t->after_connect = [&](ble::LinkHandlers&) {
        const int now = static_cast<int>(t->connected_ids.size()) - t->closes();
        int m = most.load();
        while (now > m && !most.compare_exchange_weak(m, now)) {}
        std::this_thread::sleep_for(30ms);   // widen the window
    };
    t->responder = [&](FakeLink& l, const std::vector<uint8_t>& piece) {
        if (piece.size() < 2) return;
        if (piece[1] == 0x14) l.notify(ringReply(R"({"CurBAT":"60","FileList":"20261101010101"})"));
        if (piece[1] == 0x17) l.notify(ringReply(std::string("\x60\x48\x00\x00\x00\x00\x00\x50\x00\x00\x00\x00", 12)));
    };
    O2RingBleClient c(t);
    std::vector<std::string> files;
    IO2RingClient::LiveReading live;
    std::thread a([&] { files = c.listFiles(); });
    std::thread b([&] { live = c.getLive(); });
    a.join();
    b.join();
    EXPECT_EQ(most.load(), 1) << "two links to the ring at once";
    EXPECT_EQ(files.size(), 1u);
    EXPECT_EQ(live.spo2, 0x60);
    EXPECT_EQ(live.hr, 0x48);
    EXPECT_EQ(t->connected_ids.size(), 2u);
}

TEST(O2RingBleLink, ARememberedRingThatWillNotConnectIsFoundAgainByItsAdvert) {
    // The ring as the OS remembers it (an old address) is unreachable; the
    // same ring advertising now, under another address, is not.
    auto t = std::make_shared<FakeTransport>();
    t->known_ = {dev("old", "O2Ring 1276", {}, false)};
    t->adverts_ = {dev("now", "O2Ring 1276", {}, true)};
    t->unreachable_ids = {"old"};
    t->responder = [](FakeLink& l, const std::vector<uint8_t>& piece) {
        if (piece.size() >= 2 && piece[1] == 0x14)
            l.notify(ringReply(R"({"CurBAT":"70","FileList":"20261105003602"})"));
    };
    O2RingBleClient c(t);
    EXPECT_EQ(c.listFiles().size(), 1u);
    ASSERT_EQ(t->connected_ids.size(), 2u);
    EXPECT_EQ(t->connected_ids[0], "old");
    EXPECT_EQ(t->connected_ids[1], "now");
}

TEST(O2RingBleLink, ARingFoundAdvertisingIsNotRetriedForever) {
    auto t = std::make_shared<FakeTransport>();
    t->adverts_ = {dev("now", "O2Ring 1276", {}, true)};
    t->unreachable_ids = {"now"};
    O2RingBleClient c(t);
    EXPECT_TRUE(c.listFiles().empty());
    EXPECT_EQ(t->connected_ids.size(), 1u);
}

TEST(As11BleLink, ARememberedMachineThatWillNotConnectIsFoundAgainByItsAdvert) {
    auto t = std::make_shared<FakeTransport>();
    t->known_ = {dev("old", "ResMed 123456", {}, false)};
    t->adverts_ = {dev("now", "ResMed 123456", {}, true)};
    t->unreachable_ids = {"old"};
    t->write_fail.error = "refused";
    AirSense11BleClient c(tempDir(), "", t, quick());
    c.pairStart();
    ASSERT_EQ(t->connected_ids.size(), 2u);
    EXPECT_EQ(t->connected_ids[0], "old");
    EXPECT_EQ(t->connected_ids[1], "now");
}

TEST(O2RingBleLink, AWriteFailureFailsTheRequestInsteadOfThrowing) {
    auto t = std::make_shared<FakeTransport>();
    t->adverts_ = {dev("r", "O2Ring 1234")};
    t->write_fail.error = "gone";
    O2RingBleClient c(t);
    EXPECT_NO_THROW({ EXPECT_TRUE(c.listFiles().empty()); });
    EXPECT_EQ(t->writes().size(), 1u);
}

// ── #67: a machine in sight that will not link ──
//
// The reporter's case: BlueZ holds an object for the machine whose RSSI
// another scanner (Home Assistant) keeps fresh, so it always reads as
// advertising, and every connect to it ends "services not resolved in time".

namespace {

using Step = ble::LinkError;

/// The machine as the reporter's BlueZ held it: known, and "advertising".
std::shared_ptr<FakeTransport> stuckMachine(std::vector<ble::LinkError> script, Step after = Step::None) {
    auto t = std::make_shared<FakeTransport>();
    t->known_ = {dev("m", "ResMed 535441", {}, true)};
    t->adverts_ = {dev("m", "ResMed 535441", {}, true)};
    t->connect_script = std::move(script);
    t->connect_error = after;
    // A link that comes up is let go at once: only the link matters here.
    t->write_fail.error = "refused";
    return t;
}

long indexOf(const std::vector<std::string>& v, const std::string& what, size_t from = 0) {
    for (size_t i = from; i < v.size(); i++)
        if (v[i] == what) return static_cast<long>(i);
    return -1;
}

} // namespace

TEST(As11Recovery, ALoneFailureEscalatesNothing) {
    auto t = stuckMachine({Step::Resolve});
    AirSense11BleClient c(tempDir(), "", t, quick());
    EXPECT_EQ(c.pairStart(), as11::Result::Connect);
    EXPECT_EQ(c.lastError(), "services not resolved in time");
    EXPECT_TRUE(t->forgotten.empty());
    EXPECT_EQ(t->resets, 0);
    EXPECT_EQ(t->connected_ids.size(), 1u);
    EXPECT_FALSE(c.presence().bluetooth_stuck);
}

TEST(As11Recovery, TwoFailuresInARowForgetTheObjectAndRediscoverByAdvert) {
    auto t = stuckMachine({Step::Resolve, Step::Resolve, Step::None});
    AirSense11BleClient c(tempDir(), "", t, quick());
    c.pairStart();
    EXPECT_TRUE(t->forgotten.empty()) << "one failure is not enough";
    const int discovers = t->discover_calls;
    c.pairStart();
    const std::vector<std::string> want{"connect:m", "connect:m", "forget:m", "connect:m"};
    EXPECT_EQ(t->events, want);
    EXPECT_GT(t->discover_calls, discovers) << "found again by a fresh advert, not the OS's cache";
    EXPECT_EQ(t->resets, 0);
    EXPECT_FALSE(c.presence().bluetooth_stuck);
}

TEST(As11Recovery, EveryCountedFailureKindEscalates) {
    for (auto err : {Step::Connect, Step::Resolve, Step::NoCharacteristics}) {
        auto t = stuckMachine({err, err, Step::None});
        AirSense11BleClient c(tempDir(), "", t, quick());
        c.pairStart();
        c.pairStart();
        EXPECT_EQ(t->forgotten.size(), 1u);
    }
}

TEST(As11Recovery, ANotifyFailureIsNotAStaleObject) {
    auto t = stuckMachine({Step::Notify, Step::Notify, Step::Notify});
    AirSense11BleClient c(tempDir(), "", t, quick());
    for (int i = 0; i < 3; i++) c.pairStart();
    EXPECT_TRUE(t->forgotten.empty());
    EXPECT_EQ(t->resets, 0);
}

TEST(As11Recovery, TheAdapterIsResetOnlyAfterTheForgetAndOneMoreFailure) {
    auto t = stuckMachine({Step::Resolve, Step::Resolve, Step::Resolve, Step::None});
    AirSense11BleClient c(tempDir(), "", t, quick());
    c.pairStart();
    c.pairStart();
    EXPECT_EQ(t->resets, 1);
    const long forget = indexOf(t->events, "forget:m");
    const long reset = indexOf(t->events, "reset");
    ASSERT_GE(forget, 0);
    ASSERT_GT(reset, forget);
    EXPECT_GT(indexOf(t->events, "connect:m", forget), forget) << "a fresh attempt between the forget and the reset";
    EXPECT_LT(indexOf(t->events, "connect:m", forget), reset);
    EXPECT_EQ(t->events.back(), "connect:m") << "a fresh attempt after the reset";
    EXPECT_FALSE(c.presence().bluetooth_stuck) << "the attempt after the reset linked";
}

TEST(As11Recovery, ASuccessResetsTheCount) {
    auto t = stuckMachine({Step::Resolve, Step::None, Step::Resolve, Step::None, Step::Resolve, Step::None});
    AirSense11BleClient c(tempDir(), "", t, quick());
    for (int i = 0; i < 6; i++) c.pairStart();
    EXPECT_TRUE(t->forgotten.empty()) << "never two failures in a row";
    EXPECT_EQ(t->resets, 0);
}

TEST(As11Recovery, NoEscalationWithTheRadioOff) {
    for (auto state : {ble::Radio::Off, ble::Radio::RfKilled, ble::Radio::Unauthorized, ble::Radio::Failed}) {
        auto t = stuckMachine({}, Step::Resolve);
        t->radio_ = {state, "switch Bluetooth on", ""};
        AirSense11BleClient c(tempDir(), "", t, quick());
        for (int i = 0; i < 5; i++) c.pairStart();
        EXPECT_TRUE(t->connected_ids.empty());
        EXPECT_TRUE(t->forgotten.empty());
        EXPECT_EQ(t->resets, 0);
        EXPECT_FALSE(c.presence().bluetooth_stuck);
    }
}

TEST(As11Recovery, NoEscalationWithoutAnAdapter) {
    auto t = stuckMachine({}, Step::Resolve);
    t->available_ = false;
    AirSense11BleClient c(tempDir(), "", t, quick());
    for (int i = 0; i < 5; i++) c.pairStart();
    EXPECT_TRUE(t->connected_ids.empty());
    EXPECT_TRUE(t->forgotten.empty());
    EXPECT_EQ(t->resets, 0);
    EXPECT_FALSE(c.presence().bluetooth_stuck);
}

TEST(As11Recovery, AMachineOutOfSightIsNotAFailure) {
    auto t = std::make_shared<FakeTransport>();
    t->connect_error = Step::Resolve;   // never reached: nothing is in sight
    AirSense11BleClient c(tempDir(), "", t, quick());
    for (int i = 0; i < 5; i++) c.pairStart();
    EXPECT_TRUE(t->connected_ids.empty());
    EXPECT_TRUE(t->forgotten.empty());
    EXPECT_EQ(t->resets, 0);
}

TEST(As11Recovery, OneAdapterResetPerStreakThenNotRecovering) {
    auto t = stuckMachine({}, Step::Resolve);
    AirSense11BleClient c(tempDir(), "", t, quick());
    for (int i = 0; i < 8; i++) c.pairStart();
    EXPECT_EQ(t->resets, 1);
    EXPECT_TRUE(c.presence().bluetooth_stuck);
    EXPECT_NE(c.lastError().find("Bluetooth adapter not recovering"), std::string::npos);
    // The status the page reads.
    const auto s = as11api::status(&c, true, true);
    EXPECT_TRUE(s.body["bluetooth_stuck"].asBool());
    EXPECT_FALSE(s.body["connected"].asBool());
}

TEST(As11Recovery, ALinkAfterNotRecoveringClearsItAndANewStreakStartsFromTheBottom) {
    auto t = stuckMachine({}, Step::Resolve);
    AirSense11BleClient c(tempDir(), "", t, quick());
    for (int i = 0; i < 3; i++) c.pairStart();
    ASSERT_TRUE(c.presence().bluetooth_stuck);
    t->connect_error = Step::None;
    c.pairStart();
    EXPECT_FALSE(c.presence().bluetooth_stuck);
    // A new streak: one failure does nothing, the reset comes again only
    // after a new forget.
    t->connect_error = Step::Resolve;
    const size_t forgets = t->forgotten.size();
    c.pairStart();
    EXPECT_EQ(t->forgotten.size(), forgets);
    EXPECT_EQ(t->resets, 1);
    c.pairStart();
    EXPECT_GT(t->forgotten.size(), forgets);
    EXPECT_EQ(t->resets, 2);
}

TEST(As11Recovery, NoAdapterResetWhileATransferHoldsTheRadio) {
    auto t = stuckMachine({}, Step::Resolve);
    AirSense11BleClient c(tempDir(), "", t, quick());
    {
        ble::RadioInUse ring("an O2 Ring transfer");
        for (int i = 0; i < 4; i++) c.pairStart();
        EXPECT_EQ(t->resets, 0);
        EXPECT_FALSE(t->forgotten.empty()) << "the forget does not cut anyone's transfer";
        EXPECT_FALSE(c.presence().bluetooth_stuck) << "the reset is still owed";
    }
    c.pairStart();
    EXPECT_EQ(t->resets, 1) << "asked again on the next failure once the radio is free";
}

TEST(As11Recovery, TheResetNamesOtherDevicesLinkedOnTheAdapter) {
    auto t = stuckMachine({}, Step::Resolve);
    auto ring = dev("ring", "O2Ring 1234", {}, true, "aa:bb:cc:dd:ee:ff");
    ring.connected = true;
    t->known_.push_back(ring);
    AirSense11BleClient c(tempDir(), "", t, quick());
    const auto r = c.resetBluetooth();
    ASSERT_EQ(r.others_connected.size(), 1u);
    EXPECT_EQ(r.others_connected[0], "O2Ring 1234 (aa:bb:cc:dd:ee:ff)");
}

TEST(As11Recovery, AFreshAdvertUnderAnotherNameIsNotTheMachine) {
    // Paired as "ResMed 535441": after the forget, a neighbour's AirSense
    // advertising is passed over.
    auto t = stuckMachine({Step::Resolve, Step::Resolve});
    t->adverts_ = {dev("n", "ResMed 999999", {}, true), dev("m", "ResMed 535441", {}, true)};
    t->connect_error = Step::None;
    AirSense11BleClient c(tempDir(), "", t, quick());
    c.pairStart();
    c.pairStart();
    EXPECT_EQ(t->connected_ids.back(), "m");
    EXPECT_EQ(std::count(t->connected_ids.begin(), t->connected_ids.end(), "n"), 0);
}

// ── "Reset Bluetooth" and Unpair ──

TEST(As11Recovery, ResetBluetoothForgetsAndResetsAndClearsNotRecovering) {
    auto t = stuckMachine({}, Step::Resolve);
    AirSense11BleClient c(tempDir(), "", t, quick());
    for (int i = 0; i < 3; i++) c.pairStart();
    ASSERT_TRUE(c.presence().bluetooth_stuck);
    const int resets = t->resets;
    const size_t forgets = t->forgotten.size();
    const auto r = c.resetBluetooth();
    EXPECT_EQ(r.state, IAirSense11Pairing::BluetoothReset::State::Done);
    EXPECT_TRUE(r.forgot);
    EXPECT_TRUE(r.adapter_reset);
    EXPECT_EQ(t->resets, resets + 1);
    EXPECT_GT(t->forgotten.size(), forgets);
    EXPECT_FALSE(c.presence().bluetooth_stuck);
    // The forget comes before the reset.
    EXPECT_EQ(t->events.back(), "reset");
    EXPECT_EQ(t->events[t->events.size() - 2], "forget:m");
}

TEST(As11Recovery, ResetBluetoothWaitsForATransfer) {
    auto t = stuckMachine({});
    AirSense11BleClient c(tempDir(), "", t, quick());
    ble::RadioInUse ring("an O2 Ring transfer");
    const auto r = c.resetBluetooth();
    EXPECT_EQ(r.state, IAirSense11Pairing::BluetoothReset::State::Busy);
    EXPECT_NE(r.detail.find("O2 Ring"), std::string::npos);
    EXPECT_EQ(t->resets, 0);
    EXPECT_EQ(as11api::resetBluetooth(&c, true, true).status, 409);
}

TEST(As11Recovery, ResetBluetoothSaysWhyItFailed) {
    auto t = stuckMachine({});
    t->reset_result = {false, "the adapter would not power off: org.bluez.Error.Failed"};
    AirSense11BleClient c(tempDir(), "", t, quick());
    const auto r = as11api::resetBluetooth(&c, true, true);
    EXPECT_EQ(r.status, 500);
    EXPECT_FALSE(r.body["ok"].asBool());
    EXPECT_NE(r.body["error"].asString().find("would not power off"), std::string::npos);
}

TEST(As11Recovery, ResetBluetoothWithoutAnAdapter) {
    auto t = stuckMachine({});
    t->available_ = false;
    AirSense11BleClient c(tempDir(), "", t, quick());
    EXPECT_EQ(c.resetBluetooth().state, IAirSense11Pairing::BluetoothReset::State::Failed);
    EXPECT_EQ(t->resets, 0);
}

TEST(As11Recovery, UnpairForgetsTheOsObjectToo) {
    auto t = stuckMachine({});
    t->known_.push_back(dev("watch", "Somebody's watch", {}, false));
    const std::string dir = tempDir();
    AirSense11BleClient c(dir, "", t, quick());
    const auto r = as11api::forget(&c, as11::credentialsPath(dir), false);
    EXPECT_EQ(r.status, 200);
    EXPECT_EQ(t->forgotten, std::vector<std::string>{"m"}) << "the machine's object, and nothing else";
    // The next connect finds it by a fresh advert.
    const int discovers = t->discover_calls;
    c.pairStart();
    EXPECT_GT(t->discover_calls, discovers);
}

// ── the ladder itself, no Bluetooth ──

TEST(LinkRecoveryLadder, CountedFailures) {
    EXPECT_TRUE(ble::countsTowardRecovery(ble::LinkError::Connect));
    EXPECT_TRUE(ble::countsTowardRecovery(ble::LinkError::Resolve));
    EXPECT_TRUE(ble::countsTowardRecovery(ble::LinkError::NoCharacteristics));
    EXPECT_FALSE(ble::countsTowardRecovery(ble::LinkError::Notify));
    EXPECT_FALSE(ble::countsTowardRecovery(ble::LinkError::None));
}

TEST(LinkRecoveryLadder, ForgetThenResetThenNotRecoveringThenNothing) {
    using S = ble::RecoveryStep;
    ble::LinkRecovery r;
    EXPECT_EQ(r.onFailure(), S::None);
    EXPECT_EQ(r.onFailure(), S::Forget);
    EXPECT_TRUE(r.forgot());
    EXPECT_EQ(r.onFailure(), S::ResetAdapter);
    EXPECT_TRUE(r.adapterReset());
    EXPECT_EQ(r.onFailure(), S::NotRecovering);
    EXPECT_TRUE(r.notRecovering());
    for (int i = 0; i < 10; i++) EXPECT_EQ(r.onFailure(), S::None) << "one reset per streak";
    EXPECT_EQ(r.failures(), 14);
}

TEST(LinkRecoveryLadder, SuccessStartsOver) {
    using S = ble::RecoveryStep;
    ble::LinkRecovery r;
    r.onFailure();
    r.onSuccess();
    EXPECT_EQ(r.failures(), 0);
    EXPECT_EQ(r.onFailure(), S::None);
    r.onFailure();
    r.onFailure();
    r.onFailure();
    ASSERT_TRUE(r.notRecovering());
    r.onSuccess();
    EXPECT_FALSE(r.notRecovering());
    EXPECT_FALSE(r.forgot());
    EXPECT_FALSE(r.adapterReset());
    EXPECT_EQ(r.onFailure(), S::None);
    EXPECT_EQ(r.onFailure(), S::Forget);
}

TEST(LinkRecoveryLadder, ADeferredResetIsAskedForAgain) {
    using S = ble::RecoveryStep;
    ble::LinkRecovery r;
    r.onFailure();
    r.onFailure();
    EXPECT_EQ(r.onFailure(), S::ResetAdapter);
    r.resetDeferred();
    EXPECT_EQ(r.onFailure(), S::ResetAdapter);
    EXPECT_EQ(r.onFailure(), S::NotRecovering);
}

TEST(LinkRecoveryLadder, RestartIsANewStreak) {
    using S = ble::RecoveryStep;
    ble::LinkRecovery r;
    for (int i = 0; i < 4; i++) r.onFailure();
    r.restart();
    EXPECT_EQ(r.failures(), 0);
    EXPECT_FALSE(r.notRecovering());
    EXPECT_EQ(r.onFailure(), S::None);
}

TEST(LinkRecoveryLadder, RadioInUseIsHeldForItsScope) {
    const auto before = ble::radioInUse().size();
    {
        ble::RadioInUse a("an O2 Ring transfer");
        ble::RadioInUse b("an AirSense 11 night streaming");
        const auto now = ble::radioInUse();
        EXPECT_EQ(now.size(), before + 2);
        EXPECT_NE(std::find(now.begin(), now.end(), "an O2 Ring transfer"), now.end());
    }
    EXPECT_EQ(ble::radioInUse().size(), before);
}
