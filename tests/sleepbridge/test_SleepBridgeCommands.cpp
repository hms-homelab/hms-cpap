// Commands to a bridge from this hms-cpap: sent on the bridge's own session
// (never on a test hello's), kept until the next session when it is away,
// answered by req_id on whichever session the answer comes back on, a log
// pull's text gathered ahead of its result, and refused only for what a
// bridge does not take. No refusal while the machine records.

#include "SleepBridgeTestUtil.h"

#include "sleepbridge/Conversation.h"
#include "sleepbridge/SleepBridgeService.h"

#include <gtest/gtest.h>

#include <memory>
#include <sstream>

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

Json::Value cmd(const std::string& json) { return parse(json); }

/// A connection's down frames, as the bridge would receive them.
struct Wire {
    std::shared_ptr<std::vector<Json::Value>> frames = std::make_shared<std::vector<Json::Value>>();
    std::shared_ptr<bool> up = std::make_shared<bool>(true);
    Outlet outlet() {
        auto f = frames;
        auto u = up;
        return [f, u](const std::string& text) {
            if (!*u) return false;
            f->push_back(parse(text));
            return true;
        };
    }
};

struct Fixture {
    TempDir            t;
    SleepBridgeService svc{{t.path.string(), kDefaultPort, 0.01}, fakeFactory(std::make_shared<FakeLog>())};

    std::unique_ptr<Conversation> session(Wire& w) {
        auto c = svc.open({kSender, "airsense-bench", "A1B2C3D4E5F6"}, w.outlet());
        svc.apply(kSender, c->onText(R"({"t":"hello","state":{"fw_version":"1.0.0"}})"));
        return c;
    }
    Json::Value bridge() { return svc.status()["bridges"][0]; }
    Json::Value command(uint64_t req_id) {
        const Json::Value list = bridge()["commands"];   // a copy: the status is a temporary
        for (const auto& c : list)
            if (c["req_id"].asUInt64() == req_id) return c;
        return Json::Value();
    }
};

}  // namespace

TEST(SleepBridgeCommands, ACommandGoesDownAtOnceToAnOnlineBridge) {
    Fixture f;
    Wire w;
    auto c = f.session(w);
    const auto a = f.svc.queueCommand(kSender, cmd(R"({"cmd":"stream","url":""})"));
    ASSERT_EQ(a.status, 200) << a.body.toStyledString();
    const uint64_t req = a.body["req_id"].asUInt64();
    EXPECT_GT(req, 0u);
    EXPECT_LT(req, 0x80000000ull) << "a log frame carries the req_id in 32 bits";
    EXPECT_EQ(a.body["command"]["state"].asString(), "sent");

    ASSERT_EQ(w.frames->size(), 1u);
    const auto& down = w.frames->at(0);
    EXPECT_EQ(down["t"].asString(), "cmd");
    EXPECT_EQ(down["cmd"].asString(), "stream");
    EXPECT_EQ(down["url"].asString(), "");
    EXPECT_EQ(down["req_id"].asUInt64(), req);

    const auto shown = f.command(req);
    EXPECT_EQ(shown["cmd"].asString(), "stream");
    EXPECT_EQ(shown["args"]["url"].asString(), "");
    EXPECT_EQ(shown["state"].asString(), "sent");
    EXPECT_TRUE(shown["result"].isNull());
    EXPECT_EQ(f.svc.status()["commands"].size(), localCommands().size());
}

TEST(SleepBridgeCommands, AnAwayBridgeGetsItOnItsNextHello) {
    Fixture f;
    Wire w1;
    auto c1 = f.session(w1);
    f.svc.closed(*c1);
    const auto a = f.svc.queueCommand(kSender, cmd(R"({"cmd":"restate"})"));
    ASSERT_EQ(a.status, 200);
    EXPECT_EQ(a.body["command"]["state"].asString(), "queued");
    EXPECT_TRUE(w1.frames->empty());

    Wire w2;
    auto c2 = f.svc.open({kSender, "", ""}, w2.outlet());
    EXPECT_TRUE(w2.frames->empty()) << "nothing before the hello";
    f.svc.apply(kSender, c2->onText(R"({"t":"hello","state":{}})"));
    ASSERT_EQ(w2.frames->size(), 1u);
    EXPECT_EQ(w2.frames->at(0)["cmd"].asString(), "restate");
    EXPECT_EQ(f.command(a.body["req_id"].asUInt64())["state"].asString(), "sent");
}

TEST(SleepBridgeCommands, ATestHelloNeverReceivesACommand) {
    Fixture f;
    Wire w;
    auto real = f.session(w);
    f.svc.closed(*real);
    Wire pw;
    auto probe = f.svc.open({kSender, "", ""}, pw.outlet());
    f.svc.apply(kSender, probe->onText(R"({"t":"hello","state":{"probe":true}})"));
    const auto a = f.svc.queueCommand(kSender, cmd(R"({"cmd":"reboot"})"));
    EXPECT_EQ(a.body["command"]["state"].asString(), "queued");
    EXPECT_TRUE(pw.frames->empty());
    EXPECT_FALSE(f.bridge()["online"].asBool());
}

