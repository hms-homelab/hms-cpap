// SDD-079 (platform SDD-061): each file sent to CpapDash says its source, and
// oximetry goes too.
//
// Three parts with no network. The contract: hms-cpap's builders produce the
// fixtures in tests/fixtures/cpapdash-stream/ byte for byte, the same files
// cpapdash-ingest's reader is tested against (SDD-061 section 7). The
// oximetry outbox: what each store writes, the wire name, hms-json, the live
// spool, the D7 backfill. And every oximetry store site calls the outbox.
// The uploader's half (the order, the source notes, a held oximetry) is in
// test_CpapDashStream.cpp, against its local server.
//
// Every value is made up; no real night, no real account.

#include <gtest/gtest.h>

#include "database/SQLiteDatabase.h"
#include "services/CardFileFeed.h"
#include "services/CpapDashStream.h"
#include "services/OximetryImport.h"
#include "services/OximetryOutbox.h"
#include "utils/OximetryDevice.h"

#include <cpapdash/parser/O2RingSParser.h>

#include <json/json.h>

#include <chrono>
#include <filesystem>
#include <fstream>
#include <regex>
#include <set>
#include <sstream>
#include <string>
#include <vector>

using namespace hms_cpap;
namespace fs = std::filesystem;
namespace ob = hms_cpap::oximetry_outbox;

namespace {

fs::path sourceDir() { return fs::path(__FILE__).parent_path().parent_path().parent_path(); }

std::string readFile(const fs::path& p) {
    std::ifstream in(p, std::ios::binary);
    std::ostringstream s;
    s << in.rdbuf();
    return s.str();
}

/// A contract fixture without its final newline: the frame as it goes.
std::string fixture(const std::string& name) {
    for (const fs::path& base : {sourceDir() / "tests/fixtures/cpapdash-stream", fs::path("tests/fixtures/cpapdash-stream"),
                                 fs::path("../tests/fixtures/cpapdash-stream"),
                                 fs::path("../../tests/fixtures/cpapdash-stream")}) {
        if (!fs::exists(base / name)) continue;
        std::string s = readFile(base / name);
        while (!s.empty() && (s.back() == '\n' || s.back() == '\r')) s.pop_back();
        return s;
    }
    ADD_FAILURE() << "no fixture " << name;
    return "";
}

Json::Value parse(const std::string& s) {
    Json::Value v;
    Json::CharReaderBuilder rb;
    std::istringstream in(s);
    EXPECT_TRUE(Json::parseFromStream(rb, in, &v, nullptr)) << s;
    return v;
}

std::set<std::string> keys(const Json::Value& v) {
    std::set<std::string> k;
    for (const auto& n : v.getMemberNames()) k.insert(n);
    return k;
}

cdstream::CardHeaders brpHeaders() { return {"20261118", "DATALOG\\20261118", "20261118_230846_BRP.edf"}; }

/// A minimal valid VLD v3 (as test_OximetryImport.cpp builds one).
std::string vld(int records = 3, int hour = 22) {
    std::vector<uint8_t> d(40 + 5 * records, 0);
    auto u32 = [&](size_t off, uint32_t v) {
        for (int i = 0; i < 4; ++i) d[off + i] = static_cast<uint8_t>(v >> (8 * i));
    };
    d[0] = 3;
    d[2] = 0xEA; d[3] = 0x07;                  // 2026
    d[4] = 9; d[5] = 12; d[6] = static_cast<uint8_t>(hour); d[7] = 30; d[8] = 0;
    u32(9, static_cast<uint32_t>(d.size()));
    u32(13, static_cast<uint32_t>(records * 4));
    d[22] = 4;
    for (int i = 0; i < records; ++i) {
        d[40 + 5 * i]     = static_cast<uint8_t>(95 - i);
        d[40 + 5 * i + 1] = 70;
    }
    return std::string(d.begin(), d.end());
}

/// A finished O2Ring-S recording (as test_OximetryImport.cpp builds one).
std::string o2s(int records = 3) {
    std::string d("\x01\x03\x00\x00\x00\x00\x00\x00\x04\x00", 10);
    for (int i = 0; i < records; ++i) {
        d.push_back(static_cast<char>(96 - i));
        d.push_back(static_cast<char>(62));
        d.push_back('\0');
    }
    std::string t(48, '\0');
    t[4] = '\x48'; t[5] = '\x12'; t[6] = '\x5a'; t[7] = '\xda';
    return d + t;
}

}  // namespace

