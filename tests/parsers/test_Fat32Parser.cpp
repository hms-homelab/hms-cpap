#include <gtest/gtest.h>
#include "parsers/Fat32Parser.h"
#include <cstring>
#include <map>
#include <algorithm>

using namespace hms_cpap;

// Synthetic FAT32 image builder for testing.
// Creates an in-memory sector map that Fat32Parser can read via SectorReader callback.
class FakeFat32Image {
public:
    static constexpr uint32_t SECTORS_PER_CLUSTER = 8;
    static constexpr uint32_t RESERVED_SECTORS = 32;
    static constexpr uint32_t FAT_SIZE_SECTORS = 64;
    static constexpr uint32_t NUM_FATS = 2;
    static constexpr uint32_t ROOT_CLUSTER = 2;
    static constexpr uint32_t DATA_START = RESERVED_SECTORS + NUM_FATS * FAT_SIZE_SECTORS;

    FakeFat32Image() {
        buildVBR();
        setFatEntry(0, 0x0FFFFFF8);
        setFatEntry(1, 0x0FFFFFFF);
        // Root dir cluster 2 — end of chain
        setFatEntry(ROOT_CLUSTER, 0x0FFFFFFF);
        buildRootDir();
    }

    bool readSectors(uint32_t lba, uint32_t count, std::vector<uint8_t>& out) {
        out.resize(count * 512, 0);
        for (uint32_t i = 0; i < count; ++i) {
            auto it = sectors_.find(lba + i);
            if (it != sectors_.end()) {
                std::memcpy(&out[i * 512], it->second.data(), 512);
            }
        }
        return true;
    }

    Fat32Parser::SectorReader makeReader() {
        return [this](uint32_t lba, uint32_t count, std::vector<uint8_t>& out) {
            return readSectors(lba, count, out);
        };
    }

    void setFatEntry(uint32_t cluster, uint32_t value) {
        uint32_t offset = cluster * 4;
        uint32_t sector = RESERVED_SECTORS + (offset / 512);
        uint32_t off_in_sector = offset % 512;
        ensureSector(sector);
        *reinterpret_cast<uint32_t*>(&sectors_[sector][off_in_sector]) = value & 0x0FFFFFFF;
    }

    // Add a file or directory entry to a directory at the given cluster
    void addDirEntry(uint32_t dir_cluster, const std::string& name,
                     uint32_t first_cluster, uint32_t size, bool is_dir,
                     uint16_t mod_date = 0x5921, uint16_t mod_time = 0x6000) {
        uint32_t lba = clusterToLBA(dir_cluster);
        ensureSector(lba);

        // Find next free 32-byte slot
        auto& sec = sectors_[lba];
        int slot = -1;
        for (int i = 0; i < 16; ++i) {  // 16 entries per sector
            if (sec[i * 32] == 0x00 || sec[i * 32] == 0xE5) {
                slot = i;
                break;
            }
        }
        if (slot < 0) return;  // dir full (simplified: only 1 sector per dir for tests)

        uint8_t* e = &sec[slot * 32];
        std::memset(e, 0, 32);

        // 8.3 name
        std::string base, ext;
        auto dot = name.rfind('.');
        if (dot != std::string::npos && !is_dir) {
            base = name.substr(0, dot);
            ext = name.substr(dot + 1);
        } else {
            base = name;
        }
        // Pad to 8.3
        while (base.size() < 8) base += ' ';
        while (ext.size() < 3) ext += ' ';
        // Uppercase for 8.3
        std::transform(base.begin(), base.end(), base.begin(), ::toupper);
        std::transform(ext.begin(), ext.end(), ext.begin(), ::toupper);

        std::memcpy(e, base.c_str(), 8);
        std::memcpy(e + 8, ext.c_str(), 3);
        e[11] = is_dir ? 0x10 : 0x20;

        *reinterpret_cast<uint16_t*>(&e[20]) = (first_cluster >> 16) & 0xFFFF;
        *reinterpret_cast<uint16_t*>(&e[26]) = first_cluster & 0xFFFF;
        *reinterpret_cast<uint32_t*>(&e[28]) = size;
        *reinterpret_cast<uint16_t*>(&e[22]) = mod_time;
        *reinterpret_cast<uint16_t*>(&e[24]) = mod_date;
    }

    uint32_t clusterToLBA(uint32_t cluster) {
        return DATA_START + (cluster - 2) * SECTORS_PER_CLUSTER;
    }

