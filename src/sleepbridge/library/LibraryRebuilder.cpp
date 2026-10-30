// The Sleep Bridge component behind hms-cpap's IRebuilder. Compiled only when
// BUILD_WITH_SLEEPBRIDGE linked the component (cmake/SleepBridge.cmake), which
// also defines HMS_WITH_SLEEPBRIDGE.

#include "sleepbridge/Rebuilder.h"

#include <sleepbridge/rebuild.h>

#include <utility>

namespace hms_cpap::sleepbridge {

namespace {

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

    std::string state() const override { return rebuild_.state(); }

private:
    ::sleepbridge::Rebuild rebuild_;
};

}  // namespace

std::unique_ptr<IRebuilder> makeRebuilder(const std::string& saved_state,
                                          std::function<void(const std::string&)> log) {
    ::sleepbridge::Options opt;
    opt.saved_state = saved_state;
    opt.log         = std::move(log);
    return std::make_unique<LibraryRebuilder>(std::move(opt));
}

bool rebuilderInBuild() { return true; }

std::string rebuilderVersion() { return ::sleepbridge::version(); }

}  // namespace hms_cpap::sleepbridge
