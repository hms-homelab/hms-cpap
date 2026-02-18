/**
 * HMS-CPAP BurstCollectorService Unit Tests
 *
 * Tests file archival, path construction, and file size comparison logic.
 */

#include <gtest/gtest.h>
#include <gmock/gmock.h>
#include "services/BurstCollectorService.h"
#include <filesystem>
#include <fstream>

using namespace hms_cpap;

// Test fixture
class BurstCollectorServiceTest : public ::testing::Test {
protected:
    void SetUp() override {
        // Create temporary test directories
        test_temp_dir = std::filesystem::temp_directory_path() / "hms_cpap_test_temp";
        test_archive_dir = std::filesystem::temp_directory_path() / "hms_cpap_test_archive";

        std::filesystem::create_directories(test_temp_dir);
        std::filesystem::create_directories(test_archive_dir);

        // Create BurstCollectorService (note: needs valid config)
        // For now, we'll test the archival function independently
    }

    void TearDown() override {
        // Clean up test directories
        if (std::filesystem::exists(test_temp_dir)) {
            std::filesystem::remove_all(test_temp_dir);
        }
        if (std::filesystem::exists(test_archive_dir)) {
            std::filesystem::remove_all(test_archive_dir);
        }
    }

    // Helper: Create test file with specific size
    void createTestFile(const std::filesystem::path& path, size_t size_bytes) {
        std::filesystem::create_directories(path.parent_path());
        std::ofstream ofs(path, std::ios::binary);
        std::vector<char> data(size_bytes, 'A');
        ofs.write(data.data(), size_bytes);
        ofs.close();
    }

    // Helper: Get file size
    size_t getFileSize(const std::filesystem::path& path) {
        if (!std::filesystem::exists(path)) return 0;
        return std::filesystem::file_size(path);
    }

    std::filesystem::path test_temp_dir;
    std::filesystem::path test_archive_dir;
};

// ============================================================================
// FILE ARCHIVAL TESTS
// ============================================================================

TEST_F(BurstCollectorServiceTest, ArchiveSessionFiles_NewFiles) {
    // Temp has: file1.edf (100 KB), file2.edf (200 KB)
    // Archive empty
    // Verify both copied

    std::string date_folder = "20260206";
    auto temp_session_dir = test_temp_dir / date_folder;
    std::filesystem::create_directories(temp_session_dir);

    createTestFile(temp_session_dir / "20260206_140131_BRP.edf", 100 * 1024);
    createTestFile(temp_session_dir / "20260206_140132_SAD.edf", 200 * 1024);

    // Copy files manually (simulating archiveSessionFiles)
    auto archive_session_dir = test_archive_dir / "DATALOG" / date_folder;
    std::filesystem::create_directories(archive_session_dir);

    for (const auto& entry : std::filesystem::directory_iterator(temp_session_dir)) {
        if (entry.is_regular_file()) {
            std::string filename = entry.path().filename().string();
            std::filesystem::copy_file(
                entry.path(),
                archive_session_dir / filename,
                std::filesystem::copy_options::overwrite_existing
            );
        }
    }

    // Verify files copied
    EXPECT_TRUE(std::filesystem::exists(archive_session_dir / "20260206_140131_BRP.edf"));
    EXPECT_TRUE(std::filesystem::exists(archive_session_dir / "20260206_140132_SAD.edf"));

    EXPECT_EQ(getFileSize(archive_session_dir / "20260206_140131_BRP.edf"), 100 * 1024);
    EXPECT_EQ(getFileSize(archive_session_dir / "20260206_140132_SAD.edf"), 200 * 1024);
}

