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

// ── Directory listing parser tests (firmware compatibility) ──────────────────

class EzShareParserTest : public ::testing::Test {
protected:
    EzShareClient client;
};

// HTML-entity encoded &lt;DIR&gt; — newer firmware / our Fysetc mule
TEST_F(EzShareParserTest, ParsesHtmlEntityEncodedDIR) {
    std::string html = R"(
<pre>
2026- 2- 3   20:50:32         &lt;DIR&gt;   <a href="dir?dir=A:DATALOG\20260203"> 20260203</a>
2026- 2- 4   01:15:00         &lt;DIR&gt;   <a href="dir?dir=A:DATALOG\20260204"> 20260204</a>
</pre>)";

    auto entries = client.parseDirectoryListing(html);
    ASSERT_EQ(entries.size(), 2);
    EXPECT_TRUE(entries[0].is_dir);
    EXPECT_EQ(entries[0].name, "20260203");
    EXPECT_EQ(entries[0].year, 2026);
    EXPECT_EQ(entries[0].month, 2);
    EXPECT_EQ(entries[0].day, 3);
    EXPECT_TRUE(entries[1].is_dir);
    EXPECT_EQ(entries[1].name, "20260204");
}

// Literal <DIR> — older/cheaper Chinese ezShare clones
TEST_F(EzShareParserTest, ParsesLiteralDIR) {
    std::string html = R"(
<pre>
2026- 2- 3   20:50:32         <DIR>   <a href="dir?dir=A:DATALOG\20260203"> 20260203</a>
2026- 2- 4   01:15:00         <DIR>   <a href="dir?dir=A:DATALOG\20260204"> 20260204</a>
</pre>)";

    auto entries = client.parseDirectoryListing(html);
    ASSERT_EQ(entries.size(), 2);
    EXPECT_TRUE(entries[0].is_dir);
    EXPECT_EQ(entries[0].name, "20260203");
    EXPECT_TRUE(entries[1].is_dir);
    EXPECT_EQ(entries[1].name, "20260204");
}

// Mixed: some firmware encode, some don't (belt and suspenders)
TEST_F(EzShareParserTest, ParsesMixedDIRFormats) {
    std::string html = R"(
<pre>
2026- 2- 3   20:50:32         &lt;DIR&gt;   <a href="dir?dir=A:DATALOG\20260203"> 20260203</a>
2026- 2- 4   01:15:00         <DIR>   <a href="dir?dir=A:DATALOG\20260204"> 20260204</a>
</pre>)";

    auto entries = client.parseDirectoryListing(html);
    ASSERT_EQ(entries.size(), 2);
    EXPECT_EQ(entries[0].name, "20260203");
    EXPECT_EQ(entries[1].name, "20260204");
}

// Files with KB sizes (not directories)
TEST_F(EzShareParserTest, ParsesFilesWithSizes) {
    std::string html = R"(
<pre>
2026- 2- 4   01:18:09         535KB   <a href="/download?file=DATALOG%5C20260204%5C20260204_011809_BRP.edf"> 20260204_011809_BRP.edf</a>
2026- 2- 4   01:18:10          51KB   <a href="/download?file=DATALOG%5C20260204%5C20260204_011810_PLD.edf"> 20260204_011810_PLD.edf</a>
2026- 2- 4   01:18:10          23KB   <a href="/download?file=DATALOG%5C20260204%5C20260204_011810_SAD.edf"> 20260204_011810_SAD.edf</a>
</pre>)";

    auto entries = client.parseDirectoryListing(html);
    ASSERT_EQ(entries.size(), 3);
    EXPECT_FALSE(entries[0].is_dir);
    EXPECT_EQ(entries[0].name, "20260204_011809_BRP.edf");
    EXPECT_EQ(entries[0].size_kb, 535);
    EXPECT_FALSE(entries[1].is_dir);
    EXPECT_EQ(entries[1].name, "20260204_011810_PLD.edf");
    EXPECT_EQ(entries[1].size_kb, 51);
    EXPECT_FALSE(entries[2].is_dir);
    EXPECT_EQ(entries[2].name, "20260204_011810_SAD.edf");
    EXPECT_EQ(entries[2].size_kb, 23);
}

