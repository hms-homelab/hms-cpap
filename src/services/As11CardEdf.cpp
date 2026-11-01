#include "services/As11CardEdf.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <string>

namespace hms_cpap {

using namespace cpapdash::parser;

namespace {

EDFWriteSignal signalOf(const char* label, const char* dim, double lo, double hi, int dig_lo,
                        int dig_hi, int per_record, std::vector<double> samples) {
    EDFWriteSignal s;
    s.label              = label;
    s.phys_dim           = dim;
    s.phys_min           = lo;
    s.phys_max           = hi;
    s.dig_min            = dig_lo;
    s.dig_max            = dig_hi;
    s.samples_per_record = per_record;
    s.samples            = std::move(samples);
    return s;
}

// The record-closing Crc16 signal: zeros for now, one per record, sealed once
// the file is laid out (the CRC covers bytes that exist only after writeEDF
// has made their digital values).
EDFWriteSignal crcSignal(size_t records) {
    EDFWriteSignal s;
    s.label              = "Crc16";
    s.phys_dim           = "";
    s.phys_min           = -32768.0;
    s.phys_max           = 32767.0;
    s.dig_min            = -32768;
    s.dig_max            = 32767;
    s.samples_per_record = 1;
    s.samples.assign(records, 0.0);
    return s;
}

long headerLong(const std::vector<uint8_t>& b, size_t at, size_t width) {
    return std::atol(std::string(b.begin() + static_cast<long>(at), b.begin() + static_cast<long>(at + width)).c_str());
}

// Writes each record's CRC into the Crc16 sample that ends it, and the header
// checksums into the patient field. [edf] is what writeEDF made, its last
// signal being crcSignal().
std::vector<uint8_t> sealed(std::vector<uint8_t> edf) {
    if (edf.size() < 256) return edf;
    const long ns = headerLong(edf, 252, 4);
    const long hl = headerLong(edf, 184, 8);
    const long nr = headerLong(edf, 236, 8);
    if (ns < 1 || hl != 256 + 256 * ns || nr < 1) return edf;
    size_t rec_bytes = 0;
    const size_t spr_at = 256 + static_cast<size_t>(ns) * (16 + 80 + 8 + 8 + 8 + 8 + 8 + 80);
    for (long i = 0; i < ns; i++)
        rec_bytes += static_cast<size_t>(headerLong(edf, spr_at + static_cast<size_t>(i) * 8, 8)) * 2;
    if (rec_bytes < 4 || edf.size() < static_cast<size_t>(hl) + static_cast<size_t>(nr) * rec_bytes) return edf;
    for (long r = 0; r < nr; r++) {
        uint8_t* rec = edf.data() + static_cast<size_t>(hl) + static_cast<size_t>(r) * rec_bytes;
        const uint16_t crc = crc16CcittFalse(rec, rec_bytes - 2);
        rec[rec_bytes - 2] = static_cast<uint8_t>(crc & 0xFF);
        rec[rec_bytes - 1] = static_cast<uint8_t>(crc >> 8);
    }
    stampHeaderChecksums(edf);
    return edf;
}

}  // namespace

uint16_t crc16CcittFalse(const uint8_t* data, size_t n) {
    uint16_t c = 0xFFFF;
    for (size_t i = 0; i < n; i++) {
        c = static_cast<uint16_t>(c ^ (static_cast<uint16_t>(data[i]) << 8));
        for (int b = 0; b < 8; b++)
            c = static_cast<uint16_t>((c & 0x8000) ? ((c << 1) ^ 0x1021) : (c << 1));
    }
    return c;
}

void stampHeaderChecksums(std::vector<uint8_t>& edf) {
    if (edf.size() < 256) return;
    const long ns = headerLong(edf, 252, 4);
    const long hl = headerLong(edf, 184, 8);
    if (ns < 1 || hl != 256 + 256 * ns || edf.size() < static_cast<size_t>(hl)) return;
    // The tokens sit in the patient field (bytes 8 to 87) and end by byte 24;
    // the first checksum covers what follows them, so it does not depend on them.
    const uint16_t a = crc16CcittFalse(edf.data() + 25, 256 - 25);
    const uint16_t b = crc16CcittFalse(edf.data() + 256, static_cast<size_t>(hl) - 256);
    char field[81];
    std::snprintf(field, sizeof(field), "X X X X %04X %04X", a, b);
    std::string text(field);
    text.resize(80, ' ');
    std::copy(text.begin(), text.end(), edf.begin() + 8);
}

std::vector<uint8_t> writeCardBRP(const StreamedBRP& brp, int model_id, int vid) {
    const size_t n = std::min(brp.flow_lpm.size(), brp.press_cmh2o.size());
    std::vector<double> flow_lps(n);
    for (size_t i = 0; i < n; i++) flow_lps[i] = brp.flow_lpm[i] / 60.0;
    std::vector<double> press(brp.press_cmh2o.begin(), brp.press_cmh2o.begin() + static_cast<long>(n));

    EDFWriteSpec spec;
    spec.recording       = resmedRecordingField(brp.start, brp.serial, model_id, vid);
    spec.start           = brp.start;
    spec.record_duration = 1.0;
    spec.signals.push_back(signalOf("Flow.40ms", "L/s", -2.0, 3.0, -1000, 1500, 25, std::move(flow_lps)));
    spec.signals.push_back(signalOf("Press.40ms", "cmH2O", 0.0, 40.0, 0, 2000, 25, std::move(press)));
    spec.signals.push_back(crcSignal(n / 25));
    return sealed(writeEDF(spec));
}

std::vector<uint8_t> writeCardPLD(const StreamedPLD& pld, int model_id, int vid) {
    struct Chan {
        const char*                label;
        const char*                dim;
        double                     hi;
        int                        dig_hi;
        const std::vector<double>* v;
    };
    // The card's own ranges, all from zero.
    const Chan chans[] = {
        {"MaskPress.2s", "cmH2O", 40.0, 2000, &pld.mask_press_cmh2o},
        {"Press.2s", "cmH2O", 50.0, 2500, &pld.press_cmh2o},
        {"EprPress.2s", "cmH2O", 30.0, 1500, &pld.epr_press_cmh2o},
        {"Leak.2s", "L/s", 2.0, 100, &pld.leak_lps},
        {"RespRate.2s", "bpm", 90.0, 450, &pld.resp_rate},
        {"TidVol.2s", "L", 4.0, 200, &pld.tidal_volume_l},
        {"MinVent.2s", "L/min", 30.0, 240, &pld.minute_vent_lpm},
        {"Snore.2s", "", 5.0, 250, &pld.snore},
        {"FlowLim.2s", "", 1.0, 100, &pld.flow_limitation},
    };
    size_t n = SIZE_MAX;
    for (const auto& c : chans)
        if (!c.v->empty()) n = std::min(n, c.v->size());
    if (n == SIZE_MAX) return {};

    EDFWriteSpec spec;
    spec.recording       = resmedRecordingField(pld.start, pld.serial, model_id, vid);
    spec.start           = pld.start;
    spec.record_duration = 2.0;
    for (const auto& c : chans) {
        if (c.v->empty()) continue;
        spec.signals.push_back(signalOf(c.label, c.dim, 0.0, c.hi, 0, c.dig_hi, 1,
                                        std::vector<double>(c.v->begin(), c.v->begin() + static_cast<long>(n))));
    }
    spec.signals.push_back(crcSignal(n));
    return sealed(writeEDF(spec));
}

std::vector<uint8_t> writeCardSAD(const StreamedSAD& sad, int model_id, int vid) {
    const size_t n = std::max(sad.spo2_pct.size(), sad.pulse_bpm.size());
    if (n == 0) return {};
    auto padded = [n](const std::vector<double>& v) {
        std::vector<double> out(n, 0.0);
        for (size_t i = 0; i < v.size(); i++) out[i] = std::isnan(v[i]) ? 0.0 : v[i];
        return out;
    };
    EDFWriteSpec spec;
    spec.recording       = resmedRecordingField(sad.start, sad.serial, model_id, vid);
    spec.start           = sad.start;
    spec.record_duration = 1.0;
    spec.signals.push_back(signalOf("SpO2", "%", 0.0, 100.0, 0, 100, 1, padded(sad.spo2_pct)));
    spec.signals.push_back(signalOf("Pulse", "bpm", 0.0, 300.0, 0, 300, 1, padded(sad.pulse_bpm)));
    spec.signals.push_back(crcSignal(n));
    return sealed(writeEDF(spec));
}

}  // namespace hms_cpap
