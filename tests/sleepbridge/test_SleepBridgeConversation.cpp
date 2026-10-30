// One connection's frames: hello answered with ack, batches with ack_sectors
// echoing the frame's seq, no command ever sent, and no credential read.

#include "SleepBridgeTestUtil.h"

#include "sleepbridge/Conversation.h"
#include "sleepbridge/SleepBridgeService.h"

#include <gtest/gtest.h>

#include <map>
#include <set>
#include <sstream>

using namespace sbtest;

namespace {

Json::Value parse(const std::string& s) {
    Json::Value v;
    Json::CharReaderBuilder rb;
    std::istringstream ss(s);
    Json::parseFromStream(rb, ss, &v, nullptr);
    return v;
}

Admission bridge() {
    Admission a;
    a.sender  = "TAP-S3-00000123";
    a.machine = "airsense-bench";
    a.chip_id = "A1B2C3D4E5F6";
    return a;
}

}  // namespace

TEST(SleepBridgeConversation, HelloIsAnsweredWithAck) {
    TempDir t;
    BatchStore store(t.path.string());
    Conversation c(store, bridge());
    const auto e = c.onText(R"({"t":"hello","state":{"fw_version":"1.0.0","kind":"bridge"}})");
    ASSERT_EQ(e.texts.size(), 1u);
    EXPECT_EQ(parse(e.texts[0])["t"].asString(), "ack");
    EXPECT_TRUE(e.hello);
    ASSERT_TRUE(e.state);
    EXPECT_EQ((*e.state)["fw_version"].asString(), "1.0.0");
}

TEST(SleepBridgeConversation, ThePortalsTestHelloIsAnsweredTheSameWay) {
    TempDir t;
    BatchStore store(t.path.string());
    Conversation c(store, bridge());
    const auto e = c.onText(
        R"({"t":"hello","state":{"fw_version":"1.0.0","kind":"bridge","portal_test":true}})");
    ASSERT_EQ(e.texts.size(), 1u);
    EXPECT_EQ(e.texts[0], R"({"t":"ack"})");
}

TEST(SleepBridgeConversation, StateIsKeptAndNeverAnswered) {
    TempDir t;
    BatchStore store(t.path.string());
    Conversation c(store, bridge());
    const auto e = c.onText(R"({"t":"state","state":{"uptime_s":42}})");
    EXPECT_TRUE(e.texts.empty());
    ASSERT_TRUE(e.state);
    EXPECT_EQ((*e.state)["uptime_s"].asInt(), 42);
}

TEST(SleepBridgeConversation, NoCommandIsEverSent) {
    TempDir t;
    BatchStore store(t.path.string());
    Conversation c(store, bridge());
    std::vector<Effects> all = {
        c.onText(R"({"t":"hello","state":{}})"),
        c.onText(R"({"t":"state","state":{}})"),
        c.onText(R"({"t":"result","req_id":5,"result":{"ok":true}})"),
        c.onBinary(frame(0, run(0, 2))),
        c.onBinary(frame(1, heartbeat())),
        c.onBinary(frame(2, "log text", kKindLogText)),
        c.onBinary(frame(3, "ring", kKindRingFile)),
    };
    for (const auto& e : all)
        for (const auto& t : e.texts) EXPECT_NE(parse(t)["t"].asString(), "cmd") << t;
}

TEST(SleepBridgeConversation, ABatchIsAckedWithItsFrameSeq) {
    TempDir t;
    BatchStore store(t.path.string());
    Conversation c(store, bridge());
    c.onText(R"({"t":"hello","state":{}})");
    const auto e = c.onBinary(frame(17, run(0, 3)));
    ASSERT_EQ(e.texts.size(), 1u);
    const auto a = parse(e.texts[0]);
    EXPECT_EQ(a["t"].asString(), "ack_sectors");
    EXPECT_EQ(a["seq"].asUInt(), 17u);
    EXPECT_EQ(a["status"].asInt(), 200);
    EXPECT_EQ(a["ack"].asInt64(), 2);
    EXPECT_EQ(a["accepted"].asInt64(), 3);
    EXPECT_FALSE(a["gap"].asBool());
    EXPECT_FALSE(a["duplicate"].asBool());
    EXPECT_TRUE(e.banked);
    // The ack came after the bytes landed.
    EXPECT_TRUE(fs::exists(fs::path(store.batchesDir("TAP-S3-00000123")) / "0-2.cys"));

    const auto dup = parse(c.onBinary(frame(18, run(0, 3))).texts.at(0));
    EXPECT_EQ(dup["seq"].asUInt(), 18u);
    EXPECT_TRUE(dup["duplicate"].asBool());
    EXPECT_EQ(dup["ack"].asInt64(), 2);

    const auto card = c.onBinary(frame(19, cardStatement(kCard)));
    EXPECT_TRUE(card.card);
    EXPECT_FALSE(card.banked);
    EXPECT_TRUE(parse(card.texts.at(0))["card"].asBool());
}

