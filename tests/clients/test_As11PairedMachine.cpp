#include <gtest/gtest.h>

#include "clients/AirSense11BleClient.h"
#include "clients/As11Protocol.h"
#include "clients/As11Session.h"
#include "clients/ble/BleTransport.h"
#include "utils/AppConfig.h"
#include "web/As11PairingApi.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <ctime>
#include <filesystem>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

// Issue #108: with airsense11_ble.address blank, a neighbour's AirSense 11 in
// range was connected to 3,097 times in 36 h, each refusing the pairing, and
// two nights were lost. Here, over a fake Bluetooth with several machines that
// speak the session's opening (the protocol's reference vectors, as in
// test_As11Session): only the machine that knows the pairing is used, it is
// saved, the others are passed over, and the saved one losing the pairing
// is said once and not asked again.

using namespace hms_cpap;
using nlohmann::json;

namespace {

std::vector<uint8_t> hexv(const std::string& s) {
    std::vector<uint8_t> out;
    for (size_t i = 0; i + 1 < s.size(); i += 2)
        out.push_back(static_cast<uint8_t>(std::stoul(s.substr(i, 2), nullptr, 16)));
    return out;
}

const char* kK = "B93068BFDF2B1D98577F696CC022FA049273FD4AC8F42DCC5A3D13E28FF14BC2";
const char* kChallenge = "1111111111111111111111111111111111111111111111111111111111111111";
const char* kHmac = "0BAD7C7B0B814A7EF25E2281E1EEC56058B84D251B6345ECD863A61D2BB322FF";
const char* kNonce = "2222222222222222222222222222222222222222222222222222222222222222";

as11::Key refKey() {
    as11::Key k{};
    const auto v = hexv(kK);
    std::copy(v.begin(), v.end(), k.begin());
    return k;
}

/// One AirSense 11 in range: its advert, and whether it knows the pairing.
struct Machine {
    std::string id, name, address, serial;
    bool knows_pairing = true;
    int connects = 0;
};

/// The machine's side of one link: frames in, the session's opening and the
/// state answered; everything else refused at once so a pull ends fast.
struct MachineLink : ble::ILink {
    Machine* m;
    ble::LinkHandlers handlers;
    as11::Reassembler in;
    std::optional<as11::Cipher> cipher;
    bool up = true;

    MachineLink(Machine* machine, ble::LinkHandlers h) : m(machine), handlers(std::move(h)) {}

    void send(const json& msg) {
        const std::string text = msg.dump();
        std::vector<uint8_t> frame;
        if (cipher) {
            std::array<uint8_t, 16> iv{};
            iv[0] = 0x42;
            frame = as11::buildFrame(as11::kDevEncrypted, cipher->encrypt(iv, text));
        } else {
            frame = as11::buildFrame(as11::kDevPlain, std::vector<uint8_t>(text.begin(), text.end()));
        }
        for (size_t i = 0; i < frame.size(); i += 20)
            handlers.notify(std::vector<uint8_t>(frame.begin() + i, frame.begin() + std::min(frame.size(), i + 20)));
    }
    static json reply(const json& req, const json& result) {
        return {{"id", req["id"]}, {"jsonrpc", req["jsonrpc"]}, {"result", result}};
    }
    static json error(const json& req, int code) {
        return {{"id", req["id"]}, {"jsonrpc", req["jsonrpc"]}, {"error", {{"code", code}, {"message", "refused"}}}};
    }
    void answer(const json& req) {
        const std::string method = req.value("method", "");
        if (method == "RequestSession") {
            if (!m->knows_pairing) return send(error(req, as11::Session::kVerificationFailure));
            return send(reply(req, {{"challenge", kChallenge}, {"nonce", kNonce}}));
        }
        if (method == "CheckSessionIntegrity") {
            const bool ok = req["params"]["response"] == kHmac;
            send(reply(req, {{"confirmation", ok}}));
            if (ok) cipher.emplace(refKey(), hexv(kNonce));
            return;
        }
        if (method == "Get") return send(reply(req, {{"SerialNumber", m->serial}, {"FGState", "Standby"}}));
        send(error(req, -1));
    }

