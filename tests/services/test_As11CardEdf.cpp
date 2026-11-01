#include <gtest/gtest.h>

#include "services/As11CardEdf.h"

#include <cpapdash/parser/EDFFile.h>

#include <chrono>
#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

using namespace hms_cpap;
using namespace cpapdash::parser;
namespace fs = std::filesystem;

// The EDFs of a Bluetooth night are scaled as the machine's card files are, so a
// reader that takes raw * gain, with no offset, gets the value. Values are made
// up; the ranges are the card's (read off a real card's BRP and PLD headers).

namespace {

// One signal of an EDF, read straight from the header and the data records.
struct Raw {
    std::string label, dim;
    double pmin = 0, pmax = 0;
    int dmin = 0, dmax = 0, spr = 0;
    std::vector<int> samples;   // as stored
    double gain() const { return (pmax - pmin) / (dmax - dmin); }
    // What a reader gets that has no offset: the stored number times the gain.
    double noOffset(size_t i) const { return samples[i] * gain(); }
};

std::string field(const std::vector<uint8_t>& b, size_t at, size_t n) {
    std::string s(b.begin() + static_cast<long>(at), b.begin() + static_cast<long>(at + n));
    while (!s.empty() && s.back() == ' ') s.pop_back();
    return s;
}

std::vector<Raw> decode(const std::vector<uint8_t>& b) {
    const int ns = std::atoi(field(b, 252, 4).c_str());
    const int hl = std::atoi(field(b, 184, 8).c_str());
    const int nr = std::atoi(field(b, 236, 8).c_str());
    std::vector<Raw> out(static_cast<size_t>(ns));
    size_t o = 256;
    auto each = [&](size_t width, auto&& set) {
        for (int i = 0; i < ns; i++) set(out[static_cast<size_t>(i)], field(b, o + static_cast<size_t>(i) * width, width));
        o += width * static_cast<size_t>(ns);
    };
    each(16, [](Raw& r, const std::string& s) { r.label = s; });
    o += 80 * static_cast<size_t>(ns);
    each(8, [](Raw& r, const std::string& s) { r.dim = s; });
    each(8, [](Raw& r, const std::string& s) { r.pmin = std::atof(s.c_str()); });
    each(8, [](Raw& r, const std::string& s) { r.pmax = std::atof(s.c_str()); });
    each(8, [](Raw& r, const std::string& s) { r.dmin = std::atoi(s.c_str()); });
    each(8, [](Raw& r, const std::string& s) { r.dmax = std::atoi(s.c_str()); });
    o += 80 * static_cast<size_t>(ns);
    each(8, [](Raw& r, const std::string& s) { r.spr = std::atoi(s.c_str()); });

    size_t rec = 0;
    for (const auto& r : out) rec += static_cast<size_t>(r.spr) * 2;
    for (int k = 0; k < nr; k++) {
        size_t at = static_cast<size_t>(hl) + static_cast<size_t>(k) * rec;
        for (auto& r : out) {
            for (int j = 0; j < r.spr; j++, at += 2)
                r.samples.push_back(static_cast<int16_t>(b[at] | (b[at + 1] << 8)));
        }
    }
    return out;
}

StreamedPLD pld(size_t n) {
    StreamedPLD p;
    p.start = {2026, 11, 1, 3, 0, 0};
    p.serial = "23000000001";
    p.mask_press_cmh2o.assign(n, 10.0);
    p.press_cmh2o.assign(n, 10.5);
    p.epr_press_cmh2o.assign(n, 8.5);
    p.leak_lps.assign(n, 0.1);
    p.resp_rate.assign(n, 16.0);
    p.tidal_volume_l.assign(n, 0.5);
    p.minute_vent_lpm.assign(n, 8.0);
    p.snore.assign(n, 0.2);
    p.flow_limitation.assign(n, 0.3);
    return p;
}

}  // namespace

// ── the card's checksums ──

