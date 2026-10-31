#pragma once

#include "clients/As11Stream.h"

#include <cpapdash/parser/EDFWriter.h>

#include <cstdint>
#include <functional>
#include <map>
#include <mutex>
#include <set>
#include <string>
#include <vector>

namespace hms_cpap {

/**
 * Whatever feeds an AirSense 11's night (SDD-057 step 4). Two producers drive
 * it: hms-cpap's own Bluetooth session (the machine's StreamData assembled
 * into blocks) and a bridge that sends the same CDS1 blocks over the network.
 * The writer behind it cannot tell them apart.
 */
class IAs11NightSink {
public:
    virtual ~IAs11NightSink() = default;
    /// One block of the stream; b.serial names the machine.
    virtual void onBlock(const as11::StreamBlock& b) = 0;
    /// One of the machine's own scored events.
    virtual void onEvent(const std::string& serial, const as11::RespEvent& e) = 0;
    /// The stream ended (therapy off, or the source let go): the night's
    /// files are completed, the events included.
    virtual void onStreamEnd(const std::string& serial) = 0;
};

/// The machine's wall clock for a UTC instant: what its card would name a
/// file by.
using WallClockFn = std::function<cpapdash::parser::WallClock(int64_t utc_ms)>;
WallClockFn fixedOffsetClock(int offset_min_east);
WallClockFn hostLocalClock();
/// The machine's offset east of UTC, from a Summary day's start (the
/// machine's noon as an instant); nullopt unless it falls on a quarter hour.
std::optional<int> machineOffsetFromDayStart(int64_t day_start_utc_ms);
/// "YYYYMMDD" of the sleep day: before noon belongs to the day before.
std::string sleepDayFolder(const cpapdash::parser::WallClock& wc);

/**
 * Lays the stream out as a card would, under <data_dir>/as11/<serial>/:
 *
 *   DATALOG/<sleep day>/<stamp>_BRP.edf   flow, mask pressure at 25 Hz
 *                      /<stamp>_PLD.edf   the 2 s channels, when any came
 *                      /<stamp>_SAD.edf   SpO2 and pulse, when a ring rode
 *                      /<first stamp>_EVE.edf, _CSL.edf   the machine's events
 *
 * A segment is cut at a 60 s pause; each is named by its first block on the
 * machine's wall clock. Blocks are kept on disk as they arrive
 * (stream/<sleep day>/<segment start>.cds1s, fsync'd), so a restart loses
 * nothing and a resent block is kept once. The night is written live: the
 * open segment's files are rewritten every 10 s of stream (every block from
 * a Mule & Miner), each a valid EDF at every moment, and the night's EVE and
 * CSL, one pair under the night's first BRP, are rewritten with them and the
 * moment an event arrives. An event belongs to the BRP that covers it, or
 * that ended at most 2 minutes before it. The stream's end only completes
 * what is open.
 */
class As11NightWriter : public IAs11NightSink {
public:
    As11NightWriter(std::string data_dir, WallClockFn default_clock);

    /// The wall clock for one machine (its own offset once a Summary showed
    /// it); the default otherwise.
    void setClock(const std::string& serial, WallClockFn clock);

    void onBlock(const as11::StreamBlock& b) override;
    void onEvent(const std::string& serial, const as11::RespEvent& e) override;
    void onStreamEnd(const std::string& serial) override;

    std::string rootFor(const std::string& serial) const;
    /// Rewrites every file of one sleep day from what is on disk.
    void writeNight(const std::string& serial, const std::string& night);

    static constexpr int64_t kSegmentGapMs = 60 * 1000;
    static constexpr int64_t kEventTailMs = 2 * 60 * 1000;
    static constexpr int64_t kLiveEveryMs = 10 * 1000;

private:
    struct Segment {
        std::string night;
        std::string file;           // the .cds1s
        int64_t start_ms = 0, end_ms = 0;
        std::set<int64_t> starts;   // blocks held, by start
        int64_t written_end = 0;    // end the EDFs were last written for
    };
    struct Machine {
        std::vector<Segment> segments;
        bool loaded = false;
        std::set<std::string> nights_touched;
        std::set<std::string> event_keys;
    };

    Machine& machine(const std::string& serial);       // mtx_ held
    void load(const std::string& serial, Machine& m);  // mtx_ held
    WallClockFn clockFor(const std::string& serial) const;
    void writeSegment(const std::string& serial, const Segment& s);
    void writeEvents(const std::string& serial, const std::string& night);
    std::vector<as11::StreamBlock> readSegment(const std::string& file) const;

    std::string data_dir_;
    WallClockFn default_clock_;
    std::map<std::string, WallClockFn> clocks_;
    std::map<std::string, Machine> machines_;
    mutable std::mutex mtx_;
};

/// The machine's IdentificationProfiles result written as a card holds it,
/// {"FlowGenerator": <result>}, to <root>/Identification.json, only when none
/// is there. False when the result names no serial, or the file there names
/// another machine (left alone).
bool writeIdentification(const std::string& root, const std::string& result_json);

} // namespace hms_cpap
