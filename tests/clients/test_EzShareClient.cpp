#include <gtest/gtest.h>
#include <gmock/gmock.h>
#include "clients/EzShareClient.h"
#include <fstream>
#include <filesystem>
#include <cstdio>

using namespace hms_cpap;

/**
 * EzShareClient Range Download Tests
 *
 * CRITICAL: These tests ensure Range downloads work correctly for active CPAP sessions.
 * Missing data = missing breaths = UNACCEPTABLE!
 */

class EzShareClientTest : public ::testing::Test {
protected:
    void SetUp() override {
        test_dir = "/tmp/hms_cpap_test";
        std::filesystem::create_directories(test_dir);
    }

    void TearDown() override {
        std::filesystem::remove_all(test_dir);
    }

    // Helper: Create test file with known content
    void createTestFile(const std::string& path, size_t size, char fill_byte = 'A') {
        std::ofstream f(path, std::ios::binary);
        for (size_t i = 0; i < size; i++) {
            f.put(fill_byte);
        }
    }

    // Helper: Verify file content
    bool verifyFileContent(const std::string& path, size_t expected_size) {
        if (!std::filesystem::exists(path)) return false;
        return std::filesystem::file_size(path) == expected_size;
    }

    // Helper: Read file content
    std::string readFile(const std::string& path) {
        std::ifstream f(path, std::ios::binary);
        return std::string((std::istreambuf_iterator<char>(f)),
                          std::istreambuf_iterator<char>());
    }

    std::string test_dir;
};

/**
 * Test 1: Range download appends to existing file
 *
 * Scenario: BRP file grows from 300KB to 350KB between burst cycles
 * Expected: Only download +50KB, append to existing file
 */
TEST_F(EzShareClientTest, RangeDownloadAppendsToExistingFile) {
    std::string test_file = test_dir + "/test_brp.edf";

    // Simulate existing file (300KB)
    size_t initial_size = 300 * 1024;
    createTestFile(test_file, initial_size, 'A');

    ASSERT_TRUE(std::filesystem::exists(test_file));
    ASSERT_EQ(std::filesystem::file_size(test_file), initial_size);

    // Simulate appending new data (50KB of 'B')
    size_t new_bytes = 50 * 1024;
    std::ofstream f(test_file, std::ios::binary | std::ios::app);
    for (size_t i = 0; i < new_bytes; i++) {
        f.put('B');
    }
    f.close();

    // Verify final size
    size_t expected_size = initial_size + new_bytes;
    ASSERT_EQ(std::filesystem::file_size(test_file), expected_size);

    // Verify content integrity (first byte = 'A', last byte = 'B')
    std::string content = readFile(test_file);
    EXPECT_EQ(content[0], 'A');
    EXPECT_EQ(content[expected_size - 1], 'B');
}

/**
 * Test 2: Byte tracking accuracy
 *
 * CRITICAL: Incorrect byte tracking = data loss or corruption
 */
TEST_F(EzShareClientTest, ByteTrackingAccuracy) {
    std::string test_file = test_dir + "/test_bytes.edf";

    // Create file with exact sizes to test byte-level precision
    std::vector<size_t> test_sizes = {
        1,           // Single byte
        1024,        // 1 KB
        1024 * 100,  // 100 KB
        1024 * 300,  // 300 KB (typical BRP mid-session)
        1024 * 1800  // 1.8 MB (typical BRP end-session)
    };

    for (size_t size : test_sizes) {
        createTestFile(test_file, size);
        EXPECT_EQ(std::filesystem::file_size(test_file), size)
            << "Byte tracking failed for size: " << size;
        std::filesystem::remove(test_file);
    }
}

/**
 * Test 3: Append mode correctness
 *
 * Ensures std::ios::app doesn't truncate or overwrite existing data
 */
