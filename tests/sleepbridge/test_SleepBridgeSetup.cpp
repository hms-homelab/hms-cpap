// SDD-073: what the Sleep Bridge setup steps rest on.
//
//   - the card read in this computer (D1): a FAT32 and a FAT16 card image,
//     built here, read into a baseline the upload's own check takes, and the
//     cards it refuses;
//   - "Use this bridge" (D4, D8, D10): the folder and the card format;
//   - the events the steps wait for: a bridge's own hello and its card
//     statement each fire once, a test hello never does;
//   - the QR code of the address (D3).

#include "SleepBridgeTestUtil.h"

#include "sleepbridge/BridgeSource.h"
#include "sleepbridge/CardRead.h"
#include "sleepbridge/Conversation.h"
#include "sleepbridge/LearnedCard.h"
#include "sleepbridge/SleepBridgeService.h"
#include "utils/QrCode.h"

#include <gtest/gtest.h>

#include <cstring>
#include <map>
#include <sstream>
#include <vector>

using namespace sbtest;
using namespace hms_cpap::sleepbridge;

namespace {

Json::Value parse(const std::string& s) {
    Json::Value v;
    Json::CharReaderBuilder rb;
    std::istringstream ss(s);
    Json::parseFromStream(rb, ss, &v, nullptr);
    return v;
}

void put16(std::string& s, size_t at, uint32_t v) {
    s[at]     = static_cast<char>(v & 0xFF);
    s[at + 1] = static_cast<char>((v >> 8) & 0xFF);
}
void put32(std::string& s, size_t at, uint32_t v) {
    put16(s, at, v & 0xFFFF);
    put16(s, at + 2, v >> 16);
}

/// A card image, sparse: only the sectors written are there, every other
/// reads as zeros. FAT32 (or FAT16) behind a partition table at `part`.
struct Card {
    std::map<uint32_t, std::string> sectors;
    uint32_t part{2048}, spc{8}, reserved{32}, fats{2}, fatsz{0}, total{0}, root_ent{0};
    bool     fat32{true};
    uint32_t reads{0};

    std::string& sec(uint32_t lba) {
        auto& s = sectors[lba];
        if (s.empty()) s.assign(kSectorSize, '\0');
        return s;
    }
    uint32_t fatBegin() const { return part + reserved; }
    uint32_t rootSectors() const { return (root_ent * 32 + 511) / 512; }
    uint32_t dataBegin() const { return part + reserved + fats * fatsz + rootSectors(); }
    uint32_t clusterLba(uint32_t c) const { return dataBegin() + (c - 2) * spc; }

    void setFat(uint32_t c, uint32_t v) {
        const size_t eb = fat32 ? 4 : 2;
        const size_t at = c * eb;
        auto& s = sec(fatBegin() + static_cast<uint32_t>(at / 512));
        if (fat32) put32(s, at % 512, v);
        else put16(s, at % 512, v);
    }

    void dirEntry(uint32_t lba, int index, const char name11[12], uint8_t attr, uint32_t cluster) {
        auto& s = sec(lba);
        const size_t at = static_cast<size_t>(index) * 32;
        std::memcpy(&s[at], name11, 11);
        s[at + 11] = static_cast<char>(attr);
        put16(s, at + 20, cluster >> 16);
        put16(s, at + 26, cluster & 0xFFFF);
    }

