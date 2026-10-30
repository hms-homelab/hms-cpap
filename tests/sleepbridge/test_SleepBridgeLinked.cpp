// The receiver, the bank and the worker over the LINKED Sleep Bridge
// component (BUILD_WITH_SLEEPBRIDGE=ON), not the fake: a small card written
// by hand and streamed as a bridge streams it, frame by frame through the
// conversation, rebuilt by the component, read back as a card folder.
//
// The card: FAT32, 8 sectors a cluster, 100 reserved, two FATs of 450, so the
// FATs at 100 and 550 and data at 1000, the root in cluster 2. The root holds
// DATALOG (cluster 3), which marks it a ResMed card's root, and README.TXT
// (cluster 5, 1000 bytes). No recorded data from any machine.

#include "SleepBridgeTestUtil.h"

#include "clients/LocalDataSource.h"
#include "sleepbridge/Conversation.h"
#include "sleepbridge/SleepBridgeService.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <cstring>
#include <sstream>

#ifdef HMS_WITH_SLEEPBRIDGE

using namespace sbtest;

namespace {

const std::string  kSender  = "TAP-S3-00000123";
constexpr uint32_t kRootLba = 1000;
constexpr uint32_t kFileLba = 1000 + (5 - 2) * 8;   // cluster 5
constexpr uint32_t kFileLen = 1000;

void le16(std::string& s, size_t at, uint32_t v) {
    s[at]     = static_cast<char>(v & 0xFF);
    s[at + 1] = static_cast<char>((v >> 8) & 0xFF);
}
void le32(std::string& s, size_t at, uint32_t v) {
    for (int i = 0; i < 4; i++) s[at + i] = static_cast<char>((v >> (8 * i)) & 0xFF);
}

std::string bootSector() {
    std::string s(kSectorSize, '\0');
    le16(s, 11, 512);
    s[13] = 8;
    le16(s, 14, 100);
    s[16] = 2;
    le32(s, 32, 1000 + 8 * 4000);
    le32(s, 36, 450);
    le32(s, 44, 2);
    std::memcpy(&s[82], "FAT32   ", 8);
    s[510] = static_cast<char>(0x55);
    s[511] = static_cast<char>(0xAA);
    return s;
}

std::string fatPage() {
    std::string s(kSectorSize, '\0');
    le32(s, 0 * 4, 0x0FFFFFF8);
    le32(s, 1 * 4, 0x0FFFFFFF);
    le32(s, 2 * 4, 0x0FFFFFFF);   // the root, one cluster
    le32(s, 3 * 4, 0x0FFFFFFF);   // DATALOG, one cluster
    le32(s, 5 * 4, 0x0FFFFFFF);   // the file, one cluster
    return s;
}

void dirEntry(std::string& s, size_t at, const char* name11, uint8_t attr, uint32_t cluster, uint32_t size) {
    std::memcpy(&s[at], name11, 11);
    s[at + 11] = static_cast<char>(attr);
    le16(s, at + 20, cluster >> 16);
    le16(s, at + 26, cluster & 0xFFFF);
    le32(s, at + 28, size);
}

std::string rootSector() {
    std::string s(kSectorSize, '\0');
    dirEntry(s, 0, "README  TXT", 0x20, 5, kFileLen);
    dirEntry(s, 32, "DATALOG    ", 0x10, 3, 0);
    return s;
}

/// One record batch of explicit sectors, sequences first, first+1, ...
std::string records(const char* magic, uint64_t first,
                    const std::vector<std::pair<uint32_t, std::string>>& recs) {
    std::string s = header(magic, static_cast<uint16_t>(recs.size()), first);
    for (const auto& [lba, bytes] : recs) {
        put32(s, lba);
        s += bytes;
    }
    return s;
}

uint64_t seqOf(uint32_t session, uint32_t n) { return (static_cast<uint64_t>(session) << 32) | n; }

RebuilderFactory linkedFactory() {
    return [](const std::string& saved) { return makeRebuilder(saved, nullptr); };
}

Json::Value parse(const std::string& text) {
    Json::Value v;
    Json::CharReaderBuilder rb;
    std::istringstream ss(text);
    Json::parseFromStream(rb, ss, &v, nullptr);
    return v;
}

/// The mount's boot sector read, then the writes of one save: both FATs, the
/// root, and the file's two sectors.
void streamCard(SleepBridgeService& svc, Conversation& c, uint32_t session, char fill) {
    const std::string body(kSectorSize, fill);
    uint32_t fseq = 0;
    svc.apply(kSender, c.onBinary(frame(fseq++, records("CYR1", seqOf(session, 1), {{0, bootSector()}}))));
    const auto e = c.onBinary(frame(fseq++, records("CYS1", seqOf(session, 2),
                                                    {{100, fatPage()},
                                                     {550, fatPage()},
                                                     {kRootLba, rootSector()},
                                                     {kFileLba, body},
                                                     {kFileLba + 1, body}})));
    svc.apply(kSender, e);
    const auto ack = parse(e.texts.at(0));
    EXPECT_EQ(ack["status"].asInt(), 200);
    EXPECT_EQ(ack["ack"].asUInt64(), seqOf(session, 6));
}

}  // namespace

