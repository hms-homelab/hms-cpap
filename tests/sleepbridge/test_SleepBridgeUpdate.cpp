// A firmware image to a bridge, over its own stream connection: the `ota`
// command naming the image's size and SHA-256, then the image as kind 8
// frames of 4096 bytes, the last flagged. The bridge answers the command;
// ok means it is restarting into the image, and its next hello says what it
// runs. Refused while a night is open on the bridge, while it is offline,
// while another update runs, and for bytes that are not a Sleep Bridge
// image. One job per bridge is kept and shown.

#include "SleepBridgeTestUtil.h"

#include "sleepbridge/Conversation.h"
#include "sleepbridge/SleepBridgeService.h"

#include <gtest/gtest.h>
#include <openssl/evp.h>

#include <chrono>
#include <memory>
#include <sstream>
#include <thread>

using namespace sbtest;

namespace {

const std::string kSender = "TAP-S3-00000123";

Json::Value parse(const std::string& s) {
    Json::Value v;
    Json::CharReaderBuilder rb;
    std::istringstream ss(s);
    Json::parseFromStream(rb, ss, &v, nullptr);
    return v;
}

std::string hexDigest(const std::string& bytes) {
    unsigned char md[EVP_MAX_MD_SIZE];
    unsigned int  n   = 0;
    EVP_MD_CTX*   ctx = EVP_MD_CTX_new();
    EVP_DigestInit_ex(ctx, EVP_sha256(), nullptr);
    EVP_DigestUpdate(ctx, bytes.data(), bytes.size());
    EVP_DigestFinal_ex(ctx, md, &n);
    EVP_MD_CTX_free(ctx);
    std::string out;
    for (unsigned int i = 0; i < n; i++) {
        char b[3];
        snprintf(b, sizeof(b), "%02x", md[i]);
        out += b;
    }
    return out;
}

/// An ESP32 application image of `size` bytes whose app descriptor says
/// `project` and `version`: the image magic, a segment header, then the
/// descriptor; the rest is filler.
std::string image(size_t size, const std::string& project = "sd_capture",
                  const std::string& version = "1.0.0") {
    std::string s(size, '\0');
    for (size_t i = 0; i < size; i++) s[i] = static_cast<char>((i * 7 + 3) & 0xFF);
    s[0] = static_cast<char>(0xE9);
    auto put = [&](size_t at, const std::string& text, size_t n) {
        for (size_t i = 0; i < n; i++) s[at + i] = i < text.size() ? text[i] : '\0';
    };
    const size_t d = 32;
    s[d] = 0x32; s[d + 1] = 0x54; s[d + 2] = static_cast<char>(0xCD); s[d + 3] = static_cast<char>(0xAB);
    put(d + 16, version, 32);
    put(d + 48, project, 32);
    put(d + 80, "09:28:22", 16);
    put(d + 96, "Oct 31 2026", 16);
    return s;
}

/// A connection's down frames, text and binary, as the bridge would receive
/// them. `bin_fail_after` >= 0: the connection goes after that many chunks.
struct Wire {
    std::shared_ptr<std::vector<Json::Value>> frames = std::make_shared<std::vector<Json::Value>>();
    std::shared_ptr<std::vector<std::string>> bins   = std::make_shared<std::vector<std::string>>();
    std::shared_ptr<bool> up             = std::make_shared<bool>(true);
    std::shared_ptr<int>  bin_fail_after = std::make_shared<int>(-1);
    Outlet outlet() {
        auto f = frames;
        auto u = up;
        return [f, u](const std::string& text) {
            if (!*u) return false;
            f->push_back(parse(text));
            return true;
        };
    }
    BinaryOutlet binary() {
        auto b  = bins;
        auto u  = up;
        auto fa = bin_fail_after;
        return [b, u, fa](const std::string& bytes) {
            if (!*u) return false;
            if (*fa >= 0 && static_cast<int>(b->size()) >= *fa) return false;
            b->push_back(bytes);
            return true;
        };
    }
};

struct Fixture {
    TempDir            t;
    SleepBridgeService svc;