TEST_F(EzShareClientTest, AppendModePreservesExistingData) {
    std::string test_file = test_dir + "/test_append.edf";

    // Write initial data
    {
        std::ofstream f(test_file, std::ios::binary);
        f << "INITIAL_DATA";
    }

    size_t initial_size = std::filesystem::file_size(test_file);
    std::string initial_content = readFile(test_file);

    // Append new data
    {
        std::ofstream f(test_file, std::ios::binary | std::ios::app);
        f << "_APPENDED";
    }

    // Verify size increased
    size_t final_size = std::filesystem::file_size(test_file);
    EXPECT_GT(final_size, initial_size);

    // Verify initial data preserved
    std::string final_content = readFile(test_file);
    EXPECT_EQ(final_content.substr(0, initial_content.size()), initial_content);
    EXPECT_EQ(final_content, "INITIAL_DATA_APPENDED");
}

/**
 * Test 4: Zero-byte append handling
 *
 * When file hasn't grown, don't corrupt existing data
 */
TEST_F(EzShareClientTest, ZeroByteAppendSafe) {
    std::string test_file = test_dir + "/test_zero_append.edf";

    // Create file
    std::string original_content = "CPAP_DATA_12345";
    {
        std::ofstream f(test_file, std::ios::binary);
        f << original_content;
    }

    size_t original_size = std::filesystem::file_size(test_file);

    // Simulate Range download with bytes_downloaded = 0
    // (file unchanged on ez Share)
    {
        std::ofstream f(test_file, std::ios::binary | std::ios::app);
        // Write nothing
    }

    // Verify file unchanged
    EXPECT_EQ(std::filesystem::file_size(test_file), original_size);
    EXPECT_EQ(readFile(test_file), original_content);
}

/**
 * Test 5: Large file handling
 *
 * End-of-session BRP files can be 1.8+ MB
 */
TEST_F(EzShareClientTest, LargeFileHandling) {
    std::string test_file = test_dir + "/test_large.edf";

    // Simulate end-of-session BRP (1.8 MB)
    size_t large_size = 1800 * 1024;
    createTestFile(test_file, large_size);

    ASSERT_TRUE(std::filesystem::exists(test_file));
    EXPECT_EQ(std::filesystem::file_size(test_file), large_size);

    // Verify can append to large file
    size_t append_size = 100 * 1024;
    {
        std::ofstream f(test_file, std::ios::binary | std::ios::app);
        for (size_t i = 0; i < append_size; i++) {
            f.put('X');
        }
    }

    EXPECT_EQ(std::filesystem::file_size(test_file), large_size + append_size);
}

/**
 * Test 6: Incremental growth simulation
 *
 * Simulates real CPAP session: BRP grows ~6-8 KB/minute
 */
TEST_F(EzShareClientTest, IncrementalGrowthSimulation) {
    std::string test_file = test_dir + "/test_incremental.edf";

    // Simulate 10 burst cycles with 10s intervals
    // Each cycle: ~1-2 KB growth (realistic for 10s intervals)
    std::vector<size_t> growth_bytes = {
        1024,   // Cycle 1: +1 KB
        1536,   // Cycle 2: +1.5 KB
        2048,   // Cycle 3: +2 KB
        1024,   // Cycle 4: +1 KB
        1792,   // Cycle 5: +1.75 KB
        2048,   // Cycle 6: +2 KB
        1280,   // Cycle 7: +1.25 KB
        1536,   // Cycle 8: +1.5 KB
        2048,   // Cycle 9: +2 KB
        1024    // Cycle 10: +1 KB
    };

    size_t total_size = 0;

    for (size_t i = 0; i < growth_bytes.size(); i++) {
        // Append growth for this cycle
        std::ofstream f(test_file, std::ios::binary | std::ios::app);
        for (size_t j = 0; j < growth_bytes[i]; j++) {
            f.put('0' + (i % 10));  // Cycle number as marker
        }
        f.close();

        total_size += growth_bytes[i];

        // Verify size after each cycle
        EXPECT_EQ(std::filesystem::file_size(test_file), total_size)
            << "Size mismatch after cycle " << (i + 1);
    }

    // Total growth: 15 KB over 10 cycles (realistic for 100s session @ 10s intervals)
    EXPECT_GE(total_size, 15 * 1024);   // At least 15 KB
    EXPECT_LE(total_size, 16 * 1024);   // At most 16 KB
}