TEST(SleepBridgeLinked, TheComponentIsInThisBuild) {
    EXPECT_TRUE(rebuilderInBuild());
    EXPECT_FALSE(rebuilderVersion().empty());
    EXPECT_NE(makeRebuilder("", nullptr), nullptr);
}

TEST(SleepBridgeLinked, AStreamedCardIsRebuiltIntoACardFolder) {
    TempDir t;
    SleepBridgeService svc({t.path.string(), kDefaultPort, 0.01}, linkedFactory());
    ASSERT_TRUE(svc.available());

    auto c = svc.open({kSender, "airsense-bench", "A1B2C3D4E5F6"});
    const auto hello = c->onText(R"({"t":"hello","state":{"fw_version":"1.0.0"}})");
    svc.apply(kSender, hello);
    ASSERT_EQ(parse(hello.texts.at(0))["t"].asString(), "ack");
    streamCard(svc, *c, 1, 'A');
    svc.closed(kSender);
    svc.rebuildNow(kSender);

    const fs::path root = t.path / kSender;
    const std::string got = slurp(root / "README.TXT");
    ASSERT_EQ(got.size(), kFileLen);
    EXPECT_EQ(got, std::string(kFileLen, 'A'));
    EXPECT_FALSE(slurp(root / "batches/rebuild.state").empty());

    const auto b = svc.status()["bridges"][0];
    EXPECT_EQ(b["sender"].asString(), kSender);
    EXPECT_EQ(b["card_root"].asString(), root.string());

    // The folder reads as a card: the banked batches are not a night folder.
    hms_cpap::LocalDataSource src(root.string());
    const auto days = src.listDateFolders();
    EXPECT_EQ(std::find(days.begin(), days.end(), "batches"), days.end());
}

TEST(SleepBridgeLinked, ARestartReplaysTheBankAndARewriteLands) {
    TempDir t;
    const fs::path file = t.path / kSender / "README.TXT";
    {
        SleepBridgeService svc({t.path.string(), kDefaultPort, 0.01}, linkedFactory());
        auto c = svc.open({kSender, "", ""});
        svc.apply(kSender, c->onText(R"({"t":"hello","state":{}})"));
        streamCard(svc, *c, 1, 'A');
        svc.closed(kSender);
        svc.rebuildNow(kSender);
        ASSERT_EQ(slurp(file), std::string(kFileLen, 'A'));
    }
    // A new process over the same folder: built from the saved state, fed the
    // whole bank again, and the next boot's save of the same card is taken.
    SleepBridgeService svc({t.path.string(), kDefaultPort, 0.01}, linkedFactory());
    auto c = svc.open({kSender, "", ""});
    svc.apply(kSender, c->onText(R"({"t":"hello","state":{}})"));
    streamCard(svc, *c, 2, 'B');
    svc.closed(kSender);
    svc.rebuildNow(kSender);
    EXPECT_EQ(slurp(file), std::string(kFileLen, 'B'));
}

#endif  // HMS_WITH_SLEEPBRIDGE