    ReadSectors reader() {
        return [this](uint32_t lba, uint32_t count, std::string& out) {
            out.clear();
            for (uint32_t i = 0; i < count; i++) {
                reads++;
                auto it = sectors.find(lba + i);
                out += it == sectors.end() ? std::string(kSectorSize, '\0') : it->second;
            }
            return true;
        };
    }
};

/// A ResMed-shaped card: the root holds DATALOG and SETTINGS, DATALOG holds
/// two nights.
Card fat32Card() {
    Card c;
    c.total = 15523840 - c.part;
    c.fatsz = 15200;   // 1,945,600 entries: every cluster and the two marks
    auto& mbr = c.sec(0);
    mbr[446 + 4] = 0x0C;
    put32(mbr, 446 + 8, c.part);
    put32(mbr, 446 + 12, c.total);
    mbr[510] = 0x55;
    mbr[511] = static_cast<char>(0xAA);

    auto& b = c.sec(c.part);
    b[0] = static_cast<char>(0xEB);
    std::memcpy(&b[3], "MSDOS5.0", 8);
    put16(b, 11, 512);
    b[13] = static_cast<char>(c.spc);
    put16(b, 14, c.reserved);
    b[16] = static_cast<char>(c.fats);
    put32(b, 32, c.total);
    put32(b, 36, c.fatsz);
    put32(b, 44, 2);    // root cluster
    put16(b, 48, 1);    // FSInfo
    put16(b, 50, 6);    // backup boot
    put32(b, 67, 0x1234ABCD);
    b[510] = 0x55;
    b[511] = static_cast<char>(0xAA);
    c.sec(c.part + 1)[0] = 'R';   // FSInfo
    c.sec(c.part + 6) = b;        // backup boot

    // FAT: media and end marks, the root (2), DATALOG (3, two clusters: 3,4),
    // SETTINGS (5), two nights (6, 7), one file (8).
    c.setFat(0, 0x0FFFFFF8);
    c.setFat(1, 0x0FFFFFFF);
    c.setFat(2, 0x0FFFFFFF);
    c.setFat(3, 4);
    c.setFat(4, 0x0FFFFFFF);
    c.setFat(5, 0x0FFFFFFF);
    c.setFat(6, 0x0FFFFFFF);
    c.setFat(7, 0x0FFFFFFF);
    c.setFat(8, 0x0FFFFFFF);

    const uint32_t root = c.clusterLba(2);
    c.dirEntry(root, 0, "STR     EDF", 0x20, 8);
    c.dirEntry(root, 1, "DATALOG    ", 0x10, 3);
    c.dirEntry(root, 2, "SETTINGS   ", 0x10, 5);
    const uint32_t dl = c.clusterLba(3);
    c.dirEntry(dl, 0, ".          ", 0x10, 3);
    c.dirEntry(dl, 1, "..         ", 0x10, 0);
    c.dirEntry(dl, 2, "20261101   ", 0x10, 6);
    // The rest of DATALOG's first cluster: deleted entries, so the folder
    // runs on into its second cluster.
    for (uint32_t s = 0; s < c.spc; s++)
        for (int i = s == 0 ? 3 : 0; i < 16; i++) c.sec(dl + s)[static_cast<size_t>(i) * 32] = static_cast<char>(0xE5);
    c.dirEntry(c.clusterLba(4), 0, "20261102   ", 0x10, 7);   // DATALOG's second cluster
    c.dirEntry(c.clusterLba(6), 0, "20261101_EVE", 0x20, 0);
    return c;
}

}  // namespace

// ── D1: the card read ─────────────────────────────────────────────────────

TEST(SleepBridgeSetup, AFat32CardReadsIntoABaselineTheUploadTakes) {
    Card c = fat32Card();
    std::string why;
    Json::Value labels(Json::objectValue);
    labels["brand"] = "resmed";
    std::vector<std::pair<size_t, size_t>> told;
    const auto base = readCardBaseline(c.reader(), &why, labels,
                                       [&](size_t d, size_t t) { told.emplace_back(d, t); });
    ASSERT_TRUE(base) << why;
    const auto& st = base->statement;
    EXPECT_EQ(st["fat"].asString(), "fat32");
    EXPECT_EQ(st["partition_lba"].asUInt(), 2048u);
    EXPECT_EQ(st["fat_begin_lba"].asUInt(), 2048u + 32u);
    EXPECT_EQ(st["data_begin_lba"].asUInt(), 2048u + 32u + 2u * 15200u);
    EXPECT_EQ(st["root_cluster"].asUInt(), 2u);
    EXPECT_EQ(st["volume_serial"].asString(), "1234ABCD");
    EXPECT_EQ(st["brand"].asString(), "resmed");
    EXPECT_EQ(st["nights"].asInt(), 2) << "both of DATALOG's clusters were walked";
    EXPECT_EQ(st["root_dir_lbas"].size(), 8u) << "the root's one cluster, every sector";

    // What the upload checks, it passes.
    const std::string cbl1 = encodeCbl1(st, base->sectors);
    const auto learned = decodeLearnedCard(cbl1, &why);
    ASSERT_TRUE(learned) << why;
    EXPECT_EQ(learned->folders.size(), 2u);

    // The metadata only: the partition table, the boot sector with its
    // FSInfo and backup, one FAT sector (clusters up to 8), and the folders'
    // clusters (root, DATALOG x2, SETTINGS, two nights): never a file's.
    std::map<uint32_t, bool> has;
    for (const auto& [lba, s] : base->sectors) has[lba] = true;
    EXPECT_TRUE(has[0]);
    EXPECT_TRUE(has[2048] && has[2049] && has[2054]);
    EXPECT_TRUE(has[c.fatBegin()]);
    EXPECT_FALSE(has[c.fatBegin() + 1]) << "FAT past the highest cluster in use is not sent";
    EXPECT_FALSE(has[c.fatBegin() + 15200]) << "FAT #2 is a copy";
    EXPECT_TRUE(has[c.clusterLba(4)]);
    EXPECT_FALSE(has[c.clusterLba(8)]) << "a file's cluster (STR.edf) is a night's data";
    EXPECT_EQ(base->sectors.size(), 4u + 1u + 6u * 8u);
    ASSERT_FALSE(told.empty());
    EXPECT_EQ(told.back().first, told.back().second) << "the last word says all of it was read";
}

