#pragma once
//
// The night as it is being streamed: what the bridge's rebuilt card says
// right now, before any burst cycle reads it.
//
// The rebuild writes a card file whenever it grows. A session's BRP, PLD or
// oximetry file growing is the machine writing therapy: the mask is on. The
// EVE and CSL grow by one annotation per machine event; each look that
// rewrites one is read and the annotations past the ones already seen are
// the new events. The STR is written by a ResMed at mask-off (SDD-053), so
// the STR growing after therapy is the mask coming off; a mask that writes
// nothing for `mask_quiet` is taken off too.
//
// Nothing here stores or counts for the night's record: the burst cycle
// parses the files and owns the numbers. This is the live view, pushed as
// it happens, to the house's MQTT and the log.
//
#include <chrono>
#include <cstdint>
#include <functional>
#include <map>
#include <string>
#include <vector>

namespace hms_cpap::sleepbridge {

struct LiveEvent {
    std::string type;          // OA, CA, H, UA, RERA, CSR, FL, OTHER
    std::string description;   // the machine's own label
    std::chrono::system_clock::time_point at;
    double      duration_s{0};
    std::string file;          // card-relative EVE or CSL it came from
};

/// Where the live view goes. Each may be empty.
struct LiveSink {
    std::function<void(const LiveEvent&)>                                 event;
    std::function<void(const std::map<std::string, int>& by_type, int total)> counts;
    std::function<void(bool on)>                                          mask;
};

/// The short type an annotation's label stands for ("" for a marker that is
/// no event, such as "Recording starts").
std::string liveEventType(const std::string& description);

class LiveNight {
public:
    using Clock = std::chrono::system_clock;

    LiveNight(std::string card_root, LiveSink sink,
              std::chrono::seconds mask_quiet = std::chrono::seconds(300));

    /// Files the rebuild wrote this step (card-relative paths). A catch-up
    /// step (the first after a start, the whole card written again) says
    /// nothing about now: its EVE and CSL are read so their annotations are
    /// known and not reported later as new, and the mask is left alone.
    void onWritten(const std::vector<std::string>& paths, Clock::time_point now = Clock::now(),
                   bool catch_up = false);

    /// Time passing with nothing written.
    void tick(Clock::time_point now = Clock::now());

    bool maskOn() const { return mask_on_; }
    int  total() const { return total_; }
    const std::map<std::string, int>& counts() const { return counts_; }

private:
    void setMask(bool on);
    void readEvents(const std::string& path, bool report);

    std::string                 root_;
    LiveSink                    sink_;
    std::chrono::seconds        mask_quiet_;
    bool                        mask_on_{false};
    Clock::time_point           last_therapy_write_{};
    std::map<std::string, size_t> seen_;      // annotations already reported, per file
    std::map<std::string, int>    counts_;
    int                           total_{0};
};

}  // namespace hms_cpap::sleepbridge