// A stream switch closes the session to test the new address, then answers
// on the session it comes back on.
TEST(SleepBridgeCommands, TheAnswerMayComeOnALaterSession) {
    Fixture f;
    Wire w1;
    auto c1 = f.session(w1);
    const auto a = f.svc.queueCommand(kSender, cmd(R"({"cmd":"stream","url":"192.168.1.20"})"));
    const uint64_t req = a.body["req_id"].asUInt64();
    ASSERT_EQ(w1.frames->size(), 1u);
    EXPECT_EQ(w1.frames->at(0)["url"].asString(), "192.168.1.20");
    f.svc.closed(*c1);

    Wire w2;
    auto c2 = f.session(w2);
    EXPECT_TRUE(w2.frames->empty()) << "a sent command is not sent again";
    std::ostringstream res;
    res << R"({"t":"result","req_id":)" << req
        << R"(,"result":{"ok":true,"url":"ws://192.168.1.20:8894/sleepbridge/v1/device","changed":true}})";
    f.svc.apply(kSender, c2->onText(res.str()));
    const auto shown = f.command(req);
    EXPECT_EQ(shown["state"].asString(), "answered");
    EXPECT_TRUE(shown["result"]["ok"].asBool());
    EXPECT_TRUE(shown["result"]["changed"].asBool());
    EXPECT_GT(shown["answered_at"].asInt64(), 0);
}

TEST(SleepBridgeCommands, ALogPullsTextComesAheadOfItsResult) {
    Fixture f;
    Wire w;
    auto c = f.session(w);
    const auto a = f.svc.queueCommand(kSender, cmd(R"({"cmd":"logs","n":50})"));
    const uint64_t req = a.body["req_id"].asUInt64();
    EXPECT_EQ(w.frames->at(0)["n"].asInt(), 50);
    auto chunk = [&](uint32_t seq, bool last, const std::string& text) {
        FrameHeader h;
        h.req_id = static_cast<uint32_t>(req);
        h.seq    = seq;
        h.flags  = last ? 1 : 0;
        h.kind   = kKindLogText;
        f.svc.apply(kSender, c->onBinary(encodeFrameHeader(h) + text));
    };
    chunk(0, false, "I (1) boot\n");
    chunk(1, true, "W (2) stream: now the cloud\n");
    std::ostringstream res;
    res << R"({"t":"result","req_id":)" << req << R"(,"result":{"ok":true}})";
    f.svc.apply(kSender, c->onText(res.str()));
    const auto shown = f.command(req);
    EXPECT_EQ(shown["logs"].asString(), "I (1) boot\nW (2) stream: now the cloud\n");
    EXPECT_FALSE(shown["logs_cut"].asBool());
    EXPECT_EQ(shown["state"].asString(), "answered");
    EXPECT_EQ(f.svc.queueCommand(kSender, cmd(R"({"cmd":"logs"})")).body["command"]["args"]["n"].asInt(), 200);
}

TEST(SleepBridgeCommands, OnlyWhatABridgeTakesIsSent) {
    Fixture f;
    Wire w;
    auto c = f.session(w);
    EXPECT_EQ(f.svc.queueCommand(kSender, cmd(R"({"cmd":"ota","url":"x"})")).status, 400);
    EXPECT_EQ(f.svc.queueCommand(kSender, cmd(R"({"cmd":"o2_list"})")).status, 400);
    EXPECT_EQ(f.svc.queueCommand(kSender, cmd(R"({"cmd":"stream"})")).status, 400);
    EXPECT_EQ(f.svc.queueCommand(kSender, cmd(R"({"cmd":"stream","url":"a b"})")).status, 400);
    EXPECT_EQ(f.svc.queueCommand(kSender, cmd(R"({"cmd":"logs","n":0})")).status, 400);
    EXPECT_EQ(f.svc.queueCommand(kSender, cmd(R"({"cmd":"logs","n":"9"})")).status, 400);
    EXPECT_EQ(f.svc.queueCommand(kSender, cmd(R"({"nope":1})")).status, 400);
    const auto none = f.svc.queueCommand("TAP-S3-00000999", cmd(R"({"cmd":"reboot"})"));
    EXPECT_EQ(none.status, 404);
    EXPECT_FALSE(none.body["ok"].asBool());
    EXPECT_TRUE(w.frames->empty());
    for (const auto& n : localCommands()) {
        Json::Value r(Json::objectValue);
        r["cmd"] = n;
        if (n == "stream") r["url"] = "";
        EXPECT_EQ(f.svc.queueCommand(kSender, r).status, 200) << n;
    }
    EXPECT_EQ(w.frames->size(), localCommands().size());
}

