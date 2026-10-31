#include <gtest/gtest.h>

#include "services/As11NightWriter.h"
#include "services/As11StrWriter.h"
#include "services/StrLayouts.h"

#include <cpapdash/parser/EDFWriter.h>

#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

using namespace hms_cpap;
namespace fs = std::filesystem;

// SDD-057, the STR decision of 2026-10-31: an STR.edf already in the folder
// (a card copy, or earlier nights) is appended to, never rewritten. Every value
// here is made up; no real night, no real machine.

namespace {

using Bytes = std::vector<uint8_t>;
constexpr const char* kSerial = "23000000001";
constexpr uint64_t kDay = 86'400'000ULL;
// 2026-03-01 at the machine's noon, UTC-5.
constexpr uint64_t kNoonEst = 1'772'366'400'000ULL + 5 * 3'600'000ULL;
constexpr int64_t kMarch1 = 20513;   // days since 1970-01-01

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

// One Summary day: usage minutes and an AHI in hundredths.
Bytes summaryDay(int day_index, int usage, int ahi_x100 = 150) {
    const uint64_t start_ms = kNoonEst + static_cast<uint64_t>(day_index) * kDay;
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
    append(rec, fieldVarint(7, static_cast<uint64_t>(ahi_x100)));
    append(rec, fieldVarint(39, usage ? 1 : 0));
    return fieldBytes(2, rec);
}

struct Sig { const char* label; int dmin, dmax; double pmin, pmax; int spr; };

// A small STR layout: an EDF header with no records, as a layout file is.
Bytes layout(const std::vector<Sig>& sigs) {
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

const std::vector<Sig> kSigs = {
    {"Date", 0, 24836, 0, 24836, 1},       {"MaskOn", 0, 1440, 0, 1440, 10},
    {"MaskOff", 0, 1440, 0, 1440, 10},     {"MaskEvents", 0, 255, 0, 255, 1},
    {"Duration", 0, 1440, 0, 1440, 1},     {"Mode", 0, 15, 0, 15, 1},
    {"S.A.MinPress", 0, 200, 0, 20, 1},    {"AHI", 0, 32767, 0, 32.767, 1},
    {"Crc16", -32768, 32767, -32768, 32767, 1},
};
constexpr size_t kRecBytes = 2 * (1 + 10 + 10 + 1 + 1 + 1 + 1 + 1 + 1);

Bytes readAll(const fs::path& p) {
    std::ifstream in(p, std::ios::binary);
    return Bytes((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
}
void writeAll(const fs::path& p, const Bytes& b) {
    fs::create_directories(p.parent_path());
    std::ofstream(p, std::ios::binary | std::ios::trunc).write(reinterpret_cast<const char*>(b.data()), b.size());
}
std::string field(const Bytes& b, size_t at, size_t w) { return std::string(b.begin() + at, b.begin() + at + w); }
Bytes record(const Bytes& file, size_t header, size_t i) {
    return Bytes(file.begin() + header + i * kRecBytes, file.begin() + header + (i + 1) * kRecBytes);
}
int16_t sample(const Bytes& rec, size_t at) { return static_cast<int16_t>(rec[at] | (rec[at + 1] << 8)); }

// A card's STR of this machine: `n` days from 1 March, each the machine's own
// record with a Mode, a minimum pressure and a distinct AHI, as a card
// carries what the Summary never does.
Bytes cardStr(int n) {
    Bytes h = layout(kSigs);
    auto put = [&](size_t at, size_t w, const std::string& v) {
        for (size_t i = 0; i < w; i++) h[at + i] = i < v.size() ? static_cast<uint8_t>(v[i]) : ' ';
    };
    put(88, 80, cpapdash::parser::resmedRecordingField({2026, 3, 1, 12, 0, 0}, kSerial, 36, 1));
    put(168, 8, "01.03.26");
    put(236, 8, std::to_string(n));
    Bytes out = h;
    for (int i = 0; i < n; i++) {
        cpapdash::parser::STRDayValues v;
        v.values = {{"Mode", 1}, {"S.A.MinPress", 7.4}, {"AHI", 1.0 + i * 0.1}, {"Duration", 400 + i}};
        v.sessions = {{600, 1000 + i}};
        const auto rec = cpapdash::parser::writeResMedSTRRecord(h.data(), h.size(), {}, kMarch1 + i, v);
        out.insert(out.end(), rec.begin(), rec.end());
    }
    return out;
}

class StrAppend : public ::testing::Test {
protected:
    fs::path dir_, root_, layouts_, str_;
    void SetUp() override {
        dir_ = fs::temp_directory_path() /
               ("as11_str_" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
        root_ = dir_ / "as11" / kSerial;
        layouts_ = dir_ / "layouts";
        str_ = root_ / "STR.edf";
        fs::create_directories(root_);
        fs::create_directories(layouts_);
        writeAll(layouts_ / "39420.edfh", layout(kSigs));
        ASSERT_TRUE(writeIdentification(root_.string(),
            R"({"IdentificationProfiles":{"Product":{"SerialNumber":"23000000001","ProductCode":"39420"},"Software":{"PlatformIdentifier":"36","VariantIdentifier":"1"}}})"));
    }
    void TearDown() override { fs::remove_all(dir_); }

    StrResult run() { return writeStrFromSummaries(root_.string(), layouts_.string()); }
    void summary(const std::string& from, const std::vector<Bytes>& days) {
        Bytes spool;
        for (const auto& d : days) append(spool, d);
        ASSERT_TRUE(keepSummary(root_.string(), from, spool));
    }
};

} // namespace

TEST_F(StrAppend, ANewNightIsAppendedAndEveryOldRecordStaysByteForByte) {
    const Bytes before = cardStr(15);
    writeAll(str_, before);
    const size_t header = before.size() - 15 * kRecBytes;

    // The sixteenth day, 16 March, from the Summary.
    summary("2026-03-14", {summaryDay(15, 420, 230)});
    const auto r = run();
    ASSERT_TRUE(r.written) << r.why;
    EXPECT_FALSE(r.created);
    EXPECT_EQ(r.appended, 1);
    EXPECT_EQ(r.replaced, 0);
    EXPECT_EQ(r.total, 16);

    const Bytes after = readAll(str_);
    ASSERT_EQ(after.size(), header + 16 * kRecBytes);
    EXPECT_EQ(std::atol(field(after, 236, 8).c_str()), 16);
    // The header is the card's but for the count.
    for (size_t i = 0; i < header; i++)
        if (i < 236 || i >= 244) ASSERT_EQ(after[i], before[i]) << "header byte " << i;
    // The fifteen records the card wrote are untouched.
    EXPECT_TRUE(std::equal(before.begin() + header, before.end(), after.begin() + header));
    // The new record is the Summary's day: its Date, usage and AHI; the
    // settings the Summary never carries stay -1.
    const Bytes rec = record(after, header, 15);
    EXPECT_EQ(sample(rec, 0), kMarch1 + 15);
    EXPECT_EQ(sample(rec, 2 * (1 + 10 + 10 + 1)), 420);                 // Duration
    EXPECT_EQ(sample(rec, 2 * (1 + 10 + 10 + 1 + 1)), -1);              // Mode
    EXPECT_EQ(sample(rec, 2 * (1 + 10 + 10 + 1 + 1 + 1)), -1);          // S.A.MinPress
    EXPECT_EQ(sample(rec, 2 * (1 + 10 + 10 + 1 + 1 + 1 + 1)), 2300);    // AHI 2.30
}

TEST_F(StrAppend, TheLastDayIsRewrittenInPlaceOnTheCardsOwnRecord) {
    const Bytes before = cardStr(15);
    writeAll(str_, before);
    const size_t header = before.size() - 15 * kRecBytes;

    // The Summary revises the card's last day, 15 March: more usage, another AHI.
    summary("2026-03-13", {summaryDay(14, 455, 310)});
    const auto r = run();
    ASSERT_TRUE(r.written) << r.why;
    EXPECT_EQ(r.replaced, 1);
    EXPECT_EQ(r.appended, 0);
    EXPECT_EQ(r.total, 15);

    const Bytes after = readAll(str_);
    ASSERT_EQ(after.size(), before.size());
    EXPECT_EQ(std::atol(field(after, 236, 8).c_str()), 15);
    EXPECT_TRUE(std::equal(before.begin(), before.begin() + header + 14 * kRecBytes, after.begin()));
    const Bytes rec = record(after, header, 14);
    EXPECT_EQ(sample(rec, 0), kMarch1 + 14);
    EXPECT_EQ(sample(rec, 2 * (1 + 10 + 10 + 1)), 455);                 // Duration, revised
    EXPECT_EQ(sample(rec, 2 * (1 + 10 + 10 + 1 + 1)), 1);               // Mode, the card's, kept
    EXPECT_EQ(sample(rec, 2 * (1 + 10 + 10 + 1 + 1 + 1)), 74);          // S.A.MinPress 7.4, kept
    EXPECT_EQ(sample(rec, 2 * (1 + 10 + 10 + 1 + 1 + 1 + 1)), 3100);    // AHI 3.10, revised

    // Run again with the same Summary: nothing to do, nothing changes.
    const auto again = run();
    EXPECT_FALSE(again.written);
    EXPECT_EQ(readAll(str_), after);
}

TEST_F(StrAppend, ADayTheMachineDidNotReportIsADayOffRecordSoTheIndexStaysTheDate) {
    writeAll(str_, cardStr(15));
    // 18 March, with 16 and 17 unreported.
    summary("2026-03-16", {summaryDay(17, 300)});
    const auto r = run();
    ASSERT_TRUE(r.written) << r.why;
    EXPECT_EQ(r.appended, 3);
    EXPECT_EQ(r.total, 18);
    const Bytes after = readAll(str_);
    const size_t header = after.size() - 18 * kRecBytes;
    for (int i = 15; i < 18; i++) EXPECT_EQ(sample(record(after, header, i), 0), kMarch1 + i) << i;
    EXPECT_EQ(sample(record(after, header, 15), 2 * (1 + 10 + 10 + 1)), -1);    // day off
    EXPECT_EQ(sample(record(after, header, 17), 2 * (1 + 10 + 10 + 1)), 300);
}

TEST_F(StrAppend, SeveralNightsInARowAppendInOrderAndSurviveAReread) {
    writeAll(str_, cardStr(15));
    for (int i = 15; i < 20; i++) {
        summary("2026-03-" + std::to_string(i - 1), {summaryDay(i - 1, 400 + i), summaryDay(i, 400 + i)});
        const auto r = run();
        ASSERT_TRUE(r.written) << i << ": " << r.why;
        EXPECT_EQ(r.appended, 1) << i;
        EXPECT_EQ(r.total, i + 1) << i;
    }
    const Bytes after = readAll(str_);
    const auto h = readStrHeader(after);
    ASSERT_TRUE(h.ok);
    EXPECT_EQ(h.records, 20);
    EXPECT_EQ(h.first_day, kMarch1);
    EXPECT_EQ(after.size(), h.header_bytes + 20 * kRecBytes);
}

TEST_F(StrAppend, ADayOlderThanTheFileIsLeftAloneAndCounted) {
    const Bytes before = cardStr(15);
    writeAll(str_, before);
    summary("2026-03-01", {summaryDay(2, 999), summaryDay(5, 999)});
    const auto r = run();
    EXPECT_FALSE(r.written);
    EXPECT_EQ(r.skipped, 2);
    EXPECT_EQ(readAll(str_), before);
}

TEST_F(StrAppend, AnotherMachinesStrIsNeverAppendedTo) {
    Bytes other = cardStr(3);
    const std::string rec = cpapdash::parser::resmedRecordingField({2026, 3, 1, 12, 0, 0}, "23000000999", 36, 1);
    for (size_t i = 0; i < 80; i++) other[88 + i] = i < rec.size() ? static_cast<uint8_t>(rec[i]) : ' ';
    writeAll(str_, other);
    summary("2026-03-03", {summaryDay(5, 300)});
    const auto r = run();
    EXPECT_FALSE(r.written);
    EXPECT_NE(r.why.find("another machine"), std::string::npos) << r.why;
    EXPECT_EQ(readAll(str_), other);
}

TEST_F(StrAppend, AFileWhoseSignalsDifferFromTheLayoutIsReportedNotRewritten) {
    const Bytes before = cardStr(15);
    writeAll(str_, before);
    // The layout for this product code names other signals.
    std::vector<Sig> other = kSigs;
    other.push_back({"S.RampTime", 0, 45, 0, 45, 1});
    writeAll(layouts_ / "39420.edfh", layout(other));
    summary("2026-03-14", {summaryDay(15, 420)});
    const auto r = run();
    EXPECT_FALSE(r.written);
    EXPECT_NE(r.why.find("differ"), std::string::npos) << r.why;
    EXPECT_EQ(readAll(str_), before);
}

TEST_F(StrAppend, WithNoStrOneIsCreatedFromTheLayoutAndThenAppendedTo) {
    summary("2026-03-01", {summaryDay(0, 420), summaryDay(2, 380)});
    const auto made = run();
    ASSERT_TRUE(made.written) << made.why;
    EXPECT_TRUE(made.created);
    EXPECT_EQ(made.total, 3);
    const Bytes first = readAll(str_);
    EXPECT_EQ(field(first, 168, 8), "01.03.26");
    EXPECT_NE(field(first, 88, 80).find("SRN=23000000001"), std::string::npos);

    summary("2026-03-02", {summaryDay(2, 385), summaryDay(3, 410)});
    const auto more = run();
    ASSERT_TRUE(more.written) << more.why;
    EXPECT_FALSE(more.created);
    EXPECT_EQ(more.replaced, 1);
    EXPECT_EQ(more.appended, 1);
    EXPECT_EQ(more.total, 4);
    const Bytes second = readAll(str_);
    EXPECT_EQ(second.size(), first.size() + kRecBytes);
    EXPECT_EQ(std::atol(field(second, 236, 8).c_str()), 4);
}

TEST_F(StrAppend, ATornTailPastTheCountIsOverwrittenByTheNextAppend) {
    Bytes torn = cardStr(15);
    torn.insert(torn.end(), kRecBytes / 2, 0x7F);   // a write that died before its count
    writeAll(str_, torn);
    summary("2026-03-14", {summaryDay(15, 420)});
    const auto r = run();
    ASSERT_TRUE(r.written) << r.why;
    const Bytes after = readAll(str_);
    const auto h = readStrHeader(after);
    EXPECT_EQ(h.records, 16);
    EXPECT_EQ(after.size(), h.header_bytes + 16 * kRecBytes);
}

// ── the built-in layouts ──

TEST(StrLayouts, EveryBuiltInLayoutIsAWholeAnonymousStrHeader) {
    size_t n = 0;
    const auto* all = builtinStrLayouts(&n);
    ASSERT_GE(n, 8u);
    std::vector<int> codes;
    for (size_t i = 0; i < n; i++) {
        const Bytes b(all[i].bytes, all[i].bytes + all[i].size);
        const auto h = readStrHeader(b);
        ASSERT_TRUE(h.ok) << all[i].product_code;
        EXPECT_EQ(h.header_bytes, b.size()) << all[i].product_code;
        EXPECT_EQ(h.records, 0) << all[i].product_code;
        EXPECT_TRUE(h.recording.empty()) << all[i].product_code << ": " << h.recording;
        EXPECT_EQ(field(b, 8, 7), "X X X X") << all[i].product_code;
        EXPECT_NE(std::find(h.labels.begin(), h.labels.end(), "Date"), h.labels.end());
        EXPECT_NE(std::find(h.labels.begin(), h.labels.end(), "MaskOn"), h.labels.end());
        EXPECT_NE(std::find(h.labels.begin(), h.labels.end(), "AHI"), h.labels.end());
        EXPECT_NE(std::find(h.labels.begin(), h.labels.end(), "Crc16"), h.labels.end());
        codes.push_back(all[i].product_code);
    }
    for (int code : {39420, 39485, 39488, 39494, 39517, 39520, 39523, 39532})
        EXPECT_NE(std::find(codes.begin(), codes.end(), code), codes.end()) << code;
    // An owner's own file wins over the built-in one; a code nobody has is empty.
    EXPECT_EQ(strLayoutFor(39420, "/nonexistent/dir").size(), all[0].size);
    EXPECT_TRUE(strLayoutFor(1, "/nonexistent/dir").empty());
}

TEST(StrLayouts, ABuiltInLayoutLaysASummaryDayOutAsTheCardWould) {
    const fs::path dir = fs::temp_directory_path() /
        ("as11_builtin_" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    const fs::path root = dir / "as11" / kSerial;
    fs::create_directories(root);
    ASSERT_TRUE(writeIdentification(root.string(),
        R"({"IdentificationProfiles":{"Product":{"SerialNumber":"23000000001","ProductCode":"39420"},"Software":{"PlatformIdentifier":"36","VariantIdentifier":"1"}}})"));
    Bytes spool;
    append(spool, summaryDay(0, 420, 150));
    ASSERT_TRUE(keepSummary(root.string(), "2026-03-01", spool));
    const auto r = writeStrFromSummaries(root.string(), (dir / "no-layouts").string());
    ASSERT_TRUE(r.written) << r.why;
    const Bytes str = readAll(root / "STR.edf");
    const auto h = readStrHeader(str);
    ASSERT_TRUE(h.ok);
    EXPECT_EQ(h.records, 1);
    EXPECT_EQ(h.labels.size(), 78u);
    EXPECT_EQ(h.record_bytes, 232u);
    EXPECT_EQ(str.size(), h.header_bytes + 232);
    EXPECT_NE(h.recording.find("SRN=23000000001"), std::string::npos);
    EXPECT_NE(h.recording.find("MID=36"), std::string::npos);
    fs::remove_all(dir);
}