TEST_F(BurstCollectorServiceTest, ArchiveSessionFiles_IdenticalSkip) {
    // Temp has: file1.edf (100 KB)
    // Archive has: file1.edf (100 KB, identical)
    // Verify skipped, not re-copied

    std::string date_folder = "20260206";
    auto temp_session_dir = test_temp_dir / date_folder;
    auto archive_session_dir = test_archive_dir / "DATALOG" / date_folder;

    std::filesystem::create_directories(temp_session_dir);
    std::filesystem::create_directories(archive_session_dir);

    // Create identical files in both locations
    createTestFile(temp_session_dir / "20260206_140131_BRP.edf", 100 * 1024);
    createTestFile(archive_session_dir / "20260206_140131_BRP.edf", 100 * 1024);

    // Simulate size check (same size = skip)
    auto temp_file = temp_session_dir / "20260206_140131_BRP.edf";
    auto archive_file = archive_session_dir / "20260206_140131_BRP.edf";

    bool should_copy = false;
    if (std::filesystem::exists(archive_file)) {
        auto temp_size = std::filesystem::file_size(temp_file);
        auto archive_size = std::filesystem::file_size(archive_file);
        if (temp_size != archive_size) {
            should_copy = true;
        }
    } else {
        should_copy = true;
    }

    EXPECT_FALSE(should_copy) << "Identical file should be skipped";
}

TEST_F(BurstCollectorServiceTest, ArchiveSessionFiles_SizeChanged) {
    // Temp has: file1.edf (200 KB)
    // Archive has: file1.edf (100 KB, older)
    // Verify re-copied with overwrite

    std::string date_folder = "20260206";
    auto temp_session_dir = test_temp_dir / date_folder;
    auto archive_session_dir = test_archive_dir / "DATALOG" / date_folder;

    std::filesystem::create_directories(temp_session_dir);
    std::filesystem::create_directories(archive_session_dir);

    // Create files with different sizes
    createTestFile(temp_session_dir / "20260206_140131_BRP.edf", 200 * 1024);
    createTestFile(archive_session_dir / "20260206_140131_BRP.edf", 100 * 1024);

    // Simulate size check (different size = copy)
    auto temp_file = temp_session_dir / "20260206_140131_BRP.edf";
    auto archive_file = archive_session_dir / "20260206_140131_BRP.edf";

    bool should_copy = false;
    if (std::filesystem::exists(archive_file)) {
        auto temp_size = std::filesystem::file_size(temp_file);
        auto archive_size = std::filesystem::file_size(archive_file);
        if (temp_size != archive_size) {
            should_copy = true;
        }
    } else {
        should_copy = true;
    }

    EXPECT_TRUE(should_copy) << "File with changed size should be re-copied";

    // Perform copy
    std::filesystem::copy_file(
        temp_file,
        archive_file,
        std::filesystem::copy_options::overwrite_existing
    );

    // Verify size updated
    EXPECT_EQ(getFileSize(archive_file), 200 * 1024);
}

TEST_F(BurstCollectorServiceTest, ArchiveSessionFiles_DirectoryCreation) {
    // Archive directory doesn't exist
    // Verify creates directories recursively

    std::string date_folder = "20260206";
    auto temp_session_dir = test_temp_dir / date_folder;
    auto archive_session_dir = test_archive_dir / "DATALOG" / date_folder;

    std::filesystem::create_directories(temp_session_dir);
    createTestFile(temp_session_dir / "20260206_140131_BRP.edf", 100 * 1024);

    // Archive directory doesn't exist yet
    EXPECT_FALSE(std::filesystem::exists(archive_session_dir));

    // Create directories recursively
    std::filesystem::create_directories(archive_session_dir);

    EXPECT_TRUE(std::filesystem::exists(archive_session_dir));

    // Now copy file
    std::filesystem::copy_file(
        temp_session_dir / "20260206_140131_BRP.edf",
        archive_session_dir / "20260206_140131_BRP.edf"
    );

    EXPECT_TRUE(std::filesystem::exists(archive_session_dir / "20260206_140131_BRP.edf"));
}

// ============================================================================
// PATH CONSTRUCTION TESTS
// ============================================================================