namespace {

// The data bytes of record [r] without its closing Crc16, and the Crc16 as stored.
struct Record { std::vector<uint8_t> data; uint16_t stored; };

std::vector<Record> recordsOf(const std::vector<uint8_t>& b) {
    const int ns = std::atoi(field(b, 252, 4).c_str());
    const int hl = std::atoi(field(b, 184, 8).c_str());
    const int nr = std::atoi(field(b, 236, 8).c_str());
    size_t rec = 0;
    const size_t spr_at = 256 + static_cast<size_t>(ns) * (16 + 80 + 8 + 8 + 8 + 8 + 8 + 80);
    for (int i = 0; i < ns; i++) rec += static_cast<size_t>(std::atoi(field(b, spr_at + static_cast<size_t>(i) * 8, 8).c_str())) * 2;
    std::vector<Record> out;
    for (int r = 0; r < nr; r++) {
        const size_t at = static_cast<size_t>(hl) + static_cast<size_t>(r) * rec;
        out.push_back({std::vector<uint8_t>(b.begin() + static_cast<long>(at), b.begin() + static_cast<long>(at + rec - 2)),
                       static_cast<uint16_t>(b[at + rec - 2] | (b[at + rec - 1] << 8))});
    }
    return out;
}

std::string hex4(uint16_t v) {
    char buf[8];
    std::snprintf(buf, sizeof(buf), "%04X", v);
    return buf;
}

StreamedBRP brpOf(size_t seconds) {
    StreamedBRP b;
    b.start = {2026, 11, 1, 3, 0, 0};
    b.serial = "23000000001";
    for (size_t i = 0; i < seconds * 25; i++) {
        b.flow_lpm.push_back(30.0 * std::sin(static_cast<double>(i) / 20.0));
        b.press_cmh2o.push_back(9.0 + static_cast<double>(i % 7) * 0.1);
    }
    return b;
}

// Every file of the card checks the same way: a record's CRC, and the two
// patient tokens over the main header's tail and over the signal headers.
void expectCardChecksums(const std::vector<uint8_t>& file) {
    const int hl = std::atoi(field(file, 184, 8).c_str());
    const auto recs = recordsOf(file);
    ASSERT_FALSE(recs.empty());
    for (size_t r = 0; r < recs.size(); r++)
        EXPECT_EQ(recs[r].stored, crc16CcittFalse(recs[r].data.data(), recs[r].data.size())) << "record " << r;
    const std::string patient = field(file, 8, 80);
    EXPECT_EQ(patient.substr(0, 8), "X X X X ");
    EXPECT_EQ(patient.substr(8, 4), hex4(crc16CcittFalse(file.data() + 25, 256 - 25)));
    EXPECT_EQ(patient[12], ' ');
    EXPECT_EQ(patient.substr(13, 4), hex4(crc16CcittFalse(file.data() + 256, static_cast<size_t>(hl) - 256)));
    EXPECT_EQ(patient.find_first_not_of(' ', 17), std::string::npos);
}

}  // namespace

TEST(As11CardEdf, EveryChannelHasZeroAtZero) {
    // The property that makes raw * gain the value: physical min = digital min * gain.
    StreamedBRP brp;
    brp.start = {2026, 11, 1, 3, 0, 0};
    brp.flow_lpm.assign(50, 30.0);
    brp.press_cmh2o.assign(50, 9.0);
    StreamedSAD sad;
    sad.start = brp.start;
    sad.spo2_pct.assign(10, 95.0);
    sad.pulse_bpm.assign(10, 70.0);
    for (const auto& file : {writeCardBRP(brp), writeCardPLD(pld(10)), writeCardSAD(sad)}) {
        ASSERT_FALSE(file.empty());
        const auto sigs = decode(file);
        ASSERT_FALSE(sigs.empty());
        for (const auto& s : sigs)
            EXPECT_NEAR(s.pmin, s.dmin * s.gain(), 1e-9) << s.label << " has an offset";
    }
}