    explicit Fixture(double night_quiet_s = 15 * 60.0, double update_timeout_s = 300.0)
        : svc(options(t, night_quiet_s, update_timeout_s), fakeFactory(std::make_shared<FakeLog>())) {}

    static SleepBridgeService::Options options(const TempDir& t, double night_quiet_s, double update_timeout_s) {
        SleepBridgeService::Options o;
        o.root             = t.path.string();
        o.quiet_s          = 0.01;
        o.night_quiet_s    = night_quiet_s;
        o.update_timeout_s = update_timeout_s;
        return o;
    }

    std::unique_ptr<Conversation> session(Wire& w, const std::string& fw = "1.0.0") {
        auto c = svc.open({kSender, "airsense-bench", "A1B2C3D4E5F6"}, w.outlet(), w.binary());
        svc.apply(kSender, c->onText(R"({"t":"hello","state":{"fw_version":")" + fw + R"("}})"));
        return c;
    }
    Json::Value bridge() { return svc.status()["bridges"][0]; }
    void answer(Conversation& c, uint64_t req_id, const std::string& result) {
        std::ostringstream res;
        res << R"({"t":"result","req_id":)" << req_id << R"(,"result":)" << result << "}";
        svc.apply(kSender, c.onText(res.str()));
    }
};

}  // namespace

TEST(SleepBridgeUpdate, InspectImageReadsTheDescriptor) {
    std::string why;
    const auto  info = inspectImage(image(5000), &why);
    ASSERT_TRUE(info) << why;
    EXPECT_EQ(info->project, "sd_capture");
    EXPECT_EQ(info->version, "1.0.0");
    EXPECT_EQ(info->built, "Oct 31 2026 09:28:22");
    const auto other = inspectImage(image(5000, "mule", "6.0.0"), &why);
    ASSERT_TRUE(other);
    EXPECT_EQ(other->project, "mule");
    EXPECT_EQ(other->version, "6.0.0");

    EXPECT_FALSE(inspectImage(std::string(5000, 'x'), &why));
    EXPECT_NE(why.find("image magic"), std::string::npos) << why;
    std::string no_desc = image(5000);
    no_desc[32] = 0;
    EXPECT_FALSE(inspectImage(no_desc, &why));
    EXPECT_NE(why.find("app descriptor"), std::string::npos) << why;
    EXPECT_FALSE(inspectImage(image(5000).substr(0, 100), &why));
    EXPECT_NE(why.find("short"), std::string::npos) << why;
    EXPECT_FALSE(inspectImage("", &why));
}

