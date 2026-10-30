#pragma once
//
// The rebuild: sector records in, card files out.
//
// hms-cpap does not rebuild files from sectors itself. That is the prebuilt
// Sleep Bridge component (BUILD_WITH_SLEEPBRIDGE, its own licence in
// LICENSE-sleepbridge), reached through its one public header,
// <sleepbridge/rebuild.h>. This interface is the seam between the two, shaped
// like that header: the receiver and the worker are written and tested
// against it, and src/sleepbridge/library/ adapts the component to it, compiled
// only when the component is in the build.
//
// Without the component, makeRebuilder() answers nullptr, and the receiver
// refuses every bridge with "not in this build" rather than store batches
// nothing here can read.
//
#include "sleepbridge/Wire.h"

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace hms_cpap::sleepbridge {

/// A file to write into the card root.
struct RebuildFile {
    std::string path;       // card-relative, '/' separated
    std::string bytes;      // the whole file as it stands now
    /// Non-empty: this is a piece cut from the closed file at that path, whose
    /// damaged records were cut out. The pieces replace that file.
    std::string cut_from;
};

/// A file seen in part: never written, shown so a person sees what is missing.
struct RebuildHeld {
    std::string path;
    uint64_t    size{0};
    uint64_t    missing_sectors{0};
};

struct RebuildLook {
    /// Which machine's card this is, as the rebuild recognised it: "resmed",
    /// "luna" (BMC / React Health Luna), or "" while it cannot tell. Nothing
    /// here assumes one: the files are written at the paths the rebuild
    /// gives, in that machine's own card layout.
    std::string machine;
    bool        layout_known{false};
    /// Records wait, not applied, until the card's layout is confirmed (the
    /// machine mounts the card, or the writes prove it).
    bool        holding{false};
    std::string holding_reason;
    uint64_t    held_records{0};
    /// Files whose bytes changed since the previous look (all on the first).
    std::vector<RebuildFile> files;
    /// Every file currently held.
    std::vector<RebuildHeld> held;
    uint64_t    complete_files{0};
    uint64_t    unseen_files{0};
};

/// One bridge's card, rebuilt. Records fed in sequence order; a record fed
/// twice changes nothing. Called from one thread.
class IRebuilder {
public:
    virtual ~IRebuilder() = default;
    /// A sector the machine wrote, or READ (a CYR1 batch, around a mount).
    virtual void record(uint64_t seq, uint32_t lba, const uint8_t* sector512, bool read) = 0;
    /// The card statement, at its place in the stream.
    virtual void card(const CardStatement& statement) = 0;
    virtual RebuildLook look() = 0;
    /// An opaque string to keep and hand back after a restart.
    virtual std::string state() const = 0;
};

/// Builds one bridge's rebuilder from the state it saved before a restart
/// ("" for none). Answers nullptr when this build has no rebuild.
using RebuilderFactory = std::function<std::unique_ptr<IRebuilder>(const std::string& saved_state)>;

/// The hms-cpap card format a rebuilt card root of `machine` is read as
/// ("resmed", "bmc"), or "" for a machine it does not know.
std::string formatForMachine(const std::string& machine);

/// The component's rebuilder, or nullptr when this build has none. `log`
/// receives the component's one-line notes; may be empty.
std::unique_ptr<IRebuilder> makeRebuilder(const std::string& saved_state,
                                          std::function<void(const std::string&)> log);
/// True when this build carries the component.
bool rebuilderInBuild();
/// The component's own version string, or "" without it.
std::string rebuilderVersion();

}  // namespace hms_cpap::sleepbridge