    ble::WriteResult write(const std::vector<uint8_t>& piece, bool) override {
        in.feed(piece);
        while (auto f = in.next()) {
            std::string text;
            if (f->type & as11::kEncryptedBit) {
                if (!cipher) continue;
                auto t = cipher->decrypt(f->payload);
                if (!t) continue;
                text = *t;
            } else {
                text.assign(f->payload.begin(), f->payload.end());
            }
            answer(json::parse(text));
        }
        ble::WriteResult ok;
        ok.ok = true;
        return ok;
    }
    uint16_t mtu() const override { return 0; }
    bool connected() const override { return up; }
    void close() override { up = false; }
};

/// The host's Bluetooth with several machines advertising, in list order.
struct Room : ble::ITransport {
    std::vector<Machine> machines;
    std::vector<std::string> connected;   // ids, in order

    const char* backend() const override { return "fake"; }
    bool available() const override { return true; }
    ble::RadioStatus radio(bool) override { return {ble::Radio::Ready, "", "fake0"}; }
    std::vector<ble::Device> known() override { return {}; }
    ble::Device advert(const Machine& m) const {
        ble::Device d;
        d.id = m.id;
        d.name = m.name;
        d.address = m.address;
        d.advertising = true;
        d.rssi = -50;
        return d;
    }
    ble::DiscoverResult discover(const std::function<bool(const ble::Device&)>& match, int) override {
        ble::DiscoverResult r;
        // Every advert is offered, as a real scan hears them all; the first
        // wanted one is taken.
        for (const auto& m : machines) {
            const auto d = advert(m);
            if (match(d) && !r.device) r.device = d;
        }
        return r;
    }
    ble::ConnectResult connect(const ble::Device& d, const ble::GattSpec&, ble::LinkHandlers h,
                               const ble::ConnectOptions&) override {
        connected.push_back(d.id);
        ble::ConnectResult r;
        auto it = std::find_if(machines.begin(), machines.end(), [&](const Machine& m) { return m.id == d.id; });
        if (it == machines.end()) {
            r.error = ble::LinkError::Connect;
            return r;
        }
        it->connects++;
        if (h.ready) h.ready();
        r.link = std::make_unique<MachineLink>(&*it, std::move(h));
        return r;
    }
    bool forget(const ble::Device&) override { return true; }
    ble::AdapterReset resetAdapter() override { return {true, ""}; }

    int connectsTo(const std::string& id) const {
        return static_cast<int>(std::count(connected.begin(), connected.end(), id));
    }
};

std::string pairedDir() {
    static int n = 0;
    auto p = std::filesystem::temp_directory_path() /
             ("hms_as11_paired_" + std::to_string(::time(nullptr)) + "_" + std::to_string(n++));
    std::filesystem::create_directories(p);
    as11::Credentials c;
    c.clientId = "client-0001";
    c.masterKey = refKey();
    EXPECT_TRUE(as11::saveCredentials(as11::credentialsPath(p.string()), c));
    return p.string();
}

as11::Timeouts quick() {
    as11::Timeouts t;
    t.call_ms = 300;
    t.frag_idle_ms = 300;
    return t;
}

// The reporter's room: the neighbour's machine advertises first.
std::shared_ptr<Room> twoMachines(bool ours_knows = true) {
    auto room = std::make_shared<Room>();
    room->machines.push_back({"theirs", "ResMed 123418", "aa:aa:aa:aa:aa:18", "23231123418", false});
    room->machines.push_back({"ours", "ResMed 535441", "bb:bb:bb:bb:bb:41", "23222535441", ours_knows});
    return room;
}

struct Saved {
    std::vector<std::pair<std::string, std::string>> calls;
    AirSense11BleClient::MachineSaved hook() {
        return [this](const std::string& a, const std::string& s) { calls.emplace_back(a, s); };
    }
};

} // namespace