TEST(SleepBridgeSetup, AFat16CardWithoutAPartitionTableReads) {
    Card c;
    c.part = 0;
    c.fat32 = false;
    c.spc = 4;
    c.reserved = 1;
    c.fatsz = 200;
    c.root_ent = 512;
    c.total = 200000;   // 49,891 clusters: FAT16
    auto& b = c.sec(0);
    b[0] = static_cast<char>(0xEB);
    put16(b, 11, 512);
    b[13] = static_cast<char>(c.spc);
    put16(b, 14, c.reserved);
    b[16] = static_cast<char>(c.fats);
    put16(b, 17, c.root_ent);
    put16(b, 22, c.fatsz);
    put32(b, 32, c.total);
    put32(b, 39, 0xCAFE0001);
    b[510] = 0x55;
    b[511] = static_cast<char>(0xAA);
    c.setFat(0, 0xFFF8);
    c.setFat(1, 0xFFFF);
    c.setFat(2, 0xFFFF);
    const uint32_t root = c.fatBegin() + c.fats * c.fatsz;
    c.dirEntry(root, 0, "DATALOG    ", 0x10, 2);

    std::string why;
    const auto base = readCardBaseline(c.reader(), &why);
    ASSERT_TRUE(base) << why;
    EXPECT_EQ(base->statement["fat"].asString(), "fat16");
    EXPECT_EQ(base->statement["root_cluster"].asUInt(), 0u);
    EXPECT_EQ(base->statement["root_dir_lbas"].size(), 32u);
    EXPECT_EQ(base->statement["volume_serial"].asString(), "CAFE0001");
    EXPECT_TRUE(decodeLearnedCard(encodeCbl1(base->statement, base->sectors), &why)) << why;
}

TEST(SleepBridgeSetup, ACardThatIsNotFatIsRefusedInPlainWords) {
    Card blank;
    std::string why;
    EXPECT_FALSE(readCardBaseline(blank.reader(), &why));
    EXPECT_EQ(why, "This card does not hold a FAT volume.");

    Card ex = fat32Card();
    std::memcpy(&ex.sec(ex.part)[3], "EXFAT   ", 8);
    ex.sec(ex.part)[0] = static_cast<char>(0xEB);
    put16(ex.sec(ex.part), 11, 0);   // exFAT keeps no BPB
    EXPECT_FALSE(readCardBaseline(ex.reader(), &why));
    EXPECT_NE(why.find("exFAT"), std::string::npos) << why;

    Card broken = fat32Card();
    auto failing = [&](uint32_t lba, uint32_t n, std::string& out) {
        if (lba >= broken.fatBegin()) return false;
        return broken.reader()(lba, n, out);
    };
    EXPECT_FALSE(readCardBaseline(failing, &why));
    EXPECT_EQ(why, "The card's file table could not be read.");
}

TEST(SleepBridgeSetup, AVolumeIsReadFromItsWholeDisk) {
    EXPECT_EQ(wholeDiskFor("/dev/disk4s1"), "/dev/rdisk4");
    EXPECT_EQ(wholeDiskFor("/dev/rdisk12s2"), "/dev/rdisk12");
    EXPECT_EQ(wholeDiskFor("/dev/disk4"), "/dev/rdisk4");
    EXPECT_EQ(wholeDiskFor("/dev/sdb1"), "/dev/sdb");
    EXPECT_EQ(wholeDiskFor("/dev/mmcblk0p1"), "/dev/mmcblk0");
    EXPECT_EQ(wholeDiskFor("E:"), "E:");
}

