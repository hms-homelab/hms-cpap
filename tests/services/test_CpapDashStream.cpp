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
    // Albin: never poll.cpapdash.com, never the device socket's port.
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

    void startStreamer(bool on = true, uint16_t port = 0) {
        const uint16_t p = port ? port : port_;
        streamer_ = std::make_shared<CpapDashStreamer>(
            db_,
            [p] {
                CpapDashEndpoint ep;
                ep.tls = false;
                ep.host = "127.0.0.1";
                ep.port = p;
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
            [s = streamer_](const std::string& r, const std::string& rel, const std::string& m) {
                s->fileWritten(r, rel, m);
            },
            [s = streamer_](const std::string& r, const std::string& m) { s->rootWritten(r, m); });
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
    EXPECT_FALSE(db_->getCpapDashSent(root().string(), kEve).has_value());
}

TEST_F(StreamTest, AGrowingBrpSendsOnlyItsNewRecordsAfterTheAck) {
    const auto first = edf(10);
    writeFile(root() / kBrp, first);
    startStreamer();
    ASSERT_TRUE(waitAcked(kBrp, static_cast<long long>(first.size())));

    const auto later = edf(25);
    writeFile(root() / kBrp, later);
    card_feed::fileWritten(root().string(), kBrp, kSerial);
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
    card_feed::fileWritten(root().string(), kBrp, kSerial);
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
    card_feed::fileWritten(root().string(), kBrp, kSerial);
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
    card_feed::fileWritten(root().string(), kEve, kSerial);
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

#endif  // BUILD_WITH_WEB
