#pragma once
// Builders for Sleep Bridge wire bytes, a scratch folder, and a fake rebuild,
// shared by the sleepbridge suites.

#include "sleepbridge/Rebuilder.h"
#include "sleepbridge/Wire.h"

#include <atomic>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <map>
#include <memory>
#include <random>
#include <set>
#include <string>
#include <vector>

namespace sbtest {

namespace fs = std::filesystem;
using namespace hms_cpap::sleepbridge;

inline void put16(std::string& s, uint16_t v) {
    s.push_back(static_cast<char>(v & 0xFF));
    s.push_back(static_cast<char>(v >> 8));
}
inline void put32(std::string& s, uint32_t v) {
    for (int i = 0; i < 4; i++) s.push_back(static_cast<char>((v >> (8 * i)) & 0xFF));
}
inline void put64(std::string& s, uint64_t v) {
    for (int i = 0; i < 8; i++) s.push_back(static_cast<char>((v >> (8 * i)) & 0xFF));
}

inline std::string header(const char* magic, uint16_t count, uint64_t first, uint32_t epoch = 1) {
    std::string s(magic, 4);
    put16(s, count);
    put64(s, first);
    put32(s, epoch);
    return s;
}

/// A sector whose every byte says which lba and which sequence wrote it.
inline std::string sector(uint32_t lba, uint64_t seq) {
    std::string s(kSectorSize, '\0');
    for (size_t i = 0; i < kSectorSize; i++)
        s[i] = static_cast<char>((lba * 31 + seq * 7 + i) & 0xFF);
    return s;
}

/// CYS1 (or CYR1): records at the given lbas, sequences first, first+1, ...
inline std::string batch(uint64_t first, const std::vector<uint32_t>& lbas, bool reads = false) {
    std::string s = header(reads ? "CYR1" : "CYS1", static_cast<uint16_t>(lbas.size()), first);
    for (size_t i = 0; i < lbas.size(); i++) {
        put32(s, lbas[i]);
        s += sector(lbas[i], first + i);
    }
    return s;
}

/// A run of `n` writes at lbas base, base+1, ...
inline std::string run(uint64_t first, size_t n, uint32_t base = 100) {
    std::vector<uint32_t> l;
    for (size_t i = 0; i < n; i++) l.push_back(base + static_cast<uint32_t>(i));
    return batch(first, l);
}

inline std::string heartbeat() { return header("CYS1", 0, 0); }

inline std::string cardStatement(const std::string& json) { return header("CYS1", 0, 0) + json; }

inline const char* kCard =
    R"({"cid":"0123456789abcdef0123456789abcdef","csd":"400e00325b5900003b377f800a400000","capacity_blocks":15523840,"mount":3,"seq":0})";

/// The 12-byte frame header, then the body.
inline std::string frame(uint32_t seq, const std::string& body, uint16_t kind = kKindSectorBatch) {
    FrameHeader h;
    h.seq   = seq;
    h.flags = 1;
    h.kind  = kind;
    return encodeFrameHeader(h) + body;
}

inline const uint8_t* u8(const std::string& s) { return reinterpret_cast<const uint8_t*>(s.data()); }

inline std::string slurp(const fs::path& p) {
    std::ifstream f(p, std::ios::binary);
    return std::string(std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>());
}

inline std::vector<std::string> filesIn(const fs::path& dir, const std::string& ext) {
    std::vector<std::string> out;
    std::error_code ec;
    for (const auto& e : fs::directory_iterator(dir, ec))
        if (e.is_regular_file() && e.path().extension() == ext) out.push_back(e.path().filename().string());
    return out;
}

struct TempDir {
    fs::path path;
    TempDir() {
        static std::atomic<int> n{0};
        std::random_device rd;
        path = fs::temp_directory_path() /
               ("hms_sleepbridge_" + std::to_string(rd()) + "_" + std::to_string(n++));
        fs::create_directories(path);
    }
    ~TempDir() {
        std::error_code ec;
        fs::remove_all(path, ec);
    }
};

/// A stand-in for the component, shaped like it: every written lba is a file
/// DATALOG/20261030/<lba>.edf holding the latest sector written there. It
/// holds everything until it has seen a card statement or a read of lba 0
/// (the boot sector at a mount), and its state() says whether it got there,
/// so a restart built from that state does not hold again. A look returns
/// only files that changed. STR.edf is lba 7 and 8; with only one of them it
/// is held. Once lba 501 is written, the file for lba 500 is "closed and
/// damaged": it comes back as two pieces cut from it.
struct FakeLog {
    std::vector<uint64_t> seqs;   // every sequence fed, in order
    int cards{0};
    int reads{0};
    std::vector<std::string> saved_states;   // what each rebuilder was built from
    std::string machine{"resmed"};           // what a look says, once trusted
};

class FakeRebuilder : public IRebuilder {
public:
    explicit FakeRebuilder(std::shared_ptr<FakeLog> log, std::vector<std::string> extra_paths = {},
                           const std::string& saved = "")
        : log_(std::move(log)), extra_(std::move(extra_paths)), trusted_(saved == "trusted") {
        log_->saved_states.push_back(saved);
    }

    void card(const CardStatement&) override {
        ++log_->cards;
        trusted_ = true;
    }
    void record(uint64_t seq, uint32_t lba, const uint8_t* s, bool read) override {
        log_->seqs.push_back(seq);
        if (read) {
            ++log_->reads;
            if (lba == 0) trusted_ = true;
            return;
        }
        sectors_[lba] = std::string(reinterpret_cast<const char*>(s), kSectorSize);
        ++pending_;
    }
    RebuildLook look() override {
        RebuildLook l;
        if (!trusted_) {
            l.holding        = true;
            l.holding_reason = "no_mount_since_boot";
            l.held_records   = pending_;
            return l;
        }
        l.layout_known = true;
        l.machine      = log_->machine;
        auto offer = [&](const std::string& path, const std::string& bytes, const std::string& cut) {
            ++l.complete_files;
            auto& last = sent_[path];
            if (last == bytes) return;
            last = bytes;
            l.files.push_back({path, bytes, cut});
        };
        const bool cut500 = sectors_.count(500) && sectors_.count(501);
        for (const auto& [lba, bytes] : sectors_) {
            if (lba == 7 || lba == 8) continue;
            if (lba == 500 && cut500) {
                offer("DATALOG/20261030/500-a.edf", bytes.substr(0, 256), "DATALOG/20261030/500.edf");
                offer("DATALOG/20261030/500-b.edf", bytes.substr(256), "DATALOG/20261030/500.edf");
                continue;
            }
            offer("DATALOG/20261030/" + std::to_string(lba) + ".edf", bytes, "");
        }
        const bool s7 = sectors_.count(7) != 0, s8 = sectors_.count(8) != 0;
        if (s7 && s8) offer("STR.edf", sectors_[7] + sectors_[8], "");
        else if (s7 || s8) l.held.push_back({"STR.edf", 1024, 1});
        for (const auto& p : extra_) offer(p, "x", "");
        return l;
    }
    std::string state() const override { return trusted_ ? "trusted" : ""; }

private:
    std::shared_ptr<FakeLog>           log_;
    std::vector<std::string>           extra_;
    std::map<uint32_t, std::string>    sectors_;
    std::map<std::string, std::string> sent_;
    uint64_t                           pending_{0};
    bool                               trusted_{false};
};

inline RebuilderFactory fakeFactory(std::shared_ptr<FakeLog> log, std::vector<std::string> extra = {}) {
    return [log, extra](const std::string& saved) {
        return std::make_unique<FakeRebuilder>(log, extra, saved);
    };
}

}  // namespace sbtest