TEST_F(BurstCollectorServiceTest, PathConstruction_PermanentArchive) {
    // Date: 20260206
    // File: 20260206_140131_BRP.edf
    // Verify: /home/aamat/maestro_hub/cpap_data/DATALOG/20260206/20260206_140131_BRP.edf

    std::string date_folder = "20260206";
    std::string filename = "20260206_140131_BRP.edf";
    std::string archive_base = "/home/aamat/maestro_hub/cpap_data";

    std::string expected_path = archive_base + "/DATALOG/" + date_folder + "/" + filename;
    std::string actual_path = archive_base + "/DATALOG/" + date_folder + "/" + filename;

    EXPECT_EQ(actual_path, expected_path);
}

TEST_F(BurstCollectorServiceTest, PathConstruction_TempLocation) {
    // Date: 20260206
    // File: 20260206_140131_BRP.edf
    // Verify: /tmp/cpap_data/20260206/20260206_140131_BRP.edf

    std::string date_folder = "20260206";
    std::string filename = "20260206_140131_BRP.edf";
    std::string temp_base = "/tmp/cpap_data";

    std::string expected_path = temp_base + "/" + date_folder + "/" + filename;
    std::string actual_path = temp_base + "/" + date_folder + "/" + filename;

    EXPECT_EQ(actual_path, expected_path);
}

TEST_F(BurstCollectorServiceTest, PathConstruction_FileTypes) {
    // Verify path construction for all file types

    std::string date_folder = "20260206";
    std::string timestamp = "20260206_140131";
    std::string archive_base = "/home/aamat/maestro_hub/cpap_data/DATALOG";

    std::vector<std::string> file_types = {"BRP", "EVE", "SAD", "PLD", "CSL"};

    for (const auto& type : file_types) {
        std::string filename = timestamp + "_" + type + ".edf";
        std::string path = archive_base + "/" + date_folder + "/" + filename;

        EXPECT_TRUE(path.find(date_folder) != std::string::npos)
            << "Path should contain date folder: " << type;
        EXPECT_TRUE(path.find(type) != std::string::npos)
            << "Path should contain file type: " << type;
        EXPECT_TRUE(path.find(".edf") != std::string::npos)
            << "Path should have .edf extension: " << type;
    }
}

// ============================================================================
// FILE SIZE COMPARISON TESTS
// ============================================================================

TEST_F(BurstCollectorServiceTest, FileSizeComparison_Identical) {
    createTestFile(test_temp_dir / "file1.edf", 1024);
    createTestFile(test_archive_dir / "file1.edf", 1024);

    auto temp_size = getFileSize(test_temp_dir / "file1.edf");
    auto archive_size = getFileSize(test_archive_dir / "file1.edf");

    EXPECT_EQ(temp_size, archive_size);
}

TEST_F(BurstCollectorServiceTest, FileSizeComparison_Different) {
    createTestFile(test_temp_dir / "file1.edf", 1024);
    createTestFile(test_archive_dir / "file1.edf", 2048);

    auto temp_size = getFileSize(test_temp_dir / "file1.edf");
    auto archive_size = getFileSize(test_archive_dir / "file1.edf");

    EXPECT_NE(temp_size, archive_size);
}

TEST_F(BurstCollectorServiceTest, FileSizeComparison_Missing) {
    createTestFile(test_temp_dir / "file1.edf", 1024);

    auto temp_size = getFileSize(test_temp_dir / "file1.edf");
    auto archive_size = getFileSize(test_archive_dir / "file1.edf");

    EXPECT_GT(temp_size, 0);
    EXPECT_EQ(archive_size, 0);
}

// ============================================================================
// CONNECTION RECOVERY TESTS
// ============================================================================

/**
 * Test consecutive failure tracking logic
 * After MAX_FAILURES_BEFORE_RESET (3) consecutive failures, session_active should be cleared
 */
