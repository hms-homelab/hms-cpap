#include "parsers/Fat32Parser.h"
#include <cstring>
#include <algorithm>
#include <iostream>
#include <sstream>
#include <ctime>

namespace hms_cpap {

std::chrono::system_clock::time_point Fat32DirEntry::modTime() const {
    std::tm tm = {};
    tm.tm_year = ((modify_date >> 9) & 0x7F) + 80;
    tm.tm_mon  = ((modify_date >> 5) & 0x0F) - 1;
    tm.tm_mday = modify_date & 0x1F;
    tm.tm_hour = (modify_time >> 11) & 0x1F;
    tm.tm_min  = (modify_time >> 5) & 0x3F;
    tm.tm_sec  = (modify_time & 0x1F) * 2;
    tm.tm_isdst = -1;
    return std::chrono::system_clock::from_time_t(std::mktime(&tm));
}

Fat32Parser::Fat32Parser(SectorReader reader)
    : reader_(std::move(reader)) {}

bool Fat32Parser::readSectors(uint32_t lba, uint32_t count, std::vector<uint8_t>& out) {
    return reader_(lba, count, out);
}

bool Fat32Parser::init() {
    std::vector<uint8_t> sector0(512);
    if (!readSectors(0, 1, sector0)) return false;

    // Check for MBR partition table (0x55AA signature at 510-511)
    if (sector0[510] != 0x55 || sector0[511] != 0xAA) return false;

    // Check if sector 0 is a VBR (direct FAT32 without MBR)
    if (sector0[82] == 'F' && sector0[83] == 'A' && sector0[84] == 'T') {
        partition_lba_ = 0;
    } else {
        uint8_t part_type = sector0[446 + 4];
        if (part_type != 0x0B && part_type != 0x0C) {
            std::cerr << "FAT32: Partition type 0x" << std::hex << (int)part_type
                      << " is not FAT32" << std::endl;
            return false;
        }
        std::memcpy(&partition_lba_, &sector0[446 + 8], 4);

        if (!readSectors(partition_lba_, 1, sector0)) return false;
        if (sector0[510] != 0x55 || sector0[511] != 0xAA) return false;
    }

    // Parse BPB (memcpy for alignment safety)
    std::memcpy(&bpb_.bytes_per_sector, &sector0[11], 2);
    bpb_.sectors_per_cluster = sector0[13];
    std::memcpy(&bpb_.reserved_sectors, &sector0[14], 2);
    bpb_.num_fats = sector0[16];
    uint16_t total16; std::memcpy(&total16, &sector0[19], 2);
    uint32_t total32; std::memcpy(&total32, &sector0[32], 4);
    bpb_.total_sectors = (total16 != 0) ? total16 : total32;
    std::memcpy(&bpb_.fat_size_sectors, &sector0[36], 4);
    std::memcpy(&bpb_.root_cluster, &sector0[44], 4);
    std::memcpy(&bpb_.fs_info_sector, &sector0[48], 2);

    if (bpb_.bytes_per_sector != 512) {
        std::cerr << "FAT32: Unsupported sector size " << bpb_.bytes_per_sector << std::endl;
        return false;
    }
    if (bpb_.sectors_per_cluster == 0 || bpb_.num_fats == 0) return false;

    fat_start_lba_  = partition_lba_ + bpb_.reserved_sectors;
    data_start_lba_ = fat_start_lba_ + (bpb_.num_fats * bpb_.fat_size_sectors);

    initialized_ = true;
    return true;
}

uint32_t Fat32Parser::clusterToLBA(uint32_t cluster) const {
    return data_start_lba_ + (cluster - 2) * bpb_.sectors_per_cluster;
}

bool Fat32Parser::readFatSector(uint32_t fat_sector_lba, std::vector<uint8_t>& out) {
    auto it = fat_cache_.find(fat_sector_lba);
    if (it != fat_cache_.end()) {
        out = it->second;
        return true;
    }

    // Bulk read: fetch 64 consecutive FAT sectors at once (32KB = 8192 entries)
    uint32_t bulk = 64;
    uint32_t fat_end = fat_start_lba_ + bpb_.fat_size_sectors;
    if (fat_sector_lba + bulk > fat_end) {
        bulk = fat_end - fat_sector_lba;
    }
    if (bulk == 0) bulk = 1;

    std::vector<uint8_t> bulk_buf;
    if (!readSectors(fat_sector_lba, bulk, bulk_buf)) {
        // Fallback: single sector
        out.resize(512);
        if (!readSectors(fat_sector_lba, 1, out)) return false;
        if (fat_cache_.size() < FAT_CACHE_MAX_SECTORS) {
            fat_cache_[fat_sector_lba] = out;
        }
        return true;
    }

    // Cache all fetched sectors
    for (uint32_t i = 0; i < bulk && fat_cache_.size() < FAT_CACHE_MAX_SECTORS; ++i) {
        std::vector<uint8_t> sector(bulk_buf.begin() + i * 512,
                                     bulk_buf.begin() + (i + 1) * 512);
        fat_cache_[fat_sector_lba + i] = std::move(sector);
    }

    out = fat_cache_[fat_sector_lba];
    return true;
}

uint32_t Fat32Parser::fatEntryForCluster(uint32_t cluster) {
    uint32_t fat_offset = cluster * 4;
    uint32_t fat_sector = fat_start_lba_ + (fat_offset / 512);
    uint32_t offset_in_sector = fat_offset % 512;

    std::vector<uint8_t> buf;
    if (!readFatSector(fat_sector, buf)) return 0x0FFFFFFF;

    uint32_t entry;
    std::memcpy(&entry, &buf[offset_in_sector], 4);
    return entry & 0x0FFFFFFF;
}

std::vector<uint32_t> Fat32Parser::clusterChain(uint32_t first_cluster) {
    std::vector<uint32_t> chain;
    uint32_t cluster = first_cluster;
    // 50k clusters × 4KB/cluster = 200MB max file — well beyond any EDF
    while (cluster >= 2 && cluster < 0x0FFFFFF8 && chain.size() < 50000) {
        // Cycle detection: if we've seen this cluster, the FAT is corrupt
        if (chain.size() > 1 && cluster == first_cluster) {
            std::cerr << "FAT32: Circular cluster chain at " << cluster << std::endl;
            break;
        }
        chain.push_back(cluster);
        cluster = fatEntryForCluster(cluster);
    }
    return chain;
}

std::vector<Fat32Parser::SectorRange>
Fat32Parser::clusterChainToSectorRanges(uint32_t first_cluster) {
    auto chain = clusterChain(first_cluster);
    if (chain.empty()) return {};

    std::vector<SectorRange> ranges;
    uint32_t run_start = clusterToLBA(chain[0]);
    uint32_t run_count = bpb_.sectors_per_cluster;

    for (size_t i = 1; i < chain.size(); ++i) {
        uint32_t lba = clusterToLBA(chain[i]);
        if (lba == run_start + run_count) {
            run_count += bpb_.sectors_per_cluster;
        } else {
            ranges.push_back({run_start, run_count});
            run_start = lba;
            run_count = bpb_.sectors_per_cluster;
        }
    }
    ranges.push_back({run_start, run_count});
    return ranges;
}

std::vector<Fat32Parser::SectorRange>
Fat32Parser::fileSectorRanges(uint32_t first_cluster, uint32_t file_size,
                               uint32_t byte_offset) {
    auto all_ranges = clusterChainToSectorRanges(first_cluster);
    if (byte_offset == 0 && file_size == 0) return all_ranges;

    uint32_t start_sector = byte_offset / 512;
    uint32_t end_sector = (file_size + 511) / 512;

    std::vector<SectorRange> result;
    uint32_t cumulative = 0;
    for (auto& r : all_ranges) {
        uint32_t range_end = cumulative + r.count;
        if (range_end <= start_sector) {
            cumulative = range_end;
            continue;
        }
        if (cumulative >= end_sector) break;

        uint32_t trim_start = (start_sector > cumulative) ? (start_sector - cumulative) : 0;
        uint32_t trim_end = (end_sector < range_end) ? (range_end - end_sector) : 0;
        uint32_t new_count = r.count - trim_start - trim_end;
        if (new_count > 0) {
            result.push_back({r.lba + trim_start, new_count});
        }
        cumulative = range_end;
    }
    return result;
}

std::vector<Fat32DirEntry> Fat32Parser::parseDirEntries(const std::vector<uint8_t>& data) {
    std::vector<Fat32DirEntry> entries;
    std::string lfn_name;
    size_t entry_count = data.size() / 32;

    for (size_t i = 0; i < entry_count; ++i) {
        const uint8_t* e = &data[i * 32];

        if (e[0] == 0x00) break;     // end of directory
        if (e[0] == 0xE5) continue;  // deleted entry

        uint8_t attr = e[11];

        // LFN entry
        if (attr == 0x0F) {
            bool is_last = (e[0] & 0x40) != 0;

            // Extract UCS-2 characters from LFN entry
            char16_t chars[13];
            std::memcpy(&chars[0],  &e[1], 2);
            std::memcpy(&chars[1],  &e[3], 2);
            std::memcpy(&chars[2],  &e[5], 2);
            std::memcpy(&chars[3],  &e[7], 2);
            std::memcpy(&chars[4],  &e[9], 2);
            std::memcpy(&chars[5],  &e[14], 2);
            std::memcpy(&chars[6],  &e[16], 2);
            std::memcpy(&chars[7],  &e[18], 2);
            std::memcpy(&chars[8],  &e[20], 2);
            std::memcpy(&chars[9],  &e[22], 2);
            std::memcpy(&chars[10], &e[24], 2);
            std::memcpy(&chars[11], &e[28], 2);
            std::memcpy(&chars[12], &e[30], 2);

            std::string part;
            for (int c = 0; c < 13; ++c) {
                if (chars[c] == 0x0000 || chars[c] == 0xFFFF) break;
                part += static_cast<char>(chars[c] & 0xFF);
            }

            if (is_last) {
                lfn_name = part;
            } else {
                lfn_name = part + lfn_name;
            }
            continue;
        }

        // Volume label or system entry
        if (attr & 0x08) {
            lfn_name.clear();
            continue;
        }

        Fat32DirEntry entry;

        if (!lfn_name.empty()) {
            entry.name = lfn_name;
            lfn_name.clear();
        } else {
            // 8.3 short name
            char name83[12];
            std::memcpy(name83, e, 8);
            name83[8] = '\0';
            std::string base(name83);
            while (!base.empty() && base.back() == ' ') base.pop_back();

            char ext83[4];
            std::memcpy(ext83, e + 8, 3);
            ext83[3] = '\0';
            std::string ext(ext83);
            while (!ext.empty() && ext.back() == ' ') ext.pop_back();

            entry.name = ext.empty() ? base : (base + "." + ext);
        }

        uint16_t cluster_hi, cluster_lo;
        std::memcpy(&cluster_hi, &e[20], 2);
        std::memcpy(&cluster_lo, &e[26], 2);
        entry.first_cluster = (static_cast<uint32_t>(cluster_hi) << 16) | cluster_lo;
        std::memcpy(&entry.size, &e[28], 4);
        entry.is_directory = (attr & 0x10) != 0;
        std::memcpy(&entry.modify_time, &e[22], 2);
        std::memcpy(&entry.modify_date, &e[24], 2);

        // Skip . and ..
        if (entry.name == "." || entry.name == "..") continue;

        entries.push_back(std::move(entry));
    }
    return entries;
}

std::vector<Fat32DirEntry> Fat32Parser::listDir(uint32_t dir_cluster) {
    if (!initialized_) return {};

    auto chain = clusterChain(dir_cluster);
    std::vector<uint8_t> dir_data;

    for (uint32_t cluster : chain) {
        uint32_t lba = clusterToLBA(cluster);
        std::vector<uint8_t> buf(bpb_.sectors_per_cluster * 512);
        if (!readSectors(lba, bpb_.sectors_per_cluster, buf)) break;
        dir_data.insert(dir_data.end(), buf.begin(), buf.end());
    }

    return parseDirEntries(dir_data);
}

std::vector<Fat32DirEntry> Fat32Parser::listPath(const std::string& path) {
    if (!initialized_) return {};

    std::string clean = path;
    while (!clean.empty() && clean.front() == '/') clean.erase(0, 1);
    while (!clean.empty() && clean.back() == '/') clean.pop_back();

    if (clean.empty()) return listDir(bpb_.root_cluster);

    // Walk path components
    uint32_t current_cluster = bpb_.root_cluster;
    std::istringstream ss(clean);
    std::string component;

    while (std::getline(ss, component, '/')) {
        auto entries = listDir(current_cluster);
        bool found = false;
        for (auto& e : entries) {
            // Case-insensitive comparison
            std::string name_lower = e.name;
            std::string comp_lower = component;
            std::transform(name_lower.begin(), name_lower.end(), name_lower.begin(), ::tolower);
            std::transform(comp_lower.begin(), comp_lower.end(), comp_lower.begin(), ::tolower);
            if (name_lower == comp_lower && e.is_directory) {
                current_cluster = e.first_cluster;
                found = true;
                break;
            }
        }
        if (!found) return {};
    }

    return listDir(current_cluster);
}

}  // namespace hms_cpap
