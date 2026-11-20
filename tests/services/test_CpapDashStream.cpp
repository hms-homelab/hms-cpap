// platform SDD-059: hms-cpap streams the card files it holds to CpapDash.
//
// Two halves. The rules with no network: what is a card file, what of a file
// goes (the growing-file rule, D4), the frames' shape, the install id. Then
// the uploader itself, driven through its real Drogon client against a local
// server that speaks the SDD's frames (FakeCpapDashServer.h), on a real SQLite
// index: acks move the index and nothing else does, a dropped socket resumes
// from the last ack, the switch going off and on clears the index and sends
// everything again, a 401 shows in the status, and CpapDash being unreachable
// changes nothing about what a collector writes.
//
// Every value is made up; no real night, no real account.

#include <gtest/gtest.h>

#include "database/SQLiteDatabase.h"
#include "services/As11NightWriter.h"
#include "services/CardFileFeed.h"
#include "services/CpapDashLink.h"
#include "services/CpapDashStream.h"
#include "services/OximetryOutbox.h"

#include <algorithm>

#ifdef BUILD_WITH_WEB
#include "FakeCpapDashServer.h"
#endif

#include <chrono>
#include <filesystem>
#include <fstream>
#include <memory>
#include <regex>
#include <sstream>
#include <string>
#include <vector>

using namespace hms_cpap;
namespace fs = std::filesystem;

namespace {

// A minimal EDF: a 256-byte fixed header plus one 256-byte signal header, the
// header length at 184 and the record count at 236, then [records] records of
// [record_bytes] bytes, record i filled with byte [fill + i].
std::string edf(int records, int record_bytes = 100, char fill = 'a') {
    std::string h(512, ' ');
    h[0] = '0';
    const std::string hb = "512";
    h.replace(184, hb.size(), hb);
    const std::string n = std::to_string(records);
    h.replace(236, n.size(), n);
    std::string out = h;
    for (int i = 0; i < records; ++i) out += std::string(record_bytes, static_cast<char>(fill + i % 20));
    return out;
}

void writeFile(const fs::path& p, const std::string& bytes) {
    fs::create_directories(p.parent_path());
    std::ofstream out(p, std::ios::binary | std::ios::trunc);
    out << bytes;
}

std::string readFile(const fs::path& p) {
    std::ifstream in(p, std::ios::binary);
    std::ostringstream s;
    s << in.rdbuf();
    return s.str();
}

IDatabase::CpapDashSent ackedRow(const std::string& name, const std::string& bytes) {
    IDatabase::CpapDashSent r;
    r.root = "/r";
    r.path = name;
    r.machine = "23000000001";
    r.acked_to = static_cast<long long>(bytes.size());
    r.acked_md5 = cdstream::ackedMd5(name, bytes, bytes.size());
    return r;
}

}  // namespace

// ── the rules ────────────────────────────────────────────────────────────────

TEST(CpapDashStreamRules, TheOnlyAddressIsTheGatewaysHmsRoute) {
    // Never poll.cpapdash.com, never the device socket's port.
    const CpapDashEndpoint ep;
    EXPECT_TRUE(ep.tls);
    EXPECT_EQ(ep.host, "api.cpapdash.com");
    EXPECT_EQ(ep.port, 443);
    EXPECT_EQ(ep.path, "/socket/v1/hms");
}

TEST(CpapDashStreamRules, NoSourceNamesTheDeviceSocketHostOrPort) {
    // The hard rule pinned on the tree itself: hms-cpap's code never names the
    // host the devices poll, nor the socket port they hold (SDD-059 D2).
    const std::string poll_host = std::string("poll.") + "cpapdash.com";
    const std::string device_port = std::string("414") + "45";
    for (const char* sub : {"src", "include"}) {
        const fs::path dir = fs::path(HMS_CPAP_SOURCE_DIR) / sub;
        for (const auto& e : fs::recursive_directory_iterator(dir)) {
            if (!e.is_regular_file()) continue;
            const auto text = readFile(e.path());
            EXPECT_EQ(text.find(poll_host), std::string::npos) << e.path();
            EXPECT_EQ(text.find(device_port), std::string::npos) << e.path();
        }
    }
}

TEST(CpapDashStreamRules, EdfHeaderLengthIsReadFromTheHeader) {
    EXPECT_EQ(cdstream::edfHeaderBytes(edf(3)), 512u);
    EXPECT_EQ(cdstream::edfHeaderBytes("not an edf"), 0u);
    EXPECT_EQ(cdstream::edfHeaderBytes(std::string(600, 'x')), 0u);
}

TEST(CpapDashStreamRules, ANewFileGoesWhole) {
    const auto p = cdstream::planSend("x_BRP.edf", edf(10), std::nullopt);
    EXPECT_TRUE(p.send);
    EXPECT_TRUE(p.whole);
    EXPECT_EQ(p.offset, 0u);
}

TEST(CpapDashStreamRules, AnAckedFileUnchangedSendsNothing) {
    const auto bytes = edf(10);
    EXPECT_FALSE(cdstream::planSend("x_BRP.edf", bytes, ackedRow("x_BRP.edf", bytes)).send);
    EXPECT_FALSE(cdstream::planSend("STR.edf", bytes, ackedRow("STR.edf", bytes)).send);
}

TEST(CpapDashStreamRules, AGrowingBrpGoesFromTheAckedOffsetThoughItsHeaderChanged) {
    // The Bluetooth BRP rewritten 10 s later: more records, so a new record
    // count in the header, the records before the end the same (D4).
    const auto first = edf(10);
    const auto later = edf(20);
    ASSERT_NE(first.substr(0, 512), later.substr(0, 512));
    ASSERT_EQ(first.substr(512), later.substr(512, first.size() - 512));
    const auto p = cdstream::planSend("x_BRP.edf", later, ackedRow("x_BRP.edf", first));
    EXPECT_TRUE(p.send);
    EXPECT_FALSE(p.whole);
    EXPECT_EQ(p.offset, first.size());
    // PLD, SAD and SA2 grow the same way.
    EXPECT_EQ(cdstream::planSend("x_PLD.edf", later, ackedRow("x_PLD.edf", first)).offset, first.size());
    EXPECT_EQ(cdstream::planSend("x_SA2.edf", later, ackedRow("x_SA2.edf", first)).offset, first.size());
}

TEST(CpapDashStreamRules, AChangedPrefixSendsTheWholeFile) {
    const auto first = edf(10, 100, 'a');
    auto later = edf(20, 100, 'a');
    later[600] = '#';   // a record before the acked end is not what was acked
    const auto p = cdstream::planSend("x_BRP.edf", later, ackedRow("x_BRP.edf", first));
    EXPECT_TRUE(p.send);
    EXPECT_TRUE(p.whole);
    EXPECT_EQ(p.offset, 0u);
}

TEST(CpapDashStreamRules, EveCslAndStrGoWholeWhenTheyChange) {
    // Appended to or not, they go whole: they change before their end.
    const auto first = edf(3);
    const auto later = edf(4);
    for (const char* name : {"x_EVE.edf", "x_CSL.edf", "STR.edf", "Identification.json"}) {
        const auto p = cdstream::planSend(name, later, ackedRow(name, first));
        EXPECT_TRUE(p.send) << name;
        EXPECT_TRUE(p.whole) << name;
        EXPECT_EQ(p.offset, 0u) << name;
    }
}

TEST(CpapDashStreamRules, AShorterFileGoesWhole) {
    const auto p = cdstream::planSend("x_BRP.edf", edf(5), ackedRow("x_BRP.edf", edf(10)));
    EXPECT_TRUE(p.whole);
}

TEST(CpapDashStreamRules, TheIndexKeepsTheMd5AfterAnEdfHeaderAndOfAllOfAnyOtherFile) {
    const auto bytes = edf(4);
    EXPECT_EQ(cdstream::ackedMd5("x_BRP.edf", bytes, bytes.size()),
              cdstream::md5Hex(bytes, 512, bytes.size()));
    const std::string json = "{\"FlowGenerator\":{}}";
    EXPECT_EQ(cdstream::ackedMd5("Identification.json", json, json.size()),
              cdstream::md5Hex(json, 0, json.size()));
}