// ── the contract: the fixtures, byte for byte ───────────────────────────────

TEST(CpapDashOximetryContract, ACardFileReadyIsTheFixtureAndCarriesItsSourceFrom700) {
    // Before 7.0.0 (no source): exactly file_ready_card.json.
    EXPECT_EQ(cdstream::fileReadyFrame(811, brpHeaders(), 0, 4096, 1, 4096, "620f0b67a91f7f74151bc5be745b7110",
                                       "23222222222"),
              fixture("file_ready_card.json"));
    // 7.0.0 always says it (SDD-079 D2): the same frame plus "source":"card",
    // which ingest reads as the absent case (SDD-061 D1).
    const std::string with = cdstream::fileReadyFrame(811, brpHeaders(), 0, 4096, 1, 4096,
                                                      "620f0b67a91f7f74151bc5be745b7110", "23222222222", "card");
    const std::string want = fixture("file_ready_card.json");
    EXPECT_EQ(with, want.substr(0, want.size() - 1) + ",\"source\":\"card\"}");
}

TEST(CpapDashOximetryContract, ABluetoothBuiltFileIsTheFixture) {
    EXPECT_EQ(cdstream::fileReadyFrame(812, brpHeaders(), 0, 4096, 1, 4096, "620f0b67a91f7f74151bc5be745b7110",
                                       "23222222222", card_feed::kSourceBleAs11),
              fixture("file_ready_ble_as11.json"));
}

TEST(CpapDashOximetryContract, TheSourceNoteIsTheFixtureAndNeverAtOffsetZero) {
    const auto note = cdstream::sourceNoteFrame(813, brpHeaders(), 4096, "23222222222", card_feed::kSourceBleAs11);
    EXPECT_EQ(note, fixture("file_ready_source_note.json"));
    const auto v = parse(note);
    EXPECT_EQ(v["chunks"].asUInt(), 0u);
    EXPECT_EQ(v["size"].asUInt(), 0u);
    EXPECT_EQ(v["offset"].asUInt64(), v["total_size"].asUInt64());
    EXPECT_GT(v["offset"].asUInt64(), 0u);
    EXPECT_TRUE(v["eof"].asBool());
    EXPECT_EQ(v["md5"].asString(), "d41d8cd98f00b204e9800998ecf8427e") << "md5 of nothing";
}

TEST(CpapDashOximetryContract, OximetryFramesAreTheFixtures) {
    EXPECT_EQ(cdstream::oximetryReadyFrame(814, {"20261118230512", "o2s", "o2ring", "O2Ring-S"}, 0, 30215, 1, 30215,
                                           "5d41402abc4b2a76b9719d911017c592"),
              fixture("file_ready_oximetry_o2s.json"));
    EXPECT_EQ(cdstream::oximetryReadyFrame(815, {"garmin-20261118", "hms-json", "garmin", "Garmin Fenix 7X"}, 0, 120,
                                           1, 120, "7215ee9c7d9dc229d2921a40e899ec5f"),
              fixture("file_ready_oximetry_hms_json.json"));
    // The live spool: a range from the acked offset, no source or device name.
    EXPECT_EQ(cdstream::oximetryReadyFrame(816, {"live-20261118", "live-jsonl", "", ""}, 2048, 2096, 1, 48,
                                           "b026324c6904b2a9cb4b88d6d61c81d1"),
              fixture("file_ready_oximetry_live.json"));
}

