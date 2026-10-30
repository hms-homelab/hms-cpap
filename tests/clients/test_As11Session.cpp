#include <gtest/gtest.h>

#include "clients/As11Session.h"

#include <nlohmann/json.hpp>

#include <atomic>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <functional>
#include <string>
#include <thread>

#ifndef _WIN32
#include <sys/stat.h>
#endif

using namespace hms_cpap::as11;
using nlohmann::json;

// SDD-057: the AirSense 11's conversation (pairing, session, Summary spool)
// with no Bluetooth: a fake machine reads the frames the session writes and
// answers with frames of its own, through the same byte callback a BlueZ
// notification feeds. The SRP values are the protocol's reference vectors
// (test_As11Protocol.cpp), so a pairing that passes here is byte-true.

namespace {

std::vector<uint8_t> hexv(const std::string& s) {
    std::vector<uint8_t> out;
    for (size_t i = 0; i + 1 < s.size(); i += 2)
        out.push_back(static_cast<uint8_t>(std::stoul(s.substr(i, 2), nullptr, 16)));
    return out;
}

Key keyOf(const std::string& h) {
    Key k{};
    const auto v = hexv(h);
    std::copy(v.begin(), v.end(), k.begin());
    return k;
}

const char* kA =
    "630ACDFF5D334462D92A29E0B7FA6E20020F3333292F6D3A640F1C7A76AD9D317531C57979952E5736C88DB118D060DC0539A812B9B0AF3B4002380A9F28AE4A7C45A896542DE05FBCF76A4E7E0739B9A55D5D6C7ABA4F1E1B58729A79BC084D5FF513EAEC33CE978F5BAD87E579B5A95FC773198E22697B2EADAB9EB94F84CDCF1FE94FF09F88D4CA46E968BBA443FF71167571F19FEB052869BD28D7DABF963B7FE399A1F70E7E08D00E1A3778ED1DDDC3325DD09E05D31E774D1FD295C4ABFBC613446232004D67CB03D6A034D2CE6CA0A544A0FF5B434B4B4267FA6C6D72ACBBDA2EFC1EF1D1FE36D35382B089ABE556862AEC35B29D3D0CDF359A9CFED3";
const char* kB =
    "4A543A4E438654FD009A367BE0D7399605883DCF77D66CA9089F5EE6AA02F4DD8DE04F4AD22717770E94FA03BFDDC76ECA756EAB79832DCCF55DFF0478897243DED17928EB18850BBC1A414B081D690E089D20564F524313BBF7A9BB43A8D5FA20ED1C8353662D35281F69C6DEE2858CDD2CCAC249611A541362CB76C46166A45CBDF2D5BD02820D53915E789FBC50067ABCD4BCFE9B7B18DE6260BB1884E65F35A246E24BB0CB336F0DA1D4CA936A54F8A02E2B882DEBC2F28B98B10FD1B7F26C162C3E2AE1076EAE115D2F563612B7DC8FC322D065E2028893DC3F4690E254DF4B721CAB1B878F51FE5BB8004EC655669C54CB2033BBCD2E02A85ED860D2D0";
const char* kSalt = "00112233445566778899aabbccddeeff00112233445566778899aabbccddeeff";
const char* kM1 = "4D83AB47D53E0B11E7E686CB8E35479E98B2830350CFDDC3A9A963D3318E1EB7";
const char* kM2 = "1EE6ADAE4718D5C203EBE7BE5CAD69FF1E2DAEC0E0D03A6B42EF606FF177F336";
const char* kK = "B93068BFDF2B1D98577F696CC022FA049273FD4AC8F42DCC5A3D13E28FF14BC2";
const char* kChallenge = "1111111111111111111111111111111111111111111111111111111111111111";
const char* kHmac = "0BAD7C7B0B814A7EF25E2281E1EEC56058B84D251B6345ECD863A61D2BB322FF";
const char* kNonce = "2222222222222222222222222222222222222222222222222222222222222222";
const char* kCode = "4829";

std::string b64(const std::vector<uint8_t>& in) {
    static const char* t = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string out;
    size_t i = 0;
    for (; i + 2 < in.size(); i += 3) {
        const uint32_t v = (in[i] << 16) | (in[i + 1] << 8) | in[i + 2];
        out += t[v >> 18]; out += t[(v >> 12) & 63]; out += t[(v >> 6) & 63]; out += t[v & 63];
    }
    if (i + 1 == in.size()) {
        const uint32_t v = in[i] << 16;
        out += t[v >> 18]; out += t[(v >> 12) & 63]; out += "==";
    } else if (i + 2 == in.size()) {
        const uint32_t v = (in[i] << 16) | (in[i + 1] << 8);
        out += t[v >> 18]; out += t[(v >> 12) & 63]; out += t[(v >> 6) & 63]; out += '=';
    }
    return out;
}

// Deterministic randomness: the SRP secret is 1..32, as the vectors were made.
void fixedRandom(uint8_t* out, size_t n) {
    for (size_t i = 0; i < n; i++) out[i] = static_cast<uint8_t>(i + 1);
}

/**
 * The machine's side. Each request the session writes is decoded and handed
 * to `respond`, which returns the messages to send back (empty: stay silent).
 * Replies go back through Session::onNotify, split into 20-byte notifications
 * the way a small-MTU link delivers them.
 */
struct FakeMachine {
    Session* session = nullptr;
    Key master{};
    std::optional<Cipher> cipher;
    bool encrypt_replies = true;
    std::vector<json> requests;
    std::vector<std::string> raw;   // as written, key order intact
    std::function<std::vector<json>(const json& req)> respond;