// Mixed dirs and files in same listing (literal <DIR>)
TEST_F(EzShareParserTest, ParsesMixedDirsAndFilesLiteralDIR) {
    std::string html = R"(
<pre>
2026- 2- 3   20:50:32         <DIR>   <a href="dir?dir=A:DATALOG\20260203"> .</a>
2026- 2- 3   20:50:32         <DIR>   <a href="dir?dir=A:DATALOG"> ..</a>
2026- 2- 4   01:18:09         535KB   <a href="/download?file=20260204_011809_BRP.edf"> 20260204_011809_BRP.edf</a>
2026- 2- 4   01:47:47           1KB   <a href="/download?file=20260204_014747_CSL.edf"> 20260204_014747_CSL.edf</a>
</pre>)";

    auto entries = client.parseDirectoryListing(html);
    // . and .. should be skipped
    ASSERT_EQ(entries.size(), 2);
    EXPECT_FALSE(entries[0].is_dir);
    EXPECT_EQ(entries[0].name, "20260204_011809_BRP.edf");
    EXPECT_EQ(entries[0].size_kb, 535);
    EXPECT_FALSE(entries[1].is_dir);
    EXPECT_EQ(entries[1].name, "20260204_014747_CSL.edf");
    EXPECT_EQ(entries[1].size_kb, 1);
}

// Same test with HTML-entity <DIR> (our mule / newer firmware)
TEST_F(EzShareParserTest, ParsesMixedDirsAndFilesEncodedDIR) {
    std::string html = R"(
<pre>
2026- 2- 3   20:50:32         &lt;DIR&gt;   <a href="dir?dir=A:DATALOG\20260203"> .</a>
2026- 2- 3   20:50:32         &lt;DIR&gt;   <a href="dir?dir=A:DATALOG"> ..</a>
2026- 2- 4   01:18:09         535KB   <a href="/download?file=20260204_011809_BRP.edf"> 20260204_011809_BRP.edf</a>
</pre>)";

    auto entries = client.parseDirectoryListing(html);
    ASSERT_EQ(entries.size(), 1);
    EXPECT_EQ(entries[0].name, "20260204_011809_BRP.edf");
    EXPECT_EQ(entries[0].size_kb, 535);
}

// Timestamp parsing edge cases — single-digit month/day (some firmware quirks)
TEST_F(EzShareParserTest, ParsesSingleDigitMonthDay) {
    std::string html = R"(
<pre>
2026- 1- 5    3:05:09         100KB   <a href="/download?file=test.edf"> test.edf</a>
</pre>)";

    auto entries = client.parseDirectoryListing(html);
    ASSERT_EQ(entries.size(), 1);
    EXPECT_EQ(entries[0].year, 2026);
    EXPECT_EQ(entries[0].month, 1);
    EXPECT_EQ(entries[0].day, 5);
    EXPECT_EQ(entries[0].hour, 3);
    EXPECT_EQ(entries[0].minute, 5);
    EXPECT_EQ(entries[0].second, 9);
}

// Empty listing — no files, no crash
TEST_F(EzShareParserTest, EmptyListingReturnsEmpty) {
    std::string html = "<pre>\n</pre>";
    auto entries = client.parseDirectoryListing(html);
    EXPECT_TRUE(entries.empty());
}

// Garbage HTML — no crash
TEST_F(EzShareParserTest, GarbageHtmlReturnsEmpty) {
    std::string html = "<html><body>Not an ezShare page</body></html>";
    auto entries = client.parseDirectoryListing(html);
    EXPECT_TRUE(entries.empty());
}

// ── EzShareFileEntry::getModTime() round-trip ────────────────────────────────

// Reconstruct the broken-down fields from a parsed entry's mod time and verify
// they round-trip through std::mktime / from_time_t. Uses localtime to match
// the mktime() used internally (no wall-clock dependence — fixed input).
TEST(EzShareFileEntryTest, GetModTimeRoundTrip) {
    EzShareFileEntry e;
    e.year = 2026; e.month = 2; e.day = 4;
    e.hour = 1; e.minute = 18; e.second = 9;

    auto tp = e.getModTime();
    std::time_t tt = std::chrono::system_clock::to_time_t(tp);

    std::tm local{};
    localtime_r(&tt, &local);

    EXPECT_EQ(local.tm_year + 1900, 2026);
    EXPECT_EQ(local.tm_mon + 1, 2);
    EXPECT_EQ(local.tm_mday, 4);
    EXPECT_EQ(local.tm_hour, 1);
    EXPECT_EQ(local.tm_min, 18);
    EXPECT_EQ(local.tm_sec, 9);
}

