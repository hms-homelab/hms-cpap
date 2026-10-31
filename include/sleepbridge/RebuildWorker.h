#pragma once
//
// One bridge's rebuild: the banked batches replayed into its IRebuilder, and
// the files each look returns written into the bridge's card root, which
// hms-cpap then reads as a local card folder (LocalDataSource) like any other.
//
// Incremental. Each step feeds only what was banked since the last one, in
// sequence order, every record exactly once: a record at or below the last
// sequence fed is skipped, which is what makes a batch stored twice (a crash
// between its fsync and its ack, then the bridge's resend in a different cut)
// harmless. A card statement goes in at its place: before the record whose
// sequence it was banked ahead of.
//
// What a look returns is written atomically, so the burst loop never reads
// half a file; a piece cut from a damaged file replaces that file. Held files
// (seen in part) are listed in batches/held.json and never written. A path
// that would leave the card root, or land in batches/, is refused.
//
// The card's geometry (the layout that decides where every sector belongs,
// and how it was confirmed) is kept in batches/geometry.json, written
// durably when a look first reports one and whenever it changes. After a
// restart the rebuild is built from it, before any batch is fed, so it
// neither forgets the layout nor holds the session until the bank proves it
// again; then the whole bank is fed again from the start: the batches are
// the record, the files are derived, and the geometry file is a cache of
// what they said last. When a later boot sector says otherwise, the boot
// sector wins and the file is rewritten.
//
#include "sleepbridge/BatchStore.h"
#include "sleepbridge/Rebuilder.h"

#include <cstdint>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <vector>

namespace hms_cpap::sleepbridge {

struct StepReport {
    bool     looked{false};
    /// This worker's first step: the whole bank fed again after a start, and
    /// every file of the card returned, whether or not it changed tonight.
    bool     catch_up{false};
    std::string machine;           // the look's machine: "resmed", "luna", ""
    int      batches_fed{0};
    int      records_fed{0};
    int      records_skipped{0};   // at or below what was already fed
    int      cards_fed{0};
    int      written{0};
    std::vector<std::string> written_paths;   // card-relative, as written this step
    int      removed{0};           // files replaced by their cut pieces
    int      refused_paths{0};
    int      unreadable{0};        // a banked file that would not parse
    bool     holding{false};
    std::string holding_reason;
    uint64_t held_records{0};
    uint64_t held_files{0};
    uint64_t complete_files{0};
    RebuildGeometry geometry;      // the look's: valid once a layout is trusted
};

/// A card-relative path the rebuild may write: '/' separated, no empty, '.'
/// or '..' segment, no drive or backslash, at most 255 bytes, not under
/// batches/.
bool safeCardPath(const std::string& path);

/// batches/geometry.json: the geometry as a small JSON object, one field per
/// member. parseGeometryJson answers nullopt for anything that is not one
/// (a damaged file is ignored and the bank establishes the layout again).
constexpr const char* kGeometryFile = "geometry.json";
std::string                    geometryJson(const RebuildGeometry& g);
std::optional<RebuildGeometry> parseGeometryJson(const std::string& text);

class RebuildWorker {
public:
    /// Builds its rebuilder from `factory` with the geometry kept last time
    /// (batches/geometry.json; or an older build's batches/rebuild.state,
    /// read once and replaced by the file after the next look).
    RebuildWorker(BatchStore& store, std::string sender, const RebuilderFactory& factory);

    /// Feed what is new and, when anything was (or `force`), look and write.
    StepReport step(bool force = false);

    const std::string& sender() const { return sender_; }
    std::optional<int64_t> fedThrough() const { return fed_through_; }
    bool ready() const { return rebuilder_ != nullptr; }
    /// What batches/geometry.json holds (valid() false while nothing does).
    const RebuildGeometry& geometry() const { return geometry_; }

private:
    void feedBatchFile(const std::string& path, StepReport& r);

    BatchStore&                 store_;
    std::string                 sender_;
    std::unique_ptr<IRebuilder> rebuilder_;
    std::set<std::string>       fed_files_;
    bool                        first_step_{true};
    std::optional<int64_t>      fed_through_;
    RebuildGeometry             geometry_;
    std::string                 held_json_;
};

}  // namespace hms_cpap::sleepbridge