    bool write(const std::vector<uint8_t>& frame) {
        Reassembler r;
        r.feed(frame);
        auto f = r.next();
        if (!f) return false;
        std::string text;
        if (f->type & kEncryptedBit) {
            EXPECT_EQ(f->type, kAppEncrypted);
            if (!cipher) return false;
            auto t = cipher->decrypt(f->payload);
            if (!t) return false;
            text = *t;
        } else {
            EXPECT_EQ(f->type, kAppPlain);
            text = std::string(f->payload.begin(), f->payload.end());
        }
        const json req = json::parse(text);
        raw.push_back(text);
        requests.push_back(req);
        for (const auto& msg : respond(req)) send(msg);
        return true;
    }

    void send(const json& msg) {
        const std::string text = msg.dump();
        std::vector<uint8_t> frame;
        if (cipher && encrypt_replies) {
            std::array<uint8_t, 16> iv{};
            iv[0] = 0x42;
            frame = buildFrame(kDevEncrypted, cipher->encrypt(iv, text));
        } else {
            frame = buildFrame(kDevPlain, std::vector<uint8_t>(text.begin(), text.end()));
        }
        for (size_t i = 0; i < frame.size(); i += 20)
            session->onNotify(frame.data() + i, std::min<size_t>(20, frame.size() - i));
    }

