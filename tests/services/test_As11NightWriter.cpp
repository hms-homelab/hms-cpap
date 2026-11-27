#include <gtest/gtest.h>

#include "services/As11CardEdf.h"
#include "services/As11NightWriter.h"
#include "services/As11StrWriter.h"

#include <cpapdash/parser/EDFFile.h>

#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <map>
#include <string>
#include <vector>

using namespace hms_cpap;
using namespace hms_cpap::as11;
namespace fs = std::filesystem;

// SDD-057 step 4: an AirSense 11's night laid out as its card would hold it,
// from stream blocks and the machine's events, and its STR.edf from the
// Summary. Every value is made up; no real night.

namespace {

constexpr const char* kSerial = "23000000001";
// 2026-10-17 03:00:00 UTC, which is 22:00 on the 16th at UTC-5: the night of
// the 16th.
constexpr int64_t kT0 = 1792206000000LL;

class NightTest : public ::testing::Test {
protected:
    fs::path dir_;
    void SetUp() override {
        dir_ = fs::temp_directory_path() /
               ("as11_night_" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
        fs::create_directories(dir_);
    }
    void TearDown() override {
        std::error_code ec;
        fs::remove_all(dir_, ec);
    }

    As11NightWriter writer() { return As11NightWriter(dir_.string(), fixedOffsetClock(-300)); }
    fs::path night() const { return dir_ / "as11" / kSerial / "DATALOG" / "20261016"; }
};

// One second of stream: 25 fast samples, and the slow channels given.
StreamBlock second(int64_t start, std::map<uint8_t, int16_t> slow = {}) {
    StreamBlock b;
    b.start_ms = start;
    b.serial = kSerial;
    b.channels.push_back({kFlow, 0, std::vector<int16_t>(25, 300)});
    b.channels.push_back({kMaskPressure, 0, std::vector<int16_t>(25, 900)});
    for (const auto& [id, v] : slow) b.channels.push_back({id, 1, {v}});
    return b;
}

// The EDF header's number of data records.
long records(const fs::path& p) {
    std::ifstream in(p, std::ios::binary);
    char f[9] = {};
    in.seekg(236);
    in.read(f, 8);
    return std::atol(f);
}

std::vector<std::string> files(const fs::path& d) {
    std::vector<std::string> out;
    std::error_code ec;
    for (const auto& f : fs::directory_iterator(d, ec)) out.push_back(f.path().filename().string());
    std::sort(out.begin(), out.end());
    return out;
}

} // namespace

TEST(As11Clock, SleepDayAndOffset) {
    using cpapdash::parser::WallClock;
    EXPECT_EQ(sleepDayFolder(WallClock{2026, 10, 17, 3, 0, 0}), "20261016");
    EXPECT_EQ(sleepDayFolder(WallClock{2026, 10, 17, 12, 0, 0}), "20261017");
    EXPECT_EQ(sleepDayFolder(WallClock{2026, 3, 1, 1, 0, 0}), "20260228");
    const auto wc = fixedOffsetClock(-300)(kT0);
    EXPECT_EQ(wc.day, 16);
    EXPECT_EQ(wc.hour, 22);
    // The machine's noon at UTC-5 is 17:00 UTC.
    EXPECT_EQ(machineOffsetFromDayStart(1792256400000LL), -300);   // 2026-10-17 17:00 UTC
    EXPECT_EQ(machineOffsetFromDayStart(1792231200000LL), 120);    // noon at UTC+2 is 10:00 UTC
    EXPECT_FALSE(machineOffsetFromDayStart(1792231200000LL + 7 * 60000).has_value());
}

TEST_F(NightTest, ASegmentIsCutAtASixtySecondPause) {
    auto w = writer();
    for (int i = 0; i < 30; i++) w.onBlock(second(kT0 + i * 1000));
    for (int i = 0; i < 20; i++) w.onBlock(second(kT0 + 30000 + 61000 + i * 1000));   // 61 s later
    w.onStreamEnd(kSerial);

    const auto names = files(night());
    ASSERT_EQ(std::count_if(names.begin(), names.end(), [](const std::string& n) { return n.find("_BRP") != std::string::npos; }), 2)
        << ::testing::PrintToString(names);
    EXPECT_EQ(names.front(), "20261016_220000_BRP.edf");
    EXPECT_EQ(records(night() / "20261016_220000_BRP.edf"), 30);
    EXPECT_EQ(records(night() / "20261016_220131_BRP.edf"), 20);
    // No slow channel and no ring: no PLD, no SA2. One EVE and one CSL.
    EXPECT_FALSE(fs::exists(night() / "20261016_220000_PLD.edf"));
    EXPECT_FALSE(fs::exists(night() / "20261016_220000_SA2.edf"));
    EXPECT_TRUE(fs::exists(night() / "20261016_220000_EVE.edf"));
    EXPECT_TRUE(fs::exists(night() / "20261016_220000_CSL.edf"));
}

TEST_F(NightTest, AShortGapStaysOneSegmentAndKeepsItsTime) {
    auto w = writer();
    for (int i = 0; i < 10; i++) w.onBlock(second(kT0 + i * 1000));
    for (int i = 0; i < 10; i++) w.onBlock(second(kT0 + 40000 + i * 1000));   // a 30 s hole
    w.onStreamEnd(kSerial);
    EXPECT_EQ(records(night() / "20261016_220000_BRP.edf"), 50);
}

TEST_F(NightTest, ASubSampleGapIsLaidToTheNearestSampleAndDoesNotDrift) {
    auto w = writer();
    // Each second starts 39 ms after the last one ended, about a sample lost
    // each time. The 51 seconds span 52.95 s of the machine's time, so the
    // samples laid at their time come to 1324, 52 whole records. Rounding
    // each gap down laid none of them (1275 samples, 51 records), and every
    // sample after a gap stood earlier than the card has it.
    for (int i = 0; i < 51; i++) w.onBlock(second(kT0 + i * 1039));
    w.onStreamEnd(kSerial);
    EXPECT_EQ(records(night() / "20261016_220000_BRP.edf"), 52);
}

// The machine stamps its seconds about 1000 ms apart, a few ms either way.
// Seen on a real night: the offset settled at exactly half a sample (20 ms)
// from the segment's first block, and every second then rounded the other
// way for a quarter of an hour: a real sample dropped, then a 0 cmH2O
// invented. A wander well under a sample must lay every sample, once.
TEST_F(NightTest, AWanderUnderASampleDropsNothingAndInventsNothing) {
    auto w = writer();
    for (int i = 0; i < 40; i++) {
        const int64_t off = i < 10 ? 0 : (i < 20 ? -16 : -20);
        w.onBlock(second(kT0 + i * 1000 + off));
    }
    w.onStreamEnd(kSerial);

    const auto f = night() / "20261016_220000_BRP.edf";
    EXPECT_EQ(records(f), 40);
    cpapdash::parser::EDFFile edf;
    ASSERT_TRUE(edf.open(f.string()));
    ASSERT_EQ(edf.signals[1].label, "Press.40ms");
    std::vector<double> press;
    edf.readSignal(1, press);
    ASSERT_EQ(press.size(), 1000u);
    for (size_t i = 0; i < press.size(); i++) EXPECT_GT(press[i], 5.0) << "sample " << i;
}

TEST_F(NightTest, AResentBlockIsKeptOnceAndSurvivesARestart) {
    {
        auto w = writer();
        for (int i = 0; i < 10; i++) w.onBlock(second(kT0 + i * 1000));
        w.onBlock(second(kT0 + 3000));   // the same block again
    }
    auto w = writer();                   // a restart: what is on disk is read back
    w.onBlock(second(kT0 + 5000));       // resent after the restart
    w.onBlock(second(kT0 + 10000));
    w.onStreamEnd(kSerial);
    EXPECT_EQ(records(night() / "20261016_220000_BRP.edf"), 11);
}

TEST_F(NightTest, TheOpenSegmentIsWrittenLiveEveryTenSeconds) {
    auto w = writer();
    for (int i = 0; i < 9; i++) w.onBlock(second(kT0 + i * 1000));
    EXPECT_FALSE(fs::exists(night() / "20261016_220000_BRP.edf"));
    w.onBlock(second(kT0 + 9000));
    EXPECT_EQ(records(night() / "20261016_220000_BRP.edf"), 10);
}

TEST_F(NightTest, SlowChannelsMakeAPldAndTheRingAnSa2) {
    auto w = writer();
    for (int i = 0; i < 20; i++)
        w.onBlock(second(kT0 + i * 1000, {{kLeak, 200}, {kRespRate, 150}, {kSpO2, 960}, {kHeartRate, 62}}));
    w.onStreamEnd(kSerial);
    EXPECT_EQ(records(night() / "20261016_220000_PLD.edf"), 10);   // 2 s records
    EXPECT_EQ(records(night() / "20261016_220000_SA2.edf"), 20);   // 1 s records
    EXPECT_FALSE(fs::exists(night() / "20261016_220000_SAD.edf"));
}

// Ticket 179: an ASV's streamed target ventilation is in the night's PLD as
// its card has it, TgtVent.2s in L/min, read back by the library's reader.
TEST_F(NightTest, AnAsvsTargetVentilationIsThePldsTgtVent) {
    auto w = writer();
    for (int i = 0; i < 20; i++)
        w.onBlock(second(kT0 + i * 1000, {{kMinuteVent, 650}, {kTargetVent2s, static_cast<int16_t>(500 + i * 10)}}));
    w.onStreamEnd(kSerial);
    const auto path = night() / "20261016_220000_PLD.edf";
    ASSERT_EQ(records(path), 10);
    std::ifstream in(path, std::ios::binary);
    const std::vector<uint8_t> bytes((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    cpapdash::parser::EDFFile edf;
    ASSERT_TRUE(edf.open(bytes.data(), bytes.size()));
    std::vector<double> tgt, mv;
    ASSERT_EQ(edf.readSignal(edf.findSignal("TgtVent"), tgt), 10);
    ASSERT_EQ(edf.readSignal(edf.findSignal("MinVent"), mv), 10);
    // Each 2 s slot holds the last value that arrived in it (seconds 1, 3, ...).
    for (size_t k = 0; k < 10; k++) EXPECT_NEAR(tgt[k], (500 + (2 * k + 1) * 10) / 100.0, 0.13) << k;
    EXPECT_NEAR(mv[0], 6.5, 0.13);
}

TEST_F(NightTest, AMachineThatSentNoTargetVentilationHasNoTgtVent) {
    auto w = writer();
    for (int i = 0; i < 20; i++) w.onBlock(second(kT0 + i * 1000, {{kMinuteVent, 650}}));
    w.onStreamEnd(kSerial);
    const auto path = night() / "20261016_220000_PLD.edf";
    std::ifstream in(path, std::ios::binary);
    const std::vector<uint8_t> bytes((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    cpapdash::parser::EDFFile edf;
    ASSERT_TRUE(edf.open(bytes.data(), bytes.size()));
    EXPECT_LT(edf.findSignal("TgtVent"), 0);
    EXPECT_GE(edf.findSignal("MinVent"), 0);
}

TEST_F(NightTest, AnOlderSadOfTheSameRecordingIsRemovedWhenTheSa2IsWritten) {
    fs::create_directories(night());
    { std::ofstream(night() / "20261016_220000_SAD.edf") << "old"; }
    auto w = writer();
    for (int i = 0; i < 20; i++)
        w.onBlock(second(kT0 + i * 1000, {{kLeak, 200}, {kSpO2, 960}, {kHeartRate, 62}}));
    w.onStreamEnd(kSerial);
    EXPECT_TRUE(fs::exists(night() / "20261016_220000_SA2.edf"));
    EXPECT_FALSE(fs::exists(night() / "20261016_220000_SAD.edf"));
}

TEST_F(NightTest, EventsLandOnTheirNightWithTheTwoMinuteTail) {
    auto w = writer();
    for (int i = 0; i < 60; i++) w.onBlock(second(kT0 + i * 1000));
    // ISO times: kT0 is 2026-10-17T03:00:00Z.
    w.onEvent(kSerial, {"ObstructiveApneaEnd", "2026-10-17T03:00:30.000Z", 12, 12});
    w.onEvent(kSerial, {"HypopneaEnd", "2026-10-17T03:02:30.000Z", 15, 15});   // 90 s past the end: kept
    w.onEvent(kSerial, {"HypopneaEnd", "2026-10-17T03:05:00.000Z", 15, 15});   // 4 min past: dropped
    w.onEvent(kSerial, {"ObstructiveApneaEnd", "2026-10-17T03:00:30.000Z", 12, 12});  // again: once
    w.onEvent(kSerial, {"ObstructiveApneaEnd", "2026-10-17T03:00:30Z", 12, 12, true});
    w.onEvent(kSerial, {"CsrStart", "2026-10-17T03:00:50.000Z", 0, 20});
    w.onStreamEnd(kSerial);
    // Records are "Recording starts" plus one per annotation.
    EXPECT_EQ(records(night() / "20261016_220000_EVE.edf"), 3);
    EXPECT_EQ(records(night() / "20261016_220000_CSL.edf"), 2);
}

TEST_F(NightTest, OneEveAndCslForANightOfSeveralSegments) {
    auto w = writer();
    for (int i = 0; i < 20; i++) w.onBlock(second(kT0 + i * 1000));
    for (int i = 0; i < 20; i++) w.onBlock(second(kT0 + 3600000 + i * 1000));
    w.onEvent(kSerial, {"ApneaEnd", "2026-10-17T04:00:10.000Z", 10, 10});
    w.onStreamEnd(kSerial);
    const auto names = files(night());
    EXPECT_EQ(std::count_if(names.begin(), names.end(), [](const std::string& n) { return n.find("_EVE") != std::string::npos; }), 1);
    EXPECT_EQ(records(night() / "20261016_220000_EVE.edf"), 2);
}

TEST_F(NightTest, IdentificationIsWrittenOnceAndNeverReplaced) {
    const auto root = dir_ / "as11" / kSerial;
    const std::string id = R"({"IdentificationProfiles":{"Product":{"SerialNumber":"23000000001","ProductCode":"39999"},"Software":{"PlatformIdentifier":"36","VariantIdentifier":"1"}}})";
    ASSERT_TRUE(writeIdentification(root.string(), id));
    std::ifstream in(root / "Identification.json");
    std::string text((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    EXPECT_EQ(text, std::string("{\"FlowGenerator\":") + id + "}");
    EXPECT_TRUE(writeIdentification(root.string(), id));   // same machine: nothing to do
    const std::string other = R"({"IdentificationProfiles":{"Product":{"SerialNumber":"23999999999"}}})";
    EXPECT_FALSE(writeIdentification(root.string(), other));
    EXPECT_FALSE(writeIdentification(root.string(), R"({"x":1})"));
}

TEST_F(NightTest, IdentificationCrcIsKeptBesideItAsTheCardKeepsIt) {
    const auto root = dir_ / "as11" / kSerial;
    const std::string id = R"({"IdentificationProfiles":{"Product":{"SerialNumber":"23000000001"}}})";
    auto crcOf = [&](const fs::path& json) {
        std::ifstream in(json, std::ios::binary);
        std::string text((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
        return crc32(reinterpret_cast<const uint8_t*>(text.data()), text.size());
    };
    auto stored = [&]() {
        std::ifstream in(root / "Identification.crc", std::ios::binary);
        std::string b((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
        if (b.size() != 4) return uint32_t{0};
        return static_cast<uint32_t>(static_cast<uint8_t>(b[0])) | (static_cast<uint32_t>(static_cast<uint8_t>(b[1])) << 8) |
               (static_cast<uint32_t>(static_cast<uint8_t>(b[2])) << 16) | (static_cast<uint32_t>(static_cast<uint8_t>(b[3])) << 24);
    };
    ASSERT_TRUE(writeIdentification(root.string(), id));
    ASSERT_TRUE(fs::exists(root / "Identification.crc"));
    EXPECT_EQ(stored(), crcOf(root / "Identification.json"));

    // A card's own .crc is not replaced.
    { std::ofstream(root / "Identification.crc", std::ios::binary | std::ios::trunc) << "card"; }
    EXPECT_TRUE(writeIdentification(root.string(), id));
    {
        // Scoped: Windows will not remove a file that is still open (below).
        std::ifstream in(root / "Identification.crc", std::ios::binary);
        EXPECT_EQ(std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>()), "card");
    }

    // A file written before the .crc existed gets one.
    fs::remove(root / "Identification.crc");
    EXPECT_TRUE(writeIdentification(root.string(), id));
    EXPECT_EQ(stored(), crcOf(root / "Identification.json"));
}

// ── STR.edf from the Summary ──

namespace {

using Bytes = std::vector<uint8_t>;
Bytes varint(uint64_t n) {
    Bytes out;
    do { uint8_t b = n & 0x7F; n >>= 7; out.push_back(n ? (b | 0x80) : b); } while (n);
    return out;
}
void append(Bytes& to, const Bytes& b) { to.insert(to.end(), b.begin(), b.end()); }
Bytes fieldVarint(uint32_t num, uint64_t v) { Bytes o = varint(uint64_t(num) << 3); append(o, varint(v)); return o; }
Bytes fieldBytes(uint32_t num, const Bytes& v) {
    Bytes o = varint((uint64_t(num) << 3) | 2); append(o, varint(v.size())); append(o, v); return o;
}
constexpr uint64_t kDay = 86'400'000ULL;
// 2026-03-01 at the machine's noon, UTC-5.
constexpr uint64_t kNoonEst = 1'772'366'400'000ULL + 5 * 3'600'000ULL;

Bytes summaryDay(uint64_t start_ms, int usage) {
    Bytes rec;
    append(rec, fieldVarint(2, start_ms));
    append(rec, fieldVarint(3, start_ms + kDay));
    append(rec, fieldVarint(5, static_cast<uint64_t>(usage)));
    Bytes sess;
    if (usage) {
        Bytes s;
        append(s, fieldVarint(1, start_ms + 36'000'000));
        append(s, fieldVarint(2, static_cast<uint64_t>(usage)));
        append(sess, fieldBytes(1, s));
    }
    append(rec, fieldBytes(6, sess));
    append(rec, fieldVarint(7, 150));
    append(rec, fieldVarint(39, usage ? 1 : 0));
    return fieldBytes(2, rec);
}

struct Sig { const char* label; int dmin, dmax; double pmin, pmax; int spr; };

// A small STR layout: an EDF header with no records, as a layout file is.
Bytes layout() {
    const std::vector<Sig> sigs = {
        {"Date", 0, 24836, 0, 24836, 1},       {"MaskOn", 0, 1440, 0, 1440, 10},
        {"MaskOff", 0, 1440, 0, 1440, 10},     {"MaskEvents", 0, 255, 0, 255, 1},
        {"Duration", 0, 1440, 0, 1440, 1},     {"AHI", 0, 32767, 0, 32.767, 1},
        {"Crc16", -32768, 32767, -32768, 32767, 1},
    };
    const size_t ns = sigs.size();
    std::string h(256 + 256 * ns, ' ');
    auto put = [&](size_t at, size_t w, const std::string& v) { h.replace(at, std::min(w, v.size()), v.substr(0, w)); };
    put(0, 8, "0");
    put(8, 80, "X X X X");
    put(168, 8, "01.01.00");
    put(176, 8, "12.00.00");
    put(184, 8, std::to_string(h.size()));
    put(236, 8, "0");
    put(244, 8, "86400");
    put(252, 4, std::to_string(ns));
    size_t at = 256;
    auto each = [&](size_t w, auto f) { for (size_t i = 0; i < ns; i++) put(at + i * w, w, f(sigs[i])); at += w * ns; };
    each(16, [](const Sig& s) { return std::string(s.label); });
    each(80, [](const Sig&) { return std::string(); });
    each(8, [](const Sig&) { return std::string(); });
    each(8, [](const Sig& s) { return std::to_string(s.pmin); });
    each(8, [](const Sig& s) { return std::to_string(s.pmax); });
    each(8, [](const Sig& s) { return std::to_string(s.dmin); });
    each(8, [](const Sig& s) { return std::to_string(s.dmax); });
    each(80, [](const Sig&) { return std::string(); });
    each(8, [](const Sig& s) { return std::to_string(s.spr); });
    each(32, [](const Sig&) { return std::string(); });
    return Bytes(h.begin(), h.end());
}

} // namespace

TEST_F(NightTest, StrFromTheSummaryInTheModelsLayout) {
    const auto root = dir_ / "as11" / kSerial;
    const auto layouts = dir_ / "layouts";
    fs::create_directories(layouts);
    {
        const auto l = layout();
        std::ofstream(layouts / "39999.edfh", std::ios::binary).write(reinterpret_cast<const char*>(l.data()), l.size());
    }

    // Nothing to build from yet: each missing piece says so.
    EXPECT_NE(writeStrFromSummaries(root.string(), layouts.string()).why.find("Identification"), std::string::npos);
    ASSERT_TRUE(writeIdentification(root.string(),
        R"({"IdentificationProfiles":{"Product":{"SerialNumber":"23000000001","ProductCode":"39999"},"Software":{"PlatformIdentifier":"36","VariantIdentifier":"1"}}})"));
    EXPECT_NE(writeStrFromSummaries(root.string(), layouts.string()).why.find("Summary"), std::string::npos);

    // Three days with one the machine did not report between them.
    Bytes spool;
    append(spool, summaryDay(kNoonEst, 420));
    append(spool, summaryDay(kNoonEst + kDay, 0));
    append(spool, summaryDay(kNoonEst + 3 * kDay, 380));
    ASSERT_TRUE(keepSummary(root.string(), "2026-03-01", spool));

    int off = 0;
    ASSERT_TRUE(machineOffsetFromSummaries(root.string(), &off));
    EXPECT_EQ(off, -300);

    const auto r = writeStrFromSummaries(root.string(), layouts.string());
    ASSERT_TRUE(r.written) << r.why;
    EXPECT_EQ(r.days, 4);   // 1, 2, 3 (not reported), 4 March
    const auto str = root / "STR.edf";
    EXPECT_EQ(records(str), 4);
    const size_t rec_bytes = 2 * (1 + 10 + 10 + 1 + 1 + 1 + 1);
    EXPECT_EQ(fs::file_size(str), layout().size() + 4 * rec_bytes);
    std::string head(256, '\0');
    {
        // Scoped: Windows will not remove a file that is still open (below).
        std::ifstream in(str, std::ios::binary);
        in.read(head.data(), 256);
    }
    EXPECT_EQ(head.substr(168, 8), "01.03.26");
    EXPECT_NE(head.substr(88, 80).find("SRN=23000000001"), std::string::npos);
    EXPECT_EQ(head.substr(8, 7), "X X X X");

    // A model with no layout writes nothing and says which.
    fs::remove(layouts / "39999.edfh");
    fs::remove(str);
    const auto none = writeStrFromSummaries(root.string(), layouts.string());
    EXPECT_FALSE(none.written);
    EXPECT_NE(none.why.find("39999"), std::string::npos);
    EXPECT_FALSE(fs::exists(str));
}

// ── SDD-070: a night the stream missed, from the machine's stored pages ──

namespace {

// One of the bench night's stored pages (the parser's fixtures, copied).
std::vector<uint8_t> storedFixture(const std::string& name) {
    for (const char* base : {"tests/fixtures/airsense11_pages/", "../tests/fixtures/airsense11_pages/",
                             "../../tests/fixtures/airsense11_pages/"}) {
        std::ifstream in(std::string(base) + name, std::ios::binary);
        if (in) return {std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
    }
    return {};
}

// Keeps the four signal fixtures as page 0 of their spools; the first flow
// sample's time, or 0 when a fixture is not found.
int64_t keepBenchPages(const std::string& root, const std::string& night) {
    const std::pair<const char*, const char*> spools[] = {
        {"RespiratoryFlow6p25Hz", "RespiratoryFlow6p25Hz_first4.bin"},
        {"MaskPressure6p25Hz", "MaskPressure6p25Hz_first4.bin"},
        {"Leak0p5Hz", "Leak0p5Hz.bin"},
        {"InspiratoryPressure0p5Hz", "InspiratoryPressure0p5Hz.bin"}};
    for (const auto& [spool, file] : spools) {
        const auto bytes = storedFixture(file);
        if (bytes.empty() || !keepStoredPage(root, night, spool, 0, bytes)) return 0;
    }
    const auto flow = storedChunks(keptStoredPages(root, night, "RespiratoryFlow6p25Hz"));
    return flow.empty() ? 0 : flow.front().start_ms;
}

// The bench night's stored events as page 0 of their spool; how many fall in
// [from, to].
size_t keepBenchEvents(const std::string& root, const std::string& night, int64_t from, int64_t to) {
    const auto bytes = storedFixture("TherapyEvents-RespiratoryEvents.bin");
    if (bytes.empty() || !keepStoredPage(root, night, kStoredEventsSpool, 0, bytes)) return 0;
    size_t n = 0;
    for (const auto& e : storedEvents({bytes}))
        if (e.onset_ms >= from && e.onset_ms <= to) n++;
    return n;
}

// "2026-11-04T03:00:01.000Z" for a UTC instant, as the stream reports one.
std::string isoOf(int64_t utc_ms) {
    const auto wc = fixedOffsetClock(0)(utc_ms);
    char buf[32];
    std::snprintf(buf, sizeof(buf), "%04d-%02d-%02dT%02d:%02d:%02d.%03dZ", wc.year, wc.month, wc.day,
                  wc.hour, wc.minute, wc.second, static_cast<int>(utc_ms % 1000));
    return buf;
}

std::vector<std::string> named(const std::vector<std::string>& names, const std::string& kind) {
    std::vector<std::string> out;
    for (const auto& n : names) if (n.find("_" + kind + ".edf") != std::string::npos) out.push_back(n);
    return out;
}

} // namespace

TEST_F(NightTest, AnUnstreamedNightIsFilledFromItsStoredPagesOnce) {
    // The bench night's pages, placed in the night the machine's clock puts them.
    const fs::path root = dir_ / "as11" / kSerial;
    const std::string probe = "probe";
    const int64_t t0 = keepBenchPages(root.string(), probe);
    if (t0 == 0) GTEST_SKIP() << "bench page fixtures not found";
    const std::string night = sleepDayFolder(fixedOffsetClock(-300)(t0));
    fs::rename(root / "stored" / probe, root / "stored" / night);

    auto w = writer();
    int ended = 0;   // the night is imported again once its files changed
    w.setNightEndedHook([&](const std::string& serial) { EXPECT_EQ(serial, kSerial); ended++; });
    const std::vector<StoredSpan> on = {{t0 - 60 * 60 * 1000LL, t0 + 8 * 60 * 60 * 1000LL}};
    const int n = w.writeStoredGaps(kSerial, night, on);
    EXPECT_EQ(ended, 1);
    const auto names = files(root / "DATALOG" / night);
    const auto brp = named(names, "BRP"), pld = named(names, "PLD");
    ASSERT_FALSE(brp.empty()) << ::testing::PrintToString(names);
    ASSERT_FALSE(pld.empty()) << ::testing::PrintToString(names);
    EXPECT_EQ(n, static_cast<int>(brp.size() + pld.size()));

    // The BRP is the stored rate: 4 s records, 25 samples each (6.25 Hz).
    std::string head(256 + 3 * 256, '\0');
    {
        std::ifstream in(root / "DATALOG" / night / brp.front(), std::ios::binary);
        in.read(head.data(), static_cast<std::streamsize>(head.size()));
    }
    EXPECT_EQ(std::atof(head.substr(244, 8).c_str()), 4.0);
    EXPECT_EQ(std::atol(head.substr(256 + 3 * 216, 8).c_str()), 25);   // first signal's samples per record
    // D4: the recording field says it came from the stored pages, after the
    // card's own fields.
    const std::string recording = head.substr(88, 80);
    EXPECT_NE(recording.find(std::string("SRN=") + kSerial), std::string::npos) << recording;
    EXPECT_NE(recording.find("STORED=6.25Hz"), std::string::npos) << recording;

    // Run again: every name is there already, nothing is written or replaced.
    const auto before = fs::last_write_time(root / "DATALOG" / night / brp.front());
    EXPECT_EQ(w.writeStoredGaps(kSerial, night, on), 0);
    EXPECT_EQ(fs::last_write_time(root / "DATALOG" / night / brp.front()), before);
}

TEST_F(NightTest, AStreamedStretchIsNotFilledFromStoredPages) {
    const fs::path root = dir_ / "as11" / kSerial;
    const int64_t t0 = keepBenchPages(root.string(), "probe");
    if (t0 == 0) GTEST_SKIP() << "bench page fixtures not found";
    const std::string night = sleepDayFolder(fixedOffsetClock(-300)(t0));
    fs::rename(root / "stored" / "probe", root / "stored" / night);

    // Two minutes streamed from the first stored sample.
    auto w = writer();
    for (int i = 0; i < 120; i++) w.onBlock(second(t0 + i * 1000));
    w.onStreamEnd(kSerial);
    const std::string streamed = named(files(root / "DATALOG" / night), "BRP").front();
    EXPECT_EQ(records(root / "DATALOG" / night / streamed), 120);

    const std::vector<StoredSpan> on = {{t0, t0 + 8 * 60 * 60 * 1000LL}};
    w.writeStoredGaps(kSerial, night, on);
    // The streamed BRP is untouched; anything stored starts after it ends.
    EXPECT_EQ(records(root / "DATALOG" / night / streamed), 120);
    const std::string after_stream = cpapdash::parser::resmedFileName(fixedOffsetClock(-300)(t0 + 120 * 1000), "BRP");
    for (const auto& b : named(files(root / "DATALOG" / night), "BRP"))
        if (b != streamed) EXPECT_GE(b, after_stream) << b;
}

namespace {

// The bench pages and events placed in the night their first sample falls
// in; that sample's time (0 when a fixture is missing) and the night.
std::pair<int64_t, std::string> benchNight(const fs::path& root) {
    const int64_t t0 = keepBenchPages(root.string(), "probe");
    if (t0 == 0) return {0, ""};
    const std::string night = sleepDayFolder(fixedOffsetClock(-300)(t0));
    fs::rename(root / "stored" / "probe", root / "stored" / night);
    return {t0, night};
}

constexpr int64_t kHour = 60 * 60 * 1000LL;

std::string eveOf(const std::string& brp) { return brp.substr(0, brp.find("_BRP")) + "_EVE.edf"; }

} // namespace

// The bench events page holds a later night than the signal pages' first
// page (2026-11-04 against 11-02): these place the events in their own night,
// whose first event's time this returns with the night.
std::pair<int64_t, std::string> benchEventsNight() {
    const auto stored = storedEvents({storedFixture("TherapyEvents-RespiratoryEvents.bin")});
    if (stored.empty()) return {0, ""};
    const int64_t te = stored.front().onset_ms;
    return {te, sleepDayFolder(fixedOffsetClock(-300)(te))};
}

TEST_F(NightTest, AnUnstreamedNightWithOnlyStoredEventsGetsAnEve) {
    const fs::path root = dir_ / "as11" / kSerial;
    const auto [te, night] = benchEventsNight();
    if (te == 0) GTEST_SKIP() << "bench events fixture not found";
    const int64_t on = te - 10 * 60 * 1000LL;
    const size_t in_night = keepBenchEvents(root.string(), night, on, on + 8 * kHour);
    ASSERT_GT(in_night, 1u);

    // No waveform page for this night: the EVE goes under its first mask-on gap.
    auto w = writer();
    EXPECT_EQ(w.writeStoredGaps(kSerial, night, {{on, on + 8 * kHour}}), 0);
    const auto names = files(root / "DATALOG" / night);
    ASSERT_EQ(named(names, "EVE").size(), 1u) << ::testing::PrintToString(names);
    const auto eve = cpapdash::parser::resmedFileName(fixedOffsetClock(-300)(on), "EVE");
    EXPECT_EQ(named(names, "EVE").front(), eve);
    EXPECT_EQ(records(root / "DATALOG" / night / eve), static_cast<long>(1 + in_night));
}

TEST_F(NightTest, AStoredEventTheStreamAlsoGaveIsKeptOnce) {
    const fs::path root = dir_ / "as11" / kSerial;
    const auto [te, night] = benchEventsNight();
    if (te == 0) GTEST_SKIP() << "bench events fixture not found";
    const int64_t on = te - 30 * 1000LL;
    const auto first = storedEvents({storedFixture("TherapyEvents-RespiratoryEvents.bin")}).front();

    // Two minutes streamed around the first stored event, which also came live, 1 s off.
    auto w = writer();
    for (int i = 0; i < 120; i++) w.onBlock(second(on + i * 1000));
    const int dur = static_cast<int>(first.duration_ms / 1000);
    w.onEvent(kSerial, {storedEventName(first.type), isoOf(first.onset_ms + 1000), dur, dur});
    w.onStreamEnd(kSerial);
    const auto streamed_eve = named(files(root / "DATALOG" / night), "EVE");
    ASSERT_EQ(streamed_eve.size(), 1u);
    EXPECT_EQ(records(root / "DATALOG" / night / streamed_eve.front()), 2);   // the live one

    // The pages are pulled after the night (D1).
    const size_t in_night = keepBenchEvents(root.string(), night, on, on + 8 * kHour);
    ASSERT_GT(in_night, 1u);
    w.writeStoredGaps(kSerial, night, {{on, on + 8 * kHour}});
    const auto names = files(root / "DATALOG" / night);
    ASSERT_EQ(named(names, "EVE").size(), 1u) << ::testing::PrintToString(names);
    EXPECT_EQ(named(names, "EVE").front(), streamed_eve.front());
    // Every stored event once: the live one stands for its stored twin.
    EXPECT_EQ(records(root / "DATALOG" / night / streamed_eve.front()), static_cast<long>(1 + in_night));
}

TEST_F(NightTest, AStoredRunBeforeTheStreamMovesTheEveAndLeavesOne) {
    const fs::path root = dir_ / "as11" / kSerial;
    const auto [t0, night] = benchNight(root);
    if (t0 == 0) GTEST_SKIP() << "bench page fixtures not found";

    // The stream only started an hour in; its EVE is named by its first block.
    auto w = writer();
    for (int i = 0; i < 30; i++) w.onBlock(second(t0 + kHour + i * 1000));
    w.onStreamEnd(kSerial);
    const auto streamed_brp = named(files(root / "DATALOG" / night), "BRP").front();
    ASSERT_TRUE(fs::exists(root / "DATALOG" / night / eveOf(streamed_brp)));

    w.writeStoredGaps(kSerial, night, {{t0, t0 + 2 * kHour}});
    const auto names = files(root / "DATALOG" / night);
    const auto eve = named(names, "EVE");
    ASSERT_EQ(eve.size(), 1u) << ::testing::PrintToString(names);
    EXPECT_EQ(eve.front(), eveOf(named(names, "BRP").front()));   // the stored BRP, the earlier
    EXPECT_NE(eve.front(), eveOf(streamed_brp));
    EXPECT_EQ(named(names, "CSL").size(), 1u);
}

TEST_F(NightTest, StrictGapDoesNotFabricateUsageOrReplayOlderSegment) {
    As11NightWriter w(dir_.string(), fixedOffsetClock(-300), true);
    for (int i=0; i<30; ++i) w.onBlock(second(kT0+i*1000));
    for (int i=0; i<30; ++i) w.onBlock(second(kT0+35000+i*1000));
    w.onBlock(second(kT0));
    w.onStreamEnd(kSerial);
    const auto names=files(night());
    EXPECT_EQ(std::count_if(names.begin(), names.end(), [](const auto& n) {
        return n.find("_BRP.edf") != std::string::npos;
    }), 2);
    EXPECT_EQ(records(night()/"20261016_220000_BRP.edf"), 30);
    EXPECT_EQ(records(night()/"20261016_220035_BRP.edf"), 30);
}

TEST_F(NightTest, DefaultPolicyStillJoinsFiveSecondGap) {
    auto w = writer();
    for (int i = 0; i < 30; ++i) w.onBlock(second(kT0 + i * 1000));
    for (int i = 0; i < 30; ++i) w.onBlock(second(kT0 + 35000 + i * 1000));
    w.onStreamEnd(kSerial);
    EXPECT_EQ(records(night() / "20261016_220000_BRP.edf"), 65);
    EXPECT_FALSE(fs::exists(night() / "20261016_220035_BRP.edf"));
}
TEST_F(NightTest, StrictContiguousBlocksRemainOneSegmentAcrossRestart) {
    {
        As11NightWriter w(dir_.string(), fixedOffsetClock(-300), true);
        for (int i = 0; i < 30; ++i) w.onBlock(second(kT0 + i * 1000));
        w.onStreamEnd(kSerial);
    }
    As11NightWriter w(dir_.string(), fixedOffsetClock(-300), true);
    w.onBlock(second(kT0));
    for (int i = 30; i < 60; ++i) w.onBlock(second(kT0 + i * 1000));
    w.onStreamEnd(kSerial);
    const auto names = files(night());
    EXPECT_EQ(std::count_if(names.begin(), names.end(), [](const auto& n) {
        return n.find("_BRP.edf") != std::string::npos;
    }), 1);
    EXPECT_EQ(records(night() / "20261016_220000_BRP.edf"), 60);
}

// Split, a segment under a second long and the next one can start in the
// same second; their files would share a name and the later would replace
// the earlier. A real stream did this (a lost report closed a 200 ms
// second, the next report came 160 ms on). The second block joins the first.
TEST_F(NightTest, StrictTwoSegmentsInOneSecondShareOneSegment) {
    As11NightWriter w(dir_.string(), fixedOffsetClock(-300), true);
    StreamBlock part = second(kT0 + 100);
    for (auto& c : part.channels) c.values.resize(5);   // 200 ms, a closed partial second
    w.onBlock(part);
    for (int i = 0; i < 30; ++i) w.onBlock(second(kT0 + 460 + i * 1000));   // 160 ms on
    w.onStreamEnd(kSerial);
    std::vector<std::string> segs;
    for (const auto& n : files(dir_ / "as11" / kSerial / "stream" / "20261016"))
        if (n.size() > 6 && n.substr(n.size() - 6) == ".cds1s") segs.push_back(n);
    EXPECT_EQ(segs.size(), 1u) << "one segment, so one BRP name";
    const auto names = files(night());
    EXPECT_EQ(std::count_if(names.begin(), names.end(), [](const auto& n) {
        return n.find("_BRP.edf") != std::string::npos;
    }), 1);
    EXPECT_GE(records(night() / "20261016_220000_BRP.edf"), 30);

    // A gap past the second still splits.
    for (int i = 0; i < 10; ++i) w.onBlock(second(kT0 + 40000 + i * 1000));
    w.onStreamEnd(kSerial);
    EXPECT_TRUE(fs::exists(night() / "20261016_220040_BRP.edf"));
}

// The machine stamps its seconds about 1000 ms apart, a few ms either way. A
// block a few ms late is the same stream: under the strict policy it must not
// cut the night into a file per late block (a real night had 392 of them).
// A block a sample or more late is a gap and still splits.
TEST_F(NightTest, StrictBlocksAFewMsLateStayOneSegment) {
    As11NightWriter w(dir_.string(), fixedOffsetClock(-300), true);
    int64_t t = kT0;
    for (int i = 0; i < 60; ++i) {
        w.onBlock(second(t));
        t += 1000 + (i % 3 == 1 ? 4 : i % 3 == 2 ? 8 : 0);   // 0, +4, +8 ms late
    }
    w.onStreamEnd(kSerial);
    const auto names = files(night());
    EXPECT_EQ(std::count_if(names.begin(), names.end(), [](const auto& n) {
        return n.find("_BRP.edf") != std::string::npos;
    }), 1) << ::testing::PrintToString(names);
    EXPECT_EQ(records(night() / "20261016_220000_BRP.edf"), 60);

    // A block a whole sample late (40 ms) is a missed sample: a new file.
    As11NightWriter w2(dir_.string() + "_b", fixedOffsetClock(-300), true);
    for (int i = 0; i < 20; ++i) w2.onBlock(second(kT0 + i * 1000));
    for (int i = 0; i < 20; ++i) w2.onBlock(second(kT0 + 20040 + i * 1000));
    w2.onStreamEnd(kSerial);
    EXPECT_TRUE(fs::exists(fs::path(dir_.string() + "_b") / "as11" / kSerial / "DATALOG" / "20261016" /
                           "20261016_220020_BRP.edf"));
}

// The strict threshold is three quarters of the block's own sample interval:
// that late is the same segment, a millisecond more is a new one.
TEST_F(NightTest, StrictGapThresholdFollowsTheSampleInterval) {
    // The segments two seconds of stream make, the second `late` ms late.
    const auto segments = [this](int interval_ms, int64_t late) {
        const fs::path d = dir_ / (std::to_string(interval_ms) + "_" + std::to_string(late));
        As11NightWriter w(d.string(), fixedOffsetClock(-300), true);
        for (int i = 0; i < 2; ++i) {
            StreamBlock b = second(kT0 + i * (1000 + late));
            b.interval_ms = interval_ms;
            for (auto& c : b.channels) c.values.resize(1000 / interval_ms);
            w.onBlock(b);
        }
        return files(d / "as11" / kSerial / "stream" / "20261016").size();
    };
    for (const int iv : {20, 40, 100}) {
        const int64_t slack = iv * 3 / 4;
        EXPECT_EQ(segments(iv, slack), 1u) << iv << " ms samples";
        EXPECT_EQ(segments(iv, slack + 1), 2u) << iv << " ms samples";
    }
}

// The Settings switch reaches a running writer: from the next block on.
TEST_F(NightTest, ThePolicyChangesOnARunningWriter) {
    auto w = writer();
    EXPECT_FALSE(w.preserveGaps());
    for (int i = 0; i < 30; ++i) w.onBlock(second(kT0 + i * 1000));
    w.setPreserveGaps(true);
    EXPECT_TRUE(w.preserveGaps());
    for (int i = 0; i < 30; ++i) w.onBlock(second(kT0 + 35000 + i * 1000));   // 5 s gap: split
    w.setPreserveGaps(false);
    for (int i = 0; i < 30; ++i) w.onBlock(second(kT0 + 70000 + i * 1000));   // 5 s gap: joined
    w.onStreamEnd(kSerial);
    EXPECT_EQ(records(night() / "20261016_220000_BRP.edf"), 30);
    EXPECT_EQ(records(night() / "20261016_220035_BRP.edf"), 65);
    EXPECT_FALSE(fs::exists(night() / "20261016_220110_BRP.edf"));
}