TEST(As11CardEdf, PldRangesAreTheCards) {
    const auto sigs = decode(writeCardPLD(pld(10)));
    ASSERT_EQ(sigs.size(), 10u);   // nine channels and the closing Crc16
    EXPECT_EQ(sigs[9].label, "Crc16");
    struct Want { const char* label; const char* dim; double hi; int dig_hi; };
    const Want want[] = {
        {"MaskPress.2s", "cmH2O", 40, 2000}, {"Press.2s", "cmH2O", 50, 2500},
        {"EprPress.2s", "cmH2O", 30, 1500},  {"Leak.2s", "L/s", 2, 100},
        {"RespRate.2s", "bpm", 90, 450},     {"TidVol.2s", "L", 4, 200},
        {"MinVent.2s", "L/min", 30, 240},    {"Snore.2s", "", 5, 250},
        {"FlowLim.2s", "", 1, 100},
    };
    for (size_t i = 0; i < 9; i++) {
        EXPECT_EQ(sigs[i].label, want[i].label);
        EXPECT_EQ(sigs[i].dim, want[i].dim);
        EXPECT_EQ(sigs[i].pmin, 0.0);
        EXPECT_EQ(sigs[i].pmax, want[i].hi) << want[i].label;
        EXPECT_EQ(sigs[i].dmin, 0);
        EXPECT_EQ(sigs[i].dmax, want[i].dig_hi) << want[i].label;
        EXPECT_EQ(sigs[i].spr, 1);
    }
}

TEST(As11CardEdf, ARespiratoryRateOf16IsReadAs16ByAReaderWithoutAnOffset) {
    // The case OSCAR logged: 16 bpm came out as -14 with the library's range.
    const auto sigs = decode(writeCardPLD(pld(10)));
    ASSERT_EQ(sigs[4].label, "RespRate.2s");
    for (size_t i = 0; i < sigs[4].samples.size(); i++) EXPECT_NEAR(sigs[4].noOffset(i), 16.0, 0.1);
    ASSERT_EQ(sigs[0].label, "MaskPress.2s");
    EXPECT_NEAR(sigs[0].noOffset(0), 10.0, 0.02);
    EXPECT_GE(sigs[0].samples[0], 0);   // never negative
}

TEST(As11CardEdf, FlowIsWrittenInLitresPerSecondAsTheCardHasIt) {
    StreamedBRP brp;
    brp.start = {2026, 11, 1, 3, 0, 0};
    brp.flow_lpm = {90.0, -60.0, 0.0, 30.0};
    brp.press_cmh2o = {9.0, 9.2, 9.4, 9.6};
    brp.flow_lpm.resize(25, 0.0);
    brp.press_cmh2o.resize(25, 9.0);
    const auto file = writeCardBRP(brp);
    const auto sigs = decode(file);
    ASSERT_EQ(sigs.size(), 3u);
    EXPECT_EQ(sigs[2].label, "Crc16");
    EXPECT_EQ(sigs[0].label, "Flow.40ms");
    EXPECT_EQ(sigs[0].dim, "L/s");
    EXPECT_EQ(sigs[0].pmin, -2.0);
    EXPECT_EQ(sigs[0].pmax, 3.0);
    EXPECT_EQ(sigs[0].dmin, -1000);
    EXPECT_EQ(sigs[0].dmax, 1500);
    EXPECT_NEAR(sigs[0].noOffset(0), 1.5, 0.003);    // 90 L/min
    EXPECT_NEAR(sigs[0].noOffset(1), -1.0, 0.003);   // -60 L/min
    EXPECT_EQ(sigs[1].label, "Press.40ms");
    EXPECT_NEAR(sigs[1].noOffset(1), 9.2, 0.02);

    // The library's reader multiplies a ResMed BRP's flow by 60 whatever it
    // says; with L/s in the file that gives the L/min that went in.
    EDFFile edf;
    ASSERT_TRUE(edf.open(file.data(), file.size()));
    std::vector<double> flow;
    edf.readSignal(edf.findSignal("Flow"), flow);
    ASSERT_GE(flow.size(), 4u);
    EXPECT_NEAR(flow[0] * 60.0, 90.0, 0.2);
    EXPECT_NEAR(flow[1] * 60.0, -60.0, 0.2);
}