TEST(SleepBridgeUpdate, TheCommandThenTheChunksGoOnTheSession) {
    Fixture f;
    Wire w;
    auto c = f.session(w);
    const std::string img = image(10000);
    const auto a = f.svc.startUpdate(kSender, img);
    ASSERT_EQ(a.status, 200) << a.body.toStyledString();
    const uint64_t req = a.body["req_id"].asUInt64();
    EXPECT_GT(req, 0u);
    EXPECT_LT(req, 0x80000000ull) << "the chunks carry the req_id in 32 bits";
    f.svc.waitForUpdateSend();

    // The command first, naming the image.
    ASSERT_EQ(w.frames->size(), 1u);
    const auto& cmd = w.frames->at(0);
    EXPECT_EQ(cmd["t"].asString(), "cmd");
    EXPECT_EQ(cmd["cmd"].asString(), "ota");
    EXPECT_EQ(cmd["req_id"].asUInt64(), req);
    EXPECT_EQ(cmd["target"].asString(), "tapper");
    EXPECT_EQ(cmd["size"].asUInt64(), 10000u);
    EXPECT_EQ(cmd["sha256"].asString(), hexDigest(img));
    EXPECT_FALSE(cmd.isMember("url"));

    // Then the image: 4096, 4096, 1808, seq from 0, the last flagged, kind 8.
    ASSERT_EQ(w.bins->size(), 3u);
    std::string joined;
    for (size_t i = 0; i < w.bins->size(); i++) {
        const auto h = parseFrameHeader(w.bins->at(i));
        ASSERT_TRUE(h);
        EXPECT_EQ(h->req_id, static_cast<uint32_t>(req));
        EXPECT_EQ(h->seq, i);
        EXPECT_EQ(h->kind, kKindFirmware);
        EXPECT_EQ(h->flags & 1, i == 2 ? 1 : 0) << "chunk " << i;
        const size_t body = w.bins->at(i).size() - kFrameHeaderSize;
        EXPECT_EQ(body, i == 2 ? 1808u : 4096u) << "chunk " << i;
        joined += w.bins->at(i).substr(kFrameHeaderSize);
    }
    EXPECT_EQ(joined, img);

    const auto u = f.bridge()["update"];
    EXPECT_EQ(u["phase"].asString(), "waiting");
    EXPECT_TRUE(u["running"].asBool());
    EXPECT_EQ(u["sent"].asUInt64(), 10000u);
    EXPECT_EQ(u["total"].asUInt64(), 10000u);
    EXPECT_EQ(u["image_version"].asString(), "1.0.0");
    EXPECT_EQ(u["fw_before"].asString(), "1.0.0");
    EXPECT_EQ(u["sha256"].asString(), hexDigest(img));
    EXPECT_EQ(f.svc.update(kSender)["req_id"].asUInt64(), req);
    EXPECT_TRUE(f.svc.update("TAP-S3-00000999").isNull());
}

// An image whose size is a multiple of 4096 ends on a full, flagged chunk:
// no empty frame follows.
TEST(SleepBridgeUpdate, AWholeNumberOfChunksEndsOnTheLastFullOne) {
    Fixture f;
    Wire w;
    auto c = f.session(w);
    ASSERT_EQ(f.svc.startUpdate(kSender, image(8192)).status, 200);
    f.svc.waitForUpdateSend();
    ASSERT_EQ(w.bins->size(), 2u);
    EXPECT_EQ(parseFrameHeader(w.bins->at(0))->flags & 1, 0);
    EXPECT_EQ(parseFrameHeader(w.bins->at(1))->flags & 1, 1);
    EXPECT_EQ(w.bins->at(1).size(), kFrameHeaderSize + 4096);
}

TEST(SleepBridgeUpdate, OkMeansRestartingAndTheNextHelloCloses) {
    Fixture f;
    Wire w;
    auto c = f.session(w);
    const auto a = f.svc.startUpdate(kSender, image(10000, "sd_capture", "1.0.0"));
    ASSERT_EQ(a.status, 200);
    const uint64_t req = a.body["req_id"].asUInt64();
    f.svc.waitForUpdateSend();
    f.answer(*c, req, R"({"ok":true,"slot":"ota_1","version":"1.0.0","project":"sd_capture","built":"Oct 31 2026 09:28:22"})");
    auto u = f.bridge()["update"];
    EXPECT_EQ(u["phase"].asString(), "restarting");
    EXPECT_TRUE(u["running"].asBool());
    EXPECT_EQ(u["result"]["slot"].asString(), "ota_1");
    EXPECT_GT(u["answered_at"].asInt64(), 0);
    // The answer is the update's, not an unknown command's: nothing to list.
    for (const auto& k : f.bridge()["commands"]) EXPECT_NE(k["req_id"].asUInt64(), req);

    // The bridge restarts: its session closes, and its next hello is the outcome.
    f.svc.closed(*c);
    Wire w2;
    auto c2 = f.session(w2, "1.0.0");
    u = f.bridge()["update"];
    EXPECT_EQ(u["phase"].asString(), "done");
    EXPECT_FALSE(u["running"].asBool());
    EXPECT_EQ(u["fw_after"].asString(), "1.0.0");
    EXPECT_GT(u["ended_at"].asInt64(), 0);
    EXPECT_EQ(f.bridge()["fw_version"].asString(), "1.0.0");
    EXPECT_TRUE(w2.frames->empty()) << "nothing is sent again";
    // And a new update can start.
    EXPECT_EQ(f.svc.startUpdate(kSender, image(5000)).status, 200);
    f.svc.waitForUpdateSend();
}