    static json reply(const json& req, const json& result) {
        return {{"id", req["id"]}, {"jsonrpc", req["jsonrpc"]}, {"result", result}};
    }
    static json error(const json& req, int code) {
        return {{"id", req["id"]}, {"jsonrpc", req["jsonrpc"]},
                {"error", {{"code", code}, {"message", "refused"}}}};
    }
    static json fragment(int spool, int seq, const std::vector<uint8_t>& data, const std::string& status) {
        return {{"jsonrpc", "1.0"}, {"method", "SpoolFragment"},
                {"params", {{"spoolId", spool}, {"seq", seq}, {"data", b64(data)}, {"status", status}}}};
    }
};

Timeouts quick() { return Timeouts{150, 150}; }

struct Rig {
    FakeMachine machine;
    Session session;
    explicit Rig(Timeouts t = quick())
        : session([this](const std::vector<uint8_t>& f) { return machine.write(f); }, t, fixedRandom) {
        machine.session = &session;
        session.onConnected();
    }
};

// The machine's answers for pairing with the reference vectors: it checks M1
// as the machine does and refuses a wrong one with VerificationFailure.
std::vector<json> pairingMachine(const json& req) {
    const std::string method = req["method"];
    if (method == "StartKeyExchange") {
        EXPECT_EQ(req["params"]["clientPk"], kA);
        EXPECT_EQ(req["jsonrpc"], "2.0");
        return {FakeMachine::reply(req, {{"serverPk", kB}, {"salt", kSalt}})};
    }
    if (method == "ConfirmKeyExchange") {
        if (req["params"]["clientConfirmation"] != kM1)
            return {FakeMachine::error(req, Session::kVerificationFailure)};
        return {FakeMachine::reply(req, {{"clientId", "client-0001"},
                                         {"serverConfirmation", kM2}, {"nonce", kNonce}})};
    }
    return {};
}

Credentials refCreds() {
    Credentials c;
    c.clientId = "client-0001";
    c.masterKey = keyOf(kK);
    return c;
}

// Opens the session as the machine does: the challenge, then the cipher from
// the nonce. After the integrity reply the machine encrypts too.
std::vector<json> sessionMachine(FakeMachine& m, const json& req) {
    const std::string method = req["method"];
    if (method == "RequestSession") {
        EXPECT_EQ(req["params"]["clientId"], "client-0001");
        return {FakeMachine::reply(req, {{"challenge", kChallenge}, {"nonce", kNonce}})};
    }
    if (method == "CheckSessionIntegrity") {
        const bool ok = req["params"]["response"] == kHmac;
        auto r = FakeMachine::reply(req, {{"confirmation", ok}});
        m.send(r);
        if (ok) m.cipher.emplace(m.master, hexv(kNonce));
        return {};
    }
    return {};
}

std::vector<uint8_t> payload(size_t n, uint8_t seed) {
    std::vector<uint8_t> v(n);
    for (size_t i = 0; i < n; i++) v[i] = static_cast<uint8_t>(seed + i * 7);
    return v;
}

} // namespace

// ── pairing ──

TEST(As11Session, PairsInTwoStepsWithTheReferenceVectors) {
    Rig rig;
    rig.machine.respond = pairingMachine;

    EXPECT_EQ(rig.session.pairStart(), Result::WaitCode);
    EXPECT_TRUE(rig.session.pairPending());
    ASSERT_EQ(rig.machine.requests.size(), 1u);
    // Clear JSON-RPC 2.0, id 1, keys in the order the machine was proven with.
    EXPECT_EQ(rig.machine.raw[0],
              std::string(R"({"id":1,"jsonrpc":"2.0","method":"StartKeyExchange","params":{"clientPk":")") +
                  kA + "\"}}");

    Credentials c;
    EXPECT_EQ(rig.session.pairConfirm(kCode, &c), Result::Ok);
    EXPECT_EQ(c.clientId, "client-0001");
    EXPECT_EQ(hexEncode(c.masterKey.data(), 32), kK);
    EXPECT_FALSE(rig.session.pairPending());
    EXPECT_EQ(rig.session.lastError(), "");
}

TEST(As11Session, AWrongCodeIsRefusedAndNothingIsKept) {
    Rig rig;
    rig.machine.respond = pairingMachine;
    ASSERT_EQ(rig.session.pairStart(), Result::WaitCode);
    Credentials c;
    EXPECT_EQ(rig.session.pairConfirm("4830", &c), Result::Verify);
    EXPECT_FALSE(c.valid());
    EXPECT_FALSE(rig.session.pairPending());
    EXPECT_EQ(rig.session.lastError(), "machine refused the code");
    // The exchange is closed: a second code needs a new start.
    EXPECT_EQ(rig.session.pairConfirm(kCode, &c), Result::Protocol);
}