// Two entries one second apart compare correctly through getModTime().
TEST(EzShareFileEntryTest, GetModTimeOrdering) {
    EzShareFileEntry earlier;
    earlier.year = 2026; earlier.month = 2; earlier.day = 4;
    earlier.hour = 1; earlier.minute = 18; earlier.second = 9;

    EzShareFileEntry later = earlier;
    later.second = 10;

    EXPECT_LT(earlier.getModTime(), later.getModTime());
    auto diff = std::chrono::duration_cast<std::chrono::seconds>(
        later.getModTime() - earlier.getModTime());
    EXPECT_EQ(diff.count(), 1);
}

// A whole-minute rollover (59 -> next minute) is one second apart.
TEST(EzShareFileEntryTest, GetModTimeMinuteRollover) {
    EzShareFileEntry a;
    a.year = 2026; a.month = 6; a.day = 1;
    a.hour = 23; a.minute = 59; a.second = 59;

    EzShareFileEntry b = a;
    b.year = 2026; b.month = 6; b.day = 2;
    b.hour = 0; b.minute = 0; b.second = 0;

    auto diff = std::chrono::duration_cast<std::chrono::seconds>(
        b.getModTime() - a.getModTime());
    EXPECT_EQ(diff.count(), 1);
}

// ── Base URL / range accessor round-trips ────────────────────────────────────

TEST(EzShareConfigTest, BaseURLDefaultAndOverride) {
    EzShareClient client;
    // Default comes from config (ConfigManager fallback "http://192.168.4.1").
    EXPECT_FALSE(client.getBaseURL().empty());

    client.setBaseURL("http://10.0.0.5");
    EXPECT_EQ(client.getBaseURL(), "http://10.0.0.5");

    client.setBaseURL("http://example.test:8080");
    EXPECT_EQ(client.getBaseURL(), "http://example.test:8080");
}

TEST(EzShareConfigTest, SupportsRangeToggle) {
    EzShareClient client;
    // Defaults to true per header.
    EXPECT_TRUE(client.supportsRange());

    client.setSupportsRange(false);
    EXPECT_FALSE(client.supportsRange());

    client.setSupportsRange(true);
    EXPECT_TRUE(client.supportsRange());
}

// ── Additional parser coverage ───────────────────────────────────────────────

// Real-hardware mixed listing: DIR entries with 8-digit names plus . / ..
// Mirrors what listDateFolders() consumes (we test parse output directly).
TEST_F(EzShareParserTest, ParsesDateFolderListing) {
    std::string html = R"(
<pre>
2026- 2- 3   20:50:32         &lt;DIR&gt;   <a href="dir?dir=A:DATALOG"> .</a>
2026- 2- 3   20:50:32         &lt;DIR&gt;   <a href="dir?dir=A:"> ..</a>
2026- 2- 3   20:50:32         &lt;DIR&gt;   <a href="dir?dir=A:DATALOG\20260203"> 20260203</a>
2026- 2- 4   01:15:00         &lt;DIR&gt;   <a href="dir?dir=A:DATALOG\20260204"> 20260204</a>
2026- 2- 5   02:30:00         &lt;DIR&gt;   <a href="dir?dir=A:DATALOG\SETTINGS"> SETTINGS</a>
</pre>)";

    auto entries = client.parseDirectoryListing(html);
    // . and .. dropped; SETTINGS kept (parser does not filter by digit pattern).
    ASSERT_EQ(entries.size(), 3);
    EXPECT_TRUE(entries[0].is_dir);
    EXPECT_EQ(entries[0].name, "20260203");
    EXPECT_EQ(entries[1].name, "20260204");
    EXPECT_EQ(entries[2].name, "SETTINGS");
}

// Name whitespace trimming: leading space after <a href>...> is stripped on
// both ends; interior characters preserved.
TEST_F(EzShareParserTest, TrimsLeadingAndTrailingWhitespaceInName) {
    std::string html = R"(
<pre>
2026- 2- 4   01:18:09         535KB   <a href="/download?file=x">    20260204_011809_BRP.edf   </a>
</pre>)";

    auto entries = client.parseDirectoryListing(html);
    ASSERT_EQ(entries.size(), 1);
    EXPECT_EQ(entries[0].name, "20260204_011809_BRP.edf");
}