TEST(CpapDashOximetryContract, TheHmsJsonBodyIsTheFixture) {
    cpapdash::parser::OximetrySession s;
    s.sample_interval = 60;
    cpapdash::parser::OximetrySample a{}, b{};
    a.spo2 = 95; a.heart_rate = 58; a.motion = 0;
    b.spo2 = 94; b.heart_rate = 57; b.motion = 1;
    s.samples = {a, b};
    const std::vector<long long> utc{1763506800, 1763506860};
    EXPECT_EQ(ob::hmsJson(s, &utc), fixture("hms_oximetry_v1.json"));
}

// The skill's cases, on the side that builds the frames.
TEST(CpapDashOximetryContract, WireShapeIsTheKeySetOfEachFixture) {
    const std::set<std::string> card = {"t",          "req_id", "date_folder", "rel_dir", "filename", "offset",
                                        "total_size", "eof",    "chunks",      "size",    "md5",      "machine_serial"};
    auto with_source = card;
    with_source.insert("source");
    EXPECT_EQ(keys(parse(cdstream::fileReadyFrame(1, brpHeaders(), 0, 1, 1, 1, "x", "s"))), card);
    EXPECT_EQ(keys(parse(cdstream::fileReadyFrame(1, brpHeaders(), 0, 1, 1, 1, "x", "s", "card"))), with_source);
    EXPECT_EQ(keys(parse(cdstream::sourceNoteFrame(1, brpHeaders(), 9, "s", "ble-as11"))), with_source);

    const auto oxi = parse(cdstream::oximetryReadyFrame(1, {"n", "vld", "o2ring", "O2Ring"}, 0, 1, 1, 1, "x"));
    EXPECT_EQ(keys(oxi), (std::set<std::string>{"t", "req_id", "kind", "offset", "total_size", "eof", "chunks", "size",
                                                "md5", "oxi"}));
    EXPECT_EQ(keys(oxi["oxi"]), (std::set<std::string>{"name", "format", "source", "device_name"}));
}

TEST(CpapDashOximetryContract, AnOximetryFrameNamesNoFileSoAnOldIngestRefusesIt) {
    // SDD-061 D3: no top-level filename, date_folder or rel_dir; an ingest
    // that does not know `kind` answers "file_ready names no file", ok:false.
    const auto v = parse(cdstream::oximetryReadyFrame(9, {"n", "o2s", "", ""}, 0, 3, 1, 3, "x"));
    EXPECT_FALSE(v.isMember("filename"));
    EXPECT_FALSE(v.isMember("date_folder"));
    EXPECT_FALSE(v.isMember("rel_dir"));
    EXPECT_FALSE(v.isMember("machine_serial"));
    EXPECT_FALSE(v.isMember("source")) << "D1's source is not sent on an oximetry frame";
    EXPECT_EQ(v["kind"].asString(), "oximetry");
}

TEST(CpapDashOximetryContract, UnsetOptionalKeysAreLeftOutNeverSentEmpty) {
    const auto v = parse(cdstream::oximetryReadyFrame(9, {"n", "hms-json", "", ""}, 0, 3, 1, 3, "x"));
    EXPECT_FALSE(v["oxi"].isMember("source"));
    EXPECT_FALSE(v["oxi"].isMember("device_name"));
    EXPECT_FALSE(parse(cdstream::fileReadyFrame(1, brpHeaders(), 0, 1, 1, 1, "x", "s")).isMember("source"));
}

TEST(CpapDashOximetryContract, TheIdentifierIsNeverEmptyAndAlwaysANameIngestTakes) {
    const std::regex ok("^[A-Za-z0-9_-][A-Za-z0-9._-]{0,63}$");
    for (const std::string& f : std::vector<std::string>{"20261118230512", "garmin-1795006800000",
                                                         "O2Ring export (1).csv", "", std::string(80, 'a'),
                                                         ".hidden", "x.meta", "live-20261118", "\xc3\xbc.vld"}) {
        const auto n = ob::wireName(f);
        EXPECT_TRUE(std::regex_match(n, ok)) << f << " -> " << n;
    }
}