TEST(CpapDashStreamRules, OnlyTheCardsOwnFilesAreSent) {
    for (const char* ok : {"STR.edf", "Identification.json", "Identification.crc", "SETTINGS/CurrentSettings.json",
                           "DATALOG/20261116/20261116_221502_BRP.edf", "datalog/20261116/20261116.crc"})
        EXPECT_TRUE(cdstream::isCardPath(ok)) << ok;
    for (const char* no : {"stream/20261116/1792206000000.cds1s", "AS11/summary-2026.as11", "batches/cursor.json",
                           "DATALOG/2026/x_BRP.edf", "DATALOG/20261116/x_BRP.edf.tmp",
                           "DATALOG/20261116/x_BRP.edf.part", ".hidden", "bmc/whatever.dat", ""})
        EXPECT_FALSE(cdstream::isCardPath(no)) << no;
}

TEST(CpapDashStreamRules, CardPathsBecomeTheMulesUploadFields) {
    auto h = cdstream::cardHeadersFor("STR.edf");
    ASSERT_TRUE(h);
    EXPECT_EQ(h->date_folder, "STR");
    EXPECT_EQ(h->rel_dir, "");
    EXPECT_EQ(h->filename, "STR.edf");
    h = cdstream::cardHeadersFor("Identification.json");
    EXPECT_EQ(h->date_folder, "STR");
    EXPECT_EQ(h->rel_dir, "\\");
    h = cdstream::cardHeadersFor("DATALOG/20261116/20261116_221502_BRP.edf");
    EXPECT_EQ(h->date_folder, "20261116");
    EXPECT_EQ(h->rel_dir, "");
    EXPECT_EQ(h->filename, "20261116_221502_BRP.edf");
    h = cdstream::cardHeadersFor("SETTINGS/CurrentSettings.json");
    EXPECT_EQ(h->date_folder, "SETTINGS");
    EXPECT_EQ(h->rel_dir, "SETTINGS");
    EXPECT_FALSE(cdstream::cardHeadersFor("DATALOG/../x"));
}

TEST(CpapDashStreamRules, TheBinaryHeaderIsTwelveLittleEndianBytes) {
    const auto h = cdstream::binaryHeader(0x01020304u, 0x0a0b0c0du, true, 3);
    ASSERT_EQ(h.size(), 12u);
    const unsigned char want[12] = {4, 3, 2, 1, 0x0d, 0x0c, 0x0b, 0x0a, 1, 0, 3, 0};
    for (int i = 0; i < 12; ++i) EXPECT_EQ(static_cast<unsigned char>(h[i]), want[i]) << i;
    EXPECT_EQ(static_cast<unsigned char>(cdstream::binaryHeader(1, 0, false, 3)[8]), 0);
}

TEST(CpapDashStreamRules, FileReadyCarriesTheMachineSerial) {
    cdstream::CardHeaders h{"20261116", "", "x_BRP.edf"};
    Json::Value v;
    Json::CharReaderBuilder rb;
    std::istringstream in(cdstream::fileReadyFrame(7, h, 100, 300, 2, 200, "abc", "23000000001"));
    ASSERT_TRUE(Json::parseFromStream(rb, in, &v, nullptr));
    EXPECT_EQ(v["t"].asString(), "file_ready");
    EXPECT_EQ(v["req_id"].asUInt(), 7u);
    EXPECT_EQ(v["date_folder"].asString(), "20261116");
    EXPECT_EQ(v["rel_dir"].asString(), "");
    EXPECT_EQ(v["filename"].asString(), "x_BRP.edf");
    EXPECT_EQ(v["offset"].asUInt64(), 100u);
    EXPECT_EQ(v["total_size"].asUInt64(), 300u);
    EXPECT_TRUE(v["eof"].asBool());
    EXPECT_EQ(v["chunks"].asUInt(), 2u);
    EXPECT_EQ(v["size"].asUInt64(), 200u);
    EXPECT_EQ(v["md5"].asString(), "abc");
    EXPECT_EQ(v["machine_serial"].asString(), "23000000001");
}