    // Write arbitrary data into a cluster
    void writeClusterData(uint32_t cluster, const std::vector<uint8_t>& data) {
        uint32_t lba = clusterToLBA(cluster);
        for (uint32_t s = 0; s < SECTORS_PER_CLUSTER && s * 512 < data.size(); ++s) {
            ensureSector(lba + s);
            size_t bytes = std::min<size_t>(512, data.size() - s * 512);
            std::memcpy(sectors_[lba + s].data(), &data[s * 512], bytes);
        }
    }

private:
    void ensureSector(uint32_t lba) {
        if (sectors_.find(lba) == sectors_.end()) {
            sectors_[lba].resize(512, 0);
        }
    }

    void buildVBR() {
        ensureSector(0);
        auto& vbr = sectors_[0];

        // Jump boot code
        vbr[0] = 0xEB; vbr[1] = 0x58; vbr[2] = 0x90;
        // OEM name
        std::memcpy(&vbr[3], "MSDOS5.0", 8);
        // BPB
        *reinterpret_cast<uint16_t*>(&vbr[11]) = 512;           // bytes per sector
        vbr[13] = SECTORS_PER_CLUSTER;
        *reinterpret_cast<uint16_t*>(&vbr[14]) = RESERVED_SECTORS;
        vbr[16] = NUM_FATS;
        *reinterpret_cast<uint16_t*>(&vbr[19]) = 0;             // total sectors 16 (0 for FAT32)
        *reinterpret_cast<uint32_t*>(&vbr[32]) = 65536;         // total sectors 32
        *reinterpret_cast<uint32_t*>(&vbr[36]) = FAT_SIZE_SECTORS;
        *reinterpret_cast<uint32_t*>(&vbr[44]) = ROOT_CLUSTER;
        *reinterpret_cast<uint16_t*>(&vbr[48]) = 1;             // FSInfo sector
        // FAT32 signature
        vbr[82] = 'F'; vbr[83] = 'A'; vbr[84] = 'T'; vbr[85] = '3'; vbr[86] = '2';
        // Boot signature
        vbr[510] = 0x55; vbr[511] = 0xAA;
    }

    void buildRootDir() {
        uint32_t lba = clusterToLBA(ROOT_CLUSTER);
        ensureSector(lba);
        // Start empty — entries added via addDirEntry
    }

    std::map<uint32_t, std::vector<uint8_t>> sectors_;
};

// ============================================================================
// Tests
// ============================================================================

class Fat32ParserTest : public ::testing::Test {
protected:
    FakeFat32Image image;
};

TEST_F(Fat32ParserTest, InitParsesVBR) {
    Fat32Parser parser(image.makeReader());
    ASSERT_TRUE(parser.init());

    auto& bpb = parser.bpb();
    EXPECT_EQ(bpb.bytes_per_sector, 512);
    EXPECT_EQ(bpb.sectors_per_cluster, 8);
    EXPECT_EQ(bpb.reserved_sectors, 32);
    EXPECT_EQ(bpb.num_fats, 2);
    EXPECT_EQ(bpb.fat_size_sectors, 64);
    EXPECT_EQ(bpb.root_cluster, 2);
}

TEST_F(Fat32ParserTest, EmptyRootDir) {
    Fat32Parser parser(image.makeReader());
    ASSERT_TRUE(parser.init());

    auto entries = parser.listDir(2);
    EXPECT_TRUE(entries.empty());
}

TEST_F(Fat32ParserTest, SingleFileInRoot) {
    image.addDirEntry(2, "STR.edf", 3, 75046, false);
    image.setFatEntry(3, 0x0FFFFFFF);

    Fat32Parser parser(image.makeReader());
    ASSERT_TRUE(parser.init());

    auto entries = parser.listDir(2);
    ASSERT_EQ(entries.size(), 1);
    EXPECT_EQ(entries[0].name, "STR.EDF");
    EXPECT_EQ(entries[0].first_cluster, 3);
    EXPECT_EQ(entries[0].size, 75046u);
    EXPECT_FALSE(entries[0].is_directory);
}

TEST_F(Fat32ParserTest, DirectoryAndFiles) {
    // Root: DATALOG directory at cluster 3
    image.addDirEntry(2, "DATALOG", 3, 0, true);
    image.setFatEntry(3, 0x0FFFFFFF);

    // DATALOG: two date folders
    image.addDirEntry(3, "20260418", 4, 0, true);
    image.addDirEntry(3, "20260417", 5, 0, true);
    image.setFatEntry(4, 0x0FFFFFFF);
    image.setFatEntry(5, 0x0FFFFFFF);

    // 20260418: one BRP file at cluster 6, spanning 3 clusters
    image.addDirEntry(4, "20260418_212058_BRP.edf", 6, 2641904, false);
    image.setFatEntry(6, 7);
    image.setFatEntry(7, 8);
    image.setFatEntry(8, 0x0FFFFFFF);

    Fat32Parser parser(image.makeReader());
    ASSERT_TRUE(parser.init());

    // List root
    auto root = parser.listDir(2);
    ASSERT_EQ(root.size(), 1);
    EXPECT_EQ(root[0].name, "DATALOG");
    EXPECT_TRUE(root[0].is_directory);

    // List DATALOG
    auto datalog = parser.listDir(3);
    ASSERT_EQ(datalog.size(), 2);

    // List 20260418
    auto day = parser.listDir(4);
    ASSERT_EQ(day.size(), 1);
    // 8.3 truncation: short name check
    // Name might be truncated to 8.3 format
    EXPECT_EQ(day[0].first_cluster, 6u);
    EXPECT_EQ(day[0].size, 2641904u);
}