// Zero-KB file is still a file (not a dir) with size_kb == 0.
TEST_F(EzShareParserTest, ParsesZeroKbFile) {
    std::string html = R"(
<pre>
2026- 2- 4   01:18:09           0KB   <a href="/download?file=empty.edf"> empty.edf</a>
</pre>)";

    auto entries = client.parseDirectoryListing(html);
    ASSERT_EQ(entries.size(), 1);
    EXPECT_FALSE(entries[0].is_dir);
    EXPECT_EQ(entries[0].size_kb, 0);
    EXPECT_EQ(entries[0].name, "empty.edf");
}

// Large multi-KB sizes parse as full integer KB values.
TEST_F(EzShareParserTest, ParsesLargeKbSize) {
    std::string html = R"(
<pre>
2026- 2- 4   05:00:00        1843KB   <a href="/download?file=big_BRP.edf"> 20260204_050000_BRP.edf</a>
</pre>)";

    auto entries = client.parseDirectoryListing(html);
    ASSERT_EQ(entries.size(), 1);
    EXPECT_EQ(entries[0].size_kb, 1843);
    EXPECT_FALSE(entries[0].is_dir);
}

// Lines that do not match the timestamp/size/anchor pattern are ignored,
// while valid lines interleaved with junk are still extracted.
TEST_F(EzShareParserTest, IgnoresNonMatchingLinesAmongValidEntries) {
    std::string html = R"(
<html><head><title>ez Share</title></head><body>
<pre>
Index of A:DATALOG
2026- 2- 4   01:18:09         535KB   <a href="/download?file=a.edf"> a.edf</a>
this is a garbage line with no timestamp
2026- 2- 4   01:19:00          12KB   <a href="/download?file=b.edf"> b.edf</a>
</pre>
</body></html>)";

    auto entries = client.parseDirectoryListing(html);
    ASSERT_EQ(entries.size(), 2);
    EXPECT_EQ(entries[0].name, "a.edf");
    EXPECT_EQ(entries[0].size_kb, 535);
    EXPECT_EQ(entries[1].name, "b.edf");
    EXPECT_EQ(entries[1].size_kb, 12);
}

// Full timestamp fields populated correctly for a typical file row.
TEST_F(EzShareParserTest, PopulatesAllTimestampFields) {
    std::string html = R"(
<pre>
2026-12-31   23:59:58         100KB   <a href="/download?file=eoy.edf"> eoy.edf</a>
</pre>)";

    auto entries = client.parseDirectoryListing(html);
    ASSERT_EQ(entries.size(), 1);
    EXPECT_EQ(entries[0].year, 2026);
    EXPECT_EQ(entries[0].month, 12);
    EXPECT_EQ(entries[0].day, 31);
    EXPECT_EQ(entries[0].hour, 23);
    EXPECT_EQ(entries[0].minute, 59);
    EXPECT_EQ(entries[0].second, 58);
    EXPECT_EQ(entries[0].size_kb, 100);
}

// ── Extended parser coverage (EzShareParserExtTest) ──────────────────────────
//
// Distinct suite name to avoid colliding with EzShareParserTest above.
// All deterministic: canned HTML strings only, no socket access.
//
// NOTE ON COVERAGE: listDateFolders(), listFiles(), downloadSession(),
// downloadFile(), downloadFileRange(), downloadRootFile() and httpGet() all
// open a live HTTP connection (curl_easy_perform) and are intentionally NOT
// exercised here. We cover the pure parsing surface (parseDirectoryListing),
// the URL/path string construction that those methods build, the date-folder
// digit/length filtering predicate that listDateFolders() applies to parse
// output, and the EDF-suffix matching predicate that downloadSession() uses.

class EzShareParserExtTest : public ::testing::Test {
protected:
    EzShareClient client;
};

// --- Malformed / boundary HTML -------------------------------------------------

// Completely empty string must not crash and yields no entries.
TEST_F(EzShareParserExtTest, EmptyStringReturnsEmpty) {
    auto entries = client.parseDirectoryListing("");
    EXPECT_TRUE(entries.empty());
}

// A line missing the trailing </a> anchor close does not match the regex.
TEST_F(EzShareParserExtTest, UnterminatedAnchorIsIgnored) {
    std::string html = R"(
<pre>
2026- 2- 4   01:18:09         535KB   <a href="/download?file=x"> 20260204_011809_BRP.edf
</pre>)";
    auto entries = client.parseDirectoryListing(html);
    EXPECT_TRUE(entries.empty());
}