TEST(As11PairedMachine, BlankAddressTheMachineThatKnowsThePairingIsSavedAndTheOtherPassedOver) {
    auto room = twoMachines();
    AirSense11BleClient c(pairedDir(), "", room, quick());
    Saved saved;
    c.setMachineSavedHook(saved.hook());

    c.fetchSummary("2026-11-17");
    // The neighbour's refused the session; the scan went on to ours.
    ASSERT_EQ(room->connected, (std::vector<std::string>{"theirs", "ours"}));
    ASSERT_EQ(saved.calls.size(), 1u) << "the first session with the keys saves the machine, once";
    EXPECT_EQ(saved.calls[0].first, "bb:bb:bb:bb:bb:41");
    EXPECT_EQ(saved.calls[0].second, "23222535441");

    const auto m = c.machines();
    EXPECT_EQ(m.address, "bb:bb:bb:bb:bb:41");
    EXPECT_EQ(m.serial, "23222535441");
    EXPECT_FALSE(m.pairing_lost);
    ASSERT_EQ(m.others.size(), 1u);
    EXPECT_EQ(m.others[0].name, "ResMed 123418");
    EXPECT_TRUE(m.others[0].refused);

    // From now on only ours, and the saving is not said again.
    room->connected.clear();
    c.fetchSummary("2026-11-18");
    c.fetchSummary("2026-11-19");
    EXPECT_EQ(room->connectsTo("theirs"), 0);
    EXPECT_EQ(room->connectsTo("ours"), 2);
    EXPECT_EQ(saved.calls.size(), 1u);
    EXPECT_EQ(c.machines().others.size(), 1u) << "another machine is listed once, however often it is seen";
}

TEST(As11PairedMachine, AMachineThatRefusedThePairingIsBackedOffNotAskedEvery30s) {
    // Ours is away; only the neighbour's advertises.
    auto room = twoMachines();
    room->machines.pop_back();
    AirSense11BleClient c(pairedDir(), "", room, quick());
    for (int i = 0; i < 5; i++) EXPECT_TRUE(c.fetchSummary("2026-11-17").empty());
    EXPECT_EQ(room->connectsTo("theirs"), 1) << "the neighbour's machine is not asked again in this run";
    const auto m = c.machines();
    EXPECT_FALSE(m.pairing_lost) << "a machine never saved refusing is not our pairing lost";
    EXPECT_TRUE(m.address.empty());
    ASSERT_EQ(m.others.size(), 1u);
    EXPECT_TRUE(m.others[0].refused);

    // Ours comes back in range: found, used and saved.
    room->machines.push_back({"ours", "ResMed 535441", "bb:bb:bb:bb:bb:41", "23222535441", true});
    c.fetchSummary("2026-11-18");
    EXPECT_EQ(room->connectsTo("ours"), 1);
    EXPECT_EQ(room->connectsTo("theirs"), 1);
    EXPECT_EQ(c.machines().address, "bb:bb:bb:bb:bb:41");
}

TEST(As11PairedMachine, AnAddressSetByHandWinsAndIsNeverReplaced) {
    auto room = twoMachines();
    AirSense11BleClient c(pairedDir(), "BB:BB:BB:BB:BB:41", room, quick());
    Saved saved;
    c.setMachineSavedHook(saved.hook());
    c.fetchSummary("2026-11-17");
    c.fetchSummary("2026-11-18");
    EXPECT_EQ(room->connectsTo("theirs"), 0);
    EXPECT_EQ(room->connectsTo("ours"), 2);
    // The serial is learnt; the address handed over is the one set by hand.
    ASSERT_EQ(saved.calls.size(), 1u);
    EXPECT_EQ(saved.calls[0].first, "bb:bb:bb:bb:bb:41");
    EXPECT_EQ(saved.calls[0].second, "23222535441");
    const auto m = c.machines();
    ASSERT_EQ(m.others.size(), 1u) << "the other machine in range is shown, never connected to";
    EXPECT_FALSE(m.others[0].refused);

    // And in the config: an address set by hand is kept, the serial filled in.
    AppConfig cfg;
    cfg.airsense11_ble.address = "BB:BB:BB:BB:BB:41";
    EXPECT_TRUE(cfg.rememberAs11Machine("aa:aa:aa:aa:aa:18", "23222535441"));
    EXPECT_EQ(cfg.airsense11_ble.address, "BB:BB:BB:BB:BB:41");
    EXPECT_EQ(cfg.airsense11_ble.paired_serial, "23222535441");
    EXPECT_FALSE(cfg.rememberAs11Machine("bb:bb:bb:bb:bb:41", "23222535441")) << "nothing changed, nothing saved";
}