TEST(As11Session, AMachineWhoseProofDoesNotVerifyIsRefused) {
    Rig rig;
    rig.machine.respond = [](const json& req) -> std::vector<json> {
        if (req["method"] == "ConfirmKeyExchange")
            return {FakeMachine::reply(req, {{"clientId", "client-0001"},
                                             {"serverConfirmation", kK}, {"nonce", kNonce}})};
        return pairingMachine(req);
    };
    ASSERT_EQ(rig.session.pairStart(), Result::WaitCode);
    Credentials c;
    EXPECT_EQ(rig.session.pairConfirm(kCode, &c), Result::Verify);
    EXPECT_FALSE(c.valid());
}

TEST(As11Session, PairingTimesOutWhenTheMachineIsSilent) {
    Rig rig;
    rig.machine.respond = [](const json&) { return std::vector<json>{}; };
    const auto t0 = std::chrono::steady_clock::now();
    EXPECT_EQ(rig.session.pairStart(), Result::Protocol);
    EXPECT_LT(std::chrono::steady_clock::now() - t0, std::chrono::seconds(2));
    EXPECT_FALSE(rig.session.pairPending());

    // Start answered, confirm never does.
    rig.machine.respond = [](const json& req) -> std::vector<json> {
        if (req["method"] == "StartKeyExchange") return pairingMachine(req);
        return {};
    };
    ASSERT_EQ(rig.session.pairStart(), Result::WaitCode);
    Credentials c;
    EXPECT_EQ(rig.session.pairConfirm(kCode, &c), Result::Protocol);
    EXPECT_EQ(rig.session.lastError(), "no answer to the confirmation");
}

TEST(As11Session, ADisconnectBetweenTheStepsEndsThePairing) {
    Rig rig;
    rig.machine.respond = pairingMachine;
    ASSERT_EQ(rig.session.pairStart(), Result::WaitCode);
    rig.session.onDisconnect();
    Credentials c;
    EXPECT_EQ(rig.session.pairConfirm(kCode, &c), Result::Connect);
    EXPECT_FALSE(rig.session.pairPending());
}

TEST(As11Session, ARefusedKeyExchangeIsAVerifyFailure) {
    Rig rig;
    rig.machine.respond = [](const json& req) {
        return std::vector<json>{FakeMachine::error(req, -11001)};
    };
    EXPECT_EQ(rig.session.pairStart(), Result::Verify);
    EXPECT_FALSE(rig.session.pairPending());
}

// ── the session ──

TEST(As11Session, OpensWithTheHmacChallengeThenEncrypts) {
    Rig rig;
    rig.machine.master = keyOf(kK);
    rig.machine.respond = [&](const json& req) { return sessionMachine(rig.machine, req); };
    EXPECT_FALSE(rig.session.encrypted());
    EXPECT_EQ(rig.session.open(refCreds()), Result::Ok);
    EXPECT_TRUE(rig.session.encrypted());
    ASSERT_EQ(rig.machine.requests.size(), 2u);
    EXPECT_EQ(rig.machine.requests[1]["params"]["response"], kHmac);
}

TEST(As11Session, AForgottenPairingIsAVerifyFailure) {
    Rig rig;
    rig.machine.respond = [](const json& req) {
        return std::vector<json>{FakeMachine::error(req, Session::kVerificationFailure)};
    };
    EXPECT_EQ(rig.session.open(refCreds()), Result::Verify);
    EXPECT_EQ(rig.session.lastError(), "machine no longer knows this pairing");
    EXPECT_EQ(Rig().session.open(Credentials{}), Result::NotPaired);
}

TEST(As11Session, AWrongKeyFailsTheIntegrityCheck) {
    Rig rig;
    rig.machine.master = keyOf(kK);
    rig.machine.respond = [&](const json& req) { return sessionMachine(rig.machine, req); };
    auto c = refCreds();
    c.masterKey[0] ^= 1;
    EXPECT_EQ(rig.session.open(c), Result::Verify);
    EXPECT_FALSE(rig.session.encrypted());
}

