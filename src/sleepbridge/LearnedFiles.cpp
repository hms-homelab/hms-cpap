#include "sleepbridge/LearnedFiles.h"

#include "sleepbridge/RebuildWorker.h"   // safeCardPath
#include "sleepbridge/Wire.h"            // kSectorSize

#include <algorithm>
#include <cctype>
#include <map>

namespace hms_cpap::sleepbridge {

namespace {

constexpr char kMagic[4] = {'C', 'B', 'F', '1'};

void put16(std::string& s, uint32_t v) {
    s.push_back(static_cast<char>(v & 0xFF));
    s.push_back(static_cast<char>((v >> 8) & 0xFF));
}
void put32(std::string& s, uint32_t v) {
    for (int i = 0; i < 4; i++) s.push_back(static_cast<char>((v >> (8 * i)) & 0xFF));
}
uint32_t get16(const unsigned char* p) { return p[0] | (p[1] << 8); }
uint32_t get32(const unsigned char* p) {
    return p[0] | (p[1] << 8) | (p[2] << 16) | (static_cast<uint32_t>(p[3]) << 24);
}

std::string lower(std::string s) {
    for (auto& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

std::string trimRight(std::string s) {
    while (!s.empty() && s.back() == ' ') s.pop_back();
    return s;
}

/// One directory entry: its names (long and 8.3), first cluster, size.
struct Entry {
    std::string long_name, short_name;
    uint32_t    cluster{0};
    uint32_t    size{0};
    bool        dir{false};
};

/// The learned card's sectors, by LBA, and its FAT.
class Card {
public:
    explicit Card(const LearnedCard& c) : c_(c) {
        for (const auto& [lba, data] : c.sectors) by_lba_[lba] = &data;
    }
    const std::string* sector(uint32_t lba) const {
        const auto it = by_lba_.find(lba);
        return it == by_lba_.end() ? nullptr : it->second;
    }
    uint32_t firstLba(uint32_t cluster) const {
        return c_.data_begin_lba + (cluster - 2) * c_.sectors_per_cluster;
    }
    /// The FAT entry of `cluster`, or nullopt when its sector is not learned.
    std::optional<uint32_t> next(uint32_t cluster) const {
        const uint64_t at  = uint64_t{cluster} * 4;
        const auto*    sec = sector(c_.fat_begin_lba + static_cast<uint32_t>(at / kSectorSize));
        if (!sec || sec->size() < kSectorSize) return std::nullopt;
        return get32(reinterpret_cast<const unsigned char*>(sec->data()) + at % kSectorSize) & 0x0FFFFFFF;
    }
    /// The whole chain from `first`, or nullopt when any FAT entry on it is
    /// not learned or the chain is broken. At most `cap` clusters.
    std::optional<std::vector<uint32_t>> chain(uint32_t first, size_t cap) const {
        std::vector<uint32_t> out;
        uint32_t c = first;
        while (true) {
            if (c < 2 || (c_.cluster_count && c >= c_.cluster_count + 2)) return std::nullopt;
            out.push_back(c);
            if (out.size() >= cap) return out;
            const auto n = next(c);
            if (!n) return std::nullopt;
            if (*n >= 0x0FFFFFF8) return out;
            if (std::find(out.begin(), out.end(), *n) != out.end()) return std::nullopt;   // a loop
            c = *n;
        }
    }
    /// The entries of the folder whose chain starts at `cluster`, while its
    /// sectors are learned.
    std::optional<std::vector<Entry>> folder(uint32_t cluster) const {
        const auto ch = chain(cluster, 4096);
        if (!ch) return std::nullopt;
        std::vector<Entry> out;
        std::vector<std::string> lfn;
        for (const uint32_t cl : *ch)
            for (uint32_t i = 0; i < c_.sectors_per_cluster; i++) {
                const auto* sec = sector(firstLba(cl) + i);
                if (!sec || sec->size() < kSectorSize) return std::nullopt;
                const auto* p = reinterpret_cast<const unsigned char*>(sec->data());
                for (size_t off = 0; off < kSectorSize; off += 32) {
                    const unsigned char* e = p + off;
                    if (e[0] == 0x00) return out;   // the folder's end
                    if (e[0] == 0xE5) {
                        lfn.clear();
                        continue;
                    }
                    const uint8_t attr = e[11];
                    if (attr == 0x0F) {
                        const unsigned n = e[0] & 0x1F;
                        if (n == 0) continue;
                        if (e[0] & 0x40) lfn.assign(n, std::string());
                        if (n > lfn.size()) {
                            lfn.clear();
                            continue;
                        }
                        std::string part;
                        for (const int at : {1, 3, 5, 7, 9, 14, 16, 18, 20, 22, 24, 28, 30}) {
                            const uint32_t ch16 = get16(e + at);
                            if (ch16 == 0x0000 || ch16 == 0xFFFF) break;
                            part.push_back(ch16 < 0x80 ? static_cast<char>(ch16) : '?');
                        }
                        lfn[n - 1] = part;
                        continue;
                    }
                    if (attr & 0x08) {   // a volume label
                        lfn.clear();
                        continue;
                    }
                    Entry x;
                    std::string name(reinterpret_cast<const char*>(e), 8), ext(reinterpret_cast<const char*>(e + 8), 3);
                    if (static_cast<unsigned char>(name[0]) == 0x05) name[0] = static_cast<char>(0xE5);
                    name = trimRight(name);
                    ext  = trimRight(ext);
                    x.short_name = ext.empty() ? name : name + "." + ext;
                    for (const auto& s : lfn) x.long_name += s;
                    lfn.clear();
                    x.cluster = (get16(e + 20) << 16) | get16(e + 26);
                    x.size    = get32(e + 28);
                    x.dir     = (attr & 0x10) != 0;
                    out.push_back(x);
                }
            }
        return out;
    }

    const LearnedCard& c_;

private:
    std::map<uint32_t, const std::string*> by_lba_;
};

std::vector<std::string> segments(const std::string& path) {
    std::vector<std::string> out;
    size_t at = 0;
    while (at <= path.size()) {
        const size_t slash = path.find('/', at);
        out.push_back(path.substr(at, slash == std::string::npos ? std::string::npos : slash - at));
        if (slash == std::string::npos) break;
        at = slash + 1;
    }
    return out;
}

}  // namespace

std::string encodeLearnedFiles(const std::vector<LearnedFile>& files) {
    std::string out(kMagic, kMagic + 4);
    put32(out, static_cast<uint32_t>(files.size()));
    for (const auto& f : files) {
        put16(out, static_cast<uint32_t>(f.path.size()));
        out += f.path;
        put32(out, static_cast<uint32_t>(f.bytes.size()));
        out += f.bytes;
    }
    return out;
}

std::optional<std::vector<LearnedFile>> decodeLearnedFiles(const std::string& blob, std::string* why) {
    auto fail = [why](const std::string& w) -> std::optional<std::vector<LearnedFile>> {
        if (why) *why = w;
        return std::nullopt;
    };
    if (blob.size() > kLearnedFilesMax + 1024 * 1024) return fail("the learned files are larger than allowed");
    if (blob.size() < 8 || blob.compare(0, 4, std::string(kMagic, 4)) != 0)
        return fail("not a learned files blob (no CBF1)");
    const auto* p = reinterpret_cast<const unsigned char*>(blob.data());
    const uint32_t count = get32(p + 4);
    size_t at = 8;
    std::vector<LearnedFile> out;
    for (uint32_t i = 0; i < count; i++) {
        if (blob.size() - at < 2) return fail("the learned files are cut short");
        const uint32_t plen = get16(p + at);
        at += 2;
        if (blob.size() - at < plen + 4u) return fail("the learned files are cut short");
        LearnedFile f;
        f.path = blob.substr(at, plen);
        at += plen;
        const uint32_t size = get32(p + at);
        at += 4;
        if (blob.size() - at < size) return fail("the learned files are cut short");
        f.bytes = blob.substr(at, size);
        at += size;
        if (!safeCardPath(f.path)) return fail("a learned file's path leaves the card: " + f.path);
        out.push_back(std::move(f));
    }
    if (at != blob.size()) return fail("bytes after the learned files");
    return out;
}

LearnedSeed seedLearnedFiles(const LearnedCard& learned, const std::vector<LearnedFile>& files) {
    LearnedSeed seed;
    if (!learned.fat32() || learned.sectors_per_cluster == 0 || learned.root_cluster < 2) return seed;
    const Card card(learned);
    const uint32_t cluster_bytes = learned.sectors_per_cluster * kSectorSize;
    for (const auto& f : files) {
        auto skip = [&](const std::string& w) { seed.skipped.push_back(f.path + ": " + w); };
        if (!safeCardPath(f.path)) {
            skip("leaves the card");
            continue;
        }
        // Down the folders, from the root.
        const auto parts = segments(f.path);
        uint32_t   dir   = learned.root_cluster;
        std::optional<Entry> hit;
        bool lost = false;
        for (size_t i = 0; i < parts.size() && !lost; i++) {
            const auto entries = card.folder(dir);
            if (!entries) {
                skip("its folder is not in the learn");
                lost = true;
                break;
            }
            const std::string want = lower(parts[i]);
            const auto it = std::find_if(entries->begin(), entries->end(), [&](const Entry& e) {
                return lower(e.long_name) == want || lower(e.short_name) == want;
            });
            const bool last = i + 1 == parts.size();
            if (it == entries->end() || it->dir == last) {
                skip("not in the learned folders");
                lost = true;
                break;
            }
            if (last) hit = *it;
            else dir = it->cluster;
        }
        if (lost || !hit) continue;
        const uint64_t bytes = std::min<uint64_t>(f.bytes.size(), hit->size);
        if (bytes == 0 || hit->cluster < 2) {
            skip("empty");
            continue;
        }
        const size_t clusters = static_cast<size_t>((bytes + cluster_bytes - 1) / cluster_bytes);
        const auto ch = card.chain(hit->cluster, clusters);
        if (!ch || ch->size() < clusters) {
            skip("its cluster chain is not in the learn");
            continue;
        }
        uint64_t done = 0;
        for (const uint32_t cl : *ch)
            for (uint32_t s = 0; s < learned.sectors_per_cluster && done < bytes; s++) {
                std::string sec = f.bytes.substr(static_cast<size_t>(done),
                                                 static_cast<size_t>(std::min<uint64_t>(kSectorSize, bytes - done)));
                sec.resize(kSectorSize, '\0');
                seed.sectors.emplace_back(card.firstLba(cl) + s, std::move(sec));
                done += kSectorSize;
            }
        ++seed.files;
    }
    return seed;
}

}  // namespace hms_cpap::sleepbridge
