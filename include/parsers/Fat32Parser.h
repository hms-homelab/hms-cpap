#pragma once

#include <cstdint>
#include <string>
#include <vector>
#include <functional>
#include <optional>
#include <chrono>
#include <map>

namespace hms_cpap {

struct Fat32BPB {
    uint16_t bytes_per_sector = 0;
    uint8_t  sectors_per_cluster = 0;
    uint16_t reserved_sectors = 0;
    uint8_t  num_fats = 0;
    uint32_t total_sectors = 0;
    uint32_t fat_size_sectors = 0;
    uint32_t root_cluster = 0;
    uint16_t fs_info_sector = 0;
};

struct Fat32DirEntry {
    std::string name;
    uint32_t first_cluster = 0;
    uint32_t size = 0;
    bool is_directory = false;
    uint16_t modify_date = 0;
    uint16_t modify_time = 0;

    std::chrono::system_clock::time_point modTime() const;
};

class Fat32Parser {
public:
    using SectorReader = std::function<bool(uint32_t lba, uint32_t count, std::vector<uint8_t>& out)>;

    explicit Fat32Parser(SectorReader reader);

    bool init();

    const Fat32BPB& bpb() const { return bpb_; }

    std::vector<Fat32DirEntry> listDir(uint32_t dir_cluster);

    std::vector<Fat32DirEntry> listPath(const std::string& path);

    std::vector<uint32_t> clusterChain(uint32_t first_cluster);

    struct SectorRange {
        uint32_t lba;
        uint32_t count;
    };
    std::vector<SectorRange> clusterChainToSectorRanges(uint32_t first_cluster);

    std::vector<SectorRange> fileSectorRanges(uint32_t first_cluster, uint32_t file_size,
                                               uint32_t byte_offset = 0);

    void clearFatCache() { fat_cache_.clear(); }

    uint32_t clusterToLBA(uint32_t cluster) const;

private:
    uint32_t fatEntryForCluster(uint32_t cluster);
    bool readSectors(uint32_t lba, uint32_t count, std::vector<uint8_t>& out);
    std::vector<Fat32DirEntry> parseDirEntries(const std::vector<uint8_t>& data);

    SectorReader reader_;
    Fat32BPB bpb_;
    uint32_t fat_start_lba_ = 0;
    uint32_t data_start_lba_ = 0;
    uint32_t partition_lba_ = 0;
    bool initialized_ = false;

    // FAT sector cache: maps FAT sector LBA → 512 bytes
    static constexpr size_t FAT_CACHE_MAX_SECTORS = 128;
    std::map<uint32_t, std::vector<uint8_t>> fat_cache_;
    bool readFatSector(uint32_t fat_sector_lba, std::vector<uint8_t>& out);
};

}  // namespace hms_cpap