// ── the Summary spool ──

namespace {

// A session already open, then a spool machine that sends `frags` fragments
// of the given sizes, mutated by `tweak` (seq, status, data) before sending.
struct SpoolRig : Rig {
    std::vector<std::vector<uint8_t>> frags;
    std::string final_status = "SPOOL_COMPLETE";
    std::function<void(int index, json& msg)> tweak;
    bool answer_pull = true;
    int spool_id = 7;

    SpoolRig() {
        machine.master = keyOf(kK);
        machine.respond = [this](const json& req) { return answer(req); };
        EXPECT_EQ(session.open(refCreds()), Result::Ok);
    }

    std::vector<json> answer(const json& req) {
        const std::string method = req["method"];
        if (method == "RequestSession" || method == "CheckSessionIntegrity")
            return sessionMachine(machine, req);
        if (method == "StartSpool")
            return {FakeMachine::reply(req, {{"spoolId", spool_id}})};
        if (method == "PullSpoolFragments") {
            if (!answer_pull) return {FakeMachine::error(req, -11010)};
            std::vector<json> out{FakeMachine::reply(req, json::object())};
            for (size_t i = 0; i < frags.size(); i++) {
                auto msg = FakeMachine::fragment(spool_id, static_cast<int>(i), frags[i],
                                                 i + 1 == frags.size() ? final_status : "OK");
                if (tweak) tweak(static_cast<int>(i), msg);
                out.push_back(msg);
            }
            return out;
        }
        return {};
    }
};

} // namespace

TEST(As11Session, PullsTheSummaryInOrder) {
    SpoolRig rig;
    rig.frags = {payload(2808, 1), payload(2808, 2), payload(100, 3)};
    std::vector<uint8_t> out;
    EXPECT_EQ(rig.session.pullSpool("Summary", "2026-10-01", &out), Result::Ok);
    std::vector<uint8_t> want;
    for (const auto& f : rig.frags) want.insert(want.end(), f.begin(), f.end());
    EXPECT_EQ(out, want);
    EXPECT_EQ(rig.session.lastSpoolStatus(), "SPOOL_COMPLETE");

    // What was asked, encrypted, JSON-RPC 1.0, ids from 2.
    const auto& start = rig.machine.requests[2];
    EXPECT_EQ(start["method"], "StartSpool");
    EXPECT_EQ(start["jsonrpc"], "1.0");
    EXPECT_EQ(start["id"], 2);
    EXPECT_EQ(start["params"]["maxSpoolSize"], 65535);
    EXPECT_EQ(start["params"]["spoolAddress"]["Summary"]["fromDateTime"], "2026-10-01T00:00:00.000Z");
    const auto& pull = rig.machine.requests[3];
    EXPECT_EQ(pull["id"], 3);
    EXPECT_EQ(pull["params"]["maxFragmentSize"], 2808);
    EXPECT_EQ(pull["params"]["maxNotifications"], 0);
    EXPECT_EQ(pull["params"]["spoolId"], 7);
}

TEST(As11Session, ASeqGapIsRefused) {
    SpoolRig rig;
    rig.frags = {payload(50, 1), payload(50, 2), payload(50, 3)};
    rig.tweak = [](int i, json& msg) { if (i == 1) msg["params"]["seq"] = 2; };
    std::vector<uint8_t> out = {9};
    EXPECT_EQ(rig.session.pullSpool("Summary", "2026-10-01", &out), Result::Spool);
    EXPECT_TRUE(out.empty());
    EXPECT_NE(rig.session.lastError().find("lost"), std::string::npos);
}

TEST(As11Session, ErrorDataUnavailableEndsTheSpool) {
    SpoolRig rig;
    rig.frags = {{}};
    rig.final_status = "ERROR_DATA_UNAVAILABLE";
    std::vector<uint8_t> out = {9};
    EXPECT_EQ(rig.session.pullSpool("Summary", "2026-10-29", &out), Result::Ok);
    EXPECT_TRUE(out.empty());
    EXPECT_EQ(rig.session.lastSpoolStatus(), "ERROR_DATA_UNAVAILABLE");
}