TEST(SleepBridgeSetup, TheComputersOwnFilesOnACardAreLeftOut) {
    EXPECT_TRUE(cardFileToKeep("STR.edf"));
    EXPECT_TRUE(cardFileToKeep("DATALOG/20261101/20261101_221500_EVE.edf"));
    EXPECT_TRUE(cardFileToKeep("Identification.tgt"));
    EXPECT_FALSE(cardFileToKeep(".Spotlight-V100/Store-V2/x"));
    EXPECT_FALSE(cardFileToKeep(".fseventsd/0000"));
    EXPECT_FALSE(cardFileToKeep("DATALOG/._20261101"));
    EXPECT_FALSE(cardFileToKeep("System Volume Information/IndexerVolumeGuid"));
    EXPECT_FALSE(cardFileToKeep("$RECYCLE.BIN/desktop.ini"));
    EXPECT_FALSE(cardFileToKeep(""));
}

// ── D4, D8, D10: "Use this bridge as my source" ───────────────────────────

TEST(SleepBridgeSetup, UseThisBridgeSetsItsFolderAndItsCardsFormat) {
    Json::Value status = parse(R"({"bridges":[
        {"sender":"TAP-S3-00000041","card_root":"/d/sleepbridge/TAP-S3-00000041","card_format":"bmc"},
        {"sender":"TAP-S3-00000042","card_root":"/d/sleepbridge/TAP-S3-00000042","card_format":"",
         "learned":{"brand":"ResMed"}},
        {"sender":"TAP-S3-00000043","card_root":"/d/sleepbridge/TAP-S3-00000043","card_format":""},
        {"sender":"TAP-S3-00000044","card_root":""}],
        "learned_card":null})");
    std::string why;
    auto a = bridgeSource(status, "TAP-S3-00000041", &why);
    ASSERT_TRUE(a);
    EXPECT_EQ(a->local_dir, "/d/sleepbridge/TAP-S3-00000041");
    EXPECT_EQ(a->format, "bmc");
    EXPECT_TRUE(a->format_known);

    a = bridgeSource(status, "TAP-S3-00000042", &why);
    ASSERT_TRUE(a);
    EXPECT_EQ(a->format, "resmed") << "the learned card names the brand before the rebuild does";
    EXPECT_TRUE(a->format_known);

    a = bridgeSource(status, "TAP-S3-00000043", &why);
    ASSERT_TRUE(a);
    EXPECT_EQ(a->format, "resmed") << "with nothing to go on, this build's default format";
    EXPECT_FALSE(a->format_known);

    EXPECT_FALSE(bridgeSource(status, "TAP-S3-00000044", &why));
    EXPECT_EQ(why, "This bridge has no folder yet.");
    EXPECT_FALSE(bridgeSource(status, "TAP-S3-00000099", &why));
    EXPECT_FALSE(bridgeSource(Json::Value(), "TAP-S3-00000041", &why));

    EXPECT_EQ(formatForBrand("Luna G3"), "bmc");
    EXPECT_EQ(formatForBrand("bmc"), "bmc");
    // A card learned on a computer names "sefam" for an S.Box card (the
    // upload page's own name for it).
    EXPECT_EQ(formatForBrand("sefam"), "sefam");
    EXPECT_EQ(formatForBrand("lowenstein"), "");
}

// A Sefam card rebuilt by a bridge in an S.Box: the rebuild names ResMed and
// Luna cards only, so the folder is read by its files, as a Philips card's is,
// and never as the ResMed default.
TEST(SleepBridgeSetup, UseThisBridgeReadsASefamCardInItsFolderAsSefam) {
    TempDir t;
    // A manifest the Sefam reader accepts (test_SefamIngestion's), invented serial.
    const auto session = t.path / "1263R00000000" / "DATA_1";
    std::filesystem::create_directories(session);
    std::ofstream(session / "DATA_1.INI", std::ios::binary)
        << "[Create Info]\r\nCreated By=S.Box_AUTO \r\nSerial Number=1263R00000000\r\n"
           "Version=VER :A020400\r\nDate=10/11/25 22:25:16\r\n[Start Record]\r\nHour=22\r\n"
           "Min=25\r\nSec=16\r\nDay=10\r\nMonth=11\r\nYear=2025\r\n"
           "Programmed Record Duration=28800\r\nReal Record Duration=28800\r\n"
           "[Chan0]\r\nName=FLW\r\nDescription=NO\r\nType=4\r\nUnit=lpm\r\nMin=-180\r\n"
           "Max=280\r\nFreq=25\r\nBit=8\r\n";
    Json::Value status(Json::objectValue);
    Json::Value b(Json::objectValue);
    b["sender"]      = "TAP-S3-00000045";
    b["card_root"]   = t.path.string();
    b["card_format"] = "";
    status["bridges"].append(b);
    status["learned_card"] = Json::Value(Json::nullValue);

    std::string why;
    const auto a = bridgeSource(status, "TAP-S3-00000045", &why);
    ASSERT_TRUE(a) << why;
    EXPECT_EQ(a->format, "sefam");
    EXPECT_TRUE(a->format_known);
    EXPECT_TRUE(holdsSefamCard(t.path.string()));
    EXPECT_FALSE(holdsSefamCard((t.path / "nothing").string()));
}

