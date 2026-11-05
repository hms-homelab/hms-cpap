#include <gtest/gtest.h>

#include "clients/AirSense11BleClient.h"
#include "clients/As11Protocol.h"
#include "clients/O2RingBleClient.h"
#include "clients/OxyIIProtocol.h"
#include "clients/ble/BleTransport.h"

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
        ble::ConnectResult r;
        if (connect_error != ble::LinkError::None) {
            r.error = connect_error;
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
        return true;
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

TEST(O2RingBleLink, AWriteFailureFailsTheRequestInsteadOfThrowing) {
    auto t = std::make_shared<FakeTransport>();
    t->adverts_ = {dev("r", "O2Ring 1234")};
    t->write_fail.error = "gone";
    O2RingBleClient c(t);
    EXPECT_NO_THROW({ EXPECT_TRUE(c.listFiles().empty()); });
    EXPECT_EQ(t->writes().size(), 1u);
}