TEST(As11PairedMachine, AnAddressSetByHandToAMachineThatRefusesIsAPairingLost) {
    auto room = twoMachines(/*ours_knows=*/false);
    AirSense11BleClient c(pairedDir(), "bb:bb:bb:bb:bb:41", room, quick());
    c.fetchSummary("2026-11-17");
    EXPECT_TRUE(c.machines().pairing_lost);
    EXPECT_EQ(room->connectsTo("theirs"), 0);
}

TEST(As11PairedMachine, TheSavedMachineLosingThePairingIsSaidOnceAndNotHammered) {
    auto room = twoMachines();
    AirSense11BleClient c(pairedDir(), "", room, quick());
    c.fetchSummary("2026-11-17");
    ASSERT_EQ(c.machines().address, "bb:bb:bb:bb:bb:41");

    // The machine was paired with something else: it no longer knows ours.
    room->machines[1].knows_pairing = false;
    room->connected.clear();
    c.fetchSummary("2026-11-18");
    EXPECT_EQ(room->connectsTo("ours"), 1);
    const auto m = c.machines();
    EXPECT_TRUE(m.pairing_lost);
    EXPECT_EQ(m.address, "bb:bb:bb:bb:bb:41") << "still the saved machine: pairing it again goes to it";
    EXPECT_NE(c.lastError().find("no longer knows this pairing"), std::string::npos);
    EXPECT_NE(c.lastError().find("pair it again"), std::string::npos);

    // Not asked again, and never the neighbour's instead.
    for (int i = 0; i < 4; i++) c.fetchSummary("2026-11-18");
    EXPECT_EQ(room->connectsTo("ours"), 1);
    EXPECT_EQ(room->connectsTo("theirs"), 0);

    // Removing the pairing (to pair it again) clears the lost state but keeps
    // the saved machine.
    c.forgetMachine();
    EXPECT_FALSE(c.machines().pairing_lost);
    EXPECT_EQ(c.machines().address, "bb:bb:bb:bb:bb:41");
}

TEST(As11PairedMachine, ASavedSerialAloneRulesOutAdvertsOfAnotherSerial) {
    // A restart with only the serial saved (the address cleared by hand).
    auto room = twoMachines();
    AirSense11BleClient c(pairedDir(), "", room, quick());
    c.setPairedSerial("23222535441");
    c.fetchSummary("2026-11-17");
    EXPECT_EQ(room->connectsTo("theirs"), 0);
    EXPECT_EQ(room->connectsTo("ours"), 1);
    EXPECT_EQ(c.machines().address, "bb:bb:bb:bb:bb:41");
}

TEST(As11PairedMachine, PairADifferentMachineForgetsTheSavedOne) {
    auto room = twoMachines();
    AirSense11BleClient c(pairedDir(), "", room, quick());
    c.fetchSummary("2026-11-17");
    ASSERT_FALSE(c.machines().address.empty());
    c.forgetPairedMachine();
    const auto m = c.machines();
    EXPECT_TRUE(m.address.empty());
    EXPECT_TRUE(m.serial.empty());
    for (const auto& o : m.others) EXPECT_FALSE(o.refused) << "no machine has refused keys not made yet";

    AppConfig cfg;
    cfg.airsense11_ble.address = "bb:bb:bb:bb:bb:41";
    cfg.airsense11_ble.paired_serial = "23222535441";
    EXPECT_TRUE(cfg.forgetAs11Machine());
    EXPECT_TRUE(cfg.airsense11_ble.address.empty());
    EXPECT_TRUE(cfg.airsense11_ble.paired_serial.empty());
    EXPECT_FALSE(cfg.forgetAs11Machine());
}

