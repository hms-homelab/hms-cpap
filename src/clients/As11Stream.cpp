#include "clients/As11Stream.h"

#include <cmath>
#include <cstdio>
#include <cstring>

namespace hms_cpap::as11 {

namespace {

void put16(std::vector<uint8_t>& o, uint16_t v) {
    o.push_back(static_cast<uint8_t>(v & 0xFF));
    o.push_back(static_cast<uint8_t>(v >> 8));
}
uint16_t le16(const uint8_t* p) { return static_cast<uint16_t>(p[0] | (p[1] << 8)); }

int16_t clampi(double v) {
    if (v > 32767.0) return 32767;
    if (v < -32767.0) return -32767;
    return static_cast<int16_t>(std::lround(v));
}

double scaleOf(Channel ch) {
    switch (ch) {
    case kFlow: case kLeak: case kTidalVolume: case kFlowLimitation: return 1000.0;
    case kMaskPressure: case kMaskPressure2s: case kInspPressure2s: case kExpPressure2s:
    case kMinuteVent: case kSnore: return 100.0;
    case kRespRate: case kSpO2: return 10.0;
    case kHeartRate: return 1.0;
    }
    return 0.0;
}

} // namespace

const StreamChannel* StreamBlock::channel(uint8_t id) const {
    for (const auto& c : channels)
        if (c.id == id) return &c;
    return nullptr;
}

int64_t StreamBlock::endMs() const {
    size_t fast = 0;
    for (const auto& c : channels)
        if (c.cadence == 0) fast = std::max(fast, c.values.size());
    if (fast) return start_ms + static_cast<int64_t>(fast) * interval_ms;
    return start_ms + 1000;
}

std::vector<uint8_t> encodeCds1(const StreamBlock& b) {
    std::vector<uint8_t> o;
    const size_t slen = std::min<size_t>(b.serial.size(), 255);
    o.insert(o.end(), {'C', 'D', 'S', '1'});
    o.push_back(1);
    o.push_back(static_cast<uint8_t>(b.channels.size()));
    put16(o, static_cast<uint16_t>(b.interval_ms));
    uint64_t u = static_cast<uint64_t>(b.start_ms);
    for (int i = 0; i < 8; i++) { o.push_back(static_cast<uint8_t>(u & 0xFF)); u >>= 8; }
    o.push_back(static_cast<uint8_t>(slen));
    o.insert(o.end(), b.serial.begin(), b.serial.begin() + static_cast<long>(slen));
    for (const auto& c : b.channels) {
        o.push_back(c.id);
        o.push_back(c.cadence);
        put16(o, static_cast<uint16_t>(c.values.size()));
        for (int16_t v : c.values) put16(o, static_cast<uint16_t>(v));
    }
    return o;
}

std::optional<StreamBlock> decodeCds1(const uint8_t* p, size_t n) {
    if (n < 17 || std::memcmp(p, "CDS1", 4) != 0 || p[4] != 1) return std::nullopt;
    StreamBlock b;
    const size_t nchan = p[5];
    b.interval_ms = le16(p + 6);
    uint64_t u = 0;
    for (int i = 7; i >= 0; i--) u = (u << 8) | p[8 + i];
    b.start_ms = static_cast<int64_t>(u);
    const size_t slen = p[16];
    size_t at = 17;
    if (at + slen > n || b.interval_ms == 0) return std::nullopt;
    b.serial.assign(reinterpret_cast<const char*>(p + at), slen);
    at += slen;
    for (size_t c = 0; c < nchan; c++) {
        if (at + 4 > n) return std::nullopt;
        StreamChannel ch;
        ch.id = p[at];
        ch.cadence = p[at + 1];
        const size_t count = le16(p + at + 2);
        at += 4;
        if (at + 2 * count > n || ch.cadence > 1) return std::nullopt;
        ch.values.resize(count);
        for (size_t i = 0; i < count; i++) ch.values[i] = static_cast<int16_t>(le16(p + at + 2 * i));
        at += 2 * count;
        b.channels.push_back(std::move(ch));
    }
    if (at != n) return std::nullopt;
    return b;
}

int16_t quantise(Channel ch, double v) {
    if (std::isnan(v)) return kNull;
    const double s = scaleOf(ch);
    if (s == 0.0) return kNull;
    return clampi(v * s);
}

double physical(Channel ch, int16_t v) {
    const double s = scaleOf(ch);
    if (v == kNull || s == 0.0) return std::nan("");
    return v / s;
}

int64_t isoToMs(const std::string& s) {
    int y, mo, d, h, mi, sec, ms = 0;
    if (std::sscanf(s.c_str(), "%4d-%2d-%2dT%2d:%2d:%2d.%3d", &y, &mo, &d, &h, &mi, &sec, &ms) < 6)
        return 0;
    // days from civil (Howard Hinnant)
    y -= mo <= 2;
    const int64_t era = (y >= 0 ? y : y - 399) / 400;
    const int64_t yoe = y - era * 400;
    const int64_t doy = (153 * (mo + (mo > 2 ? -3 : 9)) + 2) / 5 + d - 1;
    const int64_t doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    const int64_t days = era * 146097 + doe - 719468;
    return ((days * 86400 + h * 3600 + mi * 60 + sec) * 1000) + ms;
}

uint8_t channelOfKey(const std::string& k) {
    if (k == "PatientFlow") return kFlow;
    if (k == "MaskPressure") return kMaskPressure;
    if (k == "MaskPressure-TwoSecond") return kMaskPressure2s;
    if (k == "InspiratoryPressure-TwoSecond") return kInspPressure2s;
    if (k == "ExpiratoryPressure-TwoSecond") return kExpPressure2s;
    if (k == "Leak") return kLeak;
    if (k == "_RR2" || k == "RespiratoryRate") return kRespRate;
    if (k == "_TD2" || k == "TidalVolume") return kTidalVolume;
    if (k == "_MV2" || k == "MinuteVentilation") return kMinuteVent;
    if (k == "SnoreIndex") return kSnore;
    if (k == "FlowLimitation") return kFlowLimitation;
    if (k == "SpO2") return kSpO2;
    if (k == "HeartRate") return kHeartRate;
    return 0;
}

const std::vector<std::string>& streamIds() {
    static const std::vector<std::string> ids = {"_RFL", "_MKP", "_MKF", "_MKI", "_MKE",
                                                 "_LKF", "_RR2", "_TD2", "_MV2", "_SNI",
                                                 "_FFL", "_HRT", "_SAO"};
    return ids;
}

// ── StreamData into seconds ──

void SecondAssembler::resetSecond() {
    start_ms_ = 0;
    flow_.clear();
    press_.clear();
    for (int i = 0; i < 14; i++) { slow_[i] = kNull; slow_set_[i] = false; }
}

void SecondAssembler::emit() {
    if (start_ms_ == 0 || flow_.empty()) { resetSecond(); return; }
    StreamBlock b;
    b.start_ms = start_ms_;
    b.interval_ms = interval_ms_;
    b.serial = serial_;
    b.channels.push_back({kFlow, 0, flow_});
    b.channels.push_back({kMaskPressure, 0, press_});
    for (uint8_t id = 3; id <= 13; id++)
        if (slow_set_[id]) b.channels.push_back({id, 1, {slow_[id]}});
    resetSecond();
    if (sink_) sink_(std::move(b));
}

void SecondAssembler::flush() { emit(); }

void SecondAssembler::onStreamData(const nlohmann::json& params) {
    if (!params.is_object()) return;
    const auto st = params.find("startTime");
    const auto arr = params.find("data");
    if (st == params.end() || !st->is_string() || arr == params.end() || !arr->is_array()) return;
    const int64_t start = isoToMs(st->get<std::string>());
    const auto iv = params.find("intervalMs");
    const int interval = (iv != params.end() && iv->is_number()) ? iv->get<int>() : kIntervalMs;
    if (start == 0 || interval <= 0) return;

    std::vector<double> flow, press;
    double slow[14];
    for (double& v : slow) v = std::nan("");
    for (const auto& obj : *arr) {
        if (!obj.is_object()) continue;
        for (auto it = obj.begin(); it != obj.end(); ++it) {
            if (!it->is_array()) continue;
            const uint8_t ch = channelOfKey(it.key());
            if (!ch) continue;
            for (const auto& v : *it) {
                const double f = v.is_number() ? v.get<double>() : std::nan("");
                if (ch == kFlow) { if (flow.size() < 8) flow.push_back(f); }
                else if (ch == kMaskPressure) { if (press.size() < 8) press.push_back(f); }
                else if (!std::isnan(f)) slow[ch] = f;
            }
        }
    }
    const size_t n = std::max(flow.size(), press.size());
    if (n == 0) return;
    flow.resize(n, std::nan(""));
    press.resize(n, std::nan(""));

    // A report that does not follow on from the second being filled closes it
    // (a report's own jitter is allowed, a lost report is not).
    if (start_ms_ != 0) {
        const int64_t expect = start_ms_ + static_cast<int64_t>(flow_.size()) * interval_ms_;
        if (std::llabs(start - expect) > interval_ms_ * 2) emit();
    }
    if (start_ms_ == 0) {
        resetSecond();
        start_ms_ = start;
        interval_ms_ = interval;
    }
    for (size_t i = 0; i < n; i++) {
        flow_.push_back(quantise(kFlow, flow[i]));
        press_.push_back(quantise(kMaskPressure, press[i]));
        if (static_cast<int>(flow_.size()) == kFastPerSecond) {
            // Slow values belong to the second they arrived in.
            for (uint8_t id = 3; id <= 13; id++)
                if (!std::isnan(slow[id])) { slow_[id] = quantise(static_cast<Channel>(id), slow[id]); slow_set_[id] = true; }
            for (double& v : slow) v = std::nan("");
            emit();
            start_ms_ = start + static_cast<int64_t>(i + 1) * interval;
            interval_ms_ = interval;
        }
    }
    if (flow_.empty()) { start_ms_ = 0; return; }
    for (uint8_t id = 3; id <= 13; id++)
        if (!std::isnan(slow[id])) { slow_[id] = quantise(static_cast<Channel>(id), slow[id]); slow_set_[id] = true; }
}

} // namespace hms_cpap::as11