TEST(As11Session, ASpoolThatStopsMidwayStalls) {
    SpoolRig rig;
    rig.frags = {payload(50, 1), payload(50, 2)};
    rig.final_status = "OK";   // never completes
    std::vector<uint8_t> out;
    EXPECT_EQ(rig.session.pullSpool("Summary", "2026-10-01", &out), Result::Spool);
    EXPECT_EQ(rig.session.lastError(), "spool stalled");
}

TEST(As11Session, AnUndecodableFragmentFails) {
    SpoolRig rig;
    rig.frags = {payload(50, 1)};
    rig.tweak = [](int, json& msg) { msg["params"]["data"] = "!!not base64!!"; };
    std::vector<uint8_t> out;
    EXPECT_EQ(rig.session.pullSpool("Summary", "2026-10-01", &out), Result::Spool);
}

TEST(As11Session, FragmentsOfAnotherSpoolAreIgnored) {
    SpoolRig rig;
    rig.frags = {payload(50, 1), payload(50, 2)};
    rig.tweak = [&rig](int i, json& msg) {
        if (i == 0) {
            json stray = msg;
            stray["params"]["spoolId"] = 99;
            stray["params"]["seq"] = 5;
            rig.machine.send(stray);
        }
    };
    std::vector<uint8_t> out;
    EXPECT_EQ(rig.session.pullSpool("Summary", "2026-10-01", &out), Result::Ok);
    EXPECT_EQ(out.size(), 100u);
}

TEST(As11Session, ARefusedPullAndANoReplyStartAreProtocolErrors) {
    SpoolRig rig;
    rig.answer_pull = false;
    std::vector<uint8_t> out;
    EXPECT_EQ(rig.session.pullSpool("Summary", "2026-10-01", &out), Result::Protocol);
    EXPECT_EQ(rig.session.lastError(), "machine refused the pull");

    rig.machine.respond = [](const json&) { return std::vector<json>{}; };
    EXPECT_EQ(rig.session.pullSpool("Summary", "2026-10-01", &out), Result::Protocol);
    EXPECT_EQ(rig.session.lastError(), "no answer to StartSpool");
}

TEST(As11Session, ADisconnectMidSpoolEndsItAtOnce) {
    Rig rig(Timeouts{2000, 5000});
    rig.machine.master = keyOf(kK);
    std::thread dropper;
    rig.machine.respond = [&](const json& req) -> std::vector<json> {
        const std::string method = req["method"];
        if (method == "RequestSession" || method == "CheckSessionIntegrity")
            return sessionMachine(rig.machine, req);
        if (method == "StartSpool") return {FakeMachine::reply(req, {{"spoolId", 3}})};
        if (method == "PullSpoolFragments") {
            dropper = std::thread([&] {
                std::this_thread::sleep_for(std::chrono::milliseconds(50));
                rig.session.onDisconnect();
            });
            return {FakeMachine::reply(req, json::object()),
                    FakeMachine::fragment(3, 0, payload(10, 1), "OK")};
        }
        return {};
    };
    ASSERT_EQ(rig.session.open(refCreds()), Result::Ok);
    const auto t0 = std::chrono::steady_clock::now();
    std::vector<uint8_t> out;
    EXPECT_EQ(rig.session.pullSpool("Summary", "2026-10-01", &out), Result::Spool);
    EXPECT_LT(std::chrono::steady_clock::now() - t0, std::chrono::seconds(2));
    EXPECT_EQ(rig.session.lastError(), "machine disconnected mid-spool");
    dropper.join();
}

TEST(As11Session, NoSpoolWithoutASession) {
    Rig rig;
    std::vector<uint8_t> out;
    EXPECT_EQ(rig.session.pullSpool("Summary", "2026-10-01", &out), Result::Protocol);
    EXPECT_TRUE(rig.machine.requests.empty());
}