TEST(As11PairedMachine, TheSavedMachineSurvivesARestartThroughTheConfig) {
    const auto dir = std::filesystem::temp_directory_path() /
                     ("hms_as11_cfg_" + std::to_string(::time(nullptr)));
    std::filesystem::create_directories(dir);
    const auto path = (dir / "config.json").string();

    AppConfig cfg;
    cfg.airsense11_ble.enabled = true;
    EXPECT_TRUE(cfg.rememberAs11Machine("bb:bb:bb:bb:bb:41", ""));   // after the pairing
    EXPECT_TRUE(cfg.rememberAs11Machine("bb:bb:bb:bb:bb:41", "23222535441"));   // its first session
    ASSERT_TRUE(cfg.save(path));

    AppConfig back;
    ASSERT_TRUE(AppConfig::load(path, back));
    EXPECT_EQ(back.airsense11_ble.address, "bb:bb:bb:bb:bb:41");
    EXPECT_EQ(back.airsense11_ble.paired_serial, "23222535441");

    // The client made from it connects to that machine only.
    auto room = twoMachines();
    AirSense11BleClient c(pairedDir(), back.airsense11_ble.address, room, quick());
    c.setPairedSerial(back.airsense11_ble.paired_serial);
    Saved saved;
    c.setMachineSavedHook(saved.hook());
    c.fetchSummary("2026-11-20");
    EXPECT_EQ(room->connected, std::vector<std::string>{"ours"});
    EXPECT_TRUE(saved.calls.empty()) << "nothing new to save";
    std::filesystem::remove_all(dir);
}

TEST(As11PairedMachine, TheStatusCarriesThePairedMachineAndTheOthersNearby) {
    auto room = twoMachines();
    AirSense11BleClient c(pairedDir(), "", room, quick());
    auto before = as11api::status(&c, true, true).body;
    EXPECT_TRUE(before["paired_machine"].isNull());
    EXPECT_FALSE(before["pairing_lost"].asBool());
    EXPECT_EQ(before["others_nearby"].size(), 0u);

    c.fetchSummary("2026-11-17");
    auto r = as11api::status(&c, true, true).body;
    ASSERT_TRUE(r["paired_machine"].isObject());
    EXPECT_EQ(r["paired_machine"]["address"].asString(), "bb:bb:bb:bb:bb:41");
    EXPECT_EQ(r["paired_machine"]["serial"].asString(), "23222535441");
    ASSERT_EQ(r["others_nearby"].size(), 1u);
    EXPECT_EQ(r["others_nearby"][0]["name"].asString(), "ResMed 123418");
    EXPECT_EQ(r["others_nearby"][0]["address"].asString(), "aa:aa:aa:aa:aa:18");
    EXPECT_TRUE(r["others_nearby"][0]["refused"].asBool());
    EXPECT_GT(r["others_nearby"][0]["last_seen"].asInt64(), 0);
}

TEST(As11PairedMachine, ForgetWithTheMachineToo) {
    auto room = twoMachines();
    const auto dir = pairedDir();
    AirSense11BleClient c(dir, "", room, quick());
    c.fetchSummary("2026-11-17");

    // Remove pairing: the keys go, the saved machine stays.
    auto r = as11api::forget(&c, as11::credentialsPath(dir), false);
    EXPECT_EQ(r.status, 200);
    EXPECT_FALSE(r.body["machine_forgotten"].asBool());
    EXPECT_FALSE(c.machines().address.empty());

    // Pair a different machine: both go.
    r = as11api::forget(&c, as11::credentialsPath(dir), false, true);
    EXPECT_EQ(r.status, 200);
    EXPECT_TRUE(r.body["machine_forgotten"].asBool());
    EXPECT_TRUE(c.machines().address.empty());
    EXPECT_FALSE(c.paired());
}