// ── the outbox ───────────────────────────────────────────────────────────────

namespace {

class OutboxTest : public ::testing::Test {
protected:
    fs::path dir_;
    std::vector<std::pair<std::string, std::string>> notes_;   // (root, rel) told to the uploader

    void SetUp() override {
        dir_ = fs::temp_directory_path() /
               ("hms_outbox_" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
        fs::create_directories(dir_);
        ob::setRoot(ob::rootFor(dir_.string()));
        card_feed::setSinks(
            [this](const std::string& r, const std::string& rel, const std::string&, const std::string& src) {
                if (src == card_feed::kSourceOximetry) notes_.emplace_back(r, rel);
            },
            {});
    }
    void TearDown() override {
        card_feed::setSinks({}, {});
        ob::setRoot("");
        fs::remove_all(dir_);
    }
    fs::path outbox() const { return ob::rootFor(dir_.string()); }
    std::optional<ob::Meta> meta(const std::string& file) const { return ob::metaFor(outbox().string(), file); }
};

}  // namespace

TEST_F(OutboxTest, ARingRecordingGoesAsItsOwnBytesWithItsMeta) {
    cpapdash::parser::OximetrySession row;
    row.filename = "20261118230512";
    const std::string raw = o2s(5);
    ASSERT_EQ(ob::stored(row, {"o2ring", "O2Ring-S"}, raw), "20261118230512");
    EXPECT_EQ(readFile(outbox() / "20261118230512"), raw) << "byte for byte (SDD-061 P2)";
    const auto m = meta("20261118230512");
    ASSERT_TRUE(m);
    EXPECT_EQ(m->name, "20261118230512");
    EXPECT_EQ(m->format, "o2s");
    EXPECT_EQ(m->source, "o2ring");
    EXPECT_EQ(m->device_name, "O2Ring-S");
    ASSERT_EQ(notes_.size(), 1u);
    EXPECT_EQ(notes_[0].second, "20261118230512") << "the uploader is told, on the outbox root";
    EXPECT_FALSE(meta("20261118230512.meta")) << "a .meta is never sent itself";

    row.filename = "20260912223000.vld";
    ASSERT_FALSE(ob::stored(row, {}, vld()).empty());
    EXPECT_EQ(meta("20260912223000.vld")->format, "vld");
}

TEST_F(OutboxTest, AGarminNightGoesAsHmsJsonOnItsUtcInstantsWithABlankPulseNull) {
    cpapdash::parser::OximetrySession row;
    row.filename = "garmin-1795006800000";
    row.sample_interval = 60;
    cpapdash::parser::OximetrySample a{}, b{};
    a.spo2 = 95; a.heart_rate = 58;
    b.spo2 = 94; b.heart_rate = 0;   // no pulse within 2 minutes: blank
    a.timestamp = std::chrono::system_clock::from_time_t(1795042800);   // wall clock, UTC+10
    b.timestamp = std::chrono::system_clock::from_time_t(1795042860);
    row.samples = {a, b};
    const std::vector<long long> utc{1795006800, 1795006860};
    ASSERT_EQ(ob::stored(row, {"garmin", "Garmin Fenix 7X"}, "", &utc), "garmin-1795006800000.json");
    const auto body = parse(readFile(outbox() / "garmin-1795006800000.json"));
    EXPECT_EQ(body["clock"].asString(), "utc");
    EXPECT_EQ(body["samples"][0u][0u].asInt64(), 1795006800) << "the instant, not the wall clock";
    EXPECT_TRUE(body["samples"][1u][2u].isNull()) << "a blank pulse is null, never 0 (per field, 2026-11-19)";
    EXPECT_EQ(body["samples"][1u][1u].asInt(), 94) << "and the SpO2 still goes";
    const auto m = meta("garmin-1795006800000.json");
    ASSERT_TRUE(m);
    EXPECT_EQ(m->name, "garmin-1795006800000") << "oxi.name has no .json";
    EXPECT_EQ(m->format, "hms-json");
    EXPECT_EQ(m->source, "garmin");
}

TEST_F(OutboxTest, AGarminNightFetchedAgainRewritesItsFileWhole) {
    cpapdash::parser::OximetrySession row;
    row.filename = "garmin-1";
    row.sample_interval = 60;
    cpapdash::parser::OximetrySample a{};
    a.spo2 = 95; a.heart_rate = 60;
    row.samples = {a};
    std::vector<long long> utc{100};
    ob::stored(row, {"garmin", ""}, "", &utc);
    const auto first = readFile(outbox() / "garmin-1.json");
    row.samples.push_back(a);
    utc.push_back(160);
    ob::stored(row, {"garmin", ""}, "", &utc);
    const auto second = readFile(outbox() / "garmin-1.json");
    EXPECT_NE(first, second);
    EXPECT_EQ(parse(second)["samples"].size(), 2u);
    // A .json is not a growing name: the uploader sends it whole when it changes.
    EXPECT_FALSE(cdstream::isGrowingName("garmin-1.json"));
    IDatabase::CpapDashSent acked;
    acked.acked_to = static_cast<long long>(first.size());
    acked.acked_md5 = cdstream::ackedMd5("garmin-1.json", first, first.size());
    const auto plan = cdstream::planSend("garmin-1.json", second, acked);
    EXPECT_TRUE(plan.send);
    EXPECT_TRUE(plan.whole);
    EXPECT_EQ(plan.offset, 0u);
}

TEST_F(OutboxTest, ACsvUploadGoesAsHmsJsonOnTheWallClock) {
    const std::string csv =
        "Time,Oxygen Level,Pulse Rate,Motion,O2 Reminder,PR Reminder\n"
        "23:00:00 Mar 07 2099,95,60,0,,\n23:00:04 Mar 07 2099,94,61,0,,\n";
    SQLiteDatabase db((dir_ / "hms.db").string());
    ASSERT_TRUE(db.connect());
    const auto r = importOximetryUpload(db, csv, "export.csv", uploadOrigin("", ""));
    ASSERT_FALSE(r.isMember("error")) << r["error"].asString();
    const auto body = parse(readFile(outbox() / "export.csv.json"));
    EXPECT_EQ(body["format"].asString(), "hms-oximetry");
    EXPECT_EQ(body["clock"].asString(), "wall") << "a CSV carries the ring's printed clock only";
    EXPECT_EQ(body["samples"][0u][0u].asInt64(), 4076607600LL) << "2099-03-07 23:00:00 as printed";
    EXPECT_EQ(meta("export.csv.json")->source, "upload");
}

TEST_F(OutboxTest, EveryRingStoreSiteWritesTheOutboxAndARemovedNightWritesNothing) {
    SQLiteDatabase db((dir_ / "hms.db").string());
    ASSERT_TRUE(db.connect());
    // The .vld upload and the .o2s upload.
    ASSERT_FALSE(importOximetryUpload(db, vld(3, 22), "a.vld", uploadOrigin("", "")).isMember("error"));
    EXPECT_EQ(readFile(outbox() / "a.vld"), vld(3, 22));
    ASSERT_FALSE(importOximetryUpload(db, o2s(4), "20260913010203.o2s", uploadOrigin("", "")).isMember("error"));
    EXPECT_EQ(meta("20260913010203.o2s")->format, "o2s");
    // The card-folder scan.
    const fs::path card = dir_ / "card";
    fs::create_directories(card / "OXIMETRY");
    std::ofstream(card / "OXIMETRY" / "b.vld", std::ios::binary) << vld(3, 21);
    VldScanState state;
    importVldFolder(db, card.string(), state);
    EXPECT_EQ(readFile(outbox() / "b.vld"), vld(3, 21));
    // A removed night (SDD-029) is not stored, so it is not written.
    const auto removed = importVldFile(db, vld(3, 20), "c.vld", {"20260912"});
    EXPECT_TRUE(removed.removed_night);
    EXPECT_FALSE(fs::exists(outbox() / "c.vld"));
    EXPECT_FALSE(fs::exists(outbox() / "c.vld.meta"));
}

TEST_F(OutboxTest, ALongOrOddNameIsTheSameHashEveryTime) {
    const std::string odd = "O2Ring export (1).csv";
    const auto a = ob::wireName(odd);
    EXPECT_EQ(a, ob::wireName(odd));
    EXPECT_EQ(a.rfind("hms-", 0), 0u);
    EXPECT_EQ(a.size(), 4u + 16u);
    EXPECT_NE(a, ob::wireName(std::string(80, 'a')));
    EXPECT_EQ(ob::wireName("20261118230512"), "20261118230512") << "a plain name is kept";
}

TEST_F(OutboxTest, EachLiveSampleIsOneLineWithTheInstantItWasRead) {
    const auto read_at = std::chrono::system_clock::from_time_t(1763506804);
    const auto wall = std::chrono::system_clock::from_time_t(1763542804);   // the day's wall clock
    const auto file = ob::liveSample(read_at, wall, 96, 58, 0);
    EXPECT_EQ(file, ob::liveSpoolName(wall));
    EXPECT_TRUE(ob::isLiveSpool(file));
    ob::liveSample(read_at + std::chrono::seconds(4), wall, 95, 57, 1);
    const auto body = readFile(outbox() / file);
    EXPECT_EQ(body, "{\"t\":1763506804,\"spo2\":96,\"hr\":58,\"motion\":0}\n"
                    "{\"t\":1763506808,\"spo2\":95,\"hr\":57,\"motion\":1}\n");
    const auto m = meta(file);
    ASSERT_TRUE(m);
    EXPECT_EQ(m->format, "live-jsonl");
    EXPECT_EQ(m->name, file.substr(0, file.size() - 6)) << "live-<YYYYMMDD>";
    EXPECT_EQ(notes_.size(), 2u) << "each stored sample is an event";

    // A range from an acked offset is whole lines.
    EXPECT_TRUE(cdstream::isGrowingName(file));
    const std::string first_line = body.substr(0, body.find('\n') + 1);
    IDatabase::CpapDashSent acked;
    acked.acked_to = static_cast<long long>(first_line.size());
    acked.acked_md5 = cdstream::ackedMd5(file, first_line, first_line.size());
    const auto plan = cdstream::planSend(file, body, acked);
    EXPECT_TRUE(plan.send);
    EXPECT_FALSE(plan.whole);
    EXPECT_EQ(plan.offset, first_line.size());
    EXPECT_EQ(body.substr(plan.offset).back(), '\n');
}

TEST_F(OutboxTest, TheLiveSpoolIsDatedAsTheDatabasesLiveSession) {
    const auto wall = std::chrono::system_clock::from_time_t(1763506804);
    const std::string db_name = liveOximetryFilename(wall);   // live_<YYYYMMDD>.vld
    EXPECT_EQ(ob::liveSpoolName(wall), "live-" + db_name.substr(5, 8) + ".jsonl");
}

TEST_F(OutboxTest, NoRootWritesNothing) {
    ob::setRoot("");
    cpapdash::parser::OximetrySession row;
    row.filename = "x";
    EXPECT_EQ(ob::stored(row, {}, vld()), "");
    EXPECT_EQ(ob::liveSample(std::chrono::system_clock::now(), std::chrono::system_clock::now(), 95, 60, 0), "");
    EXPECT_TRUE(notes_.empty());
}

TEST_F(OutboxTest, ARecordingStoredBefore700GoesAsItsRetainedRawFileElseAsHmsJson) {
    SQLiteDatabase db((dir_ / "hms.db").string());
    ASSERT_TRUE(db.connect());
    // Two recordings stored before the outbox existed.
    ob::setRoot("");
    ASSERT_TRUE(importVldFile(db, vld(3, 22), "kept.vld").ok);
    ASSERT_TRUE(importVldFile(db, vld(4, 21), "lost.vld").ok);
    db.saveLiveOximetrySample(kOximetryDeviceId, std::chrono::system_clock::from_time_t(1763506804), 96, 60, 0);
    ob::setRoot(ob::rootFor(dir_.string()));
    // The ring's raw file of the first was kept beside the card (SDD-028).
    const fs::path archive = dir_ / "cpap_data";
    fs::create_directories(archive / "DATALOG");
    std::ofstream(archive / "DATALOG" / "kept.vld", std::ios::binary) << vld(3, 22);
    // A raw file of the same name that is another recording is not used.
    std::ofstream(archive / "DATALOG" / "lost.vld", std::ios::binary) << vld(4, 5);

    EXPECT_EQ(ob::backfill(db, archive.string()), 2);
    EXPECT_EQ(readFile(outbox() / "kept.vld"), vld(3, 22)) << "the retained file, byte for byte";
    EXPECT_EQ(meta("kept.vld")->format, "vld");
    ASSERT_TRUE(fs::exists(outbox() / "lost.vld.json")) << "no matching raw file: hms-json from its samples";
    const auto body = parse(readFile(outbox() / "lost.vld.json"));
    EXPECT_EQ(body["clock"].asString(), "wall");
    EXPECT_EQ(body["samples"].size(), 4u);
    EXPECT_EQ(meta("lost.vld.json")->source, "o2ring");
    for (const auto& e : fs::directory_iterator(outbox()))
        EXPECT_EQ(e.path().filename().string().rfind("live", 0), std::string::npos) << "never a live row (D7)";

    EXPECT_EQ(ob::backfill(db, archive.string()), 0) << "once in the outbox, a walk writes nothing again";
}

// ── every store site calls the outbox (D4) ──────────────────────────────────

TEST(CpapDashOximetrySites, EveryOximetryStoreCallsTheOutbox) {
    // The store calls outside the database layer, and where each must be
    // followed by the outbox. A new store site fails here until it calls it.
    const std::vector<std::string> sites = {
        "src/services/OximetryService.cpp",       // the ring over Bluetooth
        "src/services/OximetryImport.cpp",        // .vld/.o2s upload, card-folder scan, CSV upload
        "src/services/GarminService.cpp",         // Garmin
        "src/services/BurstCollectorService.cpp", // live samples
    };
    const std::regex store(R"((saveOximetrySession|saveOximetrySessionFrom|saveLiveOximetrySample)\s*\()");
    int calls = 0;
    std::set<std::string> found;
    for (const auto& e : fs::recursive_directory_iterator(sourceDir() / "src")) {
        if (!e.is_regular_file()) continue;
        const auto rel = fs::relative(e.path(), sourceDir()).generic_string();
        if (rel.rfind("src/database/", 0) == 0) continue;
        const auto ext = e.path().extension().string();
        if (ext != ".cpp" && ext != ".h" && ext != ".mm") continue;
        std::istringstream in(readFile(e.path()));
        std::vector<std::string> lines;
        for (std::string l; std::getline(in, l);) lines.push_back(l);
        for (size_t i = 0; i < lines.size(); ++i) {
            if (!std::regex_search(lines[i], store)) continue;
            ++calls;
            found.insert(rel);
            bool outboxed = false;
            for (size_t j = i; j < std::min(lines.size(), i + 14); ++j)
                if (lines[j].find("oximetry_outbox::") != std::string::npos) outboxed = true;
            EXPECT_TRUE(outboxed) << rel << ":" << i + 1 << " stores oximetry without the outbox";
        }
    }
    EXPECT_EQ(found, std::set<std::string>(sites.begin(), sites.end()));
    EXPECT_EQ(calls, 5) << "the BLE pull, importVldFile (upload and scan), the CSV upload, Garmin, the live sample";
}