// A bridge always connects to ws://<host>:8894/sleepbridge/v1/device, so a
// stream command names the host and nothing else.
TEST(SleepBridgeCommands, AStreamUrlIsAnIpOrHostnameOnly) {
    for (const char* ok : {"", "192.168.1.20", "homeassistant.local", "nas-2", "HA.lan"})
        EXPECT_TRUE(validBridgeHost(ok)) << ok;
    for (const char* bad : {"192.168.1.20:8894", "ws://192.168.1.20", "192.168.1.20/sleepbridge/v1/device",
                            "host name", ".lan", "lan.", "-a", "a..b", "[fe80::1]"})
        EXPECT_FALSE(validBridgeHost(bad)) << bad;
    EXPECT_FALSE(validBridgeHost(std::string(254, 'a')));
    // The port is its own field.
    EXPECT_TRUE(validBridgePort(8894));
    EXPECT_TRUE(validBridgePort(1));
    EXPECT_TRUE(validBridgePort(65535));
    EXPECT_FALSE(validBridgePort(0));
    EXPECT_FALSE(validBridgePort(65536));

    Fixture f;
    Wire w;
    auto c = f.session(w);
    EXPECT_EQ(f.svc.queueCommand(kSender, cmd(R"({"cmd":"stream","url":"192.168.1.20:8894"})")).status, 400);
    EXPECT_EQ(f.svc.queueCommand(kSender, cmd(R"({"cmd":"stream","url":"ws://192.168.1.20"})")).status, 400);
    EXPECT_EQ(f.svc.queueCommand(kSender, cmd(R"({"cmd":"stream","url":"192.168.1.20","port":0})")).status, 400);
    EXPECT_EQ(f.svc.queueCommand(kSender, cmd(R"({"cmd":"stream","url":"192.168.1.20","port":70000})")).status, 400);
    EXPECT_EQ(f.svc.queueCommand(kSender, cmd(R"({"cmd":"stream","url":"192.168.1.20","port":"8894"})")).status, 400);
    EXPECT_TRUE(w.frames->empty());
    // With a port, the frame carries it; without, the bridge uses 8894.
    EXPECT_EQ(f.svc.queueCommand(kSender, cmd(R"({"cmd":"stream","url":"192.168.1.20","port":9000})")).status, 200);
    {
        const auto& sent = w.frames->back();
        EXPECT_EQ(sent["url"].asString(), "192.168.1.20");
        EXPECT_EQ(sent["port"].asInt(), 9000);
    }
    EXPECT_EQ(f.svc.queueCommand(kSender, cmd(R"({"cmd":"stream","url":"homeassistant.local"})")).status, 200);
}

// The owner's call: a command goes down while batches are arriving.
TEST(SleepBridgeCommands, NoRefusalWhileTheMachineRecords) {
    Fixture f;
    Wire w;
    auto c = f.session(w);
    f.svc.apply(kSender, c->onBinary(frame(0, run(0, 3, 100))));
    const auto a = f.svc.queueCommand(kSender, cmd(R"({"cmd":"config_reset"})"));
    EXPECT_EQ(a.status, 200);
    EXPECT_EQ(a.body["command"]["state"].asString(), "sent");
    EXPECT_EQ(w.frames->back()["cmd"].asString(), "config_reset");
}

TEST(SleepBridgeCommands, ARebootIsNeverAnsweredAndTheListIsBounded) {
    Fixture f;
    Wire w;
    auto c = f.session(w);
    const auto a = f.svc.queueCommand(kSender, cmd(R"({"cmd":"reboot"})"));
    EXPECT_FALSE(a.body["command"]["answered_by_bridge"].asBool());
    for (size_t i = 0; i < kCommandsKept + 5; i++) f.svc.queueCommand(kSender, cmd(R"({"cmd":"restate"})"));
    const auto list = f.bridge()["commands"];
    EXPECT_EQ(list.size(), kCommandsKept);
    EXPECT_GT(list[0]["req_id"].asUInt64(), list[1]["req_id"].asUInt64()) << "newest first";
    // A result for a command no longer kept is ignored, not misfiled.
    std::ostringstream res;
    res << R"({"t":"result","req_id":)" << a.body["req_id"].asUInt64() << R"(,"result":{"ok":true}})";
    f.svc.apply(kSender, c->onText(res.str()));
    const Json::Value after = f.bridge()["commands"];
    for (const auto& k : after) EXPECT_EQ(k["state"].asString(), "sent");
}

TEST(SleepBridgeCommands, ACommandWhoseConnectionWentWaitsForTheNext) {
    Fixture f;
    Wire w;
    auto c = f.session(w);
    *w.up = false;   // gone, not yet seen closed
    const auto a = f.svc.queueCommand(kSender, cmd(R"({"cmd":"restate"})"));
    EXPECT_EQ(a.body["command"]["state"].asString(), "queued");
    f.svc.closed(*c);
    Wire w2;
    auto c2 = f.session(w2);
    ASSERT_EQ(w2.frames->size(), 1u);
    EXPECT_EQ(w2.frames->at(0)["req_id"].asUInt64(), a.body["req_id"].asUInt64());
}
