#include "sleepbridge/RebuildWorker.h"

#include <json/json.h>

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <tuple>
#include <vector>

namespace hms_cpap::sleepbridge {

namespace fs = std::filesystem;

namespace {

std::optional<int64_t> toInt(const std::string& s) {
    if (s.empty() || s.size() > 18) return std::nullopt;
    int64_t v = 0;
    for (char c : s) {
        if (c < '0' || c > '9') return std::nullopt;
        v = v * 10 + (c - '0');
    }
    return v;
}

// "<first>-<last>.cys"
std::optional<std::pair<int64_t, int64_t>> batchRange(const std::string& name) {
    if (name.size() < 5 || name.substr(name.size() - 4) != ".cys") return std::nullopt;
    const std::string stem = name.substr(0, name.size() - 4);
    const auto dash = stem.find('-');
    if (dash == std::string::npos) return std::nullopt;
    const auto a = toInt(stem.substr(0, dash));
    const auto b = toInt(stem.substr(dash + 1));
    if (!a || !b || *b < *a) return std::nullopt;
    return std::make_pair(*a, *b);
}

// "<next>-<ms>-<n>.json": fed before the record with sequence <next>.
std::optional<std::tuple<int64_t, int64_t, int64_t>> cardKey(const std::string& name) {
    if (name.size() < 6 || name.substr(name.size() - 5) != ".json") return std::nullopt;
    const std::string stem = name.substr(0, name.size() - 5);
    const auto d1 = stem.find('-');
    const auto d2 = d1 == std::string::npos ? d1 : stem.find('-', d1 + 1);
    if (d2 == std::string::npos) return std::nullopt;
    const auto a = toInt(stem.substr(0, d1));
    const auto b = toInt(stem.substr(d1 + 1, d2 - d1 - 1));
    const auto c = toInt(stem.substr(d2 + 1));
    if (!a || !b || !c) return std::nullopt;
    return std::make_tuple(*a, *b, *c);
}

bool readAll(const fs::path& p, std::string& out) {
    std::ifstream f(p, std::ios::binary);
    if (!f) return false;
    out.assign(std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>());
    return true;
}

}  // namespace

bool safeCardPath(const std::string& path) {
    if (path.empty() || path.size() > 255 || path.front() == '/') return false;
    if (path.find('\\') != std::string::npos || path.find(':') != std::string::npos) return false;
    if (path.find('\0') != std::string::npos) return false;
    size_t start = 0;
    bool   first = true;
    for (;;) {
        const auto end = path.find('/', start);
        const std::string seg =
            path.substr(start, end == std::string::npos ? std::string::npos : end - start);
        if (seg.empty() || seg == "." || seg == "..") return false;
        if (first) {
            std::string low = seg;
            std::transform(low.begin(), low.end(), low.begin(),
                           [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
            if (low == "batches") return false;
            first = false;
        }
        if (end == std::string::npos) break;
        start = end + 1;
    }
    return true;
}

RebuildWorker::RebuildWorker(BatchStore& store, std::string sender, const RebuilderFactory& factory)
    : store_(store), sender_(std::move(sender)) {
    readAll(fs::path(store_.batchesDir(sender_)) / "rebuild.state", saved_state_);
    if (factory) rebuilder_ = factory(saved_state_);
}

void RebuildWorker::feedBatchFile(const std::string& path, StepReport& r) {
    std::string bytes;
    Batch       b;
    if (!readAll(path, bytes) ||
        parseBatch(reinterpret_cast<const uint8_t*>(bytes.data()), bytes.size(), b) !=
            BatchError::None ||
        b.records.empty()) {
        ++r.unreadable;
        std::cout << "[sleepbridge] " << sender_ << " banked file unreadable, skipped: " << path
                  << std::endl;
        return;
    }
    for (size_t i = 0; i < b.records.size(); i++) {
        const auto seq = static_cast<int64_t>(b.first_sequence + i);
        if (fed_through_ && seq <= *fed_through_) {
            ++r.records_skipped;
            continue;
        }
        rebuilder_->record(static_cast<uint64_t>(seq), b.records[i].lba, b.records[i].data, b.reads);
        fed_through_ = seq;
        ++r.records_fed;
    }
    ++r.batches_fed;
}

StepReport RebuildWorker::step(bool force) {
    StepReport r;
    if (!rebuilder_) return r;
    const fs::path bdir = store_.batchesDir(sender_);

    // What was banked since the last step, in the order it belongs in the
    // stream: a batch at its first sequence, a statement just before the
    // sequence it was banked ahead of.
    struct Item {
        int64_t     at;
        int         order;   // 0: a card statement, 1: a batch, at the same `at`
        int64_t     t1{0}, t2{0};
        std::string path;
        std::string name;
    };
    std::vector<Item> items;
    std::error_code   ec;
    for (const auto& e : fs::directory_iterator(bdir, ec)) {
        if (!e.is_regular_file()) continue;
        const auto name = e.path().filename().string();
        if (fed_files_.count(name)) continue;
        if (const auto rg = batchRange(name))
            items.push_back({rg->first, 1, rg->second, 0, e.path().string(), name});
    }
    for (const auto& e : fs::directory_iterator(bdir / "cards", ec)) {
        if (!e.is_regular_file()) continue;
        const auto name = "cards/" + e.path().filename().string();
        if (fed_files_.count(name)) continue;
        if (const auto k = cardKey(e.path().filename().string()))
            items.push_back({std::get<0>(*k), 0, std::get<1>(*k), std::get<2>(*k),
                             e.path().string(), name});
    }
    std::sort(items.begin(), items.end(), [](const Item& a, const Item& b) {
        return std::tie(a.at, a.order, a.t1, a.t2) < std::tie(b.at, b.order, b.t1, b.t2);
    });

    for (const auto& it : items) {
        if (it.order == 0) {
            std::string text, why;
            const auto card = readAll(it.path, text) ? parseCardStatement(text, &why) : std::nullopt;
            if (card) {
                rebuilder_->card(*card);
                ++r.cards_fed;
            } else {
                ++r.unreadable;
            }
        } else {
            feedBatchFile(it.path, r);
        }
        fed_files_.insert(it.name);
    }

    if (items.empty() && !force) return r;

    const RebuildLook look = rebuilder_->look();
    r.looked         = true;
    r.machine        = look.machine;
    r.holding        = look.holding;
    r.holding_reason = look.holding_reason;
    r.held_records   = look.held_records;
    r.held_files     = look.held.size();
    r.complete_files = look.complete_files;

    const fs::path root = store_.cardRoot(sender_);
    std::set<std::string> replaced;
    for (const auto& f : look.files) {
        if (!safeCardPath(f.path) || (!f.cut_from.empty() && !safeCardPath(f.cut_from))) {
            ++r.refused_paths;
            std::cout << "[sleepbridge] " << sender_ << " refused a path: " << f.path << std::endl;
            continue;
        }
        std::string err;
        if (!durableWrite((root / fs::path(f.path)).string(), f.bytes.data(), f.bytes.size(),
                          &err)) {
            std::cout << "[sleepbridge] " << sender_ << " " << f.path << " not written: " << err
                      << std::endl;
            continue;
        }
        ++r.written;
        r.written_paths.push_back(f.path);
        if (!f.cut_from.empty()) replaced.insert(f.cut_from);
    }
    // The pieces are on disk; the file they replace goes after them.
    for (const auto& p : replaced) {
        std::error_code rec;
        if (fs::remove(root / fs::path(p), rec)) ++r.removed;
    }

    Json::Value held(Json::arrayValue);
    for (const auto& h : look.held) {
        Json::Value v(Json::objectValue);
        v["path"]            = h.path;
        v["size"]            = static_cast<Json::UInt64>(h.size);
        v["missing_sectors"] = static_cast<Json::UInt64>(h.missing_sectors);
        held.append(v);
    }
    Json::StreamWriterBuilder wb;
    wb["indentation"] = " ";
    const std::string held_text = Json::writeString(wb, held);
    std::string err;
    if (held_text != held_json_ &&
        durableWrite((bdir / "held.json").string(), held_text.data(), held_text.size(), &err))
        held_json_ = held_text;

    const std::string state = rebuilder_->state();
    if (state != saved_state_ &&
        durableWrite((bdir / "rebuild.state").string(), state.data(), state.size(), &err))
        saved_state_ = state;
    return r;
}

}  // namespace hms_cpap::sleepbridge