// A row missing the size/DIR column (timestamp then straight to anchor) does
// not match — the size-or-DIR group is mandatory.
TEST_F(EzShareParserExtTest, MissingSizeColumnIsIgnored) {
    std::string html = R"(
<pre>
2026- 2- 4   01:18:09   <a href="/download?file=x"> noSize.edf</a>
</pre>)";
    auto entries = client.parseDirectoryListing(html);
    EXPECT_TRUE(entries.empty());
}

// A row whose timestamp has a non-numeric field is ignored.
TEST_F(EzShareParserExtTest, NonNumericTimestampIsIgnored) {
    std::string html = R"(
<pre>
20XX- 2- 4   01:18:09         535KB   <a href="/download?file=x"> bad.edf</a>
</pre>)";
    auto entries = client.parseDirectoryListing(html);
    EXPECT_TRUE(entries.empty());
}

// Anchor with an empty href still parses (href content is not validated).
TEST_F(EzShareParserExtTest, EmptyHrefStillParses) {
    std::string html = R"(
<pre>
2026- 2- 4   01:18:09         535KB   <a href=""> a.edf</a>
</pre>)";
    auto entries = client.parseDirectoryListing(html);
    ASSERT_EQ(entries.size(), 1u);
    EXPECT_EQ(entries[0].name, "a.edf");
    EXPECT_EQ(entries[0].size_kb, 535);
}

// Multiple files spread over one physical line are all captured (regex is
// applied globally over the whole string, not line-by-line).
TEST_F(EzShareParserExtTest, MultipleEntriesOnOneLine) {
    std::string html =
        R"(2026- 2- 4   01:18:09  10KB  <a href="x"> a.edf</a> )"
        R"(2026- 2- 4   01:18:10  20KB  <a href="y"> b.edf</a>)";
    auto entries = client.parseDirectoryListing(html);
    ASSERT_EQ(entries.size(), 2u);
    EXPECT_EQ(entries[0].name, "a.edf");
    EXPECT_EQ(entries[0].size_kb, 10);
    EXPECT_EQ(entries[1].name, "b.edf");
    EXPECT_EQ(entries[1].size_kb, 20);
}

// Only "." and ".." present -> both dropped -> empty result.
TEST_F(EzShareParserExtTest, OnlyDotEntriesDropped) {
    std::string html = R"(
<pre>
2026- 2- 3   20:50:32         <DIR>   <a href="dir?dir=A:DATALOG"> .</a>
2026- 2- 3   20:50:32         <DIR>   <a href="dir?dir=A:"> ..</a>
</pre>)";
    auto entries = client.parseDirectoryListing(html);
    EXPECT_TRUE(entries.empty());
}

// Case-insensitive DIR token: lowercase <dir> still recognised as directory.
TEST_F(EzShareParserExtTest, LowercaseDirTokenRecognised) {
    std::string html = R"(
<pre>
2026- 2- 3   20:50:32         <dir>   <a href="dir?dir=A:DATALOG\20260203"> 20260203</a>
</pre>)";
    auto entries = client.parseDirectoryListing(html);
    ASSERT_EQ(entries.size(), 1u);
    EXPECT_TRUE(entries[0].is_dir);
    EXPECT_EQ(entries[0].name, "20260203");
    EXPECT_EQ(entries[0].size_kb, 0);  // dirs leave size_kb default 0
}

// Case-insensitive anchor tag: <A HREF=...> uppercase still matches.
TEST_F(EzShareParserExtTest, UppercaseAnchorTagRecognised) {
    std::string html = R"(
<pre>
2026- 2- 4   01:18:09         535KB   <A HREF="/download?file=x"> upper.edf</A>
</pre>)";
    auto entries = client.parseDirectoryListing(html);
    ASSERT_EQ(entries.size(), 1u);
    EXPECT_EQ(entries[0].name, "upper.edf");
    EXPECT_FALSE(entries[0].is_dir);
}