// ── The events the steps wait for ─────────────────────────────────────────

TEST(SleepBridgeSetup, AHelloAndACardStatementEachFireOnceAndATestHelloNever) {
    TempDir t;
    std::vector<std::pair<std::string, std::string>> events;
    SleepBridgeService::Options o{t.path.string(), kDefaultPort, 0.01};
    o.on_event = [&](const std::string& e, const std::string& s) { events.emplace_back(e, s); };
    SleepBridgeService svc(o, fakeFactory(std::make_shared<FakeLog>()));
    Admission who{"TAP-S3-00000041", "bench", "A1B2C3D4E5F6"};

    auto probe = svc.open(who);
    svc.apply(who.sender, probe->onText(R"({"t":"hello","state":{"probe":true}})"));
    svc.closed(*probe);
    EXPECT_TRUE(events.empty()) << "the setup page's test is not the bridge arriving";

    auto c = svc.open(who);
    svc.apply(who.sender, c->onText(R"({"t":"hello","state":{"fw_version":"1.0.1"}})"));
    ASSERT_EQ(events.size(), 1u);
    EXPECT_EQ(events[0].first, "hello");
    EXPECT_EQ(events[0].second, who.sender);
    Json::Value b = svc.status()["bridges"][0];
    EXPECT_GT(b["session_started_at"].asInt64(), 0);
    EXPECT_EQ(b["card_stated_at"].asInt64(), 0);

    svc.apply(who.sender, c->onText(R"({"t":"state","state":{"fw_version":"1.0.1"}})"));
    EXPECT_EQ(events.size(), 1u) << "a state report is no event";

    svc.apply(who.sender, c->onBinary(frame(19, cardStatement(kCard))));
    ASSERT_EQ(events.size(), 2u);
    EXPECT_EQ(events[1].first, "card");
    b = svc.status()["bridges"][0];
    EXPECT_GT(b["card_stated_at"].asInt64(), 0);

    svc.closed(*c);
    ASSERT_EQ(events.size(), 3u);
    EXPECT_EQ(events[2].first, "closed");
}

TEST(SleepBridgeSetup, TheWatchFramesCarryTheEventAndTheState) {
    Json::Value st = parse(R"({"bridges":[]})");
    const auto f = parse(bridgeWatchFrame("hello", "TAP-S3-00000041", st));
    EXPECT_EQ(f["t"].asString(), "sleepbridge");
    EXPECT_EQ(f["event"].asString(), "hello");
    EXPECT_EQ(f["sender"].asString(), "TAP-S3-00000041");
    EXPECT_TRUE(f["status"]["bridges"].isArray());
    const auto s = parse(streamWatchFrame("state", parse(R"({"state":"connected"})")));
    EXPECT_EQ(s["t"].asString(), "cpapdash");
    EXPECT_EQ(s["stream"]["state"].asString(), "connected");
}

// ── D3: the QR code ───────────────────────────────────────────────────────

TEST(SleepBridgeSetup, AnAddressFitsTheSmallestQrCode) {
    namespace qr = hms_cpap::qr;
    const auto a = qr::encode("192.168.1.20");
    ASSERT_TRUE(a);
    EXPECT_EQ(a->version, 1);
    EXPECT_EQ(a->size, 21);
    const auto rows = qr::rows(*a);
    ASSERT_EQ(rows.size(), 21u);
    // The three finder patterns' outer rings.
    EXPECT_EQ(rows[0].substr(0, 7), "1111111");
    EXPECT_EQ(rows[0].substr(14, 7), "1111111");
    EXPECT_EQ(rows[20].substr(0, 7), "1111111");
    EXPECT_EQ(rows[1].substr(0, 7), "1000001");
    // The timing pattern and the dark module.
    EXPECT_EQ(rows[6].substr(8, 5), "10101");
    EXPECT_EQ(rows[21 - 8][8], '1');

    EXPECT_EQ(qr::encode("192.168.100.200")->version, 2);
    EXPECT_EQ(qr::encode(std::string(150, 'a'))->version, 8) << "with its version blocks";
    EXPECT_FALSE(qr::encode(""));
    EXPECT_FALSE(qr::encode(std::string(214, 'a')));
}
