#include "sleepbridge/LearnedCard.h"

#include "sleepbridge/BatchStore.h"
#include "sleepbridge/Wire.h"

#include <openssl/evp.h>

#include <algorithm>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <sstream>

namespace hms_cpap::sleepbridge {

namespace fs = std::filesystem;

namespace {

uint32_t le16(const std::string& s, size_t at) {
    return static_cast<uint32_t>(static_cast<uint8_t>(s[at])) |
           (static_cast<uint32_t>(static_cast<uint8_t>(s[at + 1])) << 8);
}

uint32_t le32(const std::string& s, size_t at) {
    return static_cast<uint32_t>(static_cast<uint8_t>(s[at])) |
           (static_cast<uint32_t>(static_cast<uint8_t>(s[at + 1])) << 8) |
           (static_cast<uint32_t>(static_cast<uint8_t>(s[at + 2])) << 16) |
           (static_cast<uint32_t>(static_cast<uint8_t>(s[at + 3])) << 24);
}

void putLe32(std::string& s, uint32_t v) {
    for (int i = 0; i < 4; i++) s.push_back(static_cast<char>((v >> (8 * i)) & 0xFF));
}

std::string compact(const Json::Value& v) {
    Json::StreamWriterBuilder w;
    w["indentation"] = "";
    return Json::writeString(w, v);
}

std::optional<Json::Value> parseJson(const std::string& text) {
    Json::Value v;
    Json::CharReaderBuilder rb;
    std::string errs;
    std::istringstream in(text);
    if (!Json::parseFromStream(rb, in, &v, &errs) || !v.isObject()) return std::nullopt;
    return v;
}

bool readAll(const fs::path& p, std::string& out) {
    std::ifstream f(p, std::ios::binary);
    if (!f) return false;
    out.assign(std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>());
    return true;
}

// A sector of 32-byte entries that are each free, deleted, a long name or a
// legal short name, with at least one short entry or the end mark first.
bool readsAsDirectory(const std::string& s) {
    bool any = false;
    for (size_t at = 0; at < kSectorSize; at += 32) {
        const auto first = static_cast<uint8_t>(s[at]);
        if (first == 0x00) return any || at == 0;   // the end of the directory
        if (first == 0xE5) continue;                // deleted
        const auto attr = static_cast<uint8_t>(s[at + 11]);
        if ((attr & 0x3F) == 0x0F) continue;        // a long-name piece
        if (attr & 0xC0) return false;
        // A volume label is a clone card's to fill with anything: never judged.
        if (attr & 0x08) {
            any = true;
            continue;
        }
        for (size_t i = 0; i < 11; i++) {
            const auto c = static_cast<uint8_t>(s[at + i]);
            if (c < 0x20 && !(i == 0 && c == 0x05)) return false;
            if (c == '"' || c == '*' || c == '/' || c == ':' || c == '<' || c == '>' || c == '?' ||
                c == '\\' || c == '|' || c == '+' || c == ',' || c == ';' || c == '=' || c == '[' ||
                c == ']')
                return false;
        }
        any = true;
    }
    return any;
}

}  // namespace

std::string sha256HexOf(const std::string& bytes) {
    unsigned char md[EVP_MAX_MD_SIZE];
    unsigned int  n   = 0;
    EVP_MD_CTX*   ctx = EVP_MD_CTX_new();
    EVP_DigestInit_ex(ctx, EVP_sha256(), nullptr);
    EVP_DigestUpdate(ctx, bytes.data(), bytes.size());
    EVP_DigestFinal_ex(ctx, md, &n);
    EVP_MD_CTX_free(ctx);
    static const char* hex = "0123456789abcdef";
    std::string out;
    for (unsigned int i = 0; i < n; i++) {
        out.push_back(hex[md[i] >> 4]);
        out.push_back(hex[md[i] & 0xF]);
    }
    return out;
}

std::string encodeCbl1(const Json::Value& statement,
                       const std::vector<std::pair<uint32_t, std::string>>& sectors) {
    const std::string json = compact(statement);
    std::string out("CBL1");
    putLe32(out, static_cast<uint32_t>(json.size()));
    out += json;
    putLe32(out, static_cast<uint32_t>(sectors.size()));
    for (const auto& [lba, data] : sectors) {
        putLe32(out, lba);
        std::string s = data;
        s.resize(kSectorSize, '\0');
        out += s;
    }
    return out;
}

std::optional<LearnedCard> decodeLearnedCard(const std::string& bytes, std::string* why) {
    auto refuse = [why](const std::string& w) -> std::optional<LearnedCard> {
        if (why) *why = w;
        return std::nullopt;
    };

    // The frame.
    if (bytes.size() > kLearnedCardMax) return refuse("The card's layout is larger than 32 MB.");
    if (bytes.size() < 12 || bytes.compare(0, 4, "CBL1") != 0)
        return refuse("This is not a card layout this build reads.");
    const uint64_t json_len = le32(bytes, 4);
    if (8 + json_len + 4 > bytes.size()) return refuse("The card's layout is cut short.");
    const auto statement = parseJson(bytes.substr(8, static_cast<size_t>(json_len)));
    if (!statement) return refuse("The card's layout has no readable statement.");
    const size_t   at    = 8 + static_cast<size_t>(json_len);
    const uint64_t count = le32(bytes, at);
    if (at + 4 + count * (4 + kSectorSize) != bytes.size())
        return refuse("The card's layout is cut short.");

    LearnedCard c;
    c.statement = *statement;
    c.sectors.reserve(static_cast<size_t>(count));
    int64_t last = -1;
    for (uint64_t i = 0; i < count; i++) {
        const size_t   off = at + 4 + static_cast<size_t>(i) * (4 + kSectorSize);
        const uint32_t lba = le32(bytes, off);
        if (static_cast<int64_t>(lba) <= last)
            return refuse("The card's layout lists its sectors out of order.");
        last = lba;
        c.sectors.emplace_back(lba, bytes.substr(off + 4, kSectorSize));
    }

    // The statement.
    const Json::Value& s = c.statement;
    auto u32 = [&s](const char* key, uint32_t& out) {
        if (!s.isMember(key) || !s[key].isNumeric() || s[key].asDouble() < 0 ||
            s[key].asDouble() > 4294967295.0)
            return false;
        out = static_cast<uint32_t>(s[key].asDouble());
        return true;
    };
    auto str = [&s](const char* key, std::string& out) {
        if (!s.isMember(key) || !s[key].isString()) return false;
        out = s[key].asString();
        return true;
    };
    if (!str("fat", c.fat) || !str("boot_sha256", c.boot_sha256) ||
        !u32("partition_lba", c.partition_lba) || !u32("sectors_per_cluster", c.sectors_per_cluster) ||
        !u32("data_begin_lba", c.data_begin_lba) || !u32("root_cluster", c.root_cluster) ||
        !u32("fat_begin_lba", c.fat_begin_lba) || !s.isMember("root_dir_lbas") ||
        !s["root_dir_lbas"].isArray() || s["root_dir_lbas"].empty())
        return refuse("The card's layout leaves out part of the card's layout.");
    for (const auto& v : s["root_dir_lbas"]) {
        if (!v.isNumeric() || v.asDouble() < 0 || v.asDouble() > 4294967295.0)
            return refuse("The card's layout leaves out part of the card's layout.");
        c.root_dir_lbas.push_back(static_cast<uint32_t>(v.asDouble()));
    }
    str("volume_serial", c.volume_serial);
    str("machine_serial", c.machine_serial);
    str("brand", c.brand);
    str("read_at", c.read_at);
    str("desktop_version", c.desktop_version);
    if (s.isMember("nights") && s["nights"].isNumeric()) c.nights = s["nights"].asInt();
    if (s.isMember("folders") && s["folders"].isArray())
        for (const auto& f : s["folders"])
            if (f.isString()) c.folders.push_back(f.asString());
    if (c.fat != "fat32" && c.fat != "fat16")
        return refuse("The card is formatted " + c.fat + "; a CPAP card is FAT16 or FAT32.");

    auto find = [&c](uint32_t lba) -> const std::string* {
        auto it = std::lower_bound(c.sectors.begin(), c.sectors.end(), lba,
                                   [](const auto& p, uint32_t l) { return p.first < l; });
        return it != c.sectors.end() && it->first == lba ? &it->second : nullptr;
    };

    // The boot sector, by content.
    const std::string* boot = find(c.partition_lba);
    if (!boot) return refuse("The card's layout has no boot sector where its partition starts.");
    const std::string& b = *boot;
    if (static_cast<uint8_t>(b[510]) != 0x55 || static_cast<uint8_t>(b[511]) != 0xAA || le16(b, 11) != 512)
        return refuse("The card's boot sector does not describe a FAT volume.");
    const uint32_t spc      = static_cast<uint8_t>(b[13]);
    const uint32_t reserved = le16(b, 14);
    const uint32_t fats     = static_cast<uint8_t>(b[16]);
    const uint32_t root_ent = le16(b, 17);
    const uint32_t tot16    = le16(b, 19);
    const uint32_t fatsz16  = le16(b, 22);
    const uint32_t fatsz    = fatsz16 ? fatsz16 : le32(b, 36);
    const uint32_t total    = tot16 ? tot16 : le32(b, 32);
    if (spc == 0 || (spc & (spc - 1)) != 0 || reserved == 0 || fats == 0 || fats > 4 || fatsz == 0 ||
        total == 0)
        return refuse("The card's boot sector does not describe a FAT volume.");
    const uint32_t root_sectors = (root_ent * 32 + kSectorSize - 1) / kSectorSize;
    const uint64_t data_rel     = uint64_t(reserved) + uint64_t(fats) * fatsz + root_sectors;
    if (data_rel >= total) return refuse("The card's boot sector does not describe a FAT volume.");
    if (sha256HexOf(b) != c.boot_sha256)
        return refuse("The card's boot sector is not the one its layout names.");
    if (spc != c.sectors_per_cluster || c.partition_lba + reserved != c.fat_begin_lba ||
        c.partition_lba + data_rel != c.data_begin_lba ||
        (c.fat32() && le32(b, 44) != c.root_cluster))
        return refuse("The card's boot sector does not agree with its layout.");
    c.fats          = fats;
    c.fat_sectors   = fatsz;
    c.cluster_count = static_cast<uint32_t>((total - data_rel) / spc);

    // The root directory.
    for (const uint32_t lba : c.root_dir_lbas) {
        const std::string* d = find(lba);
        if (!d) return refuse("The card's layout leaves out its root folder.");
        if (!readsAsDirectory(*d)) return refuse("The card's root folder does not read as a folder.");
    }

    // FAT #1: the media mark, then end of chain. The second entry's top two
    // bits are the volume's clean and error flags, which a machine clears.
    const std::string* fat = find(c.fat_begin_lba);
    if (!fat) return refuse("The card's layout leaves out its file table.");
    const auto media = static_cast<uint8_t>((*fat)[0]);
    const bool marks =
        c.fat32() ? (le32(*fat, 0) & 0x0FFFFF00) == 0x0FFFFF00 && (le32(*fat, 4) & 0x03FFFFFF) == 0x03FFFFFF
                  : (le16(*fat, 0) & 0xFF00) == 0xFF00 && (le16(*fat, 2) & 0x3FFF) == 0x3FFF;
    if (media < 0xF0 || !marks) return refuse("The card's file table does not start as a FAT does.");
    return c;
}

bool sameLayout(const LearnedCard& card, const RebuildGeometry& g) {
    if (!g.valid()) return false;
    if (card.sectors_per_cluster != g.sectors_per_cluster || card.data_begin_lba != g.data_begin_lba)
        return false;
    return card.fat_begin_lba == 0 || g.fat_begin_lba == 0 || card.fat_begin_lba == g.fat_begin_lba;
}

bool sameLayout(const Json::Value& summary, const RebuildGeometry& g) {
    if (!g.valid() || !summary.isObject()) return false;
    const auto spc   = summary.get("sectors_per_cluster", 0).asUInt();
    const auto data  = summary.get("data_begin_lba", 0).asUInt();
    const auto fat   = summary.get("fat_begin_lba", 0).asUInt();
    if (spc != g.sectors_per_cluster || data != g.data_begin_lba) return false;
    return fat == 0 || g.fat_begin_lba == 0 || fat == g.fat_begin_lba;
}

bool knowsOwnLayout(const RebuildGeometry& g) {
    return g.valid() && g.origin == "read" && g.session > 0;
}

Json::Value learnedSummary(const LearnedCard& c) {
    Json::Value v(Json::objectValue);
    v["fat"]                 = c.fat;
    v["volume_serial"]       = c.volume_serial;
    v["machine_serial"]      = c.machine_serial;
    v["brand"]               = c.brand;
    v["boot_sha256"]         = c.boot_sha256;
    v["read_at"]             = c.read_at;
    v["desktop_version"]     = c.desktop_version;
    v["partition_lba"]       = c.partition_lba;
    v["sectors_per_cluster"] = c.sectors_per_cluster;
    v["data_begin_lba"]      = c.data_begin_lba;
    v["fat_begin_lba"]       = c.fat_begin_lba;
    v["root_cluster"]        = c.root_cluster;
    v["sectors"]             = static_cast<Json::UInt>(c.sectors.size());
    v["nights"]              = c.nights;
    Json::Value folders(Json::arrayValue);
    for (const auto& f : c.folders) folders.append(f);
    v["folders"] = folders;
    return v;
}

bool keepLearnedCard(const std::string& root, const LearnedCard& card, const std::string& bytes,
                     const std::string& file_name, std::string* why) {
    const fs::path dir = fs::path(root) / kLearnedDir;
    std::error_code ec;
    fs::create_directories(dir, ec);
    Json::Value meta      = learnedSummary(card);
    meta["learned_at"]    = static_cast<Json::Int64>(std::time(nullptr));
    meta["file_name"]     = file_name;
    const std::string text = compact(meta) + "\n";
    return durableWrite((dir / kLearnedCardFile).string(), bytes.data(), bytes.size(), why) &&
           durableWrite((dir / kLearnedCardMeta).string(), text.data(), text.size(), why);
}

Json::Value readLearnedMeta(const std::string& root) {
    std::string text;
    if (!readAll(fs::path(root) / kLearnedDir / kLearnedCardMeta, text)) return Json::Value::null;
    const auto v = parseJson(text);
    return v ? *v : Json::Value::null;
}

Json::Value readBridgeLearned(const std::string& batches_dir) {
    std::string text;
    if (!fs::exists(fs::path(batches_dir) / kBridgeLearned)) return Json::Value::null;
    if (!readAll(fs::path(batches_dir) / kBridgeLearnedMeta, text)) return Json::Value::null;
    const auto v = parseJson(text);
    return v ? *v : Json::Value::null;
}

bool giveBridgeLearned(const std::string& batches_dir, const std::string& bytes,
                       const Json::Value& summary, std::string* why) {
    std::error_code ec;
    fs::create_directories(batches_dir, ec);
    const std::string text = compact(summary) + "\n";
    // The sectors first: a learned.json without them names nothing.
    return durableWrite((fs::path(batches_dir) / kBridgeLearned).string(), bytes.data(), bytes.size(),
                        why) &&
           durableWrite((fs::path(batches_dir) / kBridgeLearnedMeta).string(), text.data(), text.size(),
                        why);
}

LearnedVerdict learnedVerdict(const LearnedUse& use, bool stepped, const RebuildGeometry& g, bool holding,
                              const std::string& holding_reason) {
    if (use.state == "another_layout") return {"mismatch", "the card's own boot sector says another layout"};
    if (use.state == "fat16")
        return {"not_used", "This card is FAT16; the Sleep Bridge rebuild reads FAT32 cards."};
    if (use.state == "session_zero")
        return {"not_used", "the bridge numbers its sessions from 0, where the learned card would go"};
    if (use.state == "unreadable") return {"not_used", use.why};
    if (use.state != "fed" || !stepped) return {"waiting", "the bridge has not been rebuilt since the learn"};
    if (holding && holding_reason == "geometry_failed")
        return {"mismatch", "the machine's writes contradict the learned layout"};
    if (g.valid() && g.session > 0) {
        if (!sameLayout(use.summary, g)) return {"mismatch", "the bridge sees another card's layout"};
        if (g.confirmed_by == "read") return {"latched", "boot sector matches"};
        if (g.confirmed_by == "test")
            return {"latched", "the machine's writes landed where the card's layout says"};
        if (g.confirmed_by == "cid") return {"latched", "the machine mounted the same card"};
    }
    return {"waiting", "the machine has not written to the card since it was learned"};
}

}  // namespace hms_cpap::sleepbridge