TEST(CpapDashStreamRules, TheSourceIsOneOfEzshareBridgeOrBle) {
    const fs::path dir = fs::temp_directory_path() /
                         ("cdstream_src_" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    fs::create_directories(dir);
    // Nothing else configured: a card source (ez Share, FlashAir, a folder).
    EXPECT_EQ(cdstream::sourceKind(false, true, dir.string()), "ezshare");
    // A bridge folder with no card yet is not a bridge source.
    fs::create_directories(dir / "sleepbridge" / "bridge-1" / "batches");
    EXPECT_EQ(cdstream::sourceKind(false, true, dir.string()), "ezshare");
    // A bridge that has rebuilt a card.
    fs::create_directories(dir / "sleepbridge" / "bridge-1" / "DATALOG");
    EXPECT_EQ(cdstream::sourceKind(false, true, dir.string()), "bridge");
    // The receiver switched off: not a bridge source.
    EXPECT_EQ(cdstream::sourceKind(false, false, dir.string()), "ezshare");
    // A Bluetooth reader switched on wins: one source per install.
    EXPECT_EQ(cdstream::sourceKind(true, true, dir.string()), "ble");
    fs::remove_all(dir);
}

// 2026-11-18: a new install reading a bridge's card folder said "ezshare"
// until the bridge's first rebuild, so its first nights closed by the card
// rule and came out partial.
TEST(CpapDashStreamRules, ALocalFolderThatIsABridgesCardIsABridgeFromTheFirstHello) {
    const fs::path dir = fs::temp_directory_path() /
                         ("cdstream_lb_" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    const fs::path card = dir / "sleepbridge" / "TAP-S3-00000041";
    fs::create_directories(card);   // no DATALOG yet: nothing rebuilt
    EXPECT_EQ(cdstream::sourceKind(false, true, dir.string(), card.string()), "bridge");
    EXPECT_EQ(cdstream::sourceKind(false, true, dir.string(), (card / ".." / "TAP-S3-00000041").string()),
              "bridge") << "the same folder written another way";

    const fs::path copy = dir / "my-card-copy";
    fs::create_directories(copy);
    EXPECT_EQ(cdstream::sourceKind(false, true, dir.string(), copy.string()), "ezshare")
        << "a card copied to a folder keeps the card rule";
    EXPECT_EQ(cdstream::sourceKind(false, true, dir.string(), (dir / "sleepbridge").string()), "ezshare")
        << "the receiver's own root is no one bridge's card";
    EXPECT_EQ(cdstream::sourceKind(false, false, dir.string(), card.string()), "ezshare")
        << "the receiver off: not a bridge source";
    EXPECT_EQ(cdstream::sourceKind(true, true, dir.string(), card.string()), "ble");
    fs::remove_all(dir);
}

TEST(CpapDashStreamRules, TheInstallIdIsMadeOnceAndKept) {
    const fs::path dir = fs::temp_directory_path() /
                         ("cdstream_id_" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    const auto a = CpapDashStreamer::loadOrCreateInstallId(dir.string());
    const auto b = CpapDashStreamer::loadOrCreateInstallId(dir.string());
    EXPECT_EQ(a.size(), 36u);
    EXPECT_EQ(a, b);
    EXPECT_TRUE(fs::exists(dir / "cpapdash_install_id"));
    fs::remove_all(dir);
}

TEST(CpapDashStreamRules, RootsAreFoundUnderTheDataFolder) {
    const fs::path dir = fs::temp_directory_path() /
                         ("cdstream_roots_" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    fs::create_directories(dir / "as11" / "23000000001");
    fs::create_directories(dir / "as11" / "unknown");
    fs::create_directories(dir / "sleepbridge" / "bridge-1");
    fs::create_directories(dir / "cpap_data");
    const auto roots = CpapDashStreamer::discoverRoots(
        dir.string(), {(dir / "cpap_data").string(), (dir / "nowhere").string(), ""});
    ASSERT_EQ(roots.size(), 3u);
    EXPECT_EQ(roots[0].path, (dir / "cpap_data").string());
    EXPECT_EQ(roots[1].machine, "23000000001");
    EXPECT_EQ(roots[2].path, (dir / "sleepbridge" / "bridge-1").string());
    EXPECT_EQ(roots[2].machine, "");
    fs::remove_all(dir);
}

// ── the uploader, against a local server ─────────────────────────────────────

#ifdef BUILD_WITH_WEB

namespace {

constexpr const char* kSerial = "23000000001";
constexpr const char* kToken = "cpd_hms_testtokentesttokentesttokentesttokentesttokentes";

class StreamTest : public ::testing::Test {
protected:
    fs::path dir_;
    std::shared_ptr<SQLiteDatabase> db_;
    std::unique_ptr<cdfake::FakeCpapDashServer> server_;
    std::shared_ptr<CpapDashStreamer> streamer_;
    uint16_t port_ = 0;

    void SetUp() override {
        dir_ = fs::temp_directory_path() /
               ("cdstream_" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
        fs::create_directories(dir_);
        db_ = std::make_shared<SQLiteDatabase>((dir_ / "hms.db").string());
        ASSERT_TRUE(db_->connect());
        server_ = std::make_unique<cdfake::FakeCpapDashServer>();
        port_ = server_->port();
    }

    void TearDown() override {
        card_feed::setSinks({}, {});
        if (streamer_) streamer_->stop();
        streamer_.reset();
        server_.reset();
        db_.reset();
        fs::remove_all(dir_);
    }

    fs::path root() const { return dir_ / "as11" / kSerial; }

    void startStreamer(bool on = true, uint16_t port = 0, double ping_s = 30.0) {
        const uint16_t p = port ? port : port_;
        streamer_ = std::make_shared<CpapDashStreamer>(
            db_,
            [p, ping_s] {
                CpapDashEndpoint ep;
                ep.tls = false;
                ep.host = "127.0.0.1";
                ep.port = p;
                ep.ping_every_s = ping_s;
                return makeCpapDashLink(ep);
            },
            "11111111-2222-4333-8444-555555555555", "6.3.1-test");
        CpapDashStreamer::Timing t;
        t.connect_timeout = std::chrono::milliseconds(3000);
        t.hello_timeout = std::chrono::milliseconds(3000);
        t.ack_timeout = std::chrono::milliseconds(5000);
        t.backoff_min = std::chrono::milliseconds(50);
        t.backoff_max = std::chrono::milliseconds(200);
        t.chunk_bytes = 4096;
        streamer_->setTiming(t);
        const std::string data_dir = dir_.string();
        streamer_->setRootsProvider([data_dir] { return CpapDashStreamer::discoverRoots(data_dir, {}); });
        streamer_->apply({on, kToken});
        card_feed::setSinks(
            [s = streamer_](const std::string& r, const std::string& rel, const std::string& m,
                            const std::string& src) { s->fileWritten(r, rel, m, src); },
            [s = streamer_](const std::string& r, const std::string& m, const std::string& src) {
                s->rootWritten(r, m, src);
            },
            [s = streamer_](const std::string& r) { s->burstEnded(r); });
        streamer_->start();
    }

    bool waitState(const std::string& state, int ms = 10000) {
        const auto by = std::chrono::steady_clock::now() + std::chrono::milliseconds(ms);
        while (std::chrono::steady_clock::now() < by) {
            if (streamer_->status().state == state) return true;
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
        }
        return false;
    }

    bool waitReadies(size_t n, int ms = 10000) {
        return server_->waitFor([&] { return server_->readiesLocked() >= n; }, ms);
    }

    // The index row for a path under root(), polled until it reaches [to].
    bool waitAcked(const std::string& rel, long long to, int ms = 10000) {
        const auto by = std::chrono::steady_clock::now() + std::chrono::milliseconds(ms);
        while (std::chrono::steady_clock::now() < by) {
            auto r = db_->getCpapDashSent(root().string(), rel);
            if (r && r->acked_to == to) return true;
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
        }
        return false;
    }

    // Any ack of [rel] under another root, polled.
    bool waitAcked2(const fs::path& under, const std::string& rel, int ms = 10000) {
        const auto by = std::chrono::steady_clock::now() + std::chrono::milliseconds(ms);
        while (std::chrono::steady_clock::now() < by) {
            auto r = db_->getCpapDashSent(under.string(), rel);
            if (r && r->acked_to > 0) return true;
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
        }
        return false;
    }

    std::string landedFor(const std::string& date, const std::string& name) {
        return server_->landed()[cdfake::FakeCpapDashServer::keyOf(kSerial, date, "", name)];
    }
};

const std::string kBrp = "DATALOG/20261116/20261116_220000_BRP.edf";
const std::string kEve = "DATALOG/20261116/20261116_220000_EVE.edf";

}  // namespace

TEST_F(StreamTest, ItConnectsToTheHmsRouteWithTheTokenAndSaysHelloAsHms) {
    writeFile(root() / kEve, edf(2));
    startStreamer();
    ASSERT_TRUE(waitState("connected"));
    EXPECT_EQ(server_->lastPath(), "/socket/v1/hms");
    const auto h = server_->lastHeaders();
    EXPECT_EQ(h.at("authorization"), std::string("Bearer ") + kToken);
    EXPECT_EQ(h.at("x-hms-install"), "11111111-2222-4333-8444-555555555555");
    const auto hello = server_->lastHello();
    EXPECT_EQ(hello["t"].asString(), "hello");
    EXPECT_EQ(hello["mode"].asString(), "hms");
    EXPECT_EQ(hello["fw"].asString(), "6.3.1-test");
}

TEST_F(StreamTest, EveryHelloCarriesTheInstallsSourceAsItIsNow) {
    std::string source = "ezshare";
    std::mutex m;
    startStreamer();
    streamer_->setSourceProvider([&] {
        std::lock_guard<std::mutex> lk(m);
        return source;
    });
    int n = 0;
    for (const char* want : {"ezshare", "bridge", "ble"}) {
        {
            std::lock_guard<std::mutex> lk(m);
            source = want;
        }
        // A new token reconnects: the next hello says the source as it is now.
        streamer_->apply({true, std::string(kToken) + std::to_string(++n)});
        const auto by = std::chrono::steady_clock::now() + std::chrono::seconds(5);
        // In the hello's `state`, which the socket hands ingest unchanged;
        // never at the top level.
        while (server_->lastHello()["state"]["source"].asString() != want &&
               std::chrono::steady_clock::now() < by)
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
        const auto hello = server_->lastHello();
        EXPECT_EQ(hello["state"]["source"].asString(), want);
        EXPECT_FALSE(hello.isMember("source"));
        EXPECT_EQ(hello["mode"].asString(), "hms");
    }
    streamer_->stop();   // before `source` goes
}

// ── a burst's end (ez Share) ─────────────────────────────────────────────────

namespace {
// The ez Share archive: the card's own Identification names the machine.
fs::path archiveWithIdentity(const fs::path& dir) {
    const fs::path a = dir / "cpap_data";
    writeFile(a / "Identification.json",
              R"({"FlowGenerator":{"IdentificationProfiles":{"Product":{"SerialNumber":"22000000009"}}}})");
    return a;
}
std::vector<std::string> since(const std::vector<std::string>& all, size_t from) {
    return std::vector<std::string>(all.begin() + static_cast<long>(std::min(from, all.size())), all.end());
}
}  // namespace

TEST_F(StreamTest, ABurstSendsItsFilesThenItsRound) {
    const auto archive = archiveWithIdentity(dir_);
    startStreamer();
    ASSERT_TRUE(waitState("connected"));
    // The burst collector's first pass: the root as it stands, then its end.
    card_feed::rootWritten(archive.string(), "", card_feed::kSourceCard);
    card_feed::burstEnded(archive.string());
    ASSERT_TRUE(server_->waitFor([&] { return server_->roundsLocked() >= 1; }));
    const size_t mark = server_->events().size();

    // A burst that brought two files of tonight.
    writeFile(archive / "DATALOG/20261116/20261116_221502_BRP.edf", edf(40));
    writeFile(archive / "DATALOG/20261116/20261116_221502_EVE.edf", edf(3));
    card_feed::rootWritten(archive.string(), "", card_feed::kSourceCard);
    card_feed::burstEnded(archive.string());
    ASSERT_TRUE(server_->waitFor([&] { return server_->roundsLocked() >= 2; }));
    const auto ev = since(server_->events(), mark);
    ASSERT_EQ(ev.size(), 3u);
    EXPECT_EQ(ev[0], "ready:20261116_221502_BRP.edf:kept");
    EXPECT_EQ(ev[1], "ready:20261116_221502_EVE.edf:kept");
    EXPECT_EQ(ev[2], "round");   // {"t":"round","done":true}, exactly
}

TEST_F(StreamTest, AnEmptyBurstStillSendsItsRound) {
    const auto archive = archiveWithIdentity(dir_);
    startStreamer();
    ASSERT_TRUE(waitState("connected"));
    card_feed::rootWritten(archive.string(), "", card_feed::kSourceCard);
    card_feed::burstEnded(archive.string());
    ASSERT_TRUE(server_->waitFor([&] { return server_->roundsLocked() >= 1; }));
    const size_t mark = server_->events().size();

    // Nothing new on the card: the pass still ends, and says so.
    card_feed::rootWritten(archive.string(), "", card_feed::kSourceCard);
    card_feed::burstEnded(archive.string());
    ASSERT_TRUE(server_->waitFor([&] { return server_->roundsLocked() >= 2; }));
    EXPECT_EQ(since(server_->events(), mark), std::vector<std::string>{"round"});
}

TEST_F(StreamTest, ARoundStaysBehindItsFilesAcrossAReconnect) {
    const auto archive = archiveWithIdentity(dir_);
    startStreamer();
    ASSERT_TRUE(waitState("connected"));
    card_feed::rootWritten(archive.string(), "", card_feed::kSourceCard);
    card_feed::burstEnded(archive.string());
    ASSERT_TRUE(server_->waitFor([&] { return server_->roundsLocked() >= 1; }));
    const size_t mark = server_->events().size();

    // The link drops in the middle of the burst's second file.
    const auto brp = edf(40);   // 4 KB+ : two chunks
    const auto pld = edf(200);  // six chunks
    writeFile(archive / "DATALOG/20261116/20261116_221502_BRP.edf", brp);
    writeFile(archive / "DATALOG/20261116/20261116_221502_PLD.edf", pld);
    server_->drop_after_chunks = static_cast<int>((brp.size() + 4095) / 4096) + 3;
    const int hellos = server_->hellos();
    card_feed::rootWritten(archive.string(), "", card_feed::kSourceCard);
    card_feed::burstEnded(archive.string());
    ASSERT_TRUE(server_->waitFor([&] { return server_->roundsLocked() >= 2; }, 15000));
    EXPECT_GT(server_->hellos(), hellos);   // it came back

    const auto ev = since(server_->events(), mark);
    // The round went once, last, after both files were kept, counting both.
    ASSERT_FALSE(ev.empty());
    EXPECT_EQ(ev.back(), "round");
    EXPECT_EQ(std::count_if(ev.begin(), ev.end(), [](const std::string& e) { return e.rfind("round", 0) == 0; }), 1);
    const auto kept = [&](const std::string& name) {
        return std::find(ev.begin(), ev.end(), "ready:" + name + ":kept") != ev.end();
    };
    EXPECT_TRUE(kept("20261116_221502_BRP.edf"));
    EXPECT_TRUE(kept("20261116_221502_PLD.edf"));
}

TEST_F(StreamTest, ABridgeOrBluetoothInstallSendsNoRound) {
    const auto archive = archiveWithIdentity(dir_);
    std::string source = "bridge";
    std::mutex m;
    startStreamer();
    streamer_->setSourceProvider([&] {
        std::lock_guard<std::mutex> lk(m);
        return source;
    });
    ASSERT_TRUE(waitState("connected"));
    for (const char* s : {"bridge", "ble"}) {
        {
            std::lock_guard<std::mutex> lk(m);
            source = s;
        }
        writeFile(archive / "DATALOG/20261116" / (std::string(s) + "_EVE.edf"), edf(2));
        card_feed::rootWritten(archive.string(), "", card_feed::kSourceCard);
        card_feed::burstEnded(archive.string());
        ASSERT_TRUE(waitAcked2(archive, std::string("DATALOG/20261116/") + s + "_EVE.edf"));
    }
    // Give a wrongly queued round every chance to arrive.
    std::this_thread::sleep_for(std::chrono::milliseconds(300));
    EXPECT_EQ(server_->waitFor([&] { return server_->roundsLocked() > 0; }, 1), false);
    streamer_->stop();   // before `source` goes
}

TEST_F(StreamTest, AnAckMovesTheIndexAndTheFileLandsWhole) {
    const auto bytes = edf(100);   // 10 KB: three 4 KB chunks
    writeFile(root() / kBrp, bytes);
    startStreamer();
    ASSERT_TRUE(waitAcked(kBrp, static_cast<long long>(bytes.size())));
    const auto row = db_->getCpapDashSent(root().string(), kBrp);
    EXPECT_EQ(row->machine, kSerial);
    EXPECT_EQ(row->acked_md5, cdstream::md5Hex(bytes, 512, bytes.size()));
    EXPECT_EQ(landedFor("20261116", "20261116_220000_BRP.edf"), bytes);
    const auto r = server_->readies();
    ASSERT_EQ(r.size(), 1u);
    EXPECT_EQ(r[0].frame["machine_serial"].asString(), kSerial);
    EXPECT_GT(r[0].frame["chunks"].asUInt(), 1u);
}

TEST_F(StreamTest, NoAckMovesNothing) {
    server_->ack_ok = false;
    writeFile(root() / kEve, edf(2));
    startStreamer();
    ASSERT_TRUE(waitReadies(1));
    std::this_thread::sleep_for(std::chrono::milliseconds(200));
    // ok:false: nothing acked; the row only says "from 0 next time".
    const auto row = db_->getCpapDashSent(root().string(), kEve);
    EXPECT_TRUE(!row || row->acked_to == 0);
    // A refused WHOLE file is not sent again until it is noted again.
    EXPECT_EQ(server_->readies().size(), 1u);
}

TEST_F(StreamTest, ARefusedRangeIsSentAgainWholeFromZeroThenAdvances) {
    // Ingest: ok:false is nothing kept (a range past what it holds, after a
    // purge). The next attempt goes whole from 0, never the same range.
    const auto first = edf(10);
    writeFile(root() / kBrp, first);
    startStreamer();
    ASSERT_TRUE(waitAcked(kBrp, static_cast<long long>(first.size())));

    server_->refuse_range_once = true;
    const auto later = edf(25);
    writeFile(root() / kBrp, later);
    card_feed::fileWritten(root().string(), kBrp, kSerial, card_feed::kSourceBleAs11);
    ASSERT_TRUE(waitAcked(kBrp, static_cast<long long>(later.size())));

    const auto r = server_->readies();
    ASSERT_EQ(r.size(), 3u);
    EXPECT_EQ(r[1].frame["offset"].asUInt64(), first.size());   // the range, refused
    EXPECT_FALSE(r[1].whole);
    EXPECT_EQ(r[2].frame["offset"].asUInt64(), 0u);             // then whole, kept
    EXPECT_TRUE(r[2].whole);
    EXPECT_EQ(landedFor("20261116", "20261116_220000_BRP.edf"), later);
}

TEST_F(StreamTest, TheIndexMovesOnlyWhenTheAckArrives) {
    // file_ack comes once ingest has handled the file_ready, which can take a while.
    server_->ack_delay_ms = 800;
    const auto bytes = edf(20);
    writeFile(root() / kEve, bytes);
    startStreamer();
    const auto by = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (server_->chunkBytes() < static_cast<long long>(bytes.size()) &&
           std::chrono::steady_clock::now() < by)
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    ASSERT_EQ(server_->chunkBytes(), static_cast<long long>(bytes.size()));
    // Every byte is there and file_ready went; ingest has not answered yet.
    std::this_thread::sleep_for(std::chrono::milliseconds(200));
    EXPECT_FALSE(db_->getCpapDashSent(root().string(), kEve).has_value());
    ASSERT_TRUE(waitAcked(kEve, static_cast<long long>(bytes.size())));
}

TEST_F(StreamTest, ARefusedInstallWaitsItsRetryAndShowsTheReason) {
    // The socket opens, then bye {retry_in_s, reason} and a 1008 close.
    server_->bye_reason = "this account cannot stream";
    server_->bye_retry_s = 1;
    startStreamer();
    ASSERT_TRUE(waitState("closed"));
    EXPECT_EQ(streamer_->status().reason, "this account cannot stream");
    ASSERT_TRUE(server_->waitFor([&] { return server_->upgradesLocked() >= 2; }, 5000));
    const auto t = server_->upgradeTimes();
    // Waited out as asked (1 s), not the 50 ms backoff.
    EXPECT_GE(std::chrono::duration_cast<std::chrono::milliseconds>(t[1] - t[0]).count(), 900);
}

TEST_F(StreamTest, ARevokedTokenStopsLikeA401UntilTheTokenChanges) {
    writeFile(root() / kEve, edf(2));
    startStreamer();
    ASSERT_TRUE(waitState("connected"));
    ASSERT_TRUE(waitAcked(kEve, static_cast<long long>(edf(2).size())));
    server_->closeWith(1008, "hms token revoked: regenerated");
    ASSERT_TRUE(waitState("revoked"));
    EXPECT_EQ(streamer_->status().reason, "hms token revoked: regenerated");
    const int tries = server_->upgrades();
    std::this_thread::sleep_for(std::chrono::milliseconds(400));   // several backoffs' worth
    EXPECT_EQ(server_->upgrades(), tries);

    streamer_->apply({true, std::string(kToken) + "new"});
    EXPECT_TRUE(waitState("connected"));
}

TEST_F(StreamTest, ItPingsSoAnIdleSocketIsNeverSilent) {
    startStreamer(true, 0, 0.2);
    ASSERT_TRUE(waitState("connected"));
    const auto by = std::chrono::steady_clock::now() + std::chrono::seconds(3);
    while (server_->pings() < 2 && std::chrono::steady_clock::now() < by)
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    EXPECT_GE(server_->pings(), 2);
}

TEST_F(StreamTest, ADeployByeReconnectsAndResumesFromTheIndex) {
    const auto eve = edf(3);
    writeFile(root() / kEve, eve);
    startStreamer();
    ASSERT_TRUE(waitAcked(kEve, static_cast<long long>(eve.size())));
    const size_t before = server_->readies().size();

    // The socket's deploy: bye with a retry and no reason. A night's file
    // lands while hms-cpap is away.
    server_->sendText(R"({"t":"bye","retry_in_s":1})");
    server_->dropNow();
    const auto brp = edf(40);
    writeFile(root() / kBrp, brp);
    ASSERT_TRUE(server_->waitFor([&] { return server_->hellosLocked() >= 2; }, 5000));
    ASSERT_TRUE(waitAcked(kBrp, static_cast<long long>(brp.size())));
    // Only the new file went: the acked one was resumed past, not resent.
    const auto r = server_->readies();
    ASSERT_EQ(r.size(), before + 1);
    EXPECT_EQ(r.back().frame["filename"].asString(), "20261116_220000_BRP.edf");
}

TEST_F(StreamTest, AGrowingBrpSendsOnlyItsNewRecordsAfterTheAck) {
    const auto first = edf(10);
    writeFile(root() / kBrp, first);
    startStreamer();
    ASSERT_TRUE(waitAcked(kBrp, static_cast<long long>(first.size())));

    const auto later = edf(25);
    writeFile(root() / kBrp, later);
    card_feed::fileWritten(root().string(), kBrp, kSerial, card_feed::kSourceBleAs11);
    ASSERT_TRUE(waitAcked(kBrp, static_cast<long long>(later.size())));

    const auto r = server_->readies();
    ASSERT_EQ(r.size(), 2u);
    EXPECT_EQ(r[1].frame["offset"].asUInt64(), first.size());
    EXPECT_EQ(r[1].frame["size"].asUInt64(), later.size() - first.size());
    // What landed is the first header and every record, as a mule's ranges.
    const auto landed = landedFor("20261116", "20261116_220000_BRP.edf");
    EXPECT_EQ(landed.size(), later.size());
    EXPECT_EQ(landed.substr(512), later.substr(512));
}

TEST_F(StreamTest, AChangedPrefixResendsTheWholeFile) {
    const auto first = edf(10);
    writeFile(root() / kBrp, first);
    startStreamer();
    ASSERT_TRUE(waitAcked(kBrp, static_cast<long long>(first.size())));

    auto later = edf(12);
    later[700] = '#';
    writeFile(root() / kBrp, later);
    card_feed::fileWritten(root().string(), kBrp, kSerial, card_feed::kSourceBleAs11);
    ASSERT_TRUE(waitAcked(kBrp, static_cast<long long>(later.size())));
    const auto r = server_->readies();
    ASSERT_EQ(r.size(), 2u);
    EXPECT_EQ(r[1].frame["offset"].asUInt64(), 0u);
    EXPECT_EQ(landedFor("20261116", "20261116_220000_BRP.edf"), later);
}

TEST_F(StreamTest, ADroppedSocketResumesFromTheLastAckAndEveryByteArrivesOnce) {
    // The walk goes in path order, so the BRP goes before the EVE.
    const auto brp = edf(200);   // 20 KB+, six chunks
    const auto eve = edf(3);
    writeFile(root() / kBrp, brp);
    writeFile(root() / kEve, eve);
    // The BRP's chunks: drop on the fourth, mid-file.
    server_->drop_after_chunks = 4;
    startStreamer();
    ASSERT_TRUE(waitAcked(kBrp, static_cast<long long>(brp.size()), 15000));
    ASSERT_TRUE(waitAcked(kEve, static_cast<long long>(eve.size()), 15000));
    EXPECT_GE(server_->hellos(), 2);   // it came back

    // Each file was taken whole exactly once, and what landed is what is on disk.
    int brp_whole = 0, eve_whole = 0;
    for (const auto& r : server_->readies()) {
        if (!r.whole) continue;
        if (r.frame["filename"].asString() == "20261116_220000_BRP.edf") ++brp_whole;
        if (r.frame["filename"].asString() == "20261116_220000_EVE.edf") ++eve_whole;
    }
    EXPECT_EQ(brp_whole, 1);
    EXPECT_EQ(eve_whole, 1);
    EXPECT_EQ(landedFor("20261116", "20261116_220000_BRP.edf"), brp);
    EXPECT_EQ(landedFor("20261116", "20261116_220000_EVE.edf"), eve);
}

TEST_F(StreamTest, ADroppedSocketMidRangeResendsOnlyPastTheLastAck) {
    const auto first = edf(10);
    writeFile(root() / kBrp, first);
    startStreamer();
    ASSERT_TRUE(waitAcked(kBrp, static_cast<long long>(first.size())));

    const auto later = edf(150);   // the range is ~14 KB: four chunks
    writeFile(root() / kBrp, later);
    server_->drop_after_chunks = 2 + static_cast<int>((first.size() + 4095) / 4096);
    card_feed::fileWritten(root().string(), kBrp, kSerial, card_feed::kSourceBleAs11);
    ASSERT_TRUE(waitAcked(kBrp, static_cast<long long>(later.size()), 15000));

    // After the reconnect the range went again from the acked offset, not 0.
    const auto r = server_->readies();
    ASSERT_FALSE(r.empty());
    EXPECT_EQ(r.back().frame["offset"].asUInt64(), first.size());
    EXPECT_TRUE(r.back().whole);
    const auto landed = landedFor("20261116", "20261116_220000_BRP.edf");
    EXPECT_EQ(landed.size(), later.size());
    EXPECT_EQ(landed.substr(512), later.substr(512));
}

TEST_F(StreamTest, SwitchOffAndOnClearsTheIndexAndSendsEverythingAgain) {
    const auto eve = edf(3);
    writeFile(root() / kEve, eve);
    writeFile(root() / "STR.edf", edf(5));
    startStreamer();
    ASSERT_TRUE(waitAcked(kEve, static_cast<long long>(eve.size())));
    ASSERT_TRUE(waitAcked("STR.edf", static_cast<long long>(edf(5).size())));
    const size_t before = server_->readies().size();

    streamer_->apply({false, kToken});
    ASSERT_TRUE(waitState("off"));
    const auto by = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (!db_->listCpapDashSent("").empty() && std::chrono::steady_clock::now() < by)
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    EXPECT_TRUE(db_->listCpapDashSent("").empty());

    streamer_->apply({true, kToken});
    ASSERT_TRUE(waitReadies(before + 2));
    ASSERT_TRUE(waitAcked(kEve, static_cast<long long>(eve.size())));
    const auto r = server_->readies();
    for (size_t i = before; i < r.size(); ++i) EXPECT_EQ(r[i].frame["offset"].asUInt64(), 0u);
}

TEST_F(StreamTest, BackfillSendsEveryNightHeldAtSwitchOn) {
    // Nights written while the switch was off.
    for (const char* night : {"20261110", "20261111", "20261112"})
        writeFile(root() / "DATALOG" / night / (std::string(night) + "_220000_EVE.edf"), edf(2));
    writeFile(dir_ / "sleepbridge" / "bridge-1" / "Identification.json",
              R"({"FlowGenerator":{"IdentificationProfiles":{"Product":{"SerialNumber":"22000000002"}}}})");
    writeFile(dir_ / "sleepbridge" / "bridge-1" / "DATALOG" / "20261109" / "20261109_230000_EVE.edf", edf(2));
    startStreamer(false);
    ASSERT_TRUE(waitState("off"));
    streamer_->apply({true, kToken});
    ASSERT_TRUE(waitReadies(5));
    const auto landed = server_->landed();
    EXPECT_EQ(landed.count(cdfake::FakeCpapDashServer::keyOf(kSerial, "20261110", "", "20261110_220000_EVE.edf")), 1u);
    EXPECT_EQ(landed.count(cdfake::FakeCpapDashServer::keyOf(kSerial, "20261112", "", "20261112_220000_EVE.edf")), 1u);
    // The bridge's card names its own machine: each machine kept apart (D4).
    EXPECT_EQ(landed.count(cdfake::FakeCpapDashServer::keyOf("22000000002", "20261109", "", "20261109_230000_EVE.edf")), 1u);
    EXPECT_EQ(landed.count(cdfake::FakeCpapDashServer::keyOf("22000000002", "STR", "\\", "Identification.json")), 1u);
}

TEST_F(StreamTest, A401ShowsInTheStatusAndNothingIsRetriedUntilTheTokenChanges) {
    server_->refuse_status = 401;
    startStreamer();
    ASSERT_TRUE(waitState("refused"));
    EXPECT_EQ(streamer_->status().http_status, 401);
    const int tries = server_->upgrades();
    std::this_thread::sleep_for(std::chrono::milliseconds(400));   // several backoffs' worth
    EXPECT_EQ(server_->upgrades(), tries);

    // A new token is tried at once.
    server_->refuse_status = 0;
    streamer_->apply({true, std::string(kToken) + "x"});
    EXPECT_TRUE(waitState("connected"));
}

TEST_F(StreamTest, AByeWithAReasonShowsInTheStatus) {
    server_->bye_reason = "streaming is off for this account";
    startStreamer();
    ASSERT_TRUE(waitState("closed"));
    EXPECT_EQ(streamer_->status().reason, "streaming is off for this account");
}

TEST_F(StreamTest, SwitchedOffItHoldsNoConnection) {
    writeFile(root() / kEve, edf(2));
    startStreamer(false);
    ASSERT_TRUE(waitState("off"));
    card_feed::fileWritten(root().string(), kEve, kSerial, card_feed::kSourceBleAs11);
    std::this_thread::sleep_for(std::chrono::milliseconds(200));
    EXPECT_EQ(server_->upgrades(), 0);
}

TEST_F(StreamTest, CpapDashUnreachableLeavesCollectionUnchanged) {
    // Nothing listens on this port.
    uint16_t dead = 0;
    {
        cdfake::FakeCpapDashServer gone;
        dead = gone.port();
    }
    startStreamer(true, dead);

    // A Bluetooth night written with the uploader attached and failing...
    const fs::path with = dir_;
    As11NightWriter w(with.string(), fixedOffsetClock(-300));
    // ...and the same night with no uploader at all.
    const fs::path without = dir_ / "reference";
    As11NightWriter ref(without.string(), fixedOffsetClock(-300));
    constexpr int64_t kT0 = 1792206000000LL;   // 2026-10-17 03:00 UTC, 22:00 UTC-5
    auto block = [&](int64_t start) {
        as11::StreamBlock b;
        b.start_ms = start;
        b.serial = kSerial;
        b.channels.push_back({as11::kFlow, 0, std::vector<int16_t>(25, 300)});
        b.channels.push_back({as11::kMaskPressure, 0, std::vector<int16_t>(25, 900)});
        b.channels.push_back({as11::kLeak, 1, {200}});
        return b;
    };
    const auto t0 = std::chrono::steady_clock::now();
    for (int i = 0; i < 40; ++i) w.onBlock(block(kT0 + i * 1000));
    w.onStreamEnd(kSerial);
    const auto spent = std::chrono::steady_clock::now() - t0;
    card_feed::setSinks({}, {});
    for (int i = 0; i < 40; ++i) ref.onBlock(block(kT0 + i * 1000));
    ref.onStreamEnd(kSerial);

    const fs::path a = with / "as11" / kSerial / "DATALOG" / "20261016";
    const fs::path b = without / "as11" / kSerial / "DATALOG" / "20261016";
    std::vector<std::string> names;
    for (const auto& e : fs::directory_iterator(b)) names.push_back(e.path().filename().string());
    ASSERT_FALSE(names.empty());
    for (const auto& n : names) EXPECT_EQ(readFile(a / n), readFile(b / n)) << n;
    // The writer never waited on the uploader's connection attempts.
    EXPECT_LT(std::chrono::duration_cast<std::chrono::milliseconds>(spent).count(), 2000);
    EXPECT_TRUE(waitState("unreachable"));
    EXPECT_TRUE(db_->listCpapDashSent("").empty());
}

TEST_F(StreamTest, TheBluetoothWriterFeedsTheUploaderAsItWrites) {
    startStreamer();
    ASSERT_TRUE(waitState("connected"));
    As11NightWriter w(dir_.string(), fixedOffsetClock(-300));
    constexpr int64_t kT0 = 1792206000000LL;
    auto block = [&](int64_t start) {
        as11::StreamBlock b;
        b.start_ms = start;
        b.serial = kSerial;
        b.channels.push_back({as11::kFlow, 0, std::vector<int16_t>(25, 300)});
        b.channels.push_back({as11::kMaskPressure, 0, std::vector<int16_t>(25, 900)});
        return b;
    };
    for (int i = 0; i < 10; ++i) w.onBlock(block(kT0 + i * 1000));   // the first live write
    const std::string rel = "DATALOG/20261016/20261016_220000_BRP.edf";
    const auto first = readFile(root() / rel);
    ASSERT_FALSE(first.empty());
    ASSERT_TRUE(waitAcked(rel, static_cast<long long>(first.size())));
    for (int i = 10; i < 30; ++i) w.onBlock(block(kT0 + i * 1000));   // two more live writes
    w.onStreamEnd(kSerial);
    const auto last = readFile(root() / rel);
    ASSERT_GT(last.size(), first.size());
    ASSERT_TRUE(waitAcked(rel, static_cast<long long>(last.size())));
    // After the first, the BRP only ever went as ranges.
    bool first_seen = false;
    for (const auto& r : server_->readies()) {
        if (r.frame["filename"].asString() != "20261016_220000_BRP.edf") continue;
        if (!first_seen) {
            EXPECT_EQ(r.frame["offset"].asUInt64(), 0u);
            first_seen = true;
        } else {
            EXPECT_GT(r.frame["offset"].asUInt64(), 0u);
        }
    }
    const auto landed = landedFor("20261016", "20261016_220000_BRP.edf");
    const auto h = cdstream::edfHeaderBytes(last);
    EXPECT_EQ(landed.size(), last.size());
    EXPECT_EQ(landed.substr(h), last.substr(h));
}

// ── SDD-079: each file says its source, and oximetry goes too ───────────────

namespace {

bool waitRow(IDatabase& db, const fs::path& under, const std::string& rel, long long to, int ms = 10000) {
    const auto by = std::chrono::steady_clock::now() + std::chrono::milliseconds(ms);
    while (std::chrono::steady_clock::now() < by) {
        auto r = db.getCpapDashSent(under.string(), rel);
        if (r && r->acked_to == to) return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    return false;
}

size_t indexOf(const std::vector<std::string>& ev, const std::string& prefix) {
    for (size_t i = 0; i < ev.size(); ++i)
        if (ev[i].rfind(prefix, 0) == 0) return i;
    return ev.size();
}

size_t lastIndexOf(const std::vector<std::string>& ev, const std::string& prefix) {
    size_t at = ev.size();
    for (size_t i = 0; i < ev.size(); ++i)
        if (ev[i].rfind(prefix, 0) == 0) at = i;
    return at;
}

// A row the uploader of an hms-cpap before 7.0.0 wrote: no source.
void preRow(IDatabase& db, const fs::path& root, const std::string& rel, const std::string& acked_bytes,
            const std::string& machine) {
    IDatabase::CpapDashSent r;
    r.root = root.string();
    r.path = rel;
    r.machine = machine;
    r.acked_to = static_cast<long long>(acked_bytes.size());
    r.acked_md5 = cdstream::ackedMd5(fs::path(rel).filename().string(), acked_bytes, acked_bytes.size());
    r.updated_at = "2026-11-01T00:00:00Z";
    ASSERT_TRUE(db.upsertCpapDashSent(r));
}

struct OutboxRoot {
    explicit OutboxRoot(const fs::path& data_dir) { oximetry_outbox::setRoot(oximetry_outbox::rootFor(data_dir.string())); }
    ~OutboxRoot() { oximetry_outbox::setRoot(""); }
};

cpapdash::parser::OximetrySession garminRow(const std::string& name, int samples) {
    cpapdash::parser::OximetrySession s;
    s.filename = name;
    s.sample_interval = 60;
    for (int i = 0; i < samples; ++i) {
        cpapdash::parser::OximetrySample x{};
        x.spo2 = 95;
        x.heart_rate = 60;
        x.timestamp = std::chrono::system_clock::from_time_t(1795042800 + i * 60);
        s.samples.push_back(x);
    }
    return s;
}

}  // namespace

TEST_F(StreamTest, EveryFileReadySaysItsRootsSourceAndTheIndexKeepsIt) {
    writeFile(root() / kEve, edf(2));
    const fs::path bridge = dir_ / "sleepbridge" / "bridge-1";
    writeFile(bridge / "Identification.json",
              R"({"FlowGenerator":{"IdentificationProfiles":{"Product":{"SerialNumber":"22000000009"}}}})");
    writeFile(bridge / "DATALOG/20261116/20261116_230000_EVE.edf", edf(2));
    startStreamer();
    ASSERT_TRUE(waitAcked(kEve, static_cast<long long>(edf(2).size())));
    ASSERT_TRUE(waitRow(*db_, bridge, "DATALOG/20261116/20261116_230000_EVE.edf", static_cast<long long>(edf(2).size())));
    for (const auto& r : server_->readies()) {
        const auto name = r.frame["filename"].asString();
        if (name == "20261116_220000_EVE.edf") EXPECT_EQ(r.frame["source"].asString(), "ble-as11");
        if (name == "20261116_230000_EVE.edf") EXPECT_EQ(r.frame["source"].asString(), "card")
            << "a Sleep Bridge rebuilds the machine's own card; card is sent too, never left out";
    }
    EXPECT_EQ(db_->getCpapDashSent(root().string(), kEve)->source, "ble-as11");
    EXPECT_EQ(db_->getCpapDashSent(bridge.string(), "DATALOG/20261116/20261116_230000_EVE.edf")->source, "card");
}

TEST_F(StreamTest, FilesAckedBefore700SayTheirSourceOnceBeforeAnyNewBytesOfTheRoot) {
    // An hms-cpap before 7.0.0 sent these and they were acked, with no source.
    const auto brp_then = edf(10), brp_now = edf(20);   // tonight's BRP has grown since
    writeFile(root() / kBrp, brp_now);
    preRow(*db_, root(), kBrp, brp_then, kSerial);
    writeFile(root() / kEve, edf(2));
    preRow(*db_, root(), kEve, edf(2), kSerial);
    preRow(*db_, root(), "DATALOG/20261116/20261116_220000_PLD.edf", "", kSerial);   // acked at 0
    const fs::path bridge = dir_ / "sleepbridge" / "bridge-1";   // a card root
    writeFile(bridge / "Identification.json",
              R"({"FlowGenerator":{"IdentificationProfiles":{"Product":{"SerialNumber":"22000000009"}}}})");
    writeFile(bridge / "DATALOG/20261116/20261116_230000_EVE.edf", edf(2));
    preRow(*db_, bridge, "DATALOG/20261116/20261116_230000_EVE.edf", edf(2), "22000000009");

    startStreamer();
    ASSERT_TRUE(waitAcked(kBrp, static_cast<long long>(brp_now.size())));
    const auto ev = server_->events();
    // One note each for the two files acked above 0 under the Bluetooth root.
    EXPECT_EQ(std::count(ev.begin(), ev.end(), "note:20261116_220000_BRP.edf:kept"), 1);
    EXPECT_EQ(std::count(ev.begin(), ev.end(), "note:20261116_220000_EVE.edf:kept"), 1);
    EXPECT_EQ(indexOf(ev, "note:20261116_220000_PLD.edf"), ev.size()) << "acked at 0: a note would discard it";
    EXPECT_EQ(indexOf(ev, "note:20261116_230000_EVE.edf"), ev.size()) << "a card root needs none";
    // Both notes before the night's new bytes (the ingest's ordering need).
    EXPECT_LT(lastIndexOf(ev, "note:"), indexOf(ev, "ready:20261116_220000_BRP.edf"));
    for (const auto& r : server_->readies()) {
        if (r.frame["chunks"].asUInt() != 0) continue;
        EXPECT_GT(r.frame["offset"].asUInt64(), 0u) << "never at offset 0";
        EXPECT_EQ(r.frame["offset"].asUInt64(), r.frame["total_size"].asUInt64());
        EXPECT_EQ(r.frame["source"].asString(), "ble-as11");
    }
    // The grown BRP went from its acked offset, after its note.
    const auto r = server_->readies();
    const auto brp = std::find_if(r.begin(), r.end(), [](const auto& x) {
        return x.frame["filename"].asString() == "20261116_220000_BRP.edf" && x.frame["chunks"].asUInt() > 0;
    });
    ASSERT_NE(brp, r.end());
    EXPECT_EQ(brp->frame["offset"].asUInt64(), brp_then.size());
    // Each note's ack set the source.
    EXPECT_EQ(db_->getCpapDashSent(root().string(), kEve)->source, "ble-as11");
    EXPECT_EQ(db_->getCpapDashSent(root().string(), kEve)->acked_to, static_cast<long long>(edf(2).size()));
    EXPECT_EQ(db_->getCpapDashSent(root().string(), "DATALOG/20261116/20261116_220000_PLD.edf")->source, "");

    // A second connection sends none.
    const size_t before = server_->events().size();
    const int hellos = server_->hellos();
    streamer_->apply({true, std::string(kToken) + "2"});
    ASSERT_TRUE(server_->waitFor([&] { return server_->hellosLocked() > hellos; }));
    ASSERT_TRUE(waitState("connected"));
    std::this_thread::sleep_for(std::chrono::milliseconds(400));
    const auto after = since(server_->events(), before);
    EXPECT_EQ(indexOf(after, "note:"), after.size());
}

TEST_F(StreamTest, OnAWalkTheOximetryOutboxGoesAfterEveryCardFile) {
    OutboxRoot outbox(dir_);
    cpapdash::parser::OximetrySession ring;
    ring.filename = "20261116230512";
    std::string o2s("\x01\x03\x00\x00\x00\x00\x00\x00\x04\x00", 10);
    for (int i = 0; i < 3; ++i) o2s += std::string{'\x60', '\x3e', '\0'};
    o2s += std::string(48, '\0');
    oximetry_outbox::stored(ring, {"o2ring", "O2Ring-S"}, o2s);
    oximetry_outbox::stored(garminRow("garmin-1", 5), {"garmin", "Garmin Fenix 7X"}, "");
    writeFile(root() / kBrp, edf(30));
    writeFile(root() / kEve, edf(2));

    startStreamer();
    const fs::path box = oximetry_outbox::rootFor(dir_.string());
    ASSERT_TRUE(waitRow(*db_, box, "garmin-1.json", static_cast<long long>(fs::file_size(box / "garmin-1.json"))));
    ASSERT_TRUE(waitRow(*db_, box, "20261116230512", static_cast<long long>(o2s.size())));
    ASSERT_TRUE(waitAcked(kEve, static_cast<long long>(edf(2).size())));
    const auto ev = server_->events();
    EXPECT_LT(lastIndexOf(ev, "ready:"), indexOf(ev, "oxi:")) << "a night's card files are ahead of its oximetry";
    const auto landed = server_->oxiLanded();
    EXPECT_EQ(landed.at("20261116230512"), o2s) << "the ring's file, byte for byte";
    for (const auto& r : server_->readies()) {
        if (r.frame["kind"].asString() != "oximetry") continue;
        EXPECT_FALSE(r.frame.isMember("filename"));
        EXPECT_EQ(r.frame["offset"].asUInt64(), 0u) << "a recording goes whole";
        EXPECT_TRUE(r.frame["eof"].asBool());
        if (r.frame["oxi"]["name"].asString() == "garmin-1") {
            EXPECT_EQ(r.frame["oxi"]["format"].asString(), "hms-json");
            EXPECT_EQ(r.frame["oxi"]["source"].asString(), "garmin");
            EXPECT_EQ(r.frame["oxi"]["device_name"].asString(), "Garmin Fenix 7X");
        }
    }
    EXPECT_GE(streamer_->status().oximetry_acked, 2);
    EXPECT_EQ(streamer_->status().toJson()["oximetry_state"].asString(), "sending");
}

TEST_F(StreamTest, AnIngestThatTakesNoOximetryHoldsItForTheConnectionAndCardFilesStillGo) {
    OutboxRoot outbox(dir_);
    server_->refuse_oximetry = true;
    oximetry_outbox::stored(garminRow("garmin-1", 5), {"garmin", ""}, "");
    oximetry_outbox::stored(garminRow("garmin-2", 5), {"garmin", ""}, "");
    writeFile(root() / kEve, edf(2));
    startStreamer();
    ASSERT_TRUE(waitAcked(kEve, static_cast<long long>(edf(2).size())));
    const fs::path box = oximetry_outbox::rootFor(dir_.string());
    const auto by = std::chrono::steady_clock::now() + std::chrono::seconds(10);
    while (streamer_->status().oximetry_state != "held" && std::chrono::steady_clock::now() < by)
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    EXPECT_EQ(streamer_->status().oximetry_state, "held");
    EXPECT_EQ(streamer_->status().toJson()["oximetry_state"].asString(), "held");
    // One refusal, then nothing more on this connection, even on a new store.
    oximetry_outbox::stored(garminRow("garmin-3", 5), {"garmin", ""}, "");
    writeFile(root() / "DATALOG/20261116/20261116_230000_EVE.edf", edf(3));
    card_feed::fileWritten(root().string(), "DATALOG/20261116/20261116_230000_EVE.edf", kSerial,
                           card_feed::kSourceBleAs11);
    ASSERT_TRUE(waitAcked("DATALOG/20261116/20261116_230000_EVE.edf", static_cast<long long>(edf(3).size())))
        << "card files carry on";
    std::this_thread::sleep_for(std::chrono::milliseconds(300));
    auto ev = server_->events();
    EXPECT_EQ(std::count_if(ev.begin(), ev.end(), [](const std::string& e) { return e.rfind("oxi:", 0) == 0; }), 1);
    EXPECT_FALSE(db_->getCpapDashSent(box.string(), "garmin-1.json")) << "the index does not move";

    // The next connection, to an ingest that takes it, sends it all.
    server_->refuse_oximetry = false;
    streamer_->apply({true, std::string(kToken) + "2"});
    ASSERT_TRUE(waitRow(*db_, box, "garmin-1.json",
                        static_cast<long long>(fs::file_size(box / "garmin-1.json"))));
    ASSERT_TRUE(waitRow(*db_, box, "garmin-3.json",
                        static_cast<long long>(fs::file_size(box / "garmin-3.json"))));
    EXPECT_EQ(streamer_->status().oximetry_state, "sending");
    EXPECT_EQ(db_->getCpapDashSent(box.string(), "garmin-1.json")->source, "oximetry");
}

TEST_F(StreamTest, TheLiveSpoolGoesAsRangesOfWholeLines) {
    OutboxRoot outbox(dir_);
    startStreamer();
    ASSERT_TRUE(waitState("connected"));
    const auto t0 = std::chrono::system_clock::now();
    const auto file = oximetry_outbox::liveSample(t0, t0, 96, 60, 0);
    const fs::path box = oximetry_outbox::rootFor(dir_.string());
    ASSERT_TRUE(waitRow(*db_, box, file, static_cast<long long>(fs::file_size(box / file))));
    oximetry_outbox::liveSample(t0 + std::chrono::seconds(4), t0, 95, 61, 1);
    ASSERT_TRUE(waitRow(*db_, box, file, static_cast<long long>(fs::file_size(box / file))));
    std::vector<Json::Value> live;
    for (const auto& r : server_->readies())
        if (r.frame["kind"].asString() == "oximetry") live.push_back(r.frame);
    ASSERT_EQ(live.size(), 2u);
    EXPECT_EQ(live[0]["offset"].asUInt64(), 0u);
    EXPECT_GT(live[1]["offset"].asUInt64(), 0u) << "from the acked offset";
    EXPECT_EQ(live[1]["oxi"]["format"].asString(), "live-jsonl");
    EXPECT_EQ(live[1]["oxi"]["name"].asString(), file.substr(0, file.size() - 6));
    EXPECT_EQ(server_->oxiLanded().at(file.substr(0, file.size() - 6)), readFile(box / file));
}

TEST(CpapDashStreamRules, EachRootIsFoundWithItsSourceAndTheOutboxLast) {
    const fs::path dir = fs::temp_directory_path() /
                         ("cdstream_src_roots_" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    fs::create_directories(dir / "as11" / "23000000001");
    fs::create_directories(dir / "sleepbridge" / "bridge-1");
    fs::create_directories(dir / "cpap_data");
    fs::create_directories(oximetry_outbox::rootFor(dir.string()));
    const auto roots = CpapDashStreamer::discoverRoots(dir.string(), {(dir / "cpap_data").string()});
    ASSERT_EQ(roots.size(), 4u);
    EXPECT_EQ(roots[0].source, "card");
    EXPECT_EQ(roots[1].source, "ble-as11");
    EXPECT_EQ(roots[2].source, "card");
    EXPECT_EQ(roots[3].path, oximetry_outbox::rootFor(dir.string()));
    EXPECT_EQ(roots[3].source, "oximetry");
    fs::remove_all(dir);
}

#endif  // BUILD_WITH_WEB