TEST(As11CardEdf, ValuesComeBackFromTheLibraryReaderToo) {
    const auto file = writeCardPLD(pld(10));
    EDFFile edf;
    ASSERT_TRUE(edf.open(file.data(), file.size()));
    std::vector<double> v;
    ASSERT_GT(edf.readSignal(edf.findSignal("MaskPress"), v), 0);
    EXPECT_NEAR(v[0], 10.0, 0.02);
    ASSERT_GT(edf.readSignal(edf.findSignal("RespRate"), v), 0);
    EXPECT_NEAR(v[0], 16.0, 0.1);
    ASSERT_GT(edf.readSignal(edf.findSignal("Leak"), v), 0);
    EXPECT_NEAR(v[0], 0.1, 0.02);
}

TEST(As11CardEdf, AGapIsZeroAndAChannelWithNoSamplesIsLeftOut) {
    auto p = pld(10);
    p.leak_lps.clear();
    p.mask_press_cmh2o[3] = NAN;
    const auto sigs = decode(writeCardPLD(p));
    ASSERT_EQ(sigs.size(), 9u);    // eight channels and the Crc16
    for (const auto& s : sigs) EXPECT_NE(s.label, "Leak.2s");
    EXPECT_EQ(sigs[0].samples[3], 0);
}

TEST(As11CardEdf, OximetryIsTheElevenSeriesOwnFileInTheCardsOrderAndScale) {
    StreamedSAD sad;
    sad.start = {2026, 11, 1, 3, 0, 0};
    sad.spo2_pct = {96.0, 91.0, 88.0};
    sad.pulse_bpm = {70.0, 71.0, 120.0};
    const auto sigs = decode(writeCardSAD(sad));
    ASSERT_EQ(sigs.size(), 3u);
    EXPECT_EQ(sigs[0].label, "Pulse.1s");   // pulse first, as the card has them
    EXPECT_EQ(sigs[0].dim, "bpm");
    EXPECT_EQ(sigs[0].dmax, 300);
    EXPECT_EQ(sigs[1].label, "SpO2.1s");
    EXPECT_EQ(sigs[1].dim, "%");
    EXPECT_EQ(sigs[1].dmax, 100);
    EXPECT_EQ(sigs[2].label, "Crc16");
    EXPECT_NEAR(sigs[0].noOffset(2), 120.0, 0.5);
    EXPECT_NEAR(sigs[1].noOffset(0), 96.0, 0.5);
    EXPECT_TRUE(writeCardSAD(StreamedSAD{}).empty());
}

TEST(As11CardEdf, ASecondWithNoReadingIsMinusOneAsTheCardStoresIt) {
    StreamedSAD sad;
    sad.start = {2026, 11, 1, 3, 0, 0};
    sad.spo2_pct = {96.0, NAN, 91.0};            // the second has no SpO2
    sad.pulse_bpm = {70.0, 71.0};                // and the pulse ends a second early
    const auto sigs = decode(writeCardSAD(sad));
    ASSERT_EQ(sigs.size(), 3u);
    ASSERT_EQ(sigs[0].samples.size(), 3u);
    EXPECT_EQ(sigs[0].samples, (std::vector<int>{70, 71, -1}));
    EXPECT_EQ(sigs[1].samples, (std::vector<int>{96, -1, 91}));
    // The checksums cover those bytes.
    const auto file = writeCardSAD(sad);
    for (const auto& r : recordsOf(file)) EXPECT_EQ(r.stored, crc16CcittFalse(r.data.data(), r.data.size()));
}