TEST(SleepBridgeUpdate, TheBridgesRefusalFailsTheJob) {
    Fixture f;
    Wire w;
    auto c = f.session(w);
    const auto a = f.svc.startUpdate(kSender, image(10000));
    ASSERT_EQ(a.status, 200);
    f.svc.waitForUpdateSend();
    f.answer(*c, a.body["req_id"].asUInt64(), R"({"ok":false,"error":"sha256 mismatch"})");
    const auto u = f.bridge()["update"];
    EXPECT_EQ(u["phase"].asString(), "failed");
    EXPECT_FALSE(u["running"].asBool());
    EXPECT_EQ(u["error"].asString(), "sha256 mismatch");
    // A later hello changes nothing about it.
    f.svc.closed(*c);
    Wire w2;
    auto c2 = f.session(w2);
    EXPECT_EQ(f.bridge()["update"]["phase"].asString(), "failed");
    EXPECT_EQ(f.bridge()["update"]["fw_after"].asString(), "");
}

TEST(SleepBridgeUpdate, NothingGoesWhileANightIsOpen) {
    Fixture f(0.2);
    Wire w;
    auto c = f.session(w);
    EXPECT_FALSE(f.bridge()["night_open"].asBool());
    EXPECT_FALSE(f.svc.nightOpen(kSender));
    f.svc.apply(kSender, c->onBinary(frame(0, run(0, 3, 100))));
    EXPECT_TRUE(f.bridge()["night_open"].asBool());
    EXPECT_TRUE(f.svc.nightOpen(kSender));
    const auto a = f.svc.startUpdate(kSender, image(10000));
    EXPECT_EQ(a.status, 409);
    EXPECT_NE(a.body["error"].asString().find("night is open"), std::string::npos) << a.body;
    EXPECT_TRUE(w.frames->empty());
    EXPECT_TRUE(w.bins->empty());
    EXPECT_TRUE(f.bridge()["update"].isNull());
    // A heartbeat is not a write: it keeps nothing open.
    std::this_thread::sleep_for(std::chrono::milliseconds(250));
    f.svc.apply(kSender, c->onBinary(frame(1, heartbeat())));
    EXPECT_FALSE(f.svc.nightOpen(kSender));
    EXPECT_EQ(f.svc.startUpdate(kSender, image(10000)).status, 200);
    f.svc.waitForUpdateSend();
    EXPECT_EQ(w.bins->size(), 3u);
}

TEST(SleepBridgeUpdate, OnlyOneAtATimeOnlyOnlineOnlyABridgeImage) {
    Fixture f;
    Wire w;
    auto c = f.session(w);
    EXPECT_EQ(f.svc.startUpdate("TAP-S3-00000999", image(5000)).status, 404);
    EXPECT_EQ(f.svc.startUpdate(kSender, "").status, 400);
    EXPECT_EQ(f.svc.startUpdate(kSender, std::string(5000, 'x')).status, 400);
    {
        const auto a = f.svc.startUpdate(kSender, image(5000, "mule", "6.0.0"));
        EXPECT_EQ(a.status, 400);
        EXPECT_NE(a.body["error"].asString().find("not a Sleep Bridge image"), std::string::npos) << a.body;
    }
    EXPECT_EQ(f.svc.startUpdate(kSender, image(kImageMax + 1)).status, 400);
    EXPECT_TRUE(w.frames->empty());

    ASSERT_EQ(f.svc.startUpdate(kSender, image(5000)).status, 200);
    f.svc.waitForUpdateSend();
    {
        const auto again = f.svc.startUpdate(kSender, image(5000));
        EXPECT_EQ(again.status, 409);
        EXPECT_NE(again.body["error"].asString().find("already running"), std::string::npos) << again.body;
    }
    EXPECT_EQ(w.frames->size(), 1u);

    f.svc.closed(*c);
    const auto off = f.svc.startUpdate(kSender, image(5000));
    EXPECT_EQ(off.status, 409);
    EXPECT_NE(off.body["error"].asString().find("not connected"), std::string::npos) << off.body;
}

