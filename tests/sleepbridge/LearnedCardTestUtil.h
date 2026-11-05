#pragma once
// A card learned on a computer, built by hand (SDD-061): the CBL1 payload a
// .cpapcard carries, for a small card no machine wrote. No recorded data.
//
// FAT32: the partition at LBA 8 (LBA 0's table names it), 8 sectors a
// cluster, 32 reserved, two FATs of 100, so FAT #1 at 40 and data at 240, the
// root in cluster 2 (LBA 240) holding a volume label and DATALOG (cluster 3,
// LBA 248). FAT16 when asked: 4 reserved, two FATs of 50, a fixed root of 512
// entries at 112..143, data at 144.

#include "sleepbridge/LearnedCard.h"
#include "sleepbridge/Wire.h"

#include <json/json.h>

#include <cstdint>
#include <cstring>
#include <string>
#include <utility>
#include <vector>

namespace lctest {

using hms_cpap::sleepbridge::kSectorSize;

inline void le16(std::string& s, size_t at, uint32_t v) {
    s[at]     = static_cast<char>(v & 0xFF);
    s[at + 1] = static_cast<char>((v >> 8) & 0xFF);
}
inline void le32(std::string& s, size_t at, uint32_t v) {
    for (int i = 0; i < 4; i++) s[at + i] = static_cast<char>((v >> (8 * i)) & 0xFF);
}

inline void dirEntry(std::string& s, size_t at, const char* name11, uint8_t attr, uint32_t cluster) {
    std::memcpy(&s[at], name11, 11);
    s[at + 11] = static_cast<char>(attr);
    le16(s, at + 20, cluster >> 16);
    le16(s, at + 26, cluster & 0xFFFF);
}

struct Card {
    Json::Value statement{Json::objectValue};
    std::vector<std::pair<uint32_t, std::string>> sectors;   // ascending

    std::string& at(uint32_t lba) {
        for (auto& [l, s] : sectors)
            if (l == lba) return s;
        static std::string none;
        return none;
    }
    void drop(uint32_t lba) {
        for (auto it = sectors.begin(); it != sectors.end(); ++it)
            if (it->first == lba) {
                sectors.erase(it);
                return;
            }
    }
    /// The boot sector changed: its hash in the statement follows.
    void rehash() {
        statement["boot_sha256"] =
            hms_cpap::sleepbridge::sha256HexOf(at(statement["partition_lba"].asUInt()));
    }
    std::string cbl1() const { return hms_cpap::sleepbridge::encodeCbl1(statement, sectors); }
};

inline Card card(bool fat16 = false) {
    Card c;
    const uint32_t part = 8;
    std::string mbr(kSectorSize, '\0');
    mbr[446 + 4] = fat16 ? 0x06 : 0x0C;
    le32(mbr, 446 + 8, part);
    mbr[510] = static_cast<char>(0x55);
    mbr[511] = static_cast<char>(0xAA);

    std::string boot(kSectorSize, '\0');
    boot[0] = static_cast<char>(0xEB);
    le16(boot, 11, 512);
    boot[13] = 8;
    boot[16] = 2;
    boot[21] = static_cast<char>(0xF8);
    uint32_t reserved, fatsz, fat_begin, data_begin;
    std::vector<uint32_t> root_lbas;
    if (fat16) {
        reserved = 4;
        fatsz    = 50;
        le16(boot, 14, reserved);
        le16(boot, 17, 512);
        le16(boot, 22, fatsz);
        le32(boot, 32, 200000);
        le32(boot, 0x27, 0xA1B2C3D4);
        std::memcpy(&boot[54], "FAT16   ", 8);
        fat_begin  = part + reserved;
        const uint32_t root = fat_begin + 2 * fatsz;
        for (uint32_t i = 0; i < 32; i++) root_lbas.push_back(root + i);
        data_begin = root + 32;
    } else {
        reserved = 32;
        fatsz    = 100;
        le16(boot, 14, reserved);
        le32(boot, 32, 2000000);
        le32(boot, 36, fatsz);
        le32(boot, 44, 2);
        le16(boot, 48, 1);
        le16(boot, 50, 6);
        le32(boot, 0x43, 0xA1B2C3D4);
        std::memcpy(&boot[82], "FAT32   ", 8);
        fat_begin  = part + reserved;
        data_begin = fat_begin + 2 * fatsz;
        root_lbas.push_back(data_begin);
    }
    boot[510] = static_cast<char>(0x55);
    boot[511] = static_cast<char>(0xAA);

    std::string fat(kSectorSize, '\0');
    if (fat16) {
        le16(fat, 0, 0xFFF8);
        le16(fat, 2, 0xFFFF);
        le16(fat, 6, 0xFFFF);   // DATALOG, cluster 3
    } else {
        le32(fat, 0, 0x0FFFFFF8);
        le32(fat, 4, 0x0FFFFFFF);
        le32(fat, 8, 0x0FFFFFFF);    // the root
        le32(fat, 12, 0x0FFFFFFF);   // DATALOG
    }

    std::string root(kSectorSize, '\0');
    // A clone card's label can hold anything (platform rule): never judged.
    dirEntry(root, 0, "DISK*IMG?? ", 0x08, 0);
    dirEntry(root, 32, "DATALOG    ", 0x10, 3);
    std::string datalog(kSectorSize, '\0');
    dirEntry(datalog, 0, ".          ", 0x10, 3);
    dirEntry(datalog, 32, "..         ", 0x10, 0);
    dirEntry(datalog, 64, "20261101   ", 0x10, 4);

    c.sectors.emplace_back(0, mbr);
    c.sectors.emplace_back(part, boot);
    c.sectors.emplace_back(fat_begin, fat);
    if (fat16) {
        for (size_t i = 0; i < root_lbas.size(); i++)
            c.sectors.emplace_back(root_lbas[i], i == 0 ? root : std::string(kSectorSize, '\0'));
        c.sectors.emplace_back(data_begin + 8, datalog);   // cluster 3
    } else {
        c.sectors.emplace_back(data_begin, root);
        c.sectors.emplace_back(data_begin + 8, datalog);   // cluster 3
    }

    Json::Value s(Json::objectValue);
    s["machine_serial"]      = "";
    s["brand"]               = "resmed";
    s["fat"]                 = fat16 ? "fat16" : "fat32";
    s["volume_serial"]       = "A1B2C3D4";
    s["partition_lba"]       = part;
    s["sectors_per_cluster"] = 8;
    s["data_begin_lba"]      = data_begin;
    s["root_cluster"]        = fat16 ? 0 : 2;
    Json::Value rl(Json::arrayValue);
    for (auto l : root_lbas) rl.append(l);
    s["root_dir_lbas"]    = rl;
    s["fat_begin_lba"]    = fat_begin;
    s["fat_sectors_sent"] = 1;
    s["dir_clusters"]     = 2;
    s["desktop_version"]  = "1.2.0";
    s["read_at"]          = "2026-11-01T22:10:00Z";
    Json::Value folders(Json::arrayValue);
    folders.append("DATALOG");
    s["folders"] = folders;
    s["nights"]  = 1;
    c.statement  = s;
    c.rehash();
    return c;
}

}  // namespace lctest