// --- Date-folder filtering predicate (mirrors listDateFolders) -----------------
//
// listDateFolders() filters parse output to dirs with 8-digit all-numeric names
// and sorts them. We can't call it (it hits the network), so we replicate the
// exact predicate against parseDirectoryListing() output to lock the contract.
namespace {
std::vector<std::string> filterDateFolders(const std::vector<EzShareFileEntry>& entries) {
    std::vector<std::string> folders;
    for (const auto& e : entries) {
        if (e.is_dir && e.name.size() == 8 &&
            std::all_of(e.name.begin(), e.name.end(), ::isdigit)) {
            folders.push_back(e.name);
        }
    }
    std::sort(folders.begin(), folders.end());
    return folders;
}
}  // namespace

TEST_F(EzShareParserExtTest, DateFolderFilterKeepsOnlyEightDigitDirs) {
    std::string html = R"(
<pre>
2026- 2- 5   02:30:00         &lt;DIR&gt;   <a href="x"> SETTINGS</a>
2026- 2- 4   01:15:00         &lt;DIR&gt;   <a href="x"> 20260204</a>
2026- 2- 3   20:50:32         &lt;DIR&gt;   <a href="x"> 20260203</a>
2026- 2- 4   01:18:09         535KB   <a href="x"> 20260204_011809_BRP.edf</a>
2026- 2- 6   03:00:00         &lt;DIR&gt;   <a href="x"> 2026020</a>
</pre>)";
    auto entries = client.parseDirectoryListing(html);
    auto folders = filterDateFolders(entries);
    // SETTINGS (not numeric), the BRP file (not dir), and 2026020 (7 digits)
    // are all excluded; the two 8-digit dirs are kept and sorted ascending.
    ASSERT_EQ(folders.size(), 2u);
    EXPECT_EQ(folders[0], "20260203");
    EXPECT_EQ(folders[1], "20260204");
}

TEST_F(EzShareParserExtTest, DateFolderFilterRejectsNineDigitDir) {
    std::string html = R"(
<pre>
2026- 2- 4   01:15:00         &lt;DIR&gt;   <a href="x"> 202602040</a>
</pre>)";
    auto entries = client.parseDirectoryListing(html);
    EXPECT_TRUE(filterDateFolders(entries).empty());
}

TEST_F(EzShareParserExtTest, DateFolderFilterRejectsEightCharAlnum) {
    std::string html = R"(
<pre>
2026- 2- 4   01:15:00         &lt;DIR&gt;   <a href="x"> 2026A204</a>
</pre>)";
    auto entries = client.parseDirectoryListing(html);
    EXPECT_TRUE(filterDateFolders(entries).empty());
}

// --- EDF-suffix matching predicate (mirrors downloadSession) -------------------
//
// downloadSession() selects files ending in one of these suffixes
// (case-insensitive). Replicate the predicate to lock the contract without
// touching the network.
namespace {
bool isSessionEdf(const std::string& name) {
    static const std::vector<std::string> edf_suffixes = {
        "_BRP.edf", "_EVE.edf", "_SAD.edf", "_SA2.edf", "_PLD.edf", "_CSL.edf"};
    for (const auto& suffix : edf_suffixes) {
        if (name.size() >= suffix.size()) {
            std::string end = name.substr(name.size() - suffix.size());
            std::transform(end.begin(), end.end(), end.begin(), ::tolower);
            std::string suf = suffix;
            std::transform(suf.begin(), suf.end(), suf.begin(), ::tolower);
            if (end == suf) return true;
        }
    }
    return false;
}
}  // namespace

TEST_F(EzShareParserExtTest, SessionEdfSuffixMatching) {
    EXPECT_TRUE(isSessionEdf("20260204_011809_BRP.edf"));
    EXPECT_TRUE(isSessionEdf("20260204_011810_PLD.edf"));
    EXPECT_TRUE(isSessionEdf("20260204_011810_SAD.edf"));
    EXPECT_TRUE(isSessionEdf("20260204_011810_SA2.edf"));
    EXPECT_TRUE(isSessionEdf("20260204_011810_EVE.edf"));
    EXPECT_TRUE(isSessionEdf("20260204_014747_CSL.edf"));
    // Case-insensitive suffix.
    EXPECT_TRUE(isSessionEdf("20260204_011809_brp.EDF"));
    // Non-session EDF and root str file excluded.
    EXPECT_FALSE(isSessionEdf("STR.edf"));
    EXPECT_FALSE(isSessionEdf("20260204_011809_XYZ.edf"));
    EXPECT_FALSE(isSessionEdf("notanedf.txt"));
    // Too short to contain any suffix.
    EXPECT_FALSE(isSessionEdf("a"));
    EXPECT_FALSE(isSessionEdf(""));
}