TEST(As11CardEdf, TheCrcIsTheKnownCcittFalseOne) {
    const std::string check = "123456789";
    EXPECT_EQ(crc16CcittFalse(reinterpret_cast<const uint8_t*>(check.data()), check.size()), 0x29B1);
    EXPECT_EQ(crc16CcittFalse(nullptr, 0), 0xFFFF);
}

TEST(As11CardEdf, EveryRecordEndsInItsCrcAndTheHeaderCarriesTheTwoTokens) {
    StreamedSAD sad;
    sad.start = {2026, 11, 1, 3, 0, 0};
    sad.serial = "23000000001";
    sad.spo2_pct = {96.0, 95.0, NAN, 91.0};
    sad.pulse_bpm = {70.0, 71.0, 72.0, 120.0};
    expectCardChecksums(writeCardBRP(brpOf(7), 46, 3));
    expectCardChecksums(writeCardPLD(pld(11), 46, 3));
    expectCardChecksums(writeCardSAD(sad, 46, 3));
}

TEST(As11CardEdf, TheTokensChangeWithTheHeaderAndTheRecordsWithTheirData) {
    const auto a = writeCardBRP(brpOf(3));
    auto other = brpOf(3);
    other.start.second = 1;                         // another start: another header tail
    const auto b = writeCardBRP(other);
    EXPECT_NE(field(a, 8, 80), field(b, 8, 80));
    other = brpOf(3);
    other.flow_lpm[0] += 20.0;                      // another first sample: another first CRC
    const auto c = writeCardBRP(other);
    EXPECT_NE(recordsOf(a)[0].stored, recordsOf(c)[0].stored);
    EXPECT_EQ(recordsOf(a)[1].stored, recordsOf(c)[1].stored);
}

TEST(As11CardEdf, TheRecordingFieldNamesTheModelWhenItIsKnown) {
    EXPECT_EQ(field(writeCardBRP(brpOf(2), 46, 3), 88, 80), "Startdate 01-NOV-2026 X X X SRN=23000000001 MID=46 VID=3");
    EXPECT_EQ(field(writeCardPLD(pld(4), 46, 3), 88, 80), "Startdate 01-NOV-2026 X X X SRN=23000000001 MID=46 VID=3");
    EXPECT_EQ(field(writeCardBRP(brpOf(2)), 88, 80), "Startdate 01-NOV-2026 X X X SRN=23000000001");
}

TEST(As11CardEdf, AnAnnotationFileGetsTheTokensAndKeepsItsRecords) {
    ResMedAnnotationFile ann;
    ann.start = {2026, 11, 1, 3, 0, 0};
    ann.serial = "23000000001";
    ann.model_id = 46;
    ann.vid = 3;
    ann.annotations = {{12.0, 0.0, "Hypopnea"}};
    auto file = writeResMedAnnotations(ann);
    const auto before = file;
    EXPECT_EQ(field(file, 8, 80), "X X X X");   // as the library leaves it
    stampHeaderChecksums(file);
    EXPECT_NE(field(file, 8, 80), "X X X X");
    EXPECT_EQ(file.size(), before.size());
    EXPECT_TRUE(std::equal(before.begin() + 88, before.end(), file.begin() + 88));   // nothing but the patient field changed
    const std::string patient = field(file, 8, 80);
    EXPECT_EQ(patient.substr(8, 4), hex4(crc16CcittFalse(file.data() + 25, 256 - 25)));
    EXPECT_EQ(patient.substr(13, 4), hex4(crc16CcittFalse(file.data() + 256, 768 - 256)));
    // Stamped twice, the same.
    auto again = file;
    stampHeaderChecksums(again);
    EXPECT_EQ(again, file);
}

