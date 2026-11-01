#include <gtest/gtest.h>

#include "services/As11CardEdf.h"

#include <cpapdash/parser/EDFFile.h>

#include <cmath>
#include <cstdlib>
#include <string>
#include <vector>

using namespace hms_cpap;
using namespace cpapdash::parser;

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
    ASSERT_EQ(sigs.size(), 9u);
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
    ASSERT_EQ(sigs.size(), 2u);
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
    ASSERT_EQ(sigs.size(), 8u);
    for (const auto& s : sigs) EXPECT_NE(s.label, "Leak.2s");
    EXPECT_EQ(sigs[0].samples[3], 0);
}

TEST(As11CardEdf, OximetryKeepsItsValuesFromZero) {
    StreamedSAD sad;
    sad.start = {2026, 11, 1, 3, 0, 0};
    sad.spo2_pct = {96.0, NAN, 91.0};
    sad.pulse_bpm = {70.0, 71.0, 120.0};
    const auto sigs = decode(writeCardSAD(sad));
    ASSERT_EQ(sigs.size(), 2u);
    EXPECT_EQ(sigs[0].label, "SpO2");
    EXPECT_NEAR(sigs[0].noOffset(0), 96.0, 0.5);
    EXPECT_EQ(sigs[0].samples[1], 0);
    EXPECT_NEAR(sigs[1].noOffset(2), 120.0, 0.5);
    EXPECT_TRUE(writeCardSAD(StreamedSAD{}).empty());
}