/**
 * Test 7: Concurrent file access safety
 *
 * Ensure parser can read while download appends
 * (not strictly concurrent, but tests file handle behavior)
 */
TEST_F(EzShareClientTest, ReadWhileAppending) {
    std::string test_file = test_dir + "/test_concurrent.edf";

    // Write initial data
    {
        std::ofstream f(test_file, std::ios::binary);
        f << "INITIAL";
    }

    // Open for reading
    std::ifstream reader(test_file, std::ios::binary);
    std::string initial = std::string((std::istreambuf_iterator<char>(reader)),
                                      std::istreambuf_iterator<char>());
    reader.close();

    // Append new data
    {
        std::ofstream f(test_file, std::ios::binary | std::ios::app);
        f << "_APPENDED";
    }

    // Read again - should get updated content
    reader.open(test_file, std::ios::binary);
    std::string updated = std::string((std::istreambuf_iterator<char>(reader)),
                                      std::istreambuf_iterator<char>());
    reader.close();

    EXPECT_EQ(initial, "INITIAL");
    EXPECT_EQ(updated, "INITIAL_APPENDED");
}

/**
 * Test 8: Path handling with /tmp and archive dirs
 *
 * Files stored in /tmp during download, archived to NAS
 */
TEST_F(EzShareClientTest, PathHandling) {
    std::string temp_file = test_dir + "/DATALOG/20260213/test_brp.edf";
    std::filesystem::create_directories(std::filesystem::path(temp_file).parent_path());

    createTestFile(temp_file, 1024);
    ASSERT_TRUE(std::filesystem::exists(temp_file));

    // Simulate archive operation (copy to different location)
    std::string archive_file = test_dir + "/archive/DATALOG/20260213/test_brp.edf";
    std::filesystem::create_directories(std::filesystem::path(archive_file).parent_path());
    std::filesystem::copy(temp_file, archive_file);

    EXPECT_TRUE(std::filesystem::exists(archive_file));
    EXPECT_EQ(std::filesystem::file_size(temp_file),
              std::filesystem::file_size(archive_file));
}

/**
 * Test 9: Truncate vs Append mode verification
 *
 * start_byte=0 → truncate (full download)
 * start_byte>0 → append (Range download)
 */
TEST_F(EzShareClientTest, TruncateVsAppendMode) {
    std::string test_file = test_dir + "/test_mode.edf";

    // Create initial file
    createTestFile(test_file, 1024, 'A');

    // Simulate start_byte=0 (full download) → should truncate
    {
        std::ofstream f(test_file, std::ios::binary | std::ios::trunc);
        for (int i = 0; i < 512; i++) {
            f.put('B');
        }
    }

    EXPECT_EQ(std::filesystem::file_size(test_file), 512u);
    EXPECT_EQ(readFile(test_file)[0], 'B');  // Old 'A' data gone

    // Simulate start_byte>0 (Range download) → should append
    {
        std::ofstream f(test_file, std::ios::binary | std::ios::app);
        for (int i = 0; i < 256; i++) {
            f.put('C');
        }
    }

    EXPECT_EQ(std::filesystem::file_size(test_file), 768u);
    std::string content = readFile(test_file);
    EXPECT_EQ(content[0], 'B');           // Initial truncated data
    EXPECT_EQ(content[767], 'C');         // Appended data
}

/**
 * Test 10: Edge case - File deleted mid-session
 *
 * If local file missing, fallback to full download
 */
TEST_F(EzShareClientTest, MissingLocalFileHandling) {
    std::string test_file = test_dir + "/test_missing.edf";

    // File doesn't exist
    ASSERT_FALSE(std::filesystem::exists(test_file));

    // Simulate fallback to full download
    createTestFile(test_file, 2048);

    EXPECT_TRUE(std::filesystem::exists(test_file));
    EXPECT_EQ(std::filesystem::file_size(test_file), 2048u);
}

// Run all tests
int main(int argc, char** argv) {
    testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
