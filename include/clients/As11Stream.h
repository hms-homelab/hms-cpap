#pragma once

#include <nlohmann/json.hpp>

#include <cstddef>
#include <cstdint>
#include <functional>
#include <limits>
#include <optional>
#include <string>
#include <vector>

namespace hms_cpap::as11 {

/**
 * The AirSense 11's live stream, in the one form every source hands the night
 * writer (SDD-057 step 4, push-c3 SDD-041 D10): CDS1 blocks.
 *
 * CDS1 is the stream's block format:
 *
 *   magic "CDS1" 4, version 1 (=1), nchan 1, interval_ms 2 LE, start_ms 8 LE
 *   (the machine's clock, UTC milliseconds), serial_len 1, serial, then per
 *   channel: id 1, cadence 1 (0 = one value per interval_ms, 1 = one per 2 s),
 *   count 2 LE, count int16 LE values. INT16_MIN means the machine sent none.
 *
 * Two producers make these blocks: hms-cpap's own Bluetooth session, which
 * assembles them from the machine's StreamData reports (SecondAssembler), and
 * a bridge that sends them already encoded (decodeCds1). Values are integers
 * in fixed units (see quantise/physical), so the writer cannot tell which
 * source a night came from.
 */

enum Channel : uint8_t {
    kFlow = 1,                // mL/s
    kMaskPressure = 2,        // 0.01 cmH2O
    kMaskPressure2s = 3,      // 0.01 cmH2O
    kInspPressure2s = 4,      // 0.01 cmH2O
    kExpPressure2s = 5,       // 0.01 cmH2O
    kLeak = 6,                // mL/s
    kRespRate = 7,            // 0.1 /min
    kTidalVolume = 8,         // mL
    kMinuteVent = 9,          // 0.01 L/min
    kSnore = 10,              // 0.01
    kFlowLimitation = 11,     // 0.001
    kSpO2 = 12,               // 0.1 %
    kHeartRate = 13,          // bpm
};

constexpr int16_t kNull = std::numeric_limits<int16_t>::min();
constexpr int kIntervalMs = 40;      // the fast channels: 25 Hz
constexpr int kFastPerSecond = 25;
constexpr int kSlowCadenceMs = 2000; // cadence 1

struct StreamChannel {
    uint8_t id = 0;
    uint8_t cadence = 0;             // 0: per interval_ms, 1: per 2 s
    std::vector<int16_t> values;
};

struct StreamBlock {
    int64_t start_ms = 0;            // the machine's clock, UTC ms
    int interval_ms = kIntervalMs;
    std::string serial;
    std::vector<StreamChannel> channels;

    const StreamChannel* channel(uint8_t id) const;
    /// Where the block ends: its fast samples' span, else 1 s.
    int64_t endMs() const;
};

std::vector<uint8_t> encodeCds1(const StreamBlock& b);
/// nullopt when the bytes are not a whole, valid version 1 block.
std::optional<StreamBlock> decodeCds1(const uint8_t* data, size_t len);
inline std::optional<StreamBlock> decodeCds1(const std::vector<uint8_t>& v) {
    return decodeCds1(v.data(), v.size());
}

/// A physical value (the machine's units: L/s, cmH2O, L, L/min, /min, %, bpm)
/// to the channel's integer; kNull for NaN; clamped to +-32767.
int16_t quantise(Channel ch, double v);
/// The exact inverse of quantise: the integer back to the machine's units
/// (flow and leak L/s, pressures cmH2O, tidal volume L, minute ventilation
/// L/min, resp rate /min, SpO2 %, heart rate bpm). NaN for kNull.
double physical(Channel ch, int16_t v);

/// "2026-10-17T14:02:33.656Z" to UTC milliseconds; 0 when it does not parse.
int64_t isoToMs(const std::string& s);

/// One of the machine's own scored events, as it reports them
/// (TherapyEvents-RespiratoryEvents).
struct RespEvent {
    std::string event;               // "Apnea", "Hypopnea", ... as the machine names it
    std::string reportTime;          // ISO UTC, when the machine reported it
    int durationSeconds = -1;
    int backdateSeconds = -1;        // how far before reportTime it began
};

/// The channel a StreamData key names, 0 for one not kept.
uint8_t channelOfKey(const std::string& key);

/// The stream ids StartStream asks for (the set the bridge was proven with).
const std::vector<std::string>& streamIds();

/**
 * The machine's StreamData reports into one-second CDS1 blocks, as the bridge
 * forms them: a report every 200 ms carries up to 8 samples of each fast
 * channel (flow, mask pressure) and the slow channels' latest values. A
 * second is handed on when 25 fast samples are in, or when a report does not
 * follow on from it (a lost report closes the second rather than shifting
 * the samples after it).
 */
class SecondAssembler {
public:
    using Sink = std::function<void(StreamBlock&&)>;
    explicit SecondAssembler(Sink sink) : sink_(std::move(sink)) { resetSecond(); }

    void setSerial(const std::string& s) { serial_ = s; }
    /// The params object of one StreamData notification.
    void onStreamData(const nlohmann::json& params);
    /// Hands on a partly filled second (the stream's end).
    void flush();

private:
    void emit();

    Sink sink_;
    std::string serial_;
    int64_t start_ms_ = 0;
    int interval_ms_ = kIntervalMs;
    std::vector<int16_t> flow_, press_;
    int16_t slow_[14] = {};
    bool slow_set_[14] = {};
    void resetSecond();
};

} // namespace hms_cpap::as11