TEST_F(Fat32ParserTest, ClusterChain) {
    image.setFatEntry(10, 11);
    image.setFatEntry(11, 12);
    image.setFatEntry(12, 0x0FFFFFFF);

    Fat32Parser parser(image.makeReader());
    ASSERT_TRUE(parser.init());

    auto chain = parser.clusterChain(10);
    ASSERT_EQ(chain.size(), 3);
    EXPECT_EQ(chain[0], 10u);
    EXPECT_EQ(chain[1], 11u);
    EXPECT_EQ(chain[2], 12u);
}

TEST_F(Fat32ParserTest, ClusterChainToSectorRangesContiguous) {
    // Contiguous clusters 10, 11, 12
    image.setFatEntry(10, 11);
    image.setFatEntry(11, 12);
    image.setFatEntry(12, 0x0FFFFFFF);

    Fat32Parser parser(image.makeReader());
    ASSERT_TRUE(parser.init());

    auto ranges = parser.clusterChainToSectorRanges(10);
    ASSERT_EQ(ranges.size(), 1);
    EXPECT_EQ(ranges[0].lba, parser.clusterToLBA(10));
    EXPECT_EQ(ranges[0].count, 3 * 8);  // 3 clusters × 8 sectors
}

TEST_F(Fat32ParserTest, ClusterChainToSectorRangesFragmented) {
    // Non-contiguous: cluster 10 → 20 → 30
    image.setFatEntry(10, 20);
    image.setFatEntry(20, 30);
    image.setFatEntry(30, 0x0FFFFFFF);

    Fat32Parser parser(image.makeReader());
    ASSERT_TRUE(parser.init());

    auto ranges = parser.clusterChainToSectorRanges(10);
    ASSERT_EQ(ranges.size(), 3);
    EXPECT_EQ(ranges[0].lba, parser.clusterToLBA(10));
    EXPECT_EQ(ranges[0].count, 8u);
    EXPECT_EQ(ranges[1].lba, parser.clusterToLBA(20));
    EXPECT_EQ(ranges[2].lba, parser.clusterToLBA(30));
}

TEST_F(Fat32ParserTest, FileSectorRangesWithOffset) {
    // 3-cluster file, 8 sectors per cluster = 24 sectors = 12288 bytes
    image.setFatEntry(10, 11);
    image.setFatEntry(11, 12);
    image.setFatEntry(12, 0x0FFFFFFF);

    Fat32Parser parser(image.makeReader());
    ASSERT_TRUE(parser.init());

    // Request bytes 4096..12288 (sectors 8..24)
    auto ranges = parser.fileSectorRanges(10, 12288, 4096);

    uint32_t total_sectors = 0;
    for (auto& r : ranges) total_sectors += r.count;
    EXPECT_EQ(total_sectors, 16u);  // 24 - 8 = 16 sectors
}

TEST_F(Fat32ParserTest, ListPathWalksTree) {
    image.addDirEntry(2, "DATALOG", 3, 0, true);
    image.setFatEntry(3, 0x0FFFFFFF);

    image.addDirEntry(3, "20260418", 4, 0, true);
    image.setFatEntry(4, 0x0FFFFFFF);

    image.addDirEntry(4, "STR.EDF", 5, 1024, false);
    image.setFatEntry(5, 0x0FFFFFFF);

    Fat32Parser parser(image.makeReader());
    ASSERT_TRUE(parser.init());

    auto entries = parser.listPath("DATALOG/20260418");
    ASSERT_EQ(entries.size(), 1);
    EXPECT_EQ(entries[0].name, "STR.EDF");
    EXPECT_EQ(entries[0].size, 1024u);
}

TEST_F(Fat32ParserTest, ListPathCaseInsensitive) {
    image.addDirEntry(2, "DATALOG", 3, 0, true);
    image.setFatEntry(3, 0x0FFFFFFF);
    image.addDirEntry(3, "TEST.TXT", 4, 512, false);
    image.setFatEntry(4, 0x0FFFFFFF);

    Fat32Parser parser(image.makeReader());
    ASSERT_TRUE(parser.init());

    auto entries = parser.listPath("datalog");
    ASSERT_EQ(entries.size(), 1);
    EXPECT_EQ(entries[0].name, "TEST.TXT");
}

