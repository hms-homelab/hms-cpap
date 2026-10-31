// The Sleep Bridge component behind hms-cpap's IRebuilder. Compiled only when
// BUILD_WITH_SLEEPBRIDGE linked the component (cmake/SleepBridge.cmake), which
// also defines HMS_WITH_SLEEPBRIDGE.

#include "sleepbridge/Rebuilder.h"

#include <sleepbridge/rebuild.h>

#include <utility>

namespace hms_cpap::sleepbridge {

namespace {

::sleepbridge::Geometry toComponent(const RebuildGeometry& g) {
    ::sleepbridge::Geometry c;
    c.sectors_per_cluster = g.sectors_per_cluster;
    c.data_begin_lba      = g.data_begin_lba;
    c.root_cluster        = g.root_cluster;
    c.fat_begin_lba       = g.fat_begin_lba;
    c.fat_sectors         = g.fat_sectors;
    c.fats                = g.fats;
    c.cluster_count       = g.cluster_count;
    c.cid                 = g.cid;
    c.origin              = g.origin;
    c.confirmed_by        = g.confirmed_by;
    c.session             = g.session;
    c.since               = g.since;
    return c;
}

RebuildGeometry fromComponent(const ::sleepbridge::Geometry& c) {
    RebuildGeometry g;
    if (!c.valid()) return g;
    g.sectors_per_cluster = c.sectors_per_cluster;
    g.data_begin_lba      = c.data_begin_lba;
    g.root_cluster        = c.root_cluster;
    g.fat_begin_lba       = c.fat_begin_lba;
    g.fat_sectors         = c.fat_sectors;
    g.fats                = c.fats;
    g.cluster_count       = c.cluster_count;
    g.cid                 = c.cid;
    g.origin              = c.origin;
    g.confirmed_by        = c.confirmed_by;
    g.session             = c.session;
    g.since               = c.since;
    return g;
}

class LibraryRebuilder : public IRebuilder {
public:
    explicit LibraryRebuilder(::sleepbridge::Options opt) : rebuild_(std::move(opt)) {}

    void record(uint64_t seq, uint32_t lba, const uint8_t* sector512, bool read) override {
        rebuild_.addRecord(seq, lba, sector512, read);
    }

    void card(const CardStatement& c) override {
        ::sleepbridge::CardStatement s;
        s.cid       = c.cid;
        s.csd       = c.csd;
        s.has_mount = c.mount.has_value();
        s.mount     = c.mount.value_or(0);
        s.has_seq   = c.seq.has_value();
        s.seq       = c.seq.value_or(0);
        // The layout the statement carries, for a stream that began after
        // the bridge read the boot sector: a hypothesis the component tests.
        s.has_layout          = c.sectors_per_cluster != 0;
        s.sectors_per_cluster = c.sectors_per_cluster;
        s.data_begin_lba      = c.data_begin_lba;
        rebuild_.addCardStatement(s);
    }

    RebuildLook look() override {
        ::sleepbridge::Look l = rebuild_.look();
        RebuildLook out;
        // out.machine stays "" until the header names the machine kind.
        out.layout_known   = l.layout_known;
        out.holding        = l.holding;
        out.holding_reason = l.holding_reason;
        out.held_records   = l.held_records;
        out.complete_files = l.complete_files;
        out.unseen_files   = l.unseen_files;
        out.geometry       = fromComponent(l.geometry);
        out.files.reserve(l.files.size());
        for (auto& f : l.files) {
            RebuildFile r;
            r.path     = std::move(f.path);
            r.bytes.assign(reinterpret_cast<const char*>(f.data.data()), f.data.size());
            r.cut_from = std::move(f.cut_from);
            out.files.push_back(std::move(r));
        }
        out.held.reserve(l.held.size());
        for (auto& h : l.held) out.held.push_back({std::move(h.path), h.size, h.missing_sectors});
        return out;
    }

private:
    ::sleepbridge::Rebuild rebuild_;
};

}  // namespace

std::unique_ptr<IRebuilder> makeRebuilder(const RebuildStart& start,
                                          std::function<void(const std::string&)> log) {
    ::sleepbridge::Options opt;
    if (start.geometry.valid()) opt.geometry = toComponent(start.geometry);
    else opt.saved_state = start.legacy_state;
    opt.log = std::move(log);
    return std::make_unique<LibraryRebuilder>(std::move(opt));
}

bool rebuilderInBuild() { return true; }

std::string rebuilderVersion() { return ::sleepbridge::version(); }

}  // namespace hms_cpap::sleepbridge
