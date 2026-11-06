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
// nothing for `mask_quiet` is taken off too, but only once the bridge has
// shown it is still delivering (issue #57).
//
// Silence from the bridge is not silence from the machine. A bridge on a weak
// link can hold minutes of the night and deliver them late (seen on a real
// night: four and a half minutes with nothing delivered, then the backlog,
// the therapy file continuing without a gap). So the quiet rule needs a batch delivered after
// the last therapy write that brought no therapy with it: the stream is
// alive and the machine has stopped. With no batch at all since the last
// therapy write, a stall cannot be told from an end, and the mask is held
// until `stall_limit`. Either way the moment said is the machine's own, the
// end of the session's last record.
//
// A mask said off on quiet whose same session file grows again was a stall
// after all: it is said on again, for the same session (a real mask-off and
// on starts a new session file).
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
    /// The same change with its moment: the session (the therapy file's
    /// stem, e.g. 20261030_223000) and when it happened, the session's start
    /// for on and the end of its last record for off, the file's name read
    /// in the host's zone.
    std::function<void(bool on, const std::string& session,
                       std::chrono::system_clock::time_point occurred)>   mask_change;
};

/// The short type an annotation's label stands for ("" for a marker that is
/// no event, such as "Recording starts").
std::string liveEventType(const std::string& description);

class LiveNight {
public:
    using Clock = std::chrono::system_clock;

    LiveNight(std::string card_root, LiveSink sink,
              std::chrono::seconds mask_quiet  = std::chrono::seconds(300),
              std::chrono::seconds stall_limit = std::chrono::seconds(15 * 60));

    /// How long a batch must have been with us before it counts as the
    /// stream being alive: the rebuild that follows a batch runs within the
    /// service's most_s (60 s), so a batch older than this that wrote no
    /// therapy file brought none.
    static constexpr std::chrono::seconds kDeliverySettled{60};

    /// Files the rebuild wrote this step (card-relative paths). A catch-up
    /// step (the first after a start, the whole card written again) says
    /// nothing about now: its EVE and CSL are read so their annotations are
    /// known and not reported later as new, and the mask is left alone.
    void onWritten(const std::vector<std::string>& paths, Clock::time_point now = Clock::now(),
                   bool catch_up = false);

    /// The bridge delivered a batch with records (stored, whatever it holds).
    void onDelivered(Clock::time_point now = Clock::now());

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
    std::chrono::seconds        stall_limit_;
    bool                        mask_on_{false};
    Clock::time_point           last_therapy_write_{};
    Clock::time_point           last_delivery_{};
    std::string                 therapy_file_;   // the newest therapy write, card-relative
    std::map<std::string, size_t> seen_;      // annotations already reported, per file
    std::map<std::string, int>    counts_;
    int                           total_{0};
};

}  // namespace hms_cpap::sleepbridge