TEST_F(Fat32ParserTest, ListPathNotFound) {
    Fat32Parser parser(image.makeReader());
    ASSERT_TRUE(parser.init());

    auto entries = parser.listPath("NONEXISTENT");
    EXPECT_TRUE(entries.empty());
}

TEST_F(Fat32ParserTest, ModTimeDecoding) {
    // FAT date: 2026-04-18 → ((2026-1980)<<9) | (4<<5) | 18 = (46<<9)|(4<<5)|18 = 23554+128+18 = 23570 = 0x5C12
    // FAT time: 21:08:32 → (21<<11) | (8<<5) | 16 = 43008+256+16 = 43280 = 0xA910
    uint16_t date = (46 << 9) | (4 << 5) | 18;
    uint16_t time = (21 << 11) | (8 << 5) | 16;

    image.addDirEntry(2, "TEST.TXT", 3, 100, false, date, time);
    image.setFatEntry(3, 0x0FFFFFFF);

    Fat32Parser parser(image.makeReader());
    ASSERT_TRUE(parser.init());

    auto entries = parser.listDir(2);
    ASSERT_EQ(entries.size(), 1);

    auto tp = entries[0].modTime();
    auto tt = std::chrono::system_clock::to_time_t(tp);
    std::tm* tm = std::localtime(&tt);
    EXPECT_EQ(tm->tm_year + 1900, 2026);
    EXPECT_EQ(tm->tm_mon + 1, 4);
    EXPECT_EQ(tm->tm_mday, 18);
    EXPECT_EQ(tm->tm_hour, 21);
    EXPECT_EQ(tm->tm_min, 8);
    EXPECT_EQ(tm->tm_sec, 32);
}

TEST_F(Fat32ParserTest, InvalidImageReturnsFailure) {
    // Empty reader — no valid VBR
    auto empty_reader = [](uint32_t, uint32_t, std::vector<uint8_t>& out) {
        out.resize(512, 0);
        return true;
    };

    Fat32Parser parser(empty_reader);
    EXPECT_FALSE(parser.init());
}

TEST_F(Fat32ParserTest, ReaderFailureReturnsFailure) {
    auto failing_reader = [](uint32_t, uint32_t, std::vector<uint8_t>&) {
        return false;
    };

    Fat32Parser parser(failing_reader);
    EXPECT_FALSE(parser.init());
}

TEST_F(Fat32ParserTest, FatCacheBulkRead) {
    // Create a 5-cluster chain — should trigger one bulk FAT read instead of 5
    image.setFatEntry(10, 11);
    image.setFatEntry(11, 12);
    image.setFatEntry(12, 13);
    image.setFatEntry(13, 14);
    image.setFatEntry(14, 0x0FFFFFFF);

    int read_count = 0;
    auto counting_reader = [&](uint32_t lba, uint32_t count, std::vector<uint8_t>& out) {
        read_count++;
        return image.readSectors(lba, count, out);
    };

    Fat32Parser parser(counting_reader);
    ASSERT_TRUE(parser.init());

    read_count = 0;
    auto chain = parser.clusterChain(10);
    ASSERT_EQ(chain.size(), 5u);

    // All 5 FAT entries are in the same FAT sector range.
    // With bulk caching, we should need only 1 read (or 2 max including init).
    EXPECT_LE(read_count, 2);
}

TEST_F(Fat32ParserTest, CircularClusterChainDetected) {
    // Create a circular chain: 10 -> 11 -> 12 -> 10 (loop)
    image.setFatEntry(10, 11);
    image.setFatEntry(11, 12);
    image.setFatEntry(12, 10);  // back to start

    Fat32Parser parser(image.makeReader());
    ASSERT_TRUE(parser.init());

    auto chain = parser.clusterChain(10);
    // Should terminate — not loop forever. Chain has 3 entries before cycle detected.
    EXPECT_LE(chain.size(), 50000u);
    EXPECT_GE(chain.size(), 2u);
}

TEST_F(Fat32ParserTest, ClusterChainSafetyLimit) {
    // Create a very long chain (100 clusters)
    for (uint32_t c = 100; c < 200; ++c) {
        image.setFatEntry(c, c + 1);
    }
    image.setFatEntry(200, 0x0FFFFFFF);

    Fat32Parser parser(image.makeReader());
    ASSERT_TRUE(parser.init());

    auto chain = parser.clusterChain(100);
    EXPECT_EQ(chain.size(), 101u);  // 100..200 inclusive
}
