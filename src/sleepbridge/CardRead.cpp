#include "sleepbridge/CardRead.h"

#include "sleepbridge/LearnedCard.h"
#include "sleepbridge/Wire.h"

#include <cctype>
#include <cstdio>
#include <ctime>
#include <deque>
#include <map>
#include <set>

namespace hms_cpap::sleepbridge {

namespace {

uint32_t le16(const std::string& s, size_t at) {
    return static_cast<uint32_t>(static_cast<uint8_t>(s[at])) |
           (static_cast<uint32_t>(static_cast<uint8_t>(s[at + 1])) << 8);
}

uint32_t le32(const std::string& s, size_t at) {
    return le16(s, at) | (le16(s, at + 2) << 16);
}

bool hasMark(const std::string& s) {
    return s.size() >= kSectorSize && static_cast<uint8_t>(s[510]) == 0x55 &&
           static_cast<uint8_t>(s[511]) == 0xAA;
}

bool named(const std::string& s, size_t at, const char* what) {
    return s.compare(at, std::char_traits<char>::length(what), what) == 0;
}

/// A FAT boot sector by its fields: a jump, 512-byte sectors, a power-of-two
/// cluster, reserved sectors and one to four FATs.
bool isFatBoot(const std::string& s) {
    if (!hasMark(s)) return false;
    const auto jump = static_cast<uint8_t>(s[0]);
    if (jump != 0xEB && jump != 0xE9) return false;
    const uint32_t spc  = static_cast<uint8_t>(s[13]);
    const uint32_t fats = static_cast<uint8_t>(s[16]);
    return le16(s, 11) == kSectorSize && spc != 0 && (spc & (spc - 1)) == 0 && le16(s, 14) != 0 &&
           fats >= 1 && fats <= 4;
}

std::string isoNow() {
    const std::time_t now = std::time(nullptr);
    std::tm           tm{};
#ifdef _WIN32
    gmtime_s(&tm, &now);
#else
    gmtime_r(&now, &tm);
#endif
    char buf[32];
    std::strftime(buf, sizeof buf, "%Y-%m-%dT%H:%M:%SZ", &tm);
    return buf;
}

std::string shortName(const std::string& entry) {
    auto trim = [](std::string s) {
        while (!s.empty() && s.back() == ' ') s.pop_back();
        return s;
    };
    std::string base = trim(entry.substr(0, 8));
    std::string ext  = trim(entry.substr(8, 3));
    if (!base.empty() && static_cast<uint8_t>(base[0]) == 0x05) base[0] = static_cast<char>(0xE5);
    return ext.empty() ? base : base + "." + ext;
}

bool allDigits(const std::string& s) {
    if (s.size() != 8) return false;
    for (const char c : s)
        if (!std::isdigit(static_cast<unsigned char>(c))) return false;
    return true;
}

}  // namespace

std::optional<CardBaseline> readCardBaseline(const ReadSectors& read, std::string* why,
                                             const Json::Value& labels, const ReadProgress& progress) {
    auto refuse = [why](const std::string& w) -> std::optional<CardBaseline> {
        if (why) *why = w;
        return std::nullopt;
    };

    std::map<uint32_t, std::string> kept;
    size_t done = 0, total = 0, last_told = 0;
    auto tell = [&](bool force) {
        if (progress && (force || done - last_told >= 256)) {
            last_told = done;
            progress(done, total < done ? done : total);
        }
    };
    auto get = [&](uint32_t lba, uint32_t count, std::string& out) {
        if (!read(lba, count, out) || out.size() != static_cast<size_t>(count) * kSectorSize) return false;
        done += count;
        tell(false);
        return true;
    };
    auto keep = [&kept](uint32_t lba, const std::string& sector) { kept[lba] = sector.substr(0, kSectorSize); };

    // Where the volume starts: LBA 0 is its boot sector (a card formatted
    // without a partition table), or the partition table names it.
    std::string first;
    if (!get(0, 1, first)) return refuse("The card could not be read.");
    if (!hasMark(first)) return refuse("This card does not hold a FAT volume.");
    uint32_t    part = 0;
    std::string boot;
    if (isFatBoot(first)) {
        boot = first;
    } else {
        if (named(first, 3, "EXFAT   ")) return refuse("The card is formatted exFAT; a CPAP card is FAT16 or FAT32.");
        for (int e = 0; e < 4 && part == 0; e++) {
            const size_t at   = 446 + static_cast<size_t>(e) * 16;
            const auto   type = static_cast<uint8_t>(first[at + 4]);
            const uint32_t lba = le32(first, at + 8);
            if (type != 0 && lba != 0) part = lba;
        }
        if (part == 0) return refuse("This card does not hold a FAT volume.");
        if (!get(part, 1, boot)) return refuse("The card could not be read.");
        if (named(boot, 3, "EXFAT   ")) return refuse("The card is formatted exFAT; a CPAP card is FAT16 or FAT32.");
        if (named(boot, 3, "NTFS    ")) return refuse("The card is formatted NTFS; a CPAP card is FAT16 or FAT32.");
        if (!isFatBoot(boot)) return refuse("This card does not hold a FAT volume.");
        keep(0, first);
    }
    keep(part, boot);

    const uint32_t spc      = static_cast<uint8_t>(boot[13]);
    const uint32_t reserved = le16(boot, 14);
    const uint32_t fats     = static_cast<uint8_t>(boot[16]);
    const uint32_t root_ent = le16(boot, 17);
    const uint32_t tot16    = le16(boot, 19);
    const uint32_t fatsz16  = le16(boot, 22);
    const uint32_t fatsz    = fatsz16 ? fatsz16 : le32(boot, 36);
    const uint32_t vol_tot  = tot16 ? tot16 : le32(boot, 32);
    const uint32_t root_sec = (root_ent * 32 + kSectorSize - 1) / kSectorSize;
    const uint64_t data_rel = uint64_t(reserved) + uint64_t(fats) * fatsz + root_sec;
    if (fatsz == 0 || vol_tot == 0 || data_rel >= vol_tot) return refuse("This card does not hold a FAT volume.");
    const uint32_t clusters = static_cast<uint32_t>((vol_tot - data_rel) / spc);
    if (clusters < 4085) return refuse("The card is formatted FAT12; a CPAP card is FAT16 or FAT32.");
    const bool     fat32    = clusters >= 65525;
    if (fat32 && fatsz16 != 0) return refuse("This card does not hold a FAT volume.");
    const uint32_t fat_begin  = part + reserved;
    const uint32_t data_begin = static_cast<uint32_t>(part + data_rel);
    const uint32_t root_clus  = fat32 ? le32(boot, 44) : 0;

    // FAT32's FSInfo and backup boot sector, where the boot sector says.
    if (fat32) {
        for (const uint32_t rel : {le16(boot, 48), le16(boot, 50)}) {
            if (rel == 0 || rel >= reserved || rel == 0xFFFF) continue;
            std::string s;
            if (get(part + rel, 1, s)) keep(part + rel, s);
        }
    }

    // FAT #1, whole, to find the highest cluster in use; only the sectors up
    // to it go into the baseline.
    total = done + fatsz;
    std::string fat;
    fat.reserve(static_cast<size_t>(fatsz) * kSectorSize);
    for (uint32_t at = 0; at < fatsz;) {
        const uint32_t n = std::min<uint32_t>(128, fatsz - at);
        std::string    chunk;
        if (!get(fat_begin + at, n, chunk)) return refuse("The card's file table could not be read.");
        fat += chunk;
        at += n;
    }
    const size_t entry_bytes = fat32 ? 4 : 2;
    auto next = [&](uint32_t c) -> uint32_t {
        const size_t at = static_cast<size_t>(c) * entry_bytes;
        if (at + entry_bytes > fat.size()) return 0x0FFFFFFF;
        return fat32 ? (le32(fat, at) & 0x0FFFFFFF) : le16(fat, at);
    };
    const uint32_t last_cluster = std::min<uint32_t>(clusters + 1, static_cast<uint32_t>(fat.size() / entry_bytes) - 1);
    uint32_t highest = 1;
    for (uint32_t c = last_cluster; c >= 2; c--)
        if (next(c) != 0) {
            highest = c;
            break;
        }
    const uint32_t used_fat = static_cast<uint32_t>(((static_cast<size_t>(highest) + 1) * entry_bytes + kSectorSize - 1) / kSectorSize);
    for (uint32_t i = 0; i < used_fat && i < fatsz; i++)
        keep(fat_begin + i, fat.substr(static_cast<size_t>(i) * kSectorSize, kSectorSize));
    const bool end_of_chain_16 = !fat32;
    auto isEnd = [&](uint32_t v) {
        return end_of_chain_16 ? (v >= 0xFFF8 || v < 2) : (v >= 0x0FFFFFF8 || v < 2);
    };

    // The folders, from the root.
    struct Dir {
        uint32_t    cluster{0};     // 0: FAT16's fixed root
        std::string path;           // "" for the root
    };
    std::deque<Dir>    todo{Dir{root_clus, ""}};
    std::set<uint32_t> seen;
    std::vector<uint32_t> root_lbas;
    Json::Value folders(Json::arrayValue);
    int nights = 0;
    const size_t kMaxSectors = (kLearnedCardMax - 64 * 1024) / (4 + kSectorSize);

    while (!todo.empty()) {
        const Dir dir = todo.front();
        todo.pop_front();
        std::vector<uint32_t> lbas;
        if (dir.cluster == 0) {
            if (fat32) continue;   // a FAT32 entry pointing at cluster 0: nothing
            for (uint32_t i = 0; i < root_sec; i++) lbas.push_back(fat_begin + fats * fatsz + i);
        } else {
            for (uint32_t c = dir.cluster; !isEnd(c) && c <= clusters + 1; c = next(c)) {
                if (!seen.insert(c).second) break;   // a loop in the chain
                for (uint32_t i = 0; i < spc; i++) lbas.push_back(data_begin + (c - 2) * spc + i);
            }
        }
        total += lbas.size();
        bool ended = false;
        for (const uint32_t lba : lbas) {
            std::string s;
            if (!get(lba, 1, s)) return refuse("A folder on the card could not be read.");
            keep(lba, s);
            if (dir.path.empty()) root_lbas.push_back(lba);
            if (kept.size() > kMaxSectors) return refuse("The card has more folders than a card layout holds.");
            for (size_t at = 0; at < kSectorSize && !ended; at += 32) {
                const auto first_byte = static_cast<uint8_t>(s[at]);
                if (first_byte == 0x00) {
                    ended = true;
                    break;
                }
                if (first_byte == 0xE5) continue;
                const auto attr = static_cast<uint8_t>(s[at + 11]);
                if ((attr & 0x3F) == 0x0F || (attr & 0x08) || !(attr & 0x10)) continue;
                const std::string name = shortName(s.substr(at, 11));
                if (name == "." || name == "..") continue;
                const uint32_t c = (fat32 ? (le16(s, at + 20) << 16) : 0) | le16(s, at + 26);
                if (c < 2) continue;
                todo.push_back(Dir{c, dir.path.empty() ? name : dir.path + "/" + name});
                if (dir.path.empty()) folders.append(name);
                if (dir.path == "DATALOG" && allDigits(name)) nights++;
            }
        }
    }
    if (root_lbas.empty()) return refuse("The card's root folder could not be read.");

    Json::Value st = labels.isObject() ? labels : Json::Value(Json::objectValue);
    st["fat"]                 = fat32 ? "fat32" : "fat16";
    st["partition_lba"]       = part;
    st["sectors_per_cluster"] = spc;
    st["data_begin_lba"]      = data_begin;
    st["root_cluster"]        = root_clus;
    st["fat_begin_lba"]       = fat_begin;
    Json::Value roots(Json::arrayValue);
    for (const uint32_t l : root_lbas) roots.append(l);
    st["root_dir_lbas"] = roots;
    st["boot_sha256"]   = sha256HexOf(boot.substr(0, kSectorSize));
    char serial[16];
    std::snprintf(serial, sizeof serial, "%08X", le32(boot, fat32 ? 67 : 39));
    st["volume_serial"] = serial;   // a label only: clone cards share them
    st["read_at"]       = isoNow();
    st["folders"]       = folders;
    st["nights"]        = nights;

    CardBaseline out;
    out.statement = st;
    out.sectors.assign(kept.begin(), kept.end());
    total = done;
    tell(true);
    return out;
}

std::string wholeDiskFor(const std::string& device) {
    // macOS: /dev/disk4s1 or /dev/rdisk4s1 -> /dev/rdisk4.
    for (const char* prefix : {"/dev/rdisk", "/dev/disk"}) {
        const std::string p(prefix);
        if (device.compare(0, p.size(), p) != 0) continue;
        size_t i = p.size();
        const size_t digits = i;
        while (i < device.size() && std::isdigit(static_cast<unsigned char>(device[i]))) i++;
        if (i == digits) return device;
        return "/dev/rdisk" + device.substr(digits, i - digits);
    }
    // Linux: /dev/mmcblk0p1 -> /dev/mmcblk0; /dev/sdb1 -> /dev/sdb.
    if (device.compare(0, 11, "/dev/mmcblk") == 0) {
        const auto p = device.find('p', 11);
        return p == std::string::npos ? device : device.substr(0, p);
    }
    if (device.compare(0, 7, "/dev/sd") == 0) {
        size_t end = device.size();
        while (end > 7 && std::isdigit(static_cast<unsigned char>(device[end - 1]))) end--;
        return device.substr(0, end);
    }
    return device;
}

bool cardFileToKeep(const std::string& rel) {
    if (rel.empty()) return false;
    size_t start = 0;
    while (start <= rel.size()) {
        const size_t end  = rel.find('/', start);
        const std::string part = rel.substr(start, end == std::string::npos ? std::string::npos : end - start);
        if (part.empty() || part[0] == '.') return false;
        if (part == "System Volume Information" || part == "$RECYCLE.BIN") return false;
        if (end == std::string::npos) break;
        start = end + 1;
    }
    return true;
}

}  // namespace hms_cpap::sleepbridge