TEST(As11CardEdf, WhatIsNotAnEdfHeaderIsLeftAlone) {
    std::vector<uint8_t> tiny(100, 'x');
    const auto before = tiny;
    stampHeaderChecksums(tiny);
    EXPECT_EQ(tiny, before);
    std::vector<uint8_t> bad(1024, ' ');       // 256 bytes of nothing, a signal count of 0
    const auto bad_before = bad;
    stampHeaderChecksums(bad);
    EXPECT_EQ(bad, bad_before);
}

TEST(As11CardEdf, Crc32IsTheZlibOneAndTheCrcFileIsItLittleEndian) {
    const std::string check = "123456789";
    EXPECT_EQ(crc32(reinterpret_cast<const uint8_t*>(check.data()), check.size()), 0xCBF43926u);
    EXPECT_EQ(crc32(nullptr, 0), 0u);

    const fs::path dir = fs::temp_directory_path() /
                         ("as11_crc_" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    fs::create_directories(dir);
    const fs::path json = dir / "Identification.json";
    { std::ofstream(json, std::ios::binary) << "abc"; }
    ASSERT_TRUE(writeCrcFor(json.string()));
    std::ifstream in(dir / "Identification.crc", std::ios::binary);
    std::vector<char> got((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    // crc32("abc") = 0x352441C2, stored C2 41 24 35
    ASSERT_EQ(got.size(), 4u);
    EXPECT_EQ(static_cast<uint8_t>(got[0]), 0xC2);
    EXPECT_EQ(static_cast<uint8_t>(got[1]), 0x41);
    EXPECT_EQ(static_cast<uint8_t>(got[2]), 0x24);
    EXPECT_EQ(static_cast<uint8_t>(got[3]), 0x35);

    // ensureCrcFor keeps a file that is there, and writeCrcFor refreshes it.
    { std::ofstream(dir / "Identification.crc", std::ios::binary | std::ios::trunc) << "keep"; }
    EXPECT_TRUE(ensureCrcFor(json.string()));
    std::ifstream again(dir / "Identification.crc", std::ios::binary);
    std::string kept((std::istreambuf_iterator<char>(again)), std::istreambuf_iterator<char>());
    EXPECT_EQ(kept, "keep");
    EXPECT_FALSE(writeCrcFor((dir / "nothing.json").string()));
    fs::remove_all(dir);
}

TEST(As11CardEdf, TheHeaderTextIsTheCardsToo) {
    StreamedSAD sad;
    sad.start = {2026, 11, 1, 3, 0, 0};
    sad.spo2_pct = {96.0};
    sad.pulse_bpm = {70.0};
    for (const auto& file : {writeCardBRP(brpOf(2), 46, 3), writeCardPLD(pld(4), 46, 3), writeCardSAD(sad, 46, 3)}) {
        EXPECT_EQ(field(file, 192, 44), "EDF");
        const int ns = std::atoi(field(file, 252, 4).c_str());
        const size_t pmin_at = 256 + static_cast<size_t>(ns) * (16 + 80 + 8);
        const size_t pmax_at = pmin_at + static_cast<size_t>(ns) * 8;
        for (int i = 0; i < ns; i++) {
            const std::string lo = field(file, pmin_at + static_cast<size_t>(i) * 8, 8);
            const std::string hi = field(file, pmax_at + static_cast<size_t>(i) * 8, 8);
            if (i == ns - 1) {   // the Crc16
                EXPECT_EQ(lo, "-32768.0");
                EXPECT_EQ(hi, "32767.00");
            } else {
                EXPECT_EQ(lo.substr(lo.size() - 3), std::string(".") + "00") << lo;
                EXPECT_EQ(hi.substr(hi.size() - 3), std::string(".") + "00") << hi;
            }
        }
    }
    // What the writer made is still read as the same numbers.
    const auto sigs = decode(writeCardPLD(pld(4), 46, 3));
    EXPECT_EQ(sigs[0].pmax, 40.0);
    EXPECT_EQ(sigs[4].pmax, 90.0);
}
