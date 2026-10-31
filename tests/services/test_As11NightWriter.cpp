#include <gtest/gtest.h>

#include "services/As11NightWriter.h"
#include "services/As11StrWriter.h"

#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
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
    void TearDown() override { fs::remove_all(dir_); }

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
    // No slow channel and no ring: no PLD, no SAD. One EVE and one CSL.
    EXPECT_FALSE(fs::exists(night() / "20261016_220000_PLD.edf"));
    EXPECT_FALSE(fs::exists(night() / "20261016_220000_SAD.edf"));
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

TEST_F(NightTest, SlowChannelsMakeAPldAndTheRingASad) {
    auto w = writer();
    for (int i = 0; i < 20; i++)
        w.onBlock(second(kT0 + i * 1000, {{kLeak, 200}, {kRespRate, 150}, {kSpO2, 960}, {kHeartRate, 62}}));
    w.onStreamEnd(kSerial);
    EXPECT_EQ(records(night() / "20261016_220000_PLD.edf"), 10);   // 2 s records
    EXPECT_EQ(records(night() / "20261016_220000_SAD.edf"), 20);   // 1 s records
}

TEST_F(NightTest, EventsLandOnTheirNightWithTheTwoMinuteTail) {
    auto w = writer();
    for (int i = 0; i < 60; i++) w.onBlock(second(kT0 + i * 1000));
    // ISO times: kT0 is 2026-10-17T03:00:00Z.
    w.onEvent(kSerial, {"ObstructiveApneaEnd", "2026-10-17T03:00:30.000Z", 12, 12});
    w.onEvent(kSerial, {"HypopneaEnd", "2026-10-17T03:02:30.000Z", 15, 15});   // 90 s past the end: kept
    w.onEvent(kSerial, {"HypopneaEnd", "2026-10-17T03:05:00.000Z", 15, 15});   // 4 min past: dropped
    w.onEvent(kSerial, {"ObstructiveApneaEnd", "2026-10-17T03:00:30.000Z", 12, 12});  // again: once
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
    std::ifstream in(str, std::ios::binary);
    std::string head(256, '\0');
    in.read(head.data(), 256);
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