TEST(As11Session, ClearFramesAndJunkDoNotDisturbAReply) {
    // A heartbeat, a non-JSON frame and an encrypted frame before any session
    // arrive while StartKeyExchange waits; only the reply with its id counts.
    Rig rig;
    rig.machine.respond = [&](const json& req) -> std::vector<json> {
        rig.machine.send({{"jsonrpc", "2.0"}, {"method", "HeartBeat"}});
        const std::string junk = "not json";
        const auto f = buildFrame(kDevPlain, std::vector<uint8_t>(junk.begin(), junk.end()));
        rig.session.onNotify(f);
        rig.session.onNotify(buildFrame(kDevEncrypted, std::vector<uint8_t>(32, 0)));
        rig.machine.send(FakeMachine::reply(json{{"id", 9}, {"jsonrpc", "2.0"}}, {{"x", 1}}));
        return pairingMachine(req);
    };
    EXPECT_EQ(rig.session.pairStart(), Result::WaitCode);
}

// ── credentials and scanning ──

TEST(As11Session, CredentialsRoundTripWithMode0600) {
    const auto dir = std::filesystem::temp_directory_path() /
                     ("as11_creds_" + std::to_string(::testing::UnitTest::GetInstance()->random_seed()) +
                      "_" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    std::filesystem::create_directories(dir);
    const auto path = credentialsPath(dir.string());
    EXPECT_EQ(std::filesystem::path(path).filename(), "as11.json");
    EXPECT_FALSE(loadCredentials(path).has_value());

    ASSERT_TRUE(saveCredentials(path, refCreds()));
    auto back = loadCredentials(path);
    ASSERT_TRUE(back.has_value());
    EXPECT_EQ(back->clientId, "client-0001");
    EXPECT_EQ(back->masterKey, keyOf(kK));
#ifndef _WIN32
    struct stat st{};
    ASSERT_EQ(::stat(path.c_str(), &st), 0);
    EXPECT_EQ(st.st_mode & 0777, 0600u);
#endif
    EXPECT_FALSE(std::filesystem::exists(path + ".tmp"));

    // Overwritten in place, still 0600; a malformed file is no pairing.
    auto c2 = refCreds();
    c2.clientId = "client-0002";
    ASSERT_TRUE(saveCredentials(path, c2));
    EXPECT_EQ(loadCredentials(path)->clientId, "client-0002");
    { std::ofstream(path) << "{\"clientId\":\"x\",\"masterPairKey\":\"12\"}"; }
    EXPECT_FALSE(loadCredentials(path).has_value());
    EXPECT_FALSE(saveCredentials(path, Credentials{}));

    EXPECT_TRUE(clearCredentials(path));
    EXPECT_FALSE(std::filesystem::exists(path));
    std::filesystem::remove_all(dir);
}

TEST(As11Session, RecognisesAnAirSenseAdvert) {
    EXPECT_TRUE(advertIsAirSense("ResMed 281654", {}));
    EXPECT_TRUE(advertIsAirSense("", {"0000FD56-0000-1000-8000-00805F9B34FB"}));
    EXPECT_FALSE(advertIsAirSense("ResMed", {}));
    EXPECT_FALSE(advertIsAirSense("O2Ring 1234", {"0000180d-0000-1000-8000-00805f9b34fb"}));
}

TEST(As11Session, ErrorsNeverCarryTheSecrets) {
    Rig rig;
    rig.machine.respond = pairingMachine;
    ASSERT_EQ(rig.session.pairStart(), Result::WaitCode);
    Credentials c;
    rig.session.pairConfirm("4830", &c);
    const auto e = rig.session.lastError();
    EXPECT_EQ(e.find("4830"), std::string::npos);
    EXPECT_EQ(e.find(kK), std::string::npos);
    EXPECT_EQ(e.find("client-0001"), std::string::npos);
}