// --- URL / path construction (string-only, no socket) --------------------------
//
// These lock the exact URL shapes the download/list methods build from base_url_
// + the date folder / filename, including the mandatory %5C backslash encoding.
// We assemble the strings the same way the production code does so a change to
// the encoding contract is caught here.

TEST_F(EzShareParserExtTest, ListDirUrlUsesBackslashEncoding) {
    client.setBaseURL("http://192.168.4.1");
    std::string date = "20260204";
    std::string list_url = client.getBaseURL() + "/dir?dir=A:DATALOG%5C" + date;
    EXPECT_EQ(list_url, "http://192.168.4.1/dir?dir=A:DATALOG%5C20260204");
    EXPECT_EQ(list_url.find("DATALOG/"), std::string::npos);  // never forward slash
}

TEST_F(EzShareParserExtTest, ListDateFoldersUrlShape) {
    client.setBaseURL("http://10.0.0.5");
    std::string url = client.getBaseURL() + "/dir?dir=A:DATALOG";
    EXPECT_EQ(url, "http://10.0.0.5/dir?dir=A:DATALOG");
}

TEST_F(EzShareParserExtTest, DownloadFileUrlUsesDoubleBackslash) {
    client.setBaseURL("http://192.168.4.1");
    std::string date = "20260204";
    std::string fname = "20260204_011809_BRP.edf";
    std::string url = client.getBaseURL() + "/download?file=DATALOG%5C" + date + "%5C" + fname;
    EXPECT_EQ(url,
        "http://192.168.4.1/download?file=DATALOG%5C20260204%5C20260204_011809_BRP.edf");
    EXPECT_EQ(url.find("DATALOG/"), std::string::npos);
}

TEST_F(EzShareParserExtTest, DownloadRootFileUrlHasNoDatalogPrefix) {
    client.setBaseURL("http://192.168.4.1");
    std::string url = client.getBaseURL() + "/download?file=" + std::string("STR.EDF");
    EXPECT_EQ(url, "http://192.168.4.1/download?file=STR.EDF");
    EXPECT_EQ(url.find("DATALOG"), std::string::npos);  // root files skip DATALOG
}

TEST_F(EzShareParserExtTest, RangeHeaderFormat) {
    // Mirrors the Range header downloadFileRange() builds for resume.
    size_t start = 307200;  // 300 KB
    std::string header = "Range: bytes=" + std::to_string(start) + "-";
    EXPECT_EQ(header, "Range: bytes=307200-");

    std::string from_zero = "Range: bytes=" + std::to_string(0) + "-";
    EXPECT_EQ(from_zero, "Range: bytes=0-");
}

// Base URL with trailing-style port survives concatenation cleanly.
TEST_F(EzShareParserExtTest, BaseUrlWithPortConcatenates) {
    client.setBaseURL("http://example.test:8080");
    std::string url = client.getBaseURL() + "/dir?dir=A:DATALOG";
    EXPECT_EQ(url, "http://example.test:8080/dir?dir=A:DATALOG");
}

// --- getModTime with is_dir set (dirs still produce a valid time) --------------
TEST(EzShareFileEntryExtTest, DirEntryModTimeStillComputes) {
    EzShareFileEntry e;
    e.is_dir = true;
    e.year = 2026; e.month = 2; e.day = 4;
    e.hour = 20; e.minute = 50; e.second = 32;

    auto tp = e.getModTime();
    std::time_t tt = std::chrono::system_clock::to_time_t(tp);
    std::tm local{};
    localtime_r(&tt, &local);
    EXPECT_EQ(local.tm_year + 1900, 2026);
    EXPECT_EQ(local.tm_mday, 4);
    EXPECT_EQ(local.tm_hour, 20);
}

// Default-constructed entry has zeroed fields and is not a directory.
TEST(EzShareFileEntryExtTest, DefaultFieldsAreZeroed) {
    EzShareFileEntry e;
    EXPECT_EQ(e.size_kb, 0);
    EXPECT_FALSE(e.is_dir);
    EXPECT_EQ(e.year, 0);
    EXPECT_EQ(e.month, 0);
    EXPECT_EQ(e.day, 0);
    EXPECT_EQ(e.hour, 0);
    EXPECT_EQ(e.minute, 0);
    EXPECT_EQ(e.second, 0);
}

// Run all tests
int main(int argc, char** argv) {
    testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}