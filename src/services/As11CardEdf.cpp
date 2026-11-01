#include "services/As11CardEdf.h"

#include <algorithm>
#include <cmath>
#include <cstddef>

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

}  // namespace

std::vector<uint8_t> writeCardBRP(const StreamedBRP& brp) {
    const size_t n = std::min(brp.flow_lpm.size(), brp.press_cmh2o.size());
    std::vector<double> flow_lps(n);
    for (size_t i = 0; i < n; i++) flow_lps[i] = brp.flow_lpm[i] / 60.0;
    std::vector<double> press(brp.press_cmh2o.begin(), brp.press_cmh2o.begin() + static_cast<long>(n));

    EDFWriteSpec spec;
    spec.recording       = resmedRecordingField(brp.start, brp.serial);
    spec.start           = brp.start;
    spec.record_duration = 1.0;
    spec.signals.push_back(signalOf("Flow.40ms", "L/s", -2.0, 3.0, -1000, 1500, 25, std::move(flow_lps)));
    spec.signals.push_back(signalOf("Press.40ms", "cmH2O", 0.0, 40.0, 0, 2000, 25, std::move(press)));
    return writeEDF(spec);
}

std::vector<uint8_t> writeCardPLD(const StreamedPLD& pld) {
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
    spec.recording       = resmedRecordingField(pld.start, pld.serial);
    spec.start           = pld.start;
    spec.record_duration = 2.0;
    for (const auto& c : chans) {
        if (c.v->empty()) continue;
        spec.signals.push_back(signalOf(c.label, c.dim, 0.0, c.hi, 0, c.dig_hi, 1,
                                        std::vector<double>(c.v->begin(), c.v->begin() + static_cast<long>(n))));
    }
    return writeEDF(spec);
}

std::vector<uint8_t> writeCardSAD(const StreamedSAD& sad) {
    const size_t n = std::max(sad.spo2_pct.size(), sad.pulse_bpm.size());
    if (n == 0) return {};
    auto padded = [n](const std::vector<double>& v) {
        std::vector<double> out(n, 0.0);
        for (size_t i = 0; i < v.size(); i++) out[i] = std::isnan(v[i]) ? 0.0 : v[i];
        return out;
    };
    EDFWriteSpec spec;
    spec.recording       = resmedRecordingField(sad.start, sad.serial);
    spec.start           = sad.start;
    spec.record_duration = 1.0;
    spec.signals.push_back(signalOf("SpO2", "%", 0.0, 100.0, 0, 100, 1, padded(sad.spo2_pct)));
    spec.signals.push_back(signalOf("Pulse", "bpm", 0.0, 300.0, 0, 300, 1, padded(sad.pulse_bpm)));
    return writeEDF(spec);
}

}  // namespace hms_cpap
