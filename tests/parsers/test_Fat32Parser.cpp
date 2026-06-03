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

    // Write a raw 32-byte directory entry into the next free slot of a directory.
    // Used to inject LFN entries, volume labels, deleted entries, and dotdot entries
    // that addDirEntry() cannot express.
    void addRawDirEntry(uint32_t dir_cluster, const uint8_t entry[32]) {
        uint32_t lba = clusterToLBA(dir_cluster);
        ensureSector(lba);
        auto& sec = sectors_[lba];
        int slot = -1;
        for (int i = 0; i < 16; ++i) {
            if (sec[i * 32] == 0x00 || sec[i * 32] == 0xE5) {
                slot = i;
                break;
            }
        }
        if (slot < 0) return;
        std::memcpy(&sec[slot * 32], entry, 32);
    }

    // Build a single LFN entry for the given UTF-16 (low-byte) part.
    // sequence is 1-based; set is_last for the highest-numbered (first-on-disk) entry.
    static void makeLfnEntry(uint8_t out[32], uint8_t sequence, bool is_last,
                             const std::u16string& part) {
        std::memset(out, 0, 32);
        out[0] = sequence | (is_last ? 0x40 : 0x00);
        out[11] = 0x0F;  // LFN attribute
        // Character slot byte offsets within the 32-byte entry
        static const int slots[13] = {1, 3, 5, 7, 9, 14, 16, 18, 20, 22, 24, 28, 30};
        for (int c = 0; c < 13; ++c) {
            char16_t ch;
            if (c < (int)part.size()) {
                ch = part[c];
            } else if (c == (int)part.size()) {
                ch = 0x0000;  // null terminator
            } else {
                ch = 0xFFFF;  // padding
            }
            std::memcpy(&out[slots[c]], &ch, 2);
        }
    }

    // Build an 8.3 short-name entry for a regular file (used as the LFN anchor).
    static void makeShortEntry(uint8_t out[32], const std::string& name83,
                               uint32_t first_cluster, uint32_t size, uint8_t attr) {
        std::memset(out, 0, 32);
        std::string padded = name83;
        while (padded.size() < 11) padded += ' ';
        std::memcpy(out, padded.data(), 11);
        out[11] = attr;
        *reinterpret_cast<uint16_t*>(&out[20]) = (first_cluster >> 16) & 0xFFFF;
        *reinterpret_cast<uint16_t*>(&out[26]) = first_cluster & 0xFFFF;
        *reinterpret_cast<uint32_t*>(&out[28]) = size;
    }

    // Build a fresh image that is fronted by an MBR in sector 0 and has its
    // VBR relocated to partition_lba. part_type controls the partition entry type.
    void relocateBehindMbr(uint32_t partition_lba, uint8_t part_type = 0x0C) {
        // Move the existing VBR (sector 0) to partition_lba.
        std::vector<uint8_t> vbr = sectors_[0];
        sectors_.erase(0);
        // Shift every data/FAT sector by partition_lba so cluster math still resolves.
        std::map<uint32_t, std::vector<uint8_t>> shifted;
        for (auto& kv : sectors_) {
            shifted[kv.first + partition_lba] = std::move(kv.second);
        }
        sectors_ = std::move(shifted);
        sectors_[partition_lba] = vbr;

        // Build the MBR in sector 0.
        std::vector<uint8_t> mbr(512, 0);
        mbr[446 + 4] = part_type;                                  // partition type
        *reinterpret_cast<uint32_t*>(&mbr[446 + 8]) = partition_lba;  // starting LBA
        mbr[510] = 0x55;
        mbr[511] = 0xAA;
        sectors_[0] = std::move(mbr);
        partition_lba_offset_ = partition_lba;
    }

    // After relocateBehindMbr, expose the offset so tests can compute absolute LBAs.
    uint32_t partitionLbaOffset() const { return partition_lba_offset_; }

    // Corrupt a single byte in a stored sector (used to invalidate signatures, etc.)
    void pokeSector(uint32_t lba, uint32_t offset, uint8_t value) {
        ensureSector(lba);
        sectors_[lba][offset] = value;
    }

    void pokeVbr16(uint32_t offset, uint16_t value) {
        uint32_t lba = (partition_lba_offset_ != 0) ? partition_lba_offset_ : 0;
        ensureSector(lba);
        *reinterpret_cast<uint16_t*>(&sectors_[lba][offset]) = value;
    }

    void pokeVbr8(uint32_t offset, uint8_t value) {
        uint32_t lba = (partition_lba_offset_ != 0) ? partition_lba_offset_ : 0;
        ensureSector(lba);
        sectors_[lba][offset] = value;
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
    uint32_t partition_lba_offset_ = 0;
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

// ============================================================================
// MBR (partitioned image) paths — init() reads sector 0 as an MBR, locates the
// FAT32 partition, then re-reads the VBR at the partition LBA.
// ============================================================================

TEST_F(Fat32ParserTest, InitViaMbrPartitionType0C) {
    image.addDirEntry(2, "STR.edf", 3, 1234, false);
    image.setFatEntry(3, 0x0FFFFFFF);
    image.relocateBehindMbr(2048, 0x0C);

    Fat32Parser parser(image.makeReader());
    ASSERT_TRUE(parser.init());

    // BPB parsed from the relocated VBR.
    EXPECT_EQ(parser.bpb().root_cluster, 2u);
    // clusterToLBA must now include the partition offset.
    EXPECT_EQ(parser.clusterToLBA(2), 2048u + FakeFat32Image::DATA_START);

    auto entries = parser.listDir(2);
    ASSERT_EQ(entries.size(), 1u);
    EXPECT_EQ(entries[0].name, "STR.EDF");
    EXPECT_EQ(entries[0].size, 1234u);
}

TEST_F(Fat32ParserTest, InitViaMbrPartitionType0B) {
    image.relocateBehindMbr(63, 0x0B);
    Fat32Parser parser(image.makeReader());
    EXPECT_TRUE(parser.init());
}

TEST_F(Fat32ParserTest, InitMbrUnsupportedPartitionType) {
    // 0x07 = NTFS — not FAT32, init must reject.
    image.relocateBehindMbr(2048, 0x07);
    Fat32Parser parser(image.makeReader());
    EXPECT_FALSE(parser.init());
}

TEST_F(Fat32ParserTest, InitMbrVbrMissingSignature) {
    image.relocateBehindMbr(2048, 0x0C);
    // Corrupt the relocated VBR's boot signature so the second check fails.
    image.pokeSector(2048, 510, 0x00);
    Fat32Parser parser(image.makeReader());
    EXPECT_FALSE(parser.init());
}

// ============================================================================
// Malformed VBR / BPB edge cases
// ============================================================================

TEST_F(Fat32ParserTest, InitMbrSignaturePresentButCorrupt) {
    // Valid 0x55AA on sector 0, no FAT32 string at 82, and a partition type that
    // is neither 0x0B nor 0x0C → rejected.
    auto reader = [](uint32_t lba, uint32_t, std::vector<uint8_t>& out) {
        out.assign(512, 0);
        if (lba == 0) {
            out[446 + 4] = 0x83;  // Linux partition
            out[510] = 0x55;
            out[511] = 0xAA;
        }
        return true;
    };
    Fat32Parser parser(reader);
    EXPECT_FALSE(parser.init());
}

TEST_F(Fat32ParserTest, InitRejectsNonStandardSectorSize) {
    image.pokeVbr16(11, 1024);  // bytes per sector != 512
    Fat32Parser parser(image.makeReader());
    EXPECT_FALSE(parser.init());
}

TEST_F(Fat32ParserTest, InitRejectsZeroSectorsPerCluster) {
    image.pokeVbr8(13, 0);
    Fat32Parser parser(image.makeReader());
    EXPECT_FALSE(parser.init());
}

TEST_F(Fat32ParserTest, InitRejectsZeroNumFats) {
    image.pokeVbr8(16, 0);
    Fat32Parser parser(image.makeReader());
    EXPECT_FALSE(parser.init());
}

TEST_F(Fat32ParserTest, InitUsesTotalSectors16WhenNonZero) {
    // total_sectors16 (offset 19) takes precedence over total_sectors32 (offset 32).
    image.pokeVbr16(19, 40000);
    Fat32Parser parser(image.makeReader());
    ASSERT_TRUE(parser.init());
    EXPECT_EQ(parser.bpb().total_sectors, 40000u);
}

// ============================================================================
// Long File Name (LFN) parsing
// ============================================================================

TEST_F(Fat32ParserTest, LfnSingleEntryName) {
    // One LFN entry (<=13 chars) + its 8.3 anchor.
    uint8_t lfn[32];
    FakeFat32Image::makeLfnEntry(lfn, 1, /*is_last=*/true, u"LongName.txt");
    uint8_t shortEntry[32];
    FakeFat32Image::makeShortEntry(shortEntry, "LONGNA~1TXT", 5, 4096, 0x20);

    image.addRawDirEntry(2, lfn);
    image.addRawDirEntry(2, shortEntry);
    image.setFatEntry(5, 0x0FFFFFFF);

    Fat32Parser parser(image.makeReader());
    ASSERT_TRUE(parser.init());

    auto entries = parser.listDir(2);
    ASSERT_EQ(entries.size(), 1u);
    EXPECT_EQ(entries[0].name, "LongName.txt");
    EXPECT_EQ(entries[0].first_cluster, 5u);
    EXPECT_EQ(entries[0].size, 4096u);
    EXPECT_FALSE(entries[0].is_directory);
}

TEST_F(Fat32ParserTest, LfnMultiEntryNameOrdered) {
    // A 20-char name spans two LFN entries. On disk they are stored highest-first:
    //   seq 2 (is_last) holds chars 14..20, seq 1 holds chars 1..13.
    std::string fullName = "VeryLongFileName.edf";  // 20 chars
    std::u16string u(fullName.begin(), fullName.end());
    std::u16string part1 = u.substr(0, 13);
    std::u16string part2 = u.substr(13);

    uint8_t lfn2[32], lfn1[32], shortEntry[32];
    FakeFat32Image::makeLfnEntry(lfn2, 2, /*is_last=*/true, part2);
    FakeFat32Image::makeLfnEntry(lfn1, 1, /*is_last=*/false, part1);
    FakeFat32Image::makeShortEntry(shortEntry, "VERYLO~1EDF", 9, 2048, 0x20);

    image.addRawDirEntry(2, lfn2);
    image.addRawDirEntry(2, lfn1);
    image.addRawDirEntry(2, shortEntry);
    image.setFatEntry(9, 0x0FFFFFFF);

    Fat32Parser parser(image.makeReader());
    ASSERT_TRUE(parser.init());

    auto entries = parser.listDir(2);
    ASSERT_EQ(entries.size(), 1u);
    EXPECT_EQ(entries[0].name, fullName);
    EXPECT_EQ(entries[0].size, 2048u);
}

TEST_F(Fat32ParserTest, LfnPaddingTerminatesName) {
    // Name shorter than 13 chars: the parser must stop at the 0x0000 terminator
    // and not include 0xFFFF padding bytes.
    uint8_t lfn[32], shortEntry[32];
    FakeFat32Image::makeLfnEntry(lfn, 1, true, u"abc");
    FakeFat32Image::makeShortEntry(shortEntry, "ABC     TXT", 6, 10, 0x20);

    image.addRawDirEntry(2, lfn);
    image.addRawDirEntry(2, shortEntry);
    image.setFatEntry(6, 0x0FFFFFFF);

    Fat32Parser parser(image.makeReader());
    ASSERT_TRUE(parser.init());

    auto entries = parser.listDir(2);
    ASSERT_EQ(entries.size(), 1u);
    EXPECT_EQ(entries[0].name, "abc");
}

// ============================================================================
// Special directory entries: volume label, deleted, dot/dotdot
// ============================================================================

TEST_F(Fat32ParserTest, VolumeLabelEntryIgnored) {
    // attr & 0x08 = volume label; must be skipped and must clear any pending LFN.
    uint8_t vol[32];
    FakeFat32Image::makeShortEntry(vol, "MYVOLUME   ", 0, 0, 0x08);
    image.addRawDirEntry(2, vol);

    image.addDirEntry(2, "REAL.txt", 4, 50, false);
    image.setFatEntry(4, 0x0FFFFFFF);

    Fat32Parser parser(image.makeReader());
    ASSERT_TRUE(parser.init());

    auto entries = parser.listDir(2);
    ASSERT_EQ(entries.size(), 1u);
    EXPECT_EQ(entries[0].name, "REAL.TXT");
}

TEST_F(Fat32ParserTest, DeletedEntrySkipped) {
    // First slot is a deleted entry (0xE5), second is a live file.
    uint8_t deleted[32];
    FakeFat32Image::makeShortEntry(deleted, "GHOST   TXT", 7, 99, 0x20);
    deleted[0] = 0xE5;
    image.addRawDirEntry(2, deleted);

    image.addDirEntry(2, "ALIVE.txt", 8, 100, false);
    image.setFatEntry(8, 0x0FFFFFFF);

    Fat32Parser parser(image.makeReader());
    ASSERT_TRUE(parser.init());

    auto entries = parser.listDir(2);
    ASSERT_EQ(entries.size(), 1u);
    EXPECT_EQ(entries[0].name, "ALIVE.TXT");
}

TEST_F(Fat32ParserTest, DotAndDotDotSkipped) {
    uint8_t dot[32], dotdot[32];
    FakeFat32Image::makeShortEntry(dot,    ".          ", 3, 0, 0x10);
    FakeFat32Image::makeShortEntry(dotdot, "..         ", 2, 0, 0x10);
    image.addRawDirEntry(3, dot);
    image.addRawDirEntry(3, dotdot);
    image.addDirEntry(3, "FILE.txt", 4, 1, false);
    image.setFatEntry(3, 0x0FFFFFFF);
    image.setFatEntry(4, 0x0FFFFFFF);

    Fat32Parser parser(image.makeReader());
    ASSERT_TRUE(parser.init());

    auto entries = parser.listDir(3);
    ASSERT_EQ(entries.size(), 1u);
    EXPECT_EQ(entries[0].name, "FILE.TXT");
}

TEST_F(Fat32ParserTest, EndOfDirectoryStopsParsing) {
    // A 0x00 lead byte halts parsing; entries after it must be ignored.
    image.addDirEntry(2, "FIRST.txt", 3, 1, false);
    image.setFatEntry(3, 0x0FFFFFFF);

    // Manually place a live entry in slot 3, but leave slot 1 (the terminator)
    // as zero so parsing stops before reaching it.
    uint8_t hidden[32];
    FakeFat32Image::makeShortEntry(hidden, "HIDDEN  TXT", 4, 2, 0x20);
    uint32_t lba = image.clusterToLBA(2);
    // addDirEntry already filled slot 0; write hidden into slot 3 directly.
    std::vector<uint8_t> sec(512, 0);
    image.readSectors(lba, 1, sec);
    std::memcpy(&sec[3 * 32], hidden, 32);
    image.writeClusterData(2, sec);

    Fat32Parser parser(image.makeReader());
    ASSERT_TRUE(parser.init());

    auto entries = parser.listDir(2);
    ASSERT_EQ(entries.size(), 1u);
    EXPECT_EQ(entries[0].name, "FIRST.TXT");
}

// ============================================================================
// listDir / listPath guards and edge cases
// ============================================================================

TEST_F(Fat32ParserTest, ListDirOnUninitializedReturnsEmpty) {
    Fat32Parser parser(image.makeReader());
    // init() NOT called.
    EXPECT_TRUE(parser.listDir(2).empty());
}

TEST_F(Fat32ParserTest, ListPathOnUninitializedReturnsEmpty) {
    Fat32Parser parser(image.makeReader());
    EXPECT_TRUE(parser.listPath("DATALOG").empty());
}

TEST_F(Fat32ParserTest, ListPathEmptyReturnsRoot) {
    image.addDirEntry(2, "FILE.txt", 3, 10, false);
    image.setFatEntry(3, 0x0FFFFFFF);

    Fat32Parser parser(image.makeReader());
    ASSERT_TRUE(parser.init());

    // Leading/trailing slashes are stripped; empty path → root dir listing.
    auto entries = parser.listPath("///");
    ASSERT_EQ(entries.size(), 1u);
    EXPECT_EQ(entries[0].name, "FILE.TXT");
}

TEST_F(Fat32ParserTest, ListPathComponentIsFileNotDirectory) {
    // A path component that matches a file (not a directory) must fail the walk.
    image.addDirEntry(2, "FILE.txt", 3, 10, false);
    image.setFatEntry(3, 0x0FFFFFFF);

    Fat32Parser parser(image.makeReader());
    ASSERT_TRUE(parser.init());

    auto entries = parser.listPath("FILE.txt/more");
    EXPECT_TRUE(entries.empty());
}

TEST_F(Fat32ParserTest, ListPathTrailingSlashStripped) {
    image.addDirEntry(2, "DATALOG", 3, 0, true);
    image.setFatEntry(3, 0x0FFFFFFF);
    image.addDirEntry(3, "F.txt", 4, 5, false);
    image.setFatEntry(4, 0x0FFFFFFF);

    Fat32Parser parser(image.makeReader());
    ASSERT_TRUE(parser.init());

    auto entries = parser.listPath("/DATALOG/");
    ASSERT_EQ(entries.size(), 1u);
    EXPECT_EQ(entries[0].name, "F.TXT");
}

// ============================================================================
// fileSectorRanges edge cases
// ============================================================================

TEST_F(Fat32ParserTest, FileSectorRangesWholeFileWhenZeroArgs) {
    // byte_offset == 0 && file_size == 0 → return all cluster ranges untrimmed.
    image.setFatEntry(10, 11);
    image.setFatEntry(11, 0x0FFFFFFF);

    Fat32Parser parser(image.makeReader());
    ASSERT_TRUE(parser.init());

    auto all = parser.clusterChainToSectorRanges(10);
    auto ranges = parser.fileSectorRanges(10, 0, 0);
    ASSERT_EQ(ranges.size(), all.size());
    EXPECT_EQ(ranges[0].lba, all[0].lba);
    EXPECT_EQ(ranges[0].count, all[0].count);
}

TEST_F(Fat32ParserTest, FileSectorRangesTrimsTailToFileSize) {
    // 2 contiguous clusters = 16 sectors = 8192 bytes, but file is only 1000 bytes
    // → just 2 sectors (ceil(1000/512)).
    image.setFatEntry(10, 11);
    image.setFatEntry(11, 0x0FFFFFFF);

    Fat32Parser parser(image.makeReader());
    ASSERT_TRUE(parser.init());

    auto ranges = parser.fileSectorRanges(10, 1000, 0);
    uint32_t total = 0;
    for (auto& r : ranges) total += r.count;
    EXPECT_EQ(total, 2u);
    ASSERT_FALSE(ranges.empty());
    EXPECT_EQ(ranges[0].lba, parser.clusterToLBA(10));
}

TEST_F(Fat32ParserTest, FileSectorRangesOffsetSkipsEarlyRanges) {
    // Fragmented file: cluster 10, 20, 30 (each 8 sectors). Offset past the first
    // cluster (byte 4096 == sector 8) must drop the first range entirely.
    image.setFatEntry(10, 20);
    image.setFatEntry(20, 30);
    image.setFatEntry(30, 0x0FFFFFFF);

    Fat32Parser parser(image.makeReader());
    ASSERT_TRUE(parser.init());

    // Read from sector 8 to end (24 sectors total → 16 remaining).
    auto ranges = parser.fileSectorRanges(10, 24 * 512, 4096);
    uint32_t total = 0;
    for (auto& r : ranges) total += r.count;
    EXPECT_EQ(total, 16u);
    // First surviving range must start at cluster 20 (the first range was skipped).
    ASSERT_FALSE(ranges.empty());
    EXPECT_EQ(ranges[0].lba, parser.clusterToLBA(20));
}

TEST_F(Fat32ParserTest, FileSectorRangesEmptyChain) {
    // first_cluster < 2 yields an empty chain → empty ranges.
    Fat32Parser parser(image.makeReader());
    ASSERT_TRUE(parser.init());

    auto ranges = parser.fileSectorRanges(0, 1024, 0);
    EXPECT_TRUE(ranges.empty());
}

// ============================================================================
// clusterChain edge cases
// ============================================================================

TEST_F(Fat32ParserTest, ClusterChainEmptyForInvalidStart) {
    Fat32Parser parser(image.makeReader());
    ASSERT_TRUE(parser.init());

    // Cluster 0 and 1 are reserved; chain must be empty.
    EXPECT_TRUE(parser.clusterChain(0).empty());
    EXPECT_TRUE(parser.clusterChain(1).empty());
}

TEST_F(Fat32ParserTest, ClusterChainSingleCluster) {
    image.setFatEntry(42, 0x0FFFFFFF);
    Fat32Parser parser(image.makeReader());
    ASSERT_TRUE(parser.init());

    auto chain = parser.clusterChain(42);
    ASSERT_EQ(chain.size(), 1u);
    EXPECT_EQ(chain[0], 42u);
}

// ============================================================================
// FAT cache behavior
// ============================================================================

TEST_F(Fat32ParserTest, ClearFatCacheForcesReread) {
    image.setFatEntry(10, 11);
    image.setFatEntry(11, 0x0FFFFFFF);

    int read_count = 0;
    auto counting_reader = [&](uint32_t lba, uint32_t count, std::vector<uint8_t>& out) {
        read_count++;
        return image.readSectors(lba, count, out);
    };

    Fat32Parser parser(counting_reader);
    ASSERT_TRUE(parser.init());

    parser.clusterChain(10);
    int after_first = read_count;

    // Second call hits the cache — no additional FAT reads.
    parser.clusterChain(10);
    EXPECT_EQ(read_count, after_first);

    // Clearing the cache forces at least one fresh FAT read.
    parser.clearFatCache();
    parser.clusterChain(10);
    EXPECT_GT(read_count, after_first);
}

TEST_F(Fat32ParserTest, FatBulkReadFallbackToSingleSector) {
    // A reader that rejects multi-sector bulk reads but allows single-sector reads
    // exercises the fallback path in readFatSector().
    image.setFatEntry(10, 11);
    image.setFatEntry(11, 0x0FFFFFFF);

    auto fallback_reader = [&](uint32_t lba, uint32_t count, std::vector<uint8_t>& out) {
        if (count > 1) return false;  // bulk read fails
        return image.readSectors(lba, count, out);
    };

    Fat32Parser parser(fallback_reader);
    ASSERT_TRUE(parser.init());

    auto chain = parser.clusterChain(10);
    ASSERT_EQ(chain.size(), 2u);
    EXPECT_EQ(chain[0], 10u);
    EXPECT_EQ(chain[1], 11u);
}

// ============================================================================
// listDir spanning multiple directory clusters
// ============================================================================

TEST_F(Fat32ParserTest, ListDirFollowsMultiClusterChain) {
    // A directory whose chain spans two clusters (3 → 50). listDir must read both
    // clusters (exercising the per-cluster read loop more than once) before
    // parsing. The entry lives in the first cluster; the second cluster is all
    // zeros, so the result is exactly one entry — but the second readSectors call
    // for cluster 50 is still performed.
    image.addDirEntry(3, "AFILE.txt", 60, 11, false);
    image.setFatEntry(60, 0x0FFFFFFF);

    image.setFatEntry(3, 50);   // dir cluster 3 chains to cluster 50
    image.setFatEntry(50, 0x0FFFFFFF);
    image.writeClusterData(50, std::vector<uint8_t>(512, 0));  // materialize cluster 50

    int reads_of_cluster50 = 0;
    uint32_t lba50 = image.clusterToLBA(50);
    auto counting_reader = [&](uint32_t lba, uint32_t count, std::vector<uint8_t>& out) {
        if (lba == lba50) reads_of_cluster50++;
        return image.readSectors(lba, count, out);
    };

    Fat32Parser parser(counting_reader);
    ASSERT_TRUE(parser.init());

    auto entries = parser.listDir(3);
    ASSERT_EQ(entries.size(), 1u);
    EXPECT_EQ(entries[0].name, "AFILE.TXT");
    EXPECT_GE(reads_of_cluster50, 1);  // second cluster was actually read
}
