#include "services/As11StoredNight.h"

#include <algorithm>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <map>
#include <set>

#ifdef _WIN32
#include <io.h>
#else
#include <unistd.h>
#endif

namespace fs = std::filesystem;
using namespace cpapdash::parser;

namespace hms_cpap {

namespace {

std::vector<uint8_t> readAll(const fs::path& p) {
    std::ifstream in(p, std::ios::binary);
    if (!in) return {};
    return {std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
}

// Written whole beside its place, made durable, then moved in: a page on disk
// is always a whole page.
bool writeDurable(const fs::path& path, const std::vector<uint8_t>& bytes) {
    std::error_code ec;
    fs::create_directories(path.parent_path(), ec);
    const fs::path tmp = path.string() + ".tmp";
    FILE* f = std::fopen(tmp.string().c_str(), "wb");
    if (!f) return false;
    bool ok = std::fwrite(bytes.data(), 1, bytes.size(), f) == bytes.size() && std::fflush(f) == 0;
#ifdef _WIN32
    ok = ok && _commit(_fileno(f)) == 0;
#else
    ok = ok && ::fsync(fileno(f)) == 0;
#endif
    ok = std::fclose(f) == 0 && ok;
    if (ok) fs::rename(tmp, path, ec);
    if (!ok || ec) { fs::remove(tmp, ec); return false; }
    return true;
}

fs::path pagePath(const std::string& root, const std::string& night, const std::string& spool, int n) {
    return fs::path(storedNightDir(root, night)) / (spool + "." + std::to_string(n) + ".page");
}

int64_t msOf(std::chrono::system_clock::time_point t) {
    return std::chrono::duration_cast<std::chrono::milliseconds>(t.time_since_epoch()).count();
}

}  // namespace

const std::vector<std::string>& storedSpools() {
    static const std::vector<std::string> all = {
        kStoredEventsSpool, "RespiratoryFlow6p25Hz", "MaskPressure6p25Hz", "Leak0p5Hz",
        "InspiratoryPressure0p5Hz"};
    return all;
}

std::string storedNightDir(const std::string& root, const std::string& night) {
    return (fs::path(root) / "stored" / night).string();
}

bool keepStoredPage(const std::string& root, const std::string& night, const std::string& spool,
                    int n, const std::vector<uint8_t>& bytes) {
    const fs::path p = pagePath(root, night, spool, n);
    std::error_code ec;
    if (fs::exists(p, ec) && readAll(p) == bytes) return true;
    return writeDurable(p, bytes);
}

std::vector<std::vector<uint8_t>> keptStoredPages(const std::string& root, const std::string& night,
                                                  const std::string& spool) {
    std::vector<std::vector<uint8_t>> pages;
    for (int n = 0;; n++) {
        const fs::path p = pagePath(root, night, spool, n);
        std::error_code ec;
        if (!fs::exists(p, ec)) break;
        pages.push_back(readAll(p));
    }
    return pages;
}

bool storedNightDone(const std::string& root, const std::string& night) {
    std::error_code ec;
    return fs::exists(fs::path(storedNightDir(root, night)) / "done", ec);
}

bool markStoredNightDone(const std::string& root, const std::string& night) {
    return writeDurable(fs::path(storedNightDir(root, night)) / "done", {});
}

std::vector<StoredSpan> maskOnSpans(const AirSense11Day& day) {
    std::vector<StoredSpan> out;
    for (const auto& s : day.sessions) {
        if (s.minutes <= 0) continue;
        const int64_t start = msOf(s.start);
        out.push_back({start, start + static_cast<int64_t>(s.minutes) * 60 * 1000});
    }
    std::sort(out.begin(), out.end(),
              [](const StoredSpan& a, const StoredSpan& b) { return a.start_ms < b.start_ms; });
    return out;
}

std::vector<StoredSpan> gapsToFill(std::vector<StoredSpan> mask_on, std::vector<StoredSpan> covered,
                                   int64_t min_ms) {
    auto byStart = [](const StoredSpan& a, const StoredSpan& b) { return a.start_ms < b.start_ms; };
    std::sort(mask_on.begin(), mask_on.end(), byStart);
    std::sort(covered.begin(), covered.end(), byStart);
    std::vector<StoredSpan> gaps;
    for (const auto& on : mask_on) {
        int64_t at = on.start_ms;
        for (const auto& c : covered) {
            if (c.end_ms <= at || c.start_ms >= on.end_ms) continue;
            if (c.start_ms > at) gaps.push_back({at, c.start_ms});
            at = std::max(at, c.end_ms);
            if (at >= on.end_ms) break;
        }
        if (at < on.end_ms) gaps.push_back({at, on.end_ms});
    }
    gaps.erase(std::remove_if(gaps.begin(), gaps.end(),
                              [min_ms](const StoredSpan& g) { return g.length() < min_ms; }),
               gaps.end());
    return gaps;
}

std::vector<StoredRun> storedRuns(const std::vector<AirSense11PageChunk>& a,
                                  const std::vector<AirSense11PageChunk>& b,
                                  const std::vector<StoredSpan>& gaps) {
    // Both signals by instant, inside the gaps only.
    auto inGap = [&gaps](int64_t t) {
        for (const auto& g : gaps)
            if (g.start_ms <= t && t < g.end_ms) return &g;
        return static_cast<const StoredSpan*>(nullptr);
    };
    std::map<int64_t, double> va, vb;
    int64_t interval = 0;
    for (const auto& c : a)
        for (size_t i = 0; i < c.raw.size(); i++)
            if (inGap(c.timeOf(i))) { va[c.timeOf(i)] = c.value(i); interval = c.interval_ms; }
    for (const auto& c : b)
        for (size_t i = 0; i < c.raw.size(); i++)
            if (inGap(c.timeOf(i))) vb[c.timeOf(i)] = c.value(i);

    std::vector<StoredRun> runs;
    if (interval <= 0) return runs;
    const StoredSpan* gap = nullptr;
    for (const auto& [t, x] : va) {
        const auto y = vb.find(t);
        if (y == vb.end()) continue;
        const StoredSpan* g = inGap(t);
        const bool extends = !runs.empty() && g == gap &&
                             t == runs.back().start_ms + static_cast<int64_t>(runs.back().a.size()) * interval;
        if (!extends) {
            runs.push_back({t, interval, {}, {}});
            gap = g;
        }
        runs.back().a.push_back(x);
        runs.back().b.push_back(y->second);
    }
    return runs;
}

std::vector<AirSense11PageChunk> storedChunks(const std::vector<std::vector<uint8_t>>& pages,
                                              int* skipped) {
    std::vector<AirSense11PageChunk> out;
    std::set<int64_t> seen;
    int bad = 0;
    for (const auto& p : pages) {
        auto page = AirSense11PagesParser::parseSignalPage(p.data(), p.size());
        bad += page.skipped;
        for (auto& c : page.chunks)
            if (seen.insert(c.start_ms).second) out.push_back(std::move(c));
    }
    std::sort(out.begin(), out.end(), [](const AirSense11PageChunk& x, const AirSense11PageChunk& y) {
        return x.start_ms < y.start_ms;
    });
    if (skipped) *skipped = bad;
    return out;
}

std::vector<AirSense11PageEvent> storedEvents(const std::vector<std::vector<uint8_t>>& pages) {
    std::vector<AirSense11PageEvent> out;
    std::set<std::pair<int, int64_t>> seen;
    for (const auto& p : pages)
        for (const auto& e : AirSense11PagesParser::parseEventsPage(p.data(), p.size()))
            if (seen.insert({e.type, e.onset_ms}).second) out.push_back(e);
    std::sort(out.begin(), out.end(), [](const AirSense11PageEvent& x, const AirSense11PageEvent& y) {
        return x.onset_ms < y.onset_ms;
    });
    return out;
}

std::string storedEventName(int type) {
    switch (type) {
    case 3: return "CentralApneaEnd";
    case 4: return "ObstructiveApneaEnd";
    default: return "";
    }
}

}  // namespace hms_cpap