TEST(SleepBridgeUpdate, AConnectionThatGoesFailsTheJob) {
    Fixture f;
    Wire w;
    auto c = f.session(w);
    *w.bin_fail_after = 1;   // the second chunk finds the connection gone
    ASSERT_EQ(f.svc.startUpdate(kSender, image(10000)).status, 200);
    f.svc.waitForUpdateSend();
    auto u = f.bridge()["update"];
    EXPECT_EQ(u["phase"].asString(), "failed");
    EXPECT_NE(u["error"].asString().find("during the image"), std::string::npos) << u;
    EXPECT_EQ(u["sent"].asUInt64(), 4096u);
    EXPECT_EQ(w.bins->size(), 1u);
    // Failed is not running: the next one may start.
    *w.bin_fail_after = -1;
    ASSERT_EQ(f.svc.startUpdate(kSender, image(10000)).status, 200);
    f.svc.waitForUpdateSend();
    EXPECT_EQ(f.bridge()["update"]["phase"].asString(), "waiting");

    // Gone before the command: failed on the spot.
    Fixture g;
    Wire gw;
    auto gc = g.session(gw);
    *gw.up  = false;
    ASSERT_EQ(g.svc.startUpdate(kSender, image(10000)).status, 200);
    g.svc.waitForUpdateSend();
    EXPECT_EQ(g.bridge()["update"]["phase"].asString(), "failed");
    EXPECT_TRUE(gw.bins->empty());
}

TEST(SleepBridgeUpdate, AnUnansweredJobTimesOut) {
    Fixture f(15 * 60.0, 0.0);
    Wire w;
    auto c = f.session(w);
    const auto a = f.svc.startUpdate(kSender, image(10000));
    ASSERT_EQ(a.status, 200);
    f.svc.waitForUpdateSend();
    const auto u = f.svc.update(kSender);
    EXPECT_EQ(u["phase"].asString(), "failed");
    EXPECT_NE(u["error"].asString().find("no answer"), std::string::npos) << u;
    EXPECT_EQ(f.bridge()["update"]["phase"].asString(), "failed");
    // A late answer no longer changes it.
    f.answer(*c, a.body["req_id"].asUInt64(), R"({"ok":true})");
    EXPECT_EQ(f.bridge()["update"]["phase"].asString(), "failed");
}

TEST(SleepBridgeUpdate, StatusCarriesTheFirmwareAndOtaIsNotAQueuedCommand) {
    Fixture f;
    Wire w;
    auto c = f.session(w, "1.0.0");
    EXPECT_EQ(f.bridge()["fw_version"].asString(), "1.0.0");
    EXPECT_FALSE(f.bridge()["night_open"].asBool());
    EXPECT_TRUE(f.bridge()["update"].isNull());
    // The image comes with the update route, never through the command one.
    EXPECT_EQ(f.svc.queueCommand(kSender, parse(R"({"cmd":"ota"})")).status, 400);
    EXPECT_TRUE(w.frames->empty());
    // The pure frame builders, as the wire doc states them.
    const auto cmd = parse(otaCommandFrame(77, 10000, "ab"));
    EXPECT_EQ(cmd["cmd"].asString(), "ota");
    EXPECT_EQ(cmd["size"].asUInt64(), 10000u);
    const std::string chunk = firmwareChunkFrame(77, 2, true, "xyz");
    const auto h = parseFrameHeader(chunk);
    ASSERT_TRUE(h);
    EXPECT_EQ(h->req_id, 77u);
    EXPECT_EQ(h->seq, 2u);
    EXPECT_EQ(h->flags, 1u);
    EXPECT_EQ(h->kind, 8u);
    EXPECT_EQ(chunk.substr(kFrameHeaderSize), "xyz");
}