TEST(SleepBridgeConversation, BadFramesAreAnsweredWithAnError) {
    TempDir t;
    BatchStore store(t.path.string());
    Conversation c(store, bridge());
    EXPECT_EQ(parse(c.onText("not json").texts.at(0))["t"].asString(), "error");
    EXPECT_EQ(parse(c.onText(R"({"t":"bogus"})").texts.at(0))["t"].asString(), "error");
    EXPECT_EQ(parse(c.onBinary("short").texts.at(0))["t"].asString(), "error");
    const auto bad = parse(c.onBinary(frame(4, "CYS1")).texts.at(0));
    EXPECT_EQ(bad["t"].asString(), "ack_sectors");
    EXPECT_EQ(bad["status"].asInt(), 400);
    EXPECT_TRUE(c.onBinary(frame(5, "x", kKindLogText)).texts.empty());
}

TEST(SleepBridgeConversation, ABridgeWithNoTokenIsAdmitted) {
    const std::map<std::string, std::string> headers = {
        {"X-Coyote-Sender", "TAP-S3-00000123"},
        {"X-Coyote-Machine", "airsense-bench"},
        {"X-Chip-Id", "A1B2C3D4E5F6"},
    };
    std::string why;
    const auto a = admit(
        [&](const std::string& h) {
            const auto it = headers.find(h);
            return it == headers.end() ? std::string() : it->second;
        },
        &why);
    ASSERT_TRUE(a) << why;
    EXPECT_EQ(a->sender, "TAP-S3-00000123");
    EXPECT_EQ(a->machine, "airsense-bench");
    EXPECT_EQ(a->chip_id, "A1B2C3D4E5F6");
}

TEST(SleepBridgeConversation, AnAuthorizationHeaderIsNeverReadStoredOrLogged) {
    const std::string token = "Bearer eyJ.SECRET-DEVICE-TOKEN.sig";
    const std::map<std::string, std::string> headers = {
        {"X-Coyote-Sender", "TAP-S3-00000123"},
        {"X-Chip-Id", "A1B2C3D4E5F6"},
        {"Authorization", token},
    };
    std::set<std::string> asked;
    const auto header = [&](const std::string& h) {
        asked.insert(h);
        const auto it = headers.find(h);
        return it == headers.end() ? std::string() : it->second;
    };
    std::string why;
    const auto a = admit(header, &why);
    ASSERT_TRUE(a);
    EXPECT_EQ(asked.count("Authorization"), 0u);
    EXPECT_EQ(asked.size(), 3u);

    // A whole session through the service: nothing on disk carries the token.
    TempDir t;
    SleepBridgeService svc({t.path.string(), kDefaultPort, 0.01},
                           fakeFactory(std::make_shared<FakeLog>()));
    testing::internal::CaptureStdout();
    auto conv = svc.open(*a);
    svc.apply(a->sender, conv->onText(R"({"t":"hello","state":{}})"));
    svc.apply(a->sender, conv->onBinary(frame(0, run(0, 2))));
    svc.closed(a->sender);
    const std::string out = testing::internal::GetCapturedStdout();
    EXPECT_EQ(out.find("SECRET"), std::string::npos);
    for (const auto& e : fs::recursive_directory_iterator(t.path))
        if (e.is_regular_file())
            EXPECT_EQ(slurp(e.path()).find("SECRET"), std::string::npos) << e.path();
    EXPECT_EQ(svc.status().toStyledString().find("SECRET"), std::string::npos);
}

TEST(SleepBridgeConversation, AdmissionRefusesABadName) {
    std::string why;
    EXPECT_FALSE(admit([](const std::string&) { return std::string(); }, &why));
    EXPECT_FALSE(why.empty());
    EXPECT_FALSE(admit(
        [](const std::string& h) { return h == "X-Coyote-Sender" ? std::string("..") : std::string(); },
        &why));
}