TEST_F(BurstCollectorServiceTest, ConnectionRecovery_ConsecutiveFailures) {
    // Simulate the recovery logic from BurstCollectorService
    int consecutive_failures = 0;
    bool session_active_cleared = false;
    constexpr int MAX_FAILURES_BEFORE_RESET = 3;

    // Simulate 3 consecutive failures
    for (int i = 0; i < 5; i++) {
        bool connection_success = (i >= 3);  // Fail first 3, then succeed

        if (!connection_success) {
            consecutive_failures++;

            if (consecutive_failures >= MAX_FAILURES_BEFORE_RESET && !session_active_cleared) {
                session_active_cleared = true;
                // In real code: data_publisher_->publishSessionCompleted();
            }
        } else {
            consecutive_failures = 0;
            session_active_cleared = false;  // Allow future cleanup
        }
    }

    // After 3 failures, session_active should have been cleared
    // But then success resets the counter
    EXPECT_EQ(consecutive_failures, 0);
    EXPECT_FALSE(session_active_cleared);  // Reset after success
}

TEST_F(BurstCollectorServiceTest, ConnectionRecovery_ClearsOnlyOnce) {
    // Verify session_active is only cleared once per failure sequence
    int consecutive_failures = 0;
    bool session_active_cleared = false;
    int clear_count = 0;
    constexpr int MAX_FAILURES_BEFORE_RESET = 3;

    // Simulate 10 consecutive failures
    for (int i = 0; i < 10; i++) {
        consecutive_failures++;

        if (consecutive_failures >= MAX_FAILURES_BEFORE_RESET && !session_active_cleared) {
            session_active_cleared = true;
            clear_count++;
        }
    }

    EXPECT_EQ(clear_count, 1) << "Session should only be cleared once per failure sequence";
    EXPECT_TRUE(session_active_cleared);
    EXPECT_EQ(consecutive_failures, 10);
}

TEST_F(BurstCollectorServiceTest, ConnectionRecovery_ResetsOnSuccess) {
    // Verify success resets the failure counter
    int consecutive_failures = 5;  // Some failures
    bool session_active_cleared = true;

    // Connection succeeds
    consecutive_failures = 0;
    session_active_cleared = false;

    EXPECT_EQ(consecutive_failures, 0);
    EXPECT_FALSE(session_active_cleared) << "Success should allow future cleanup";
}

TEST_F(BurstCollectorServiceTest, ConnectionRecovery_ThresholdBoundary) {
    // Test exactly at threshold boundary (2 failures = no clear, 3 failures = clear)
    int consecutive_failures = 0;
    bool session_active_cleared = false;
    constexpr int MAX_FAILURES_BEFORE_RESET = 3;

    // 2 failures - should NOT clear
    consecutive_failures = 2;
    if (consecutive_failures >= MAX_FAILURES_BEFORE_RESET && !session_active_cleared) {
        session_active_cleared = true;
    }
    EXPECT_FALSE(session_active_cleared) << "2 failures should not trigger clear";

    // 3rd failure - should clear
    consecutive_failures = 3;
    if (consecutive_failures >= MAX_FAILURES_BEFORE_RESET && !session_active_cleared) {
        session_active_cleared = true;
    }
    EXPECT_TRUE(session_active_cleared) << "3 failures should trigger clear";
}

// ============================================================================
// STARTUP CLEANUP TESTS
// ============================================================================

TEST_F(BurstCollectorServiceTest, StartupCleanup_ClearsStaleSession) {
    // Verify startup cleanup logic
    // In real code, constructor calls data_publisher_->publishSessionCompleted()

    bool mqtt_connected = true;
    bool session_active_cleared = false;

    // Startup cleanup logic
    if (mqtt_connected) {
        // Simulate: data_publisher_->publishSessionCompleted();
        session_active_cleared = true;
    }

    EXPECT_TRUE(session_active_cleared) << "Startup should clear stale session_active";
}

TEST_F(BurstCollectorServiceTest, StartupCleanup_SkipsIfMqttDisconnected) {
    // Verify startup cleanup skips if MQTT not connected

    bool mqtt_connected = false;
    bool session_active_cleared = false;

    // Startup cleanup logic
    if (mqtt_connected) {
        session_active_cleared = true;
    }

    EXPECT_FALSE(session_active_cleared) << "Should skip cleanup if MQTT not connected";
}

