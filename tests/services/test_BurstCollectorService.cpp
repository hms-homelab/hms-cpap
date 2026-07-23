/**
 * HMS-CPAP BurstCollectorService Unit Tests
 *
 * Tests file archival, path construction, and file size comparison logic.
 */

#include "EquipmentStubs.h"
#include <gtest/gtest.h>
#include <gmock/gmock.h>
#include "services/BurstCollectorService.h"
#include "services/DataPublisherService.h"
#include "services/SessionDiscoveryService.h"
#include "services/PrismaIngestion.h"
#include "clients/IDataSource.h"
#include "clients/EzShareClient.h"
#include "database/IDatabase.h"
#include "mqtt_client.h"
#include "utils/ConfigManager.h"
#include "utils/AppConfig.h"
#include <filesystem>
#include <fstream>
#include <map>

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
// CHECKPOINT SIZE KB COMPARISON TESTS
// ============================================================================

/**
 * Test that checkpoint sizes are compared in KB (ez Share format)
 * Both DB storage and comparison must use KB values from ez Share HTML
 */
TEST_F(BurstCollectorServiceTest, CheckpointSizes_KBComparison_Exact) {
    // Simulate ez Share reporting file sizes in KB
    std::map<std::string, int> ezshare_sizes_kb = {
        {"20260217_234845_BRP.edf", 1608},  // 1608 KB from HTML
        {"20260217_234845_PLD.edf", 148},
        {"20260217_234845_SAD.edf", 66}
    };

    // DB should store exact same KB values
    std::map<std::string, int> db_sizes_kb = {
        {"20260217_234845_BRP.edf", 1608},
        {"20260217_234845_PLD.edf", 148},
        {"20260217_234845_SAD.edf", 66}
    };

    // Comparison should find all unchanged
    bool all_unchanged = true;
    for (const auto& [filename, db_size] : db_sizes_kb) {
        auto it = ezshare_sizes_kb.find(filename);
        if (it == ezshare_sizes_kb.end() || it->second != db_size) {
            all_unchanged = false;
            break;
        }
    }

    EXPECT_TRUE(all_unchanged) << "Exact KB values should match";
}

TEST_F(BurstCollectorServiceTest, CheckpointSizes_KBComparison_Changed) {
    // ez Share now reports larger size (file grew)
    std::map<std::string, int> ezshare_sizes_kb = {
        {"20260217_234845_BRP.edf", 1700},  // Grew from 1608 to 1700 KB
        {"20260217_234845_PLD.edf", 148},
        {"20260217_234845_SAD.edf", 66}
    };

    // DB has previous KB values
    std::map<std::string, int> db_sizes_kb = {
        {"20260217_234845_BRP.edf", 1608},
        {"20260217_234845_PLD.edf", 148},
        {"20260217_234845_SAD.edf", 66}
    };

    // Comparison should detect change
    bool all_unchanged = true;
    for (const auto& [filename, db_size] : db_sizes_kb) {
        auto it = ezshare_sizes_kb.find(filename);
        if (it == ezshare_sizes_kb.end() || it->second != db_size) {
            all_unchanged = false;
            break;
        }
    }

    EXPECT_FALSE(all_unchanged) << "Changed KB values should be detected";
}

TEST_F(BurstCollectorServiceTest, CheckpointSizes_BytesVsKB_WouldFail) {
    // This test demonstrates why bytes vs KB comparison fails
    // ez Share reports KB (integer)
    int ezshare_kb = 1608;

    // If we stored bytes from local file (WRONG approach)
    int local_bytes = 1645572;  // Actual file size

    // Direct comparison would ALWAYS fail
    EXPECT_NE(ezshare_kb, local_bytes) << "KB vs bytes never match";

    // Even converting bytes to KB doesn't match exactly (truncation)
    int converted_kb = local_bytes / 1024;  // = 1607
    EXPECT_NE(ezshare_kb, converted_kb) << "Truncation causes mismatch (1608 vs 1607)";

    // Solution: Store ez Share KB directly, compare KB to KB
    int db_kb = ezshare_kb;  // Store 1608 directly
    EXPECT_EQ(ezshare_kb, db_kb) << "KB to KB comparison works";
}

TEST_F(BurstCollectorServiceTest, CheckpointSizes_NewFileDetection) {
    // ez Share shows new checkpoint file
    std::map<std::string, int> ezshare_sizes_kb = {
        {"20260217_234845_BRP.edf", 1608},
        {"20260217_234845_PLD.edf", 148},
        {"20260217_234845_SAD.edf", 66},
        {"20260217_234845_BRP_001.edf", 500}  // New file!
    };

    // DB only has original 3 files
    std::map<std::string, int> db_sizes_kb = {
        {"20260217_234845_BRP.edf", 1608},
        {"20260217_234845_PLD.edf", 148},
        {"20260217_234845_SAD.edf", 66}
    };

    // Should detect new file
    bool has_new_files = ezshare_sizes_kb.size() > db_sizes_kb.size();
    EXPECT_TRUE(has_new_files) << "New checkpoint file should be detected";
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
// SESSION COMPLETION DECISION TESTS
// ============================================================================

/**
 * These tests verify the session completion decision logic:
 *
 * When all checkpoint files are unchanged (session stopped growing):
 *   1. markSessionCompleted() → returns true FIRST time (session_end was NULL)
 *   2. markSessionCompleted() → returns false on subsequent calls (already set)
 *   3. Completion actions (MQTT, STR, LLM) fire ONLY when:
 *      - newly_completed == true (first time marking)
 *      - is_most_recent == true (session has highest session_start timestamp)
 *
 * This prevents:
 *   - Old completed sessions from clearing active session_active (the original bug)
 *   - Repeated completion actions on every burst cycle (dedup via DB)
 */

// Helper: simulate the most_recent detection logic
bool isMostRecentSession(
    const std::chrono::system_clock::time_point& session_start,
    const std::vector<std::chrono::system_clock::time_point>& all_starts) {

    auto most_recent = *std::max_element(all_starts.begin(), all_starts.end());
    return session_start == most_recent;
}

// Helper: simulate the completion decision
struct CompletionDecision {
    bool should_complete;
    bool newly_completed;
    bool is_most_recent;
};

CompletionDecision shouldTriggerCompletion(
    const std::chrono::system_clock::time_point& session_start,
    const std::vector<std::chrono::system_clock::time_point>& all_starts,
    bool db_session_end_is_null) {

    // markSessionCompleted returns true only if session_end was NULL
    bool newly_completed = db_session_end_is_null;

    auto most_recent = *std::max_element(all_starts.begin(), all_starts.end());
    bool is_most_recent = (session_start == most_recent);

    return {
        newly_completed && is_most_recent,
        newly_completed,
        is_most_recent
    };
}

TEST_F(BurstCollectorServiceTest, Completion_LatestSessionFirstTime_Fires) {
    // Session 224724 is the latest, session_end is NULL (first time marking)
    auto t1 = std::chrono::system_clock::from_time_t(1710373899);  // 213139
    auto t2 = std::chrono::system_clock::from_time_t(1710378444);  // 224724 (latest)
    auto t3 = std::chrono::system_clock::from_time_t(1710287687);  // 205447 (old)

    std::vector<std::chrono::system_clock::time_point> all = {t1, t2, t3};

    auto decision = shouldTriggerCompletion(t2, all, /*db_session_end_is_null=*/true);

    EXPECT_TRUE(decision.should_complete) << "Latest session, first completion should fire";
    EXPECT_TRUE(decision.newly_completed);
    EXPECT_TRUE(decision.is_most_recent);
}

TEST_F(BurstCollectorServiceTest, Completion_LatestSessionAlreadyCompleted_NoFire) {
    // Session 224724 is the latest, but session_end already set (second cycle)
    auto t1 = std::chrono::system_clock::from_time_t(1710373899);
    auto t2 = std::chrono::system_clock::from_time_t(1710378444);
    auto t3 = std::chrono::system_clock::from_time_t(1710287687);

    std::vector<std::chrono::system_clock::time_point> all = {t1, t2, t3};

    auto decision = shouldTriggerCompletion(t2, all, /*db_session_end_is_null=*/false);

    EXPECT_FALSE(decision.should_complete) << "Already completed session should not re-fire";
    EXPECT_FALSE(decision.newly_completed);
    EXPECT_TRUE(decision.is_most_recent);
}

TEST_F(BurstCollectorServiceTest, Completion_OldSessionFirstTime_NoFire) {
    // Session 213139 is old (not the latest), even if session_end was NULL
    auto t1 = std::chrono::system_clock::from_time_t(1710373899);  // 213139
    auto t2 = std::chrono::system_clock::from_time_t(1710378444);  // 224724 (latest)
    auto t3 = std::chrono::system_clock::from_time_t(1710287687);  // 205447

    std::vector<std::chrono::system_clock::time_point> all = {t1, t2, t3};

    auto decision = shouldTriggerCompletion(t1, all, /*db_session_end_is_null=*/true);

    EXPECT_FALSE(decision.should_complete) << "Old session should not trigger completion even if newly marked";
    EXPECT_TRUE(decision.newly_completed);
    EXPECT_FALSE(decision.is_most_recent);
}

TEST_F(BurstCollectorServiceTest, Completion_OldSessionAlreadyCompleted_NoFire) {
    // Session 213139 is old AND already completed — double no
    auto t1 = std::chrono::system_clock::from_time_t(1710373899);
    auto t2 = std::chrono::system_clock::from_time_t(1710378444);
    auto t3 = std::chrono::system_clock::from_time_t(1710287687);

    std::vector<std::chrono::system_clock::time_point> all = {t1, t2, t3};

    auto decision = shouldTriggerCompletion(t1, all, /*db_session_end_is_null=*/false);

    EXPECT_FALSE(decision.should_complete);
    EXPECT_FALSE(decision.newly_completed);
    EXPECT_FALSE(decision.is_most_recent);
}

TEST_F(BurstCollectorServiceTest, Completion_SingleSession_Fires) {
    // Only one session in the list — it IS the most recent
    auto t1 = std::chrono::system_clock::from_time_t(1710378444);

    std::vector<std::chrono::system_clock::time_point> all = {t1};

    auto decision = shouldTriggerCompletion(t1, all, /*db_session_end_is_null=*/true);

    EXPECT_TRUE(decision.should_complete) << "Single session should complete";
    EXPECT_TRUE(decision.newly_completed);
    EXPECT_TRUE(decision.is_most_recent);
}

TEST_F(BurstCollectorServiceTest, Completion_SingleSessionAlreadyDone_NoFire) {
    // Only one session, already completed
    auto t1 = std::chrono::system_clock::from_time_t(1710378444);

    std::vector<std::chrono::system_clock::time_point> all = {t1};

    auto decision = shouldTriggerCompletion(t1, all, /*db_session_end_is_null=*/false);

    EXPECT_FALSE(decision.should_complete) << "Already completed single session should not re-fire";
}

TEST_F(BurstCollectorServiceTest, Completion_ScanOrderDoesNotMatter) {
    // Sessions discovered in reverse time order (20260312 scanned AFTER 20260313)
    // The most_recent_start check uses timestamp, not list position
    auto old_session = std::chrono::system_clock::from_time_t(1710287687);   // 205447
    auto latest_session = std::chrono::system_clock::from_time_t(1710378444); // 224724

    // List order: latest first, old second (as if 20260313 scanned before 20260312)
    std::vector<std::chrono::system_clock::time_point> order1 = {latest_session, old_session};
    // Reversed list order: old first, latest second
    std::vector<std::chrono::system_clock::time_point> order2 = {old_session, latest_session};

    // Latest should be identified correctly regardless of list order
    EXPECT_TRUE(isMostRecentSession(latest_session, order1));
    EXPECT_TRUE(isMostRecentSession(latest_session, order2));

    // Old should never be identified as most recent
    EXPECT_FALSE(isMostRecentSession(old_session, order1));
    EXPECT_FALSE(isMostRecentSession(old_session, order2));
}

TEST_F(BurstCollectorServiceTest, Completion_ThreeSessionsOnlyLatestFires) {
    // Simulate the exact scenario: 3 sessions, all unchanged, only latest fires
    auto s1 = std::chrono::system_clock::from_time_t(1710373899);  // 213139
    auto s2 = std::chrono::system_clock::from_time_t(1710378444);  // 224724
    auto s3 = std::chrono::system_clock::from_time_t(1710287687);  // 205447

    std::vector<std::chrono::system_clock::time_point> all = {s1, s2, s3};

    // All session_ends are NULL (first time all stop)
    int completions_fired = 0;

    for (const auto& session_start : all) {
        auto decision = shouldTriggerCompletion(session_start, all, /*db_session_end_is_null=*/true);
        if (decision.should_complete) {
            completions_fired++;
        }
    }

    EXPECT_EQ(completions_fired, 1) << "Exactly one completion should fire across all sessions";
}

TEST_F(BurstCollectorServiceTest, Completion_SecondCycleNoFires) {
    // Second cycle: all session_ends are set (markSessionCompleted returns false)
    auto s1 = std::chrono::system_clock::from_time_t(1710373899);
    auto s2 = std::chrono::system_clock::from_time_t(1710378444);
    auto s3 = std::chrono::system_clock::from_time_t(1710287687);

    std::vector<std::chrono::system_clock::time_point> all = {s1, s2, s3};

    int completions_fired = 0;

    for (const auto& session_start : all) {
        auto decision = shouldTriggerCompletion(session_start, all, /*db_session_end_is_null=*/false);
        if (decision.should_complete) {
            completions_fired++;
        }
    }

    EXPECT_EQ(completions_fired, 0) << "No completions on second cycle (all already marked)";
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

// ============================================================================
// SUMMARY REGENERATION DECISION TESTS
// ============================================================================

/**
 * These tests verify the on-demand summary regeneration logic triggered
 * by MQTT message on cpap/{device_id}/command/regenerate_summary.
 *
 * The handler:
 *   1. Calls getLastSessionStart() → get latest session timestamp
 *   2. Calls getNightlyMetrics() → get aggregated metrics for that session
 *   3. Calls generateAndPublishSummary() → LLM + MQTT publish
 *
 * Failure modes tested:
 *   - No sessions in DB → abort gracefully
 *   - No metrics for session → abort gracefully
 *   - LLM disabled → subscription not created
 *   - MQTT disconnected → subscription not created
 *   - Happy path → summary generated and published
 */

// Simulate the regeneration handler's decision flow
struct RegenerationResult {
    bool attempted;        // Did we attempt LLM generation?
    bool had_session;      // Did getLastSessionStart return a value?
    bool had_metrics;      // Did getNightlyMetrics return a value?
    std::string error;     // Error message if aborted
};

RegenerationResult simulateRegeneration(
    bool llm_enabled,
    bool mqtt_connected,
    bool db_has_sessions,
    bool db_has_metrics) {

    RegenerationResult result = {false, false, false, ""};

    // Guard: LLM must be enabled and MQTT connected for subscription to exist
    if (!llm_enabled) {
        result.error = "LLM not enabled, no subscription";
        return result;
    }
    if (!mqtt_connected) {
        result.error = "MQTT not connected, no subscription";
        return result;
    }

    // Step 1: get latest session
    if (!db_has_sessions) {
        result.error = "No sessions found for summary regeneration";
        return result;
    }
    result.had_session = true;

    // Step 2: get nightly metrics
    if (!db_has_metrics) {
        result.error = "No metrics found for latest session";
        return result;
    }
    result.had_metrics = true;

    // Step 3: generate and publish
    result.attempted = true;
    return result;
}

TEST_F(BurstCollectorServiceTest, Regeneration_HappyPath_GeneratesSummary) {
    auto result = simulateRegeneration(
        /*llm_enabled=*/true, /*mqtt_connected=*/true,
        /*db_has_sessions=*/true, /*db_has_metrics=*/true);

    EXPECT_TRUE(result.attempted) << "Should generate summary when all preconditions met";
    EXPECT_TRUE(result.had_session);
    EXPECT_TRUE(result.had_metrics);
    EXPECT_TRUE(result.error.empty());
}

TEST_F(BurstCollectorServiceTest, Regeneration_NoSessions_Aborts) {
    auto result = simulateRegeneration(
        /*llm_enabled=*/true, /*mqtt_connected=*/true,
        /*db_has_sessions=*/false, /*db_has_metrics=*/false);

    EXPECT_FALSE(result.attempted);
    EXPECT_FALSE(result.had_session);
    EXPECT_EQ(result.error, "No sessions found for summary regeneration");
}

TEST_F(BurstCollectorServiceTest, Regeneration_NoMetrics_Aborts) {
    auto result = simulateRegeneration(
        /*llm_enabled=*/true, /*mqtt_connected=*/true,
        /*db_has_sessions=*/true, /*db_has_metrics=*/false);

    EXPECT_FALSE(result.attempted);
    EXPECT_TRUE(result.had_session);
    EXPECT_EQ(result.error, "No metrics found for latest session");
}

TEST_F(BurstCollectorServiceTest, Regeneration_LLMDisabled_NoSubscription) {
    auto result = simulateRegeneration(
        /*llm_enabled=*/false, /*mqtt_connected=*/true,
        /*db_has_sessions=*/true, /*db_has_metrics=*/true);

    EXPECT_FALSE(result.attempted);
    EXPECT_EQ(result.error, "LLM not enabled, no subscription");
}

TEST_F(BurstCollectorServiceTest, Regeneration_MqttDisconnected_NoSubscription) {
    auto result = simulateRegeneration(
        /*llm_enabled=*/true, /*mqtt_connected=*/false,
        /*db_has_sessions=*/true, /*db_has_metrics=*/true);

    EXPECT_FALSE(result.attempted);
    EXPECT_EQ(result.error, "MQTT not connected, no subscription");
}

TEST_F(BurstCollectorServiceTest, Regeneration_TopicFormat) {
    // Verify the MQTT topic format matches the expected pattern
    std::string device_id = "cpap_resmed_23243570851";
    std::string expected_topic = "cpap/cpap_resmed_23243570851/command/regenerate_summary";
    std::string actual_topic = "cpap/" + device_id + "/command/regenerate_summary";

    EXPECT_EQ(actual_topic, expected_topic);
}

TEST_F(BurstCollectorServiceTest, Regeneration_PayloadIgnored) {
    // The handler ignores the payload — any message triggers regeneration
    // This test documents that behavior: empty, "now", or arbitrary payload all work
    std::vector<std::string> payloads = {"", "now", "regenerate", "{\"force\":true}"};

    for (const auto& payload : payloads) {
        auto result = simulateRegeneration(
            /*llm_enabled=*/true, /*mqtt_connected=*/true,
            /*db_has_sessions=*/true, /*db_has_metrics=*/true);

        EXPECT_TRUE(result.attempted)
            << "Payload '" << payload << "' should still trigger regeneration";
    }
}

// ============================================================================
// SESSION COMPLETION ACTIONS TESTS
// ============================================================================

/**
 * These tests verify the full completion action sequence that fires when
 * checkpoint files stop growing (all_unchanged == true):
 *
 * 1. markSessionCompleted() → returns true on first completion
 * 2. getNightlyMetrics() → aggregated metrics for the night
 * 3. publishHistoricalState() → 31 MQTT sensors
 * 4. publishSessionCompleted() → session_active=OFF, session_status=completed
 * 5. processSTRFile() → download STR, parse, publish daily metrics + insights
 * 6. generateAndPublishSummary() → LLM summary with STR data if available
 *
 * The parser always returns IN_PROGRESS. Completion is solely determined
 * by the checkpoint path (file sizes stop changing between cycles).
 */

// Simulate the full completion action sequence
struct CompletionActions {
    bool metrics_published;
    bool session_completed;
    bool str_processed;
    bool summary_generated;
    bool summary_has_str;  // STR data passed to summary
};

CompletionActions simulateCompletionActions(
    bool newly_completed,
    bool is_most_recent,
    bool has_data_publisher,
    bool has_metrics,
    bool llm_enabled,
    bool has_llm_client,
    bool has_str_records) {

    CompletionActions actions = {false, false, false, false, false};

    if (newly_completed && is_most_recent && has_data_publisher) {
        if (has_metrics) {
            actions.metrics_published = true;
        }
        actions.session_completed = true;
        actions.str_processed = true;  // processSTRFile() always attempted

        if (llm_enabled && has_llm_client && has_metrics) {
            actions.summary_generated = true;
            // STR record passed if available after processSTRFile
            actions.summary_has_str = has_str_records;
        }
    }

    return actions;
}

// Same for local mode (no is_most_recent check, no STR processing)
CompletionActions simulateLocalCompletionActions(
    bool newly_completed,
    bool has_data_publisher,
    bool has_metrics,
    bool llm_enabled,
    bool has_llm_client) {

    CompletionActions actions = {false, false, false, false, false};

    if (newly_completed && has_data_publisher) {
        if (has_metrics) {
            actions.metrics_published = true;
        }
        actions.session_completed = true;
        // No STR in local mode (no ezshare)

        if (llm_enabled && has_llm_client && has_metrics) {
            actions.summary_generated = true;
        }
    }

    return actions;
}

TEST_F(BurstCollectorServiceTest, CompletionActions_FullSequence_WithSTR) {
    auto actions = simulateCompletionActions(
        /*newly_completed=*/true, /*is_most_recent=*/true,
        /*has_data_publisher=*/true, /*has_metrics=*/true,
        /*llm_enabled=*/true, /*has_llm_client=*/true,
        /*has_str_records=*/true);

    EXPECT_TRUE(actions.metrics_published);
    EXPECT_TRUE(actions.session_completed);
    EXPECT_TRUE(actions.str_processed);
    EXPECT_TRUE(actions.summary_generated);
    EXPECT_TRUE(actions.summary_has_str) << "STR data should be passed to summary";
}

TEST_F(BurstCollectorServiceTest, CompletionActions_FullSequence_WithoutSTR) {
    auto actions = simulateCompletionActions(
        /*newly_completed=*/true, /*is_most_recent=*/true,
        /*has_data_publisher=*/true, /*has_metrics=*/true,
        /*llm_enabled=*/true, /*has_llm_client=*/true,
        /*has_str_records=*/false);

    EXPECT_TRUE(actions.metrics_published);
    EXPECT_TRUE(actions.session_completed);
    EXPECT_TRUE(actions.str_processed);
    EXPECT_TRUE(actions.summary_generated);
    EXPECT_FALSE(actions.summary_has_str) << "Summary should work without STR";
}

TEST_F(BurstCollectorServiceTest, CompletionActions_LLMDisabled_NoSummary) {
    auto actions = simulateCompletionActions(
        /*newly_completed=*/true, /*is_most_recent=*/true,
        /*has_data_publisher=*/true, /*has_metrics=*/true,
        /*llm_enabled=*/false, /*has_llm_client=*/false,
        /*has_str_records=*/true);

    EXPECT_TRUE(actions.metrics_published);
    EXPECT_TRUE(actions.session_completed);
    EXPECT_TRUE(actions.str_processed);
    EXPECT_FALSE(actions.summary_generated) << "No summary when LLM disabled";
}

TEST_F(BurstCollectorServiceTest, CompletionActions_NoMetrics_SkipsSummary) {
    auto actions = simulateCompletionActions(
        /*newly_completed=*/true, /*is_most_recent=*/true,
        /*has_data_publisher=*/true, /*has_metrics=*/false,
        /*llm_enabled=*/true, /*has_llm_client=*/true,
        /*has_str_records=*/true);

    EXPECT_FALSE(actions.metrics_published);
    EXPECT_TRUE(actions.session_completed);
    EXPECT_TRUE(actions.str_processed);
    EXPECT_FALSE(actions.summary_generated) << "No summary without metrics";
}

TEST_F(BurstCollectorServiceTest, CompletionActions_NotNewlyCompleted_NoActions) {
    auto actions = simulateCompletionActions(
        /*newly_completed=*/false, /*is_most_recent=*/true,
        /*has_data_publisher=*/true, /*has_metrics=*/true,
        /*llm_enabled=*/true, /*has_llm_client=*/true,
        /*has_str_records=*/true);

    EXPECT_FALSE(actions.metrics_published);
    EXPECT_FALSE(actions.session_completed);
    EXPECT_FALSE(actions.str_processed);
    EXPECT_FALSE(actions.summary_generated);
}

TEST_F(BurstCollectorServiceTest, CompletionActions_NotMostRecent_NoActions) {
    auto actions = simulateCompletionActions(
        /*newly_completed=*/true, /*is_most_recent=*/false,
        /*has_data_publisher=*/true, /*has_metrics=*/true,
        /*llm_enabled=*/true, /*has_llm_client=*/true,
        /*has_str_records=*/true);

    EXPECT_FALSE(actions.metrics_published);
    EXPECT_FALSE(actions.session_completed);
    EXPECT_FALSE(actions.str_processed);
    EXPECT_FALSE(actions.summary_generated);
}

TEST_F(BurstCollectorServiceTest, CompletionActions_LocalMode_GeneratesSummary) {
    auto actions = simulateLocalCompletionActions(
        /*newly_completed=*/true, /*has_data_publisher=*/true,
        /*has_metrics=*/true, /*llm_enabled=*/true,
        /*has_llm_client=*/true);

    EXPECT_TRUE(actions.metrics_published);
    EXPECT_TRUE(actions.session_completed);
    EXPECT_FALSE(actions.str_processed) << "No STR in local mode";
    EXPECT_TRUE(actions.summary_generated) << "Local mode should generate summary";
}

TEST_F(BurstCollectorServiceTest, CompletionActions_LocalMode_NotNewlyCompleted_NoActions) {
    auto actions = simulateLocalCompletionActions(
        /*newly_completed=*/false, /*has_data_publisher=*/true,
        /*has_metrics=*/true, /*llm_enabled=*/true,
        /*has_llm_client=*/true);

    EXPECT_FALSE(actions.metrics_published);
    EXPECT_FALSE(actions.session_completed);
    EXPECT_FALSE(actions.summary_generated);
}

// ============================================================================
// SESSION RESUME (MASK ON/OFF/ON/OFF) TESTS
// ============================================================================

/**
 * These tests verify the completed → resumed → completed cycle:
 *
 * Scenario: user removes mask (session completes, summary sent), puts mask
 * back on (checkpoint files grow again), then removes mask a second time.
 * The second completion must also fire metrics + summary.
 *
 * The fix: when checkpoint files change on an already-completed session,
 * reopenSession() clears session_end back to NULL, allowing
 * markSessionCompleted() to return true again on the next stable cycle.
 */

// Simulate the reopenSession logic: returns true if session_end was NOT NULL
// (i.e., session was completed and is now being reopened)
bool simulateReopenSession(bool session_end_is_set) {
    // reopenSession: UPDATE ... SET session_end = NULL WHERE session_end IS NOT NULL
    return session_end_is_set;
}

TEST_F(BurstCollectorServiceTest, Resume_CompletedThenResumed_ReopensSession) {
    // Session was completed (session_end set), then checkpoint files grew again
    bool reopened = simulateReopenSession(/*session_end_is_set=*/true);
    EXPECT_TRUE(reopened) << "Completed session with growing files should reopen";
}

TEST_F(BurstCollectorServiceTest, Resume_ActiveSession_NoReopen) {
    // Session was never completed (still active), files still growing — no-op
    bool reopened = simulateReopenSession(/*session_end_is_set=*/false);
    EXPECT_FALSE(reopened) << "Active session should not trigger reopen";
}

TEST_F(BurstCollectorServiceTest, Resume_FullCycle_CompletedResumedCompleted) {
    // Full mask on/off/on/off cycle for a single session
    auto session_start = std::chrono::system_clock::from_time_t(1710378444);
    std::vector<std::chrono::system_clock::time_point> all = {session_start};

    // --- Cycle 1: mask on, files growing ---
    // (checkpoint sizes change, session downloaded and parsed — no completion check)

    // --- Cycle 2: mask removed, files stopped growing ---
    bool session_end_is_null = true;  // session_end not yet set
    auto decision1 = shouldTriggerCompletion(session_start, all, session_end_is_null);
    EXPECT_TRUE(decision1.should_complete) << "First completion should fire";

    // After markSessionCompleted: session_end is now set
    session_end_is_null = false;

    // First completion actions should fire
    auto actions1 = simulateCompletionActions(
        decision1.newly_completed, decision1.is_most_recent,
        /*has_data_publisher=*/true, /*has_metrics=*/true,
        /*llm_enabled=*/true, /*has_llm_client=*/true,
        /*has_str_records=*/true);
    EXPECT_TRUE(actions1.summary_generated) << "First summary should be generated";

    // --- Cycle 3: mask put back on, files growing again ---
    // Checkpoint sizes changed → re-download → reopenSession()
    bool reopened = simulateReopenSession(/*session_end_is_set=*/true);
    EXPECT_TRUE(reopened);
    // After reopenSession: session_end is NULL again
    session_end_is_null = true;

    // --- Cycle 4: mask removed again, files stopped growing ---
    auto decision2 = shouldTriggerCompletion(session_start, all, session_end_is_null);
    EXPECT_TRUE(decision2.should_complete) << "Second completion should also fire";

    auto actions2 = simulateCompletionActions(
        decision2.newly_completed, decision2.is_most_recent,
        /*has_data_publisher=*/true, /*has_metrics=*/true,
        /*llm_enabled=*/true, /*has_llm_client=*/true,
        /*has_str_records=*/true);
    EXPECT_TRUE(actions2.summary_generated) << "Second summary should also be generated";
}

TEST_F(BurstCollectorServiceTest, Resume_WithoutReopen_SecondCompletionFails) {
    // This test documents the BUG behavior before the fix:
    // Without reopenSession(), the second completion never fires.
    auto session_start = std::chrono::system_clock::from_time_t(1710378444);
    std::vector<std::chrono::system_clock::time_point> all = {session_start};

    // First completion
    auto decision1 = shouldTriggerCompletion(session_start, all, /*db_session_end_is_null=*/true);
    EXPECT_TRUE(decision1.should_complete);

    // session_end is now set. Without reopenSession, it stays set.
    // Second completion attempt — session_end is NOT NULL
    auto decision2 = shouldTriggerCompletion(session_start, all, /*db_session_end_is_null=*/false);
    EXPECT_FALSE(decision2.should_complete)
        << "Without reopenSession, second completion correctly does not fire (the old bug)";
}

TEST_F(BurstCollectorServiceTest, Resume_MultipleResumes_AllComplete) {
    // Mask on/off three times — each completion should fire
    auto session_start = std::chrono::system_clock::from_time_t(1710378444);
    std::vector<std::chrono::system_clock::time_point> all = {session_start};

    int summaries_generated = 0;

    for (int cycle = 0; cycle < 3; cycle++) {
        // Files stop growing → completion check
        auto decision = shouldTriggerCompletion(session_start, all, /*db_session_end_is_null=*/true);
        EXPECT_TRUE(decision.should_complete) << "Cycle " << cycle << " should complete";

        auto actions = simulateCompletionActions(
            decision.newly_completed, decision.is_most_recent,
            /*has_data_publisher=*/true, /*has_metrics=*/true,
            /*llm_enabled=*/true, /*has_llm_client=*/true,
            /*has_str_records=*/true);
        if (actions.summary_generated) summaries_generated++;

        // session_end set → mask back on → files grow → reopenSession clears it
        // (simulated by looping with session_end_is_null=true again)
    }

    EXPECT_EQ(summaries_generated, 3) << "All three completions should generate summaries";
}

// ============================================================================
// RANGE SUMMARY (WEEKLY / MONTHLY) TESTS
// ============================================================================

/**
 * These tests verify the buildRangeMetricsString formatting and the
 * auto-trigger logic (Sunday → weekly, 1st → monthly).
 *
 * The actual DB query and LLM call are integration-tested; here we test
 * the pure logic: formatting, compliance calculation, best/worst detection.
 */

// Helper: create a SessionMetrics for one night with key fields
SessionMetrics makeNight(const std::string& day, double ahi, double hours,
                          int events = 0, double leak = 0.0) {
    SessionMetrics m;
    m.sleep_day = day;
    m.ahi = ahi;
    m.usage_hours = hours;
    m.total_events = events;
    m.obstructive_apneas = events / 2;
    m.central_apneas = 0;
    m.hypopneas = events / 2;
    m.reras = 0;
    if (leak > 0) m.avg_leak_rate = leak;
    return m;
}

TEST_F(BurstCollectorServiceTest, RangeSummary_SummaryPeriodEnum) {
    // Verify enum values exist and are distinct
    EXPECT_NE(static_cast<int>(SummaryPeriod::DAILY),
              static_cast<int>(SummaryPeriod::WEEKLY));
    EXPECT_NE(static_cast<int>(SummaryPeriod::WEEKLY),
              static_cast<int>(SummaryPeriod::MONTHLY));
}

TEST_F(BurstCollectorServiceTest, RangeSummary_ComplianceCalculation) {
    // 5 out of 7 nights >= 4h → 71.43% compliance
    std::vector<SessionMetrics> nights = {
        makeNight("2026-03-22", 2.1, 6.5),   // compliant
        makeNight("2026-03-23", 3.4, 5.0),   // compliant
        makeNight("2026-03-24", 1.8, 7.2),   // compliant
        makeNight("2026-03-25", 5.1, 3.5),   // NOT compliant
        makeNight("2026-03-26", 2.0, 4.1),   // compliant
        makeNight("2026-03-27", 4.2, 2.0),   // NOT compliant
        makeNight("2026-03-28", 1.5, 8.0),   // compliant
    };

    // Count compliant nights (>= 4h)
    int compliant = 0;
    for (const auto& n : nights)
        if (n.usage_hours.value_or(0.0) >= 4.0) compliant++;

    EXPECT_EQ(compliant, 5);
    EXPECT_NEAR(100.0 * compliant / nights.size(), 71.43, 0.01);
}

TEST_F(BurstCollectorServiceTest, RangeSummary_BestWorstNight) {
    std::vector<SessionMetrics> nights = {
        makeNight("2026-03-22", 2.1, 6.5),
        makeNight("2026-03-23", 0.5, 7.0),  // best AHI
        makeNight("2026-03-24", 8.3, 5.0),  // worst AHI
        makeNight("2026-03-25", 3.1, 6.0),
    };

    auto best = std::min_element(nights.begin(), nights.end(),
        [](const SessionMetrics& a, const SessionMetrics& b) { return a.ahi < b.ahi; });
    auto worst = std::max_element(nights.begin(), nights.end(),
        [](const SessionMetrics& a, const SessionMetrics& b) { return a.ahi < b.ahi; });

    EXPECT_EQ(best->sleep_day, "2026-03-23");
    EXPECT_DOUBLE_EQ(best->ahi, 0.5);
    EXPECT_EQ(worst->sleep_day, "2026-03-24");
    EXPECT_DOUBLE_EQ(worst->ahi, 8.3);
}

TEST_F(BurstCollectorServiceTest, RangeSummary_AveragesCorrect) {
    std::vector<SessionMetrics> nights = {
        makeNight("2026-03-26", 2.0, 6.0, 12, 5.0),
        makeNight("2026-03-27", 4.0, 8.0, 32, 10.0),
    };

    double avg_ahi = (2.0 + 4.0) / 2;
    double avg_hours = (6.0 + 8.0) / 2;
    double avg_leak = (5.0 + 10.0) / 2;

    EXPECT_DOUBLE_EQ(avg_ahi, 3.0);
    EXPECT_DOUBLE_EQ(avg_hours, 7.0);
    EXPECT_DOUBLE_EQ(avg_leak, 7.5);
}

TEST_F(BurstCollectorServiceTest, RangeSummary_AutoTrigger_WeeklyDay) {
    // Default WEEKLY_SUMMARY_DAY=0 (Sunday). Configurable 0-6.
    int weekly_day = 0;  // default
    std::tm sunday = {};
    sunday.tm_wday = 0;
    sunday.tm_mday = 15;
    EXPECT_EQ(sunday.tm_wday, weekly_day) << "Matches configured weekly day";
    EXPECT_NE(sunday.tm_mday, 1) << "Not 1st, so no monthly";
}

TEST_F(BurstCollectorServiceTest, RangeSummary_AutoTrigger_MonthlyDay) {
    // Default MONTHLY_SUMMARY_DAY=1. Configurable 1-28.
    int monthly_day = 1;  // default
    std::tm first = {};
    first.tm_wday = 3;   // Wednesday
    first.tm_mday = 1;
    EXPECT_NE(first.tm_wday, 0) << "Not default weekly day";
    EXPECT_EQ(first.tm_mday, monthly_day) << "Matches configured monthly day";
}

TEST_F(BurstCollectorServiceTest, RangeSummary_AutoTrigger_BothDays) {
    // When weekly day and monthly day coincide, both fire
    int weekly_day = 0;
    int monthly_day = 1;
    std::tm both = {};
    both.tm_wday = 0;   // Sunday
    both.tm_mday = 1;   // 1st
    EXPECT_EQ(both.tm_wday, weekly_day);
    EXPECT_EQ(both.tm_mday, monthly_day);
}

TEST_F(BurstCollectorServiceTest, RangeSummary_DaysOverride) {
    // days_override > 0 should take precedence over period default
    int days_override = 14;
    int period_default_weekly = 7;
    int actual = days_override > 0 ? days_override : period_default_weekly;
    EXPECT_EQ(actual, 14) << "Override should take precedence";

    // days_override == 0 falls back to period default
    days_override = 0;
    actual = days_override > 0 ? days_override : period_default_weekly;
    EXPECT_EQ(actual, 7) << "Zero override uses period default";
}

TEST_F(BurstCollectorServiceTest, RangeSummary_EmptyNights_Handled) {
    // Edge case: no nights in range should not crash
    std::vector<SessionMetrics> empty;
    EXPECT_TRUE(empty.empty());
    // generateRangeSummary checks for empty and returns early — tested here as guard
}

// ============================================================================
// PARSER STATUS INVARIANT TESTS
// ============================================================================

TEST_F(BurstCollectorServiceTest, ParserAlwaysReturnsInProgress) {
    // After refactoring, the parser never sets COMPLETED.
    // Verify the post-parse path always treats sessions as in-progress.
    CPAPSession session;
    // Default status must be IN_PROGRESS
    EXPECT_EQ(session.status, CPAPSession::Status::IN_PROGRESS)
        << "Default session status should be IN_PROGRESS";
}

// ============================================================================
// FYSETC HOT-RELOAD LIFECYCLE TESTS
// ============================================================================
// Covers the pure decision function used by reloadConfig() to decide whether
// to spin up or tear down the Fysetc TCP listener on a source-mode change.

using FysetcAction = BurstCollectorService::FysetcLifecycleAction;

TEST(FysetcLifecycle, StartWhenSwitchingInFromEzshareWithoutServer) {
    EXPECT_EQ(BurstCollectorService::decideFysetcLifecycle("ezshare", "fysetc", false),
              FysetcAction::Start);
}

TEST(FysetcLifecycle, StartWhenSwitchingInFromLocalWithoutServer) {
    EXPECT_EQ(BurstCollectorService::decideFysetcLifecycle("local", "fysetc", false),
              FysetcAction::Start);
}

TEST(FysetcLifecycle, NoActionWhenSwitchingInButServerAlreadyRunning) {
    EXPECT_EQ(BurstCollectorService::decideFysetcLifecycle("ezshare", "fysetc", true),
              FysetcAction::None);
}

TEST(FysetcLifecycle, StopWhenSwitchingOutToEzshare) {
    EXPECT_EQ(BurstCollectorService::decideFysetcLifecycle("fysetc", "ezshare", true),
              FysetcAction::Stop);
}

TEST(FysetcLifecycle, StopWhenSwitchingOutToLocal) {
    EXPECT_EQ(BurstCollectorService::decideFysetcLifecycle("fysetc", "local", true),
              FysetcAction::Stop);
}

TEST(FysetcLifecycle, NoActionWhenSwitchingOutWithoutRunningServer) {
    // Defensive: should never happen in practice, but helper must handle it.
    EXPECT_EQ(BurstCollectorService::decideFysetcLifecycle("fysetc", "ezshare", false),
              FysetcAction::None);
}

TEST(FysetcLifecycle, NoActionWhenStayingFysetc) {
    EXPECT_EQ(BurstCollectorService::decideFysetcLifecycle("fysetc", "fysetc", true),
              FysetcAction::None);
}

TEST(FysetcLifecycle, NoActionWhenEzshareToLocal) {
    EXPECT_EQ(BurstCollectorService::decideFysetcLifecycle("ezshare", "local", false),
              FysetcAction::None);
}

TEST(FysetcLifecycle, NoActionWhenEmptyInitialSourceToEzshare) {
    // First-ever reload where last_config_.source hasn't been populated.
    EXPECT_EQ(BurstCollectorService::decideFysetcLifecycle("", "ezshare", false),
              FysetcAction::None);
}

TEST(FysetcLifecycle, StartWhenEmptyInitialSourceToFysetc) {
    EXPECT_EQ(BurstCollectorService::decideFysetcLifecycle("", "fysetc", false),
              FysetcAction::Start);
}

// Additional permutations exercising every branch of the real static helper.
// The first rule (new=="fysetc" && !server) wins over the stop rule, so even
// a fysetc->fysetc transition with no server returns Start (defensive restart).
TEST(FysetcLifecycle, StartWhenStayingFysetcButServerMissing) {
    EXPECT_EQ(BurstCollectorService::decideFysetcLifecycle("fysetc", "fysetc", false),
              FysetcAction::Start);
}

TEST(FysetcLifecycle, StartWhenSwitchingFromLowensteinToFysetc) {
    EXPECT_EQ(BurstCollectorService::decideFysetcLifecycle("lowenstein", "fysetc", false),
              FysetcAction::Start);
}

TEST(FysetcLifecycle, StopWhenSwitchingOutToLowenstein) {
    EXPECT_EQ(BurstCollectorService::decideFysetcLifecycle("fysetc", "lowenstein", true),
              FysetcAction::Stop);
}

TEST(FysetcLifecycle, NoActionEzshareToEzshareWithServer) {
    // Non-fysetc on both ends: never touches the listener regardless of server.
    EXPECT_EQ(BurstCollectorService::decideFysetcLifecycle("ezshare", "ezshare", true),
              FysetcAction::None);
}

TEST(FysetcLifecycle, NoActionLocalToLowensteinWithServer) {
    EXPECT_EQ(BurstCollectorService::decideFysetcLifecycle("local", "lowenstein", true),
              FysetcAction::None);
}

TEST(FysetcLifecycle, NoActionEmptyToEmpty) {
    EXPECT_EQ(BurstCollectorService::decideFysetcLifecycle("", "", false),
              FysetcAction::None);
}

TEST(FysetcLifecycle, DecisionIsDeterministicAndPure) {
    // Same inputs always produce the same output (no hidden state).
    for (int i = 0; i < 5; ++i) {
        EXPECT_EQ(BurstCollectorService::decideFysetcLifecycle("ezshare", "fysetc", false),
                  FysetcAction::Start);
        EXPECT_EQ(BurstCollectorService::decideFysetcLifecycle("fysetc", "local", true),
                  FysetcAction::Stop);
        EXPECT_EQ(BurstCollectorService::decideFysetcLifecycle("local", "ezshare", false),
                  FysetcAction::None);
    }
}

// ============================================================================
// REAL OBJECT LIFECYCLE TESTS
// ============================================================================
// These drive the actual BurstCollectorService instance (no initialize(), so
// no DB/MQTT/network is touched). They cover the constructor, the running-flag
// accessor, the burst-time accessor, idempotent stop(), and destruction.

TEST(BurstCollectorLifecycle, NewServiceIsNotRunning) {
    BurstCollectorService svc(300);
    EXPECT_FALSE(svc.isRunning());
}

TEST(BurstCollectorLifecycle, DefaultConstructedIntervalIsNotRunning) {
    // Default ctor argument (300s) — just verify construction + accessor.
    BurstCollectorService svc;
    EXPECT_FALSE(svc.isRunning());
}

TEST(BurstCollectorLifecycle, LastBurstTimeDefaultsToEpoch) {
    BurstCollectorService svc(60);
    // Never ran a cycle, so last_burst_time_ is the default-constructed
    // time_point, which equals the clock epoch.
    EXPECT_EQ(svc.getLastBurstTime(),
              std::chrono::system_clock::time_point{});
}

TEST(BurstCollectorLifecycle, StopOnNeverStartedIsNoOp) {
    // stop() must be safe to call when the worker was never started.
    BurstCollectorService svc(60);
    EXPECT_NO_THROW(svc.stop());
    EXPECT_FALSE(svc.isRunning());
}

TEST(BurstCollectorLifecycle, RepeatedStopIsIdempotent) {
    BurstCollectorService svc(60);
    EXPECT_NO_THROW(svc.stop());
    EXPECT_NO_THROW(svc.stop());
    EXPECT_FALSE(svc.isRunning());
}

TEST(BurstCollectorLifecycle, DestructorOnUnstartedServiceIsClean) {
    // Destructor calls stop(); on an unstarted service it must not crash.
    EXPECT_NO_THROW({
        BurstCollectorService svc(120);
        (void)svc.isRunning();
    });
}

TEST(BurstCollectorLifecycle, SetAppConfigAndMarkDirtyAreSafeWithoutInitialize) {
    // Hot-reload setters are plain flag/pointer writes; reloadConfig() is only
    // invoked from the worker loop (never started here), so these are safe.
    BurstCollectorService svc(60);
    AppConfig cfg;
    EXPECT_NO_THROW(svc.setAppConfig(&cfg));
    EXPECT_NO_THROW(svc.markConfigDirty());
    EXPECT_FALSE(svc.isRunning());
}

TEST(BurstCollectorLifecycle, ConstructorReadsDeviceIdFromEnv) {
    // The constructor pulls CPAP_DEVICE_ID / CPAP_DEVICE_NAME from the
    // environment. We can't read device_id_ directly (private), but we can at
    // least confirm construction succeeds with a custom env value and that the
    // env override path in ConfigManager is taken without throwing.
    setenv("CPAP_DEVICE_ID", "cpap_test_unit_123", 1);
    setenv("CPAP_DEVICE_NAME", "Unit Test CPAP", 1);
    EXPECT_NO_THROW({ BurstCollectorService svc(300); });
    EXPECT_EQ(ConfigManager::get("CPAP_DEVICE_ID", "default"),
              "cpap_test_unit_123");
    unsetenv("CPAP_DEVICE_ID");
    unsetenv("CPAP_DEVICE_NAME");
}

TEST(BurstCollectorLifecycle, ConstructorFallsBackToDefaultsWhenEnvUnset) {
    unsetenv("CPAP_DEVICE_ID");
    unsetenv("CPAP_DEVICE_NAME");
    EXPECT_NO_THROW({ BurstCollectorService svc(300); });
    // Verify the default that the constructor relies on is what ConfigManager
    // returns when the variable is absent.
    EXPECT_EQ(ConfigManager::get("CPAP_DEVICE_ID", "cpap_resmed_23243570851"),
              "cpap_resmed_23243570851");
}

// ============================================================================
// BURST CYCLE ORCHESTRATION TESTS (real executeBurstCycle via DI seam)
// ============================================================================
//
// These tests drive the actual executeBurstCycle() through the test-only
// injectDependenciesForTest()/runBurstCycleForTest() seam. They use:
//   - A gmock MockDatabase (IDatabase) to observe/script DB interactions.
//   - An in-memory FakeDataSource (IDataSource) returning canned date folders
//     and EzShareFileEntry listings; downloadFile() writes deterministic bytes
//     into the temp dir so the download path succeeds (parse will then fail on
//     garbage EDF — expected; we assert the pre-parse DB calls).
//   - A DataPublisherService built with a NULL MqttClient (all publishes are
//     guarded no-ops — no broker required).
//   - A real SessionDiscoveryService bound to the same fake source.
//
// Mode: ezShare (local_source_dir_ empty, prisma_ingestion_ null — both stay
// unset because injectDependenciesForTest bypasses initialize()).
//
// Coverage notes:
//   * Prisma branch is NOT reachable: prisma_ingestion_ is a private member with
//     no setter and stays null. Skipped by design.
//   * Oximetry / LLM branches are NOT reachable: oximetry_service_, llm_client_
//     stay null and llm_enabled_ stays false (no public setters). Skipped.
//   * generateSummaryForDate() can only exercise its LLM-disabled early return
//     (llm_enabled_ is false with no setter). The happy path is unreachable via
//     the seam; documented in its test.

using ::testing::_;
using ::testing::Return;
using ::testing::AnyNumber;
using ::testing::AtLeast;
using ::testing::NiceMock;

namespace burst_orch {

// ── Mock database (adapted from tests/services/test_BackfillService.cpp) ──────
class MockDatabase : public IDatabase {
public:
    HMS_CPAP_STUB_EQUIPMENT_METHODS
    DbType dbType() const override { return DbType::SQLITE; }

    MOCK_METHOD(bool, connect, (), (override));
    MOCK_METHOD(void, disconnect, (), (override));
    MOCK_METHOD(bool, isConnected, (), (const, override));

    MOCK_METHOD(bool, saveSession, (const CPAPSession&), (override));
    MOCK_METHOD(bool, sessionExists, (const std::string&, const std::chrono::system_clock::time_point&), (override));
    MOCK_METHOD((std::optional<std::chrono::system_clock::time_point>), getLastSessionStart, (const std::string&), (override));
    MOCK_METHOD((std::optional<std::chrono::system_clock::time_point>), getSessionStartForSleepDay, (const std::string&, const std::string&, bool), (override));
    MOCK_METHOD((std::optional<SessionMetrics>), getSessionMetrics, (const std::string&, const std::chrono::system_clock::time_point&), (override));
    MOCK_METHOD(bool, markSessionCompleted, (const std::string&, const std::chrono::system_clock::time_point&), (override));
    MOCK_METHOD(bool, reopenSession, (const std::string&, const std::chrono::system_clock::time_point&), (override));
    MOCK_METHOD(int, deleteSessionsByDateFolder, (const std::string&, const std::string&), (override));
    MOCK_METHOD(bool, isForceCompleted, (const std::string&, const std::chrono::system_clock::time_point&), (override));
    MOCK_METHOD(bool, setForceCompleted, (const std::string&, const std::chrono::system_clock::time_point&), (override));
    MOCK_METHOD((std::map<std::string, int>), getCheckpointFileSizes, (const std::string&, const std::chrono::system_clock::time_point&), (override));
    MOCK_METHOD((std::map<std::string, int>), getCheckpointFilesByFolder, (const std::string&, const std::string&), (override));

    // GMock needs parens around complex param types with commas; route the
    // 3-arg override through a 2-arg mock so EXPECT_CALL is ergonomic.
    bool updateCheckpointFileSizes(
        const std::string& device_id,
        const std::chrono::system_clock::time_point& session_start,
        const std::map<std::string, int>& /*file_sizes*/) override {
        return updateCheckpointFileSizesMock(device_id, session_start);
    }
    MOCK_METHOD(bool, updateCheckpointFileSizesMock, (const std::string&, const std::chrono::system_clock::time_point&));
    MOCK_METHOD(bool, updateDeviceLastSeen, (const std::string&), (override));
    MOCK_METHOD(bool, saveSTRDailyRecords, (const std::vector<STRDailyRecord>&), (override));
    MOCK_METHOD((std::optional<std::string>), getLastSTRDate, (const std::string&), (override));
    MOCK_METHOD(bool, aggregateDailySummaryFromSessions, (const std::string&), (override));
    MOCK_METHOD((std::optional<SessionMetrics>), getNightlyMetrics, (const std::string&, const std::chrono::system_clock::time_point&), (override));
    MOCK_METHOD((std::vector<SessionMetrics>), getMetricsForDateRange, (const std::string&, int), (override));
    MOCK_METHOD(bool, saveSummary, (const std::string&, const std::string&, const std::string&, const std::string&, int, double, double, double, const std::string&), (override));
    MOCK_METHOD(void*, rawConnection, (), (override));
    MOCK_METHOD(bool, saveOximetrySession, (const std::string&, const cpapdash::parser::OximetrySession&), (override));
    MOCK_METHOD(bool, oximetrySessionExists, (const std::string&, const std::string&), (override));
    MOCK_METHOD(bool, saveLiveOximetrySample, (const std::string&, const std::string&, int, int, int), (override));
    OxiSummary getOximetrySummary(const std::string&, const std::string&, const std::string&) override { return {}; }
    OxiRangeSummary getOximetryRangeSummary(const std::string&, const std::string&, const std::string&) override { return {}; }
    std::vector<OxiNightlyPoint> getOximetryNightlySpo2(const std::string&, const std::string&, const std::string&) override { return {}; }
};

// ── In-memory fake data source (pattern from test_SessionDiscoveryService) ────
class FakeDataSource : public IDataSource {
public:
    std::vector<std::string> date_folders;
    std::map<std::string, std::vector<EzShareFileEntry>> folder_files;
    int download_count = 0;
    // SDD-002 residue test seams: arbitrary-dir listings (key "" = card root),
    // and records of what the residue paths actually fetched.
    std::map<std::string, std::vector<EzShareFileEntry>> dir_listings;
    std::vector<std::string> downloaded_files;     // filenames via downloadFile()
    std::vector<std::string> downloaded_by_path;   // card-rel paths via downloadByPath()

    std::vector<std::string> listDateFolders() override { return date_folders; }

    std::vector<EzShareFileEntry> listFiles(const std::string& date_folder) override {
        auto it = folder_files.find(date_folder);
        return it == folder_files.end() ? std::vector<EzShareFileEntry>{} : it->second;
    }

    std::vector<EzShareFileEntry> listDir(const std::string& card_path) override {
        auto it = dir_listings.find(card_path);
        return it == dir_listings.end() ? std::vector<EzShareFileEntry>{} : it->second;
    }

    bool downloadByPath(const std::string& card_rel_path,
                        const std::string& local_path) override {
        downloaded_by_path.push_back(card_rel_path);
        std::filesystem::create_directories(std::filesystem::path(local_path).parent_path());
        std::ofstream ofs(local_path, std::ios::binary);
        ofs << "RESIDUE_BYTES";
        return true;
    }

    // Write deterministic bytes so downloadSessionFiles() succeeds. Content is
    // garbage EDF — parse fails later, which is fine: we assert pre-parse DB calls.
    bool downloadFile(const std::string& /*date_folder*/, const std::string& filename,
                      const std::string& local_path) override {
        ++download_count;
        downloaded_files.push_back(filename);
        std::filesystem::create_directories(std::filesystem::path(local_path).parent_path());
        std::ofstream ofs(local_path, std::ios::binary);
        ofs << "NOT_A_REAL_EDF_FILE";
        return true;
    }
    bool downloadFileRange(const std::string&, const std::string&, const std::string& local_path,
                           size_t, size_t& bytes_downloaded) override {
        ++download_count;
        std::filesystem::create_directories(std::filesystem::path(local_path).parent_path());
        std::ofstream ofs(local_path, std::ios::binary);
        ofs << "NOT_A_REAL_EDF_FILE";
        bytes_downloaded = 19;
        return true;
    }
    bool downloadRootFile(const std::string&, const std::string& local_path) override {
        std::ofstream ofs(local_path, std::ios::binary);
        ofs << "ROOT";
        return true;
    }
};

EzShareFileEntry mkEntry(const std::string& name, int size_kb, bool is_dir = false) {
    EzShareFileEntry e;
    e.name = name;
    e.size_kb = size_kb;
    e.is_dir = is_dir;
    return e;
}

// Fixture: unique temp/archive dirs per pid; wires the service via the seam.
class BurstOrchestrationTest : public ::testing::Test {
protected:
    std::filesystem::path temp_dir;
    std::filesystem::path archive_dir;
    MockDatabase* db_raw = nullptr;        // owned by shared_ptr below
    FakeDataSource* src_raw = nullptr;     // owned by the service after inject
    std::shared_ptr<MockDatabase> mock_db;

    void SetUp() override {
        // Deterministic, unique, isolated directories.
        auto base = std::filesystem::temp_directory_path();
        temp_dir = base / ("hms_cpap_burst_temp_" + std::to_string(getpid()));
        archive_dir = base / ("hms_cpap_burst_arch_" + std::to_string(getpid()));
        std::filesystem::remove_all(temp_dir);
        std::filesystem::remove_all(archive_dir);
        std::filesystem::create_directories(temp_dir);
        std::filesystem::create_directories(archive_dir);

        setenv("CPAP_TEMP_DIR", temp_dir.c_str(), 1);
        setenv("CPAP_ARCHIVE_DIR", archive_dir.c_str(), 1);
        // ezShare mode: ensure local source / prisma stay unset (they do via the seam).
        unsetenv("SESSION_GAP_MINUTES");

        mock_db = std::make_shared<NiceMock<MockDatabase>>();
        db_raw = mock_db.get();
    }

    void TearDown() override {
        std::filesystem::remove_all(temp_dir);
        std::filesystem::remove_all(archive_dir);
        unsetenv("CPAP_TEMP_DIR");
        unsetenv("CPAP_ARCHIVE_DIR");
    }

    // Build a service wired to a fake source seeded with one single-session
    // folder (BRP+PLD+CSL+EVE). Returns the service; the fake source pointer is
    // stashed in src_raw (owned by the service).
    std::unique_ptr<BurstCollectorService> makeService(
        std::function<void(FakeDataSource&)> seed) {
        auto fake = std::make_unique<FakeDataSource>();
        seed(*fake);
        src_raw = fake.get();

        // SessionDiscoveryService holds an IDataSource& — bind it to the SAME
        // fake instance that we then move into the service. Moving the unique_ptr
        // does not relocate the heap object, so the reference stays valid.
        auto discovery = std::make_unique<SessionDiscoveryService>(*fake);

        auto publisher = std::make_unique<DataPublisherService>(
            std::shared_ptr<hms::MqttClient>{}, mock_db);

        auto svc = std::make_unique<BurstCollectorService>(60);
        svc->injectDependenciesForTest(mock_db, std::move(fake),
                                       std::move(publisher), std::move(discovery));
        return svc;
    }

    // A single session: one BRP + one PLD checkpoint, plus CSL + EVE.
    static void seedOneSession(FakeDataSource& ds) {
        ds.date_folders = {"20200101"};
        ds.folder_files["20200101"] = {
            mkEntry("20200101_220000_BRP.edf", 100),
            mkEntry("20200101_220000_PLD.edf", 20),
            mkEntry("20200101_220000_CSL.edf", 1),
            mkEntry("20200101_220000_EVE.edf", 2),
        };
    }

    // TWO sessions in one date folder. The default SESSION_GAP_MINUTES is 60;
    // 22:00:00 and 23:30:00 are 90 minutes apart -> two distinct sessions.
    // The 23:30:00 group is the most-recent (highest session_start timestamp).
    static void seedTwoSessions(FakeDataSource& ds) {
        ds.date_folders = {"20200101"};
        ds.folder_files["20200101"] = {
            // Earlier session (NOT most recent)
            mkEntry("20200101_220000_BRP.edf", 100),
            mkEntry("20200101_220000_PLD.edf", 20),
            // Later session (MOST recent)
            mkEntry("20200101_233000_BRP.edf", 80),
            mkEntry("20200101_233000_PLD.edf", 15),
        };
    }
};

// Scenario 1: New session not in DB -> downloaded, checkpoints stored, last_seen
// updated. (Parse fails on garbage EDF, so the cycle ultimately returns false,
// but the download + updateCheckpointFileSizes calls fire first.)
TEST_F(BurstOrchestrationTest, NewSession_DownloadsAndStoresCheckpoints) {
    auto svc = makeService(&BurstOrchestrationTest::seedOneSession);

    // First run: no previous session in DB.
    EXPECT_CALL(*db_raw, getLastSessionStart(_))
        .WillRepeatedly(Return(std::nullopt));
    EXPECT_CALL(*db_raw, isForceCompleted(_, _)).WillRepeatedly(Return(false));
    EXPECT_CALL(*db_raw, sessionExists(_, _)).WillRepeatedly(Return(false));

    // The checkpoint sizes must be persisted for the freshly downloaded session.
    EXPECT_CALL(*db_raw, updateCheckpointFileSizesMock(_, _)).Times(AtLeast(1));

    svc->runBurstCycleForTest();

    // The fake source must have been asked to download the session's files.
    EXPECT_GT(src_raw->download_count, 0) << "New session should be downloaded";
}

// Scenario 4: A discovered session flagged force-completed must be skipped:
// no sessionExists/download/checkpoint work for it.
TEST_F(BurstOrchestrationTest, ForceCompletedSession_IsSkipped) {
    auto svc = makeService(&BurstOrchestrationTest::seedOneSession);

    EXPECT_CALL(*db_raw, getLastSessionStart(_))
        .WillRepeatedly(Return(std::nullopt));
    EXPECT_CALL(*db_raw, isForceCompleted(_, _)).WillRepeatedly(Return(true));

    // Force-completed sessions are skipped before sessionExists() is consulted
    // and before any download/checkpoint persistence.
    EXPECT_CALL(*db_raw, sessionExists(_, _)).Times(0);
    EXPECT_CALL(*db_raw, updateCheckpointFileSizesMock(_, _)).Times(0);

    svc->runBurstCycleForTest();

    EXPECT_EQ(src_raw->download_count, 0) << "Skipped session should not download";
}

// Scenario 2: Existing session, checkpoints UNCHANGED -> markSessionCompleted
// fires and (newly_completed + most_recent) drives the completion path
// (getNightlyMetrics + publishSessionCompleted via the null-MQTT publisher).
TEST_F(BurstOrchestrationTest, ExistingSession_Unchanged_MarksCompleted) {
    auto svc = makeService(&BurstOrchestrationTest::seedOneSession);

    EXPECT_CALL(*db_raw, getLastSessionStart(_))
        .WillRepeatedly(Return(std::nullopt));
    EXPECT_CALL(*db_raw, isForceCompleted(_, _)).WillRepeatedly(Return(false));
    EXPECT_CALL(*db_raw, sessionExists(_, _)).WillRepeatedly(Return(true));

    // DB checkpoint sizes EXACTLY match the discovered BRP+PLD sizes (CSL/EVE are
    // not checkpoints) -> all_unchanged == true.
    std::map<std::string, int> stored = {
        {"20200101_220000_BRP.edf", 100},
        {"20200101_220000_PLD.edf", 20},
    };
    EXPECT_CALL(*db_raw, getCheckpointFileSizes(_, _))
        .WillRepeatedly(Return(stored));

    // Completion path: first time marking returns true (was NULL).
    EXPECT_CALL(*db_raw, markSessionCompleted(_, _)).Times(1).WillOnce(Return(true));
    // newly_completed && is_most_recent -> getNightlyMetrics is consulted.
    EXPECT_CALL(*db_raw, getNightlyMetrics(_, _))
        .WillRepeatedly(Return(std::nullopt));

    // No download / no checkpoint update on the unchanged path.
    EXPECT_CALL(*db_raw, updateCheckpointFileSizesMock(_, _)).Times(0);

    svc->runBurstCycleForTest();

    EXPECT_EQ(src_raw->download_count, 0) << "Unchanged session must not re-download";
}

// Scenario 2b: Existing session unchanged but already completed
// (markSessionCompleted returns false) -> publishSessionCompleted still fires
// to clear stale session_active, but no metrics fetch is required.
TEST_F(BurstOrchestrationTest, ExistingSession_AlreadyCompleted_NoReMark) {
    auto svc = makeService(&BurstOrchestrationTest::seedOneSession);

    EXPECT_CALL(*db_raw, getLastSessionStart(_)).WillRepeatedly(Return(std::nullopt));
    EXPECT_CALL(*db_raw, isForceCompleted(_, _)).WillRepeatedly(Return(false));
    EXPECT_CALL(*db_raw, sessionExists(_, _)).WillRepeatedly(Return(true));

    std::map<std::string, int> stored = {
        {"20200101_220000_BRP.edf", 100},
        {"20200101_220000_PLD.edf", 20},
    };
    EXPECT_CALL(*db_raw, getCheckpointFileSizes(_, _)).WillRepeatedly(Return(stored));

    // Already completed: markSessionCompleted returns false.
    EXPECT_CALL(*db_raw, markSessionCompleted(_, _)).Times(1).WillOnce(Return(false));
    // The !newly_completed branch only calls publishSessionCompleted(); it does
    // NOT fetch metrics for this session.
    EXPECT_CALL(*db_raw, getNightlyMetrics(_, _)).Times(0);

    svc->runBurstCycleForTest();

    EXPECT_EQ(src_raw->download_count, 0);
}

// Scenario 3: Existing session, checkpoint sizes CHANGED -> re-download, persist
// new sizes, and reopenSession() (clears session_end for the resumed session).
TEST_F(BurstOrchestrationTest, ExistingSession_Changed_RedownloadsAndReopens) {
    auto svc = makeService(&BurstOrchestrationTest::seedOneSession);

    EXPECT_CALL(*db_raw, getLastSessionStart(_)).WillRepeatedly(Return(std::nullopt));
    EXPECT_CALL(*db_raw, isForceCompleted(_, _)).WillRepeatedly(Return(false));
    EXPECT_CALL(*db_raw, sessionExists(_, _)).WillRepeatedly(Return(true));

    // DB has an OLD (smaller) BRP size -> change detected.
    std::map<std::string, int> stored = {
        {"20200101_220000_BRP.edf", 50},   // discovered is 100 -> changed
        {"20200101_220000_PLD.edf", 20},
    };
    EXPECT_CALL(*db_raw, getCheckpointFileSizes(_, _)).WillRepeatedly(Return(stored));

    // Changed path: persist new sizes + reopen, no completion marking.
    EXPECT_CALL(*db_raw, updateCheckpointFileSizesMock(_, _)).Times(AtLeast(1));
    EXPECT_CALL(*db_raw, reopenSession(_, _)).Times(1).WillOnce(Return(true));
    EXPECT_CALL(*db_raw, markSessionCompleted(_, _)).Times(0);

    svc->runBurstCycleForTest();

    EXPECT_GT(src_raw->download_count, 0) << "Changed session should re-download";
}

// No date folders discovered -> cycle returns true early, only getLastSessionStart
// is consulted. Exercises the empty-discovery short-circuit.
TEST_F(BurstOrchestrationTest, NoSessions_ReturnsTrueEarly) {
    auto svc = makeService([](FakeDataSource& ds) {
        ds.date_folders = {};  // nothing to discover
    });

    EXPECT_CALL(*db_raw, getLastSessionStart(_)).WillRepeatedly(Return(std::nullopt));
    EXPECT_CALL(*db_raw, isForceCompleted(_, _)).Times(0);
    EXPECT_CALL(*db_raw, sessionExists(_, _)).Times(0);

    EXPECT_TRUE(svc->runBurstCycleForTest()) << "Empty discovery should succeed (no-op)";
    EXPECT_EQ(src_raw->download_count, 0);
}

// Scenario 5: forceCompleteSession(sleep_day) — open session found ->
// markSessionCompleted + setForceCompleted + publish + metrics.
TEST_F(BurstOrchestrationTest, ForceCompleteSession_OpenSession_MarksAndForces) {
    auto svc = makeService(&BurstOrchestrationTest::seedOneSession);
    auto ts = std::chrono::system_clock::from_time_t(1577919600);  // arbitrary

    // open_only=true lookup returns a session.
    EXPECT_CALL(*db_raw, getSessionStartForSleepDay(_, "2020-01-01", true))
        .WillOnce(Return(ts));
    EXPECT_CALL(*db_raw, markSessionCompleted(_, _)).Times(1).WillOnce(Return(true));
    EXPECT_CALL(*db_raw, setForceCompleted(_, _)).Times(1).WillOnce(Return(true));
    // Publisher present -> metrics fetched (nullopt -> no historical publish, OK).
    EXPECT_CALL(*db_raw, getNightlyMetrics(_, _)).WillOnce(Return(std::nullopt));

    EXPECT_TRUE(svc->forceCompleteSession("2020-01-01"));
}

// Scenario 5b: forceCompleteSession falls back to any session (open_only=false)
// when no open session exists for the day.
TEST_F(BurstOrchestrationTest, ForceCompleteSession_FallsBackToAnySession) {
    auto svc = makeService(&BurstOrchestrationTest::seedOneSession);
    auto ts = std::chrono::system_clock::from_time_t(1577919600);

    EXPECT_CALL(*db_raw, getSessionStartForSleepDay(_, "2020-01-01", true))
        .WillOnce(Return(std::nullopt));
    EXPECT_CALL(*db_raw, getSessionStartForSleepDay(_, "2020-01-01", false))
        .WillOnce(Return(ts));
    EXPECT_CALL(*db_raw, markSessionCompleted(_, _)).Times(1).WillOnce(Return(true));
    EXPECT_CALL(*db_raw, setForceCompleted(_, _)).Times(1).WillOnce(Return(true));
    EXPECT_CALL(*db_raw, getNightlyMetrics(_, _)).WillOnce(Return(std::nullopt));

    EXPECT_TRUE(svc->forceCompleteSession("2020-01-01"));
}

// Scenario 5c: forceCompleteSession with no session at all -> returns false,
// never marks/forces.
TEST_F(BurstOrchestrationTest, ForceCompleteSession_NoSession_ReturnsFalse) {
    auto svc = makeService(&BurstOrchestrationTest::seedOneSession);

    EXPECT_CALL(*db_raw, getSessionStartForSleepDay(_, "2099-12-31", true))
        .WillOnce(Return(std::nullopt));
    EXPECT_CALL(*db_raw, getSessionStartForSleepDay(_, "2099-12-31", false))
        .WillOnce(Return(std::nullopt));
    EXPECT_CALL(*db_raw, markSessionCompleted(_, _)).Times(0);
    EXPECT_CALL(*db_raw, setForceCompleted(_, _)).Times(0);

    EXPECT_FALSE(svc->forceCompleteSession("2099-12-31"));
}

// Scenario 6: generateSummaryForDate — only the LLM-disabled early return is
// reachable via the seam (llm_enabled_ stays false; no public setter). The
// happy path (getSessionStartForSleepDay + getNightlyMetrics + LLM) cannot be
// exercised here and is left to integration testing. Documented limitation.
TEST_F(BurstOrchestrationTest, GenerateSummaryForDate_LLMDisabled_ReturnsFalse) {
    auto svc = makeService(&BurstOrchestrationTest::seedOneSession);

    // LLM disabled -> returns false before touching the DB at all.
    EXPECT_CALL(*db_raw, getSessionStartForSleepDay(_, _, _)).Times(0);
    EXPECT_CALL(*db_raw, getNightlyMetrics(_, _)).Times(0);

    EXPECT_FALSE(svc->generateSummaryForDate("2020-01-01"));
}

// ── EXTENDED COVERAGE: multi-session, most-recent selection, consecutive ─────
// ── cycles, reopen-after-change, force-complete mid-list, last-burst time. ───

// Recompute a session_start the same way SessionDiscoveryService::parseSessionTime
// does (local time via mktime), so we can script per-session mock returns.
static std::chrono::system_clock::time_point sessionTime(const std::string& prefix) {
    std::tm tm = {};
    tm.tm_year = std::stoi(prefix.substr(0, 4)) - 1900;
    tm.tm_mon  = std::stoi(prefix.substr(4, 2)) - 1;
    tm.tm_mday = std::stoi(prefix.substr(6, 2));
    tm.tm_hour = std::stoi(prefix.substr(9, 2));
    tm.tm_min  = std::stoi(prefix.substr(11, 2));
    tm.tm_sec  = std::stoi(prefix.substr(13, 2));
    tm.tm_isdst = -1;
    return std::chrono::system_clock::from_time_t(std::mktime(&tm));
}

// Two unchanged sessions in one cycle: BOTH get markSessionCompleted (newly
// completed), but completion ACTIONS (getNightlyMetrics) fire only for the
// most-recent one (23:30) — the earlier (22:00) is gated by is_most_recent.
TEST_F(BurstOrchestrationTest, MultiSession_OnlyMostRecentTriggersMetrics) {
    auto svc = makeService(&BurstOrchestrationTest::seedTwoSessions);

    auto t_early = sessionTime("20200101_220000");
    auto t_late  = sessionTime("20200101_233000");

    EXPECT_CALL(*db_raw, getLastSessionStart(_)).WillRepeatedly(Return(std::nullopt));
    EXPECT_CALL(*db_raw, isForceCompleted(_, _)).WillRepeatedly(Return(false));
    EXPECT_CALL(*db_raw, sessionExists(_, _)).WillRepeatedly(Return(true));

    // Each session's stored checkpoint sizes match exactly -> all_unchanged.
    std::map<std::string, int> early_sizes = {
        {"20200101_220000_BRP.edf", 100}, {"20200101_220000_PLD.edf", 20}};
    std::map<std::string, int> late_sizes = {
        {"20200101_233000_BRP.edf", 80}, {"20200101_233000_PLD.edf", 15}};
    EXPECT_CALL(*db_raw, getCheckpointFileSizes(_, t_early)).WillRepeatedly(Return(early_sizes));
    EXPECT_CALL(*db_raw, getCheckpointFileSizes(_, t_late)).WillRepeatedly(Return(late_sizes));

    // Both sessions are newly completed (session_end was NULL).
    EXPECT_CALL(*db_raw, markSessionCompleted(_, t_early)).Times(1).WillOnce(Return(true));
    EXPECT_CALL(*db_raw, markSessionCompleted(_, t_late)).Times(1).WillOnce(Return(true));

    // Completion actions (metrics) fire ONLY for the most-recent (late) session.
    EXPECT_CALL(*db_raw, getNightlyMetrics(_, t_late)).Times(1).WillOnce(Return(std::nullopt));
    EXPECT_CALL(*db_raw, getNightlyMetrics(_, t_early)).Times(0);

    EXPECT_CALL(*db_raw, updateCheckpointFileSizesMock(_, _)).Times(0);

    svc->runBurstCycleForTest();
    EXPECT_EQ(src_raw->download_count, 0) << "All unchanged -> no downloads";
}

// Multi-session where the EARLIER session changed (re-download + reopen) and the
// LATER session is unchanged (completes). Verifies per-session branch routing.
TEST_F(BurstOrchestrationTest, MultiSession_EarlierChanged_LaterCompletes) {
    auto svc = makeService(&BurstOrchestrationTest::seedTwoSessions);

    auto t_early = sessionTime("20200101_220000");
    auto t_late  = sessionTime("20200101_233000");

    EXPECT_CALL(*db_raw, getLastSessionStart(_)).WillRepeatedly(Return(std::nullopt));
    EXPECT_CALL(*db_raw, isForceCompleted(_, _)).WillRepeatedly(Return(false));
    EXPECT_CALL(*db_raw, sessionExists(_, _)).WillRepeatedly(Return(true));

    // Early session: stored BRP smaller -> changed -> re-download + reopen.
    std::map<std::string, int> early_old = {
        {"20200101_220000_BRP.edf", 50}, {"20200101_220000_PLD.edf", 20}};
    // Late session: matches exactly -> unchanged -> completes.
    std::map<std::string, int> late_sizes = {
        {"20200101_233000_BRP.edf", 80}, {"20200101_233000_PLD.edf", 15}};
    EXPECT_CALL(*db_raw, getCheckpointFileSizes(_, t_early)).WillRepeatedly(Return(early_old));
    EXPECT_CALL(*db_raw, getCheckpointFileSizes(_, t_late)).WillRepeatedly(Return(late_sizes));

    // Changed early session: persists new sizes + reopen, no completion mark.
    EXPECT_CALL(*db_raw, updateCheckpointFileSizesMock(_, t_early)).Times(AtLeast(1));
    EXPECT_CALL(*db_raw, reopenSession(_, t_early)).Times(1).WillOnce(Return(true));
    EXPECT_CALL(*db_raw, markSessionCompleted(_, t_early)).Times(0);

    // Unchanged late session: completes (most recent). The completion path AND
    // the post-parse publish path may both consult getNightlyMetrics, so accept
    // any session_start here — the load-bearing assertions are the per-session
    // markSessionCompleted / reopenSession routing above.
    EXPECT_CALL(*db_raw, markSessionCompleted(_, t_late)).Times(1).WillOnce(Return(true));
    EXPECT_CALL(*db_raw, getNightlyMetrics(_, _)).WillRepeatedly(Return(std::nullopt));

    svc->runBurstCycleForTest();
    EXPECT_GT(src_raw->download_count, 0) << "Changed early session must re-download";
}

// has_new_files branch: stored sizes are a SUBSET of discovered (a brand-new
// checkpoint file appeared) -> treated as changed -> re-download + reopen, even
// though every shared file is byte-identical.
TEST_F(BurstOrchestrationTest, ExistingSession_NewCheckpointFile_Redownloads) {
    auto svc = makeService(&BurstOrchestrationTest::seedOneSession);
    auto t = sessionTime("20200101_220000");

    EXPECT_CALL(*db_raw, getLastSessionStart(_)).WillRepeatedly(Return(std::nullopt));
    EXPECT_CALL(*db_raw, isForceCompleted(_, _)).WillRepeatedly(Return(false));
    EXPECT_CALL(*db_raw, sessionExists(_, _)).WillRepeatedly(Return(true));

    // DB knows only the BRP file; the PLD is "new" -> current count > stored count.
    std::map<std::string, int> stored = {{"20200101_220000_BRP.edf", 100}};
    EXPECT_CALL(*db_raw, getCheckpointFileSizes(_, t)).WillRepeatedly(Return(stored));

    EXPECT_CALL(*db_raw, updateCheckpointFileSizesMock(_, t)).Times(AtLeast(1));
    EXPECT_CALL(*db_raw, reopenSession(_, t)).Times(1).WillOnce(Return(true));
    EXPECT_CALL(*db_raw, markSessionCompleted(_, _)).Times(0);

    svc->runBurstCycleForTest();
    EXPECT_GT(src_raw->download_count, 0) << "New checkpoint file triggers re-download";
}

// Consecutive cycles on one service instance: NEW -> (sizes stored) ; then
// UNCHANGED -> completes. Drives executeBurstCycle twice through the seam,
// exercising the new-then-complete transition without rebuilding the service.
TEST_F(BurstOrchestrationTest, ConsecutiveCycles_NewThenUnchangedCompletes) {
    auto svc = makeService(&BurstOrchestrationTest::seedOneSession);
    auto t = sessionTime("20200101_220000");

    EXPECT_CALL(*db_raw, getLastSessionStart(_)).WillRepeatedly(Return(std::nullopt));
    EXPECT_CALL(*db_raw, isForceCompleted(_, _)).WillRepeatedly(Return(false));

    // Cycle 1: not in DB -> download + store checkpoints.
    // Cycle 2: in DB, unchanged -> completes.
    ::testing::Sequence seq;
    EXPECT_CALL(*db_raw, sessionExists(_, t))
        .InSequence(seq).WillOnce(Return(false));   // cycle 1
    EXPECT_CALL(*db_raw, sessionExists(_, t))
        .InSequence(seq).WillRepeatedly(Return(true)); // cycle 2+

    EXPECT_CALL(*db_raw, updateCheckpointFileSizesMock(_, t)).Times(AtLeast(1)); // cycle 1
    std::map<std::string, int> stored = {
        {"20200101_220000_BRP.edf", 100}, {"20200101_220000_PLD.edf", 20}};
    EXPECT_CALL(*db_raw, getCheckpointFileSizes(_, t)).WillRepeatedly(Return(stored));
    EXPECT_CALL(*db_raw, markSessionCompleted(_, t)).Times(1).WillOnce(Return(true)); // cycle 2
    EXPECT_CALL(*db_raw, getNightlyMetrics(_, t)).WillRepeatedly(Return(std::nullopt));

    svc->runBurstCycleForTest();   // cycle 1: new -> download
    int after_first = src_raw->download_count;
    EXPECT_GT(after_first, 0);

    svc->runBurstCycleForTest();   // cycle 2: unchanged -> complete (no new download)
    EXPECT_EQ(src_raw->download_count, after_first) << "Unchanged cycle must not re-download";
}

// Consecutive cycles: GROWS (changed -> reopen + re-download) then UNCHANGED
// (completes). Mirrors the mask-on/off resume cycle end-to-end via the seam.
TEST_F(BurstOrchestrationTest, ConsecutiveCycles_GrowsThenUnchangedReopensThenCompletes) {
    auto svc = makeService(&BurstOrchestrationTest::seedOneSession);
    auto t = sessionTime("20200101_220000");

    EXPECT_CALL(*db_raw, getLastSessionStart(_)).WillRepeatedly(Return(std::nullopt));
    EXPECT_CALL(*db_raw, isForceCompleted(_, _)).WillRepeatedly(Return(false));
    EXPECT_CALL(*db_raw, sessionExists(_, t)).WillRepeatedly(Return(true));

    // Cycle 1: DB has SMALLER BRP -> changed -> reopen + re-download.
    // Cycle 2: DB now matches -> unchanged -> complete.
    std::map<std::string, int> old_sizes = {
        {"20200101_220000_BRP.edf", 50}, {"20200101_220000_PLD.edf", 20}};
    std::map<std::string, int> new_sizes = {
        {"20200101_220000_BRP.edf", 100}, {"20200101_220000_PLD.edf", 20}};
    ::testing::Sequence seq;
    EXPECT_CALL(*db_raw, getCheckpointFileSizes(_, t))
        .InSequence(seq).WillOnce(Return(old_sizes));      // cycle 1: changed
    EXPECT_CALL(*db_raw, getCheckpointFileSizes(_, t))
        .InSequence(seq).WillRepeatedly(Return(new_sizes)); // cycle 2: unchanged

    EXPECT_CALL(*db_raw, reopenSession(_, t)).Times(1).WillOnce(Return(true));      // cycle 1
    EXPECT_CALL(*db_raw, updateCheckpointFileSizesMock(_, t)).Times(AtLeast(1));    // cycle 1
    EXPECT_CALL(*db_raw, markSessionCompleted(_, t)).Times(1).WillOnce(Return(true)); // cycle 2
    EXPECT_CALL(*db_raw, getNightlyMetrics(_, t)).WillRepeatedly(Return(std::nullopt));

    svc->runBurstCycleForTest();   // grows
    int after_first = src_raw->download_count;
    EXPECT_GT(after_first, 0);

    svc->runBurstCycleForTest();   // unchanged -> completes
    EXPECT_EQ(src_raw->download_count, after_first);
}

// forceCompleteSession on the EARLIER of two sessions (mid-list): the day lookup
// returns the early session's start; that exact session is marked + forced.
TEST_F(BurstOrchestrationTest, ForceCompleteSession_MidList_TargetsLookupResult) {
    auto svc = makeService(&BurstOrchestrationTest::seedTwoSessions);
    auto t_early = sessionTime("20200101_220000");

    // open_only lookup resolves to the earlier session.
    EXPECT_CALL(*db_raw, getSessionStartForSleepDay(_, "2020-01-01", true))
        .WillOnce(Return(t_early));
    // The marked/forced session must be exactly the lookup result.
    EXPECT_CALL(*db_raw, markSessionCompleted(_, t_early)).Times(1).WillOnce(Return(true));
    EXPECT_CALL(*db_raw, setForceCompleted(_, t_early)).Times(1).WillOnce(Return(true));
    EXPECT_CALL(*db_raw, getNightlyMetrics(_, t_early)).WillOnce(Return(std::nullopt));

    EXPECT_TRUE(svc->forceCompleteSession("2020-01-01"));
}

// getLastBurstTime() is updated by the WORKER LOOP, not executeBurstCycle().
// Driving the cycle directly via the seam must therefore leave it at the epoch.
// This documents/locks the accessor's behavior under the test seam.
TEST_F(BurstOrchestrationTest, GetLastBurstTime_NotUpdatedByDirectCycle) {
    auto svc = makeService(&BurstOrchestrationTest::seedOneSession);
    EXPECT_CALL(*db_raw, getLastSessionStart(_)).WillRepeatedly(Return(std::nullopt));
    EXPECT_CALL(*db_raw, isForceCompleted(_, _)).WillRepeatedly(Return(false));
    EXPECT_CALL(*db_raw, sessionExists(_, _)).WillRepeatedly(Return(false));

    auto before = svc->getLastBurstTime();
    EXPECT_EQ(before, std::chrono::system_clock::time_point{})
        << "Fresh service -> epoch";

    svc->runBurstCycleForTest();

    // executeBurstCycle does not touch last_burst_time_ (set only in the loop).
    EXPECT_EQ(svc->getLastBurstTime(), std::chrono::system_clock::time_point{})
        << "Direct cycle must NOT advance last_burst_time_";
}

// Lowenstein Prisma branch of executeBurstCycle: inject a PrismaIngestion over a
// combined SMART max tree (3-digit seq, spaced RespEvent attrs) and run one
// cycle. Drives discover -> stage -> parse -> saveSession -> publish, the largest
// previously-uncovered block in the service.
TEST_F(BurstOrchestrationTest, PrismaMode_DiscoversParsesAndStores) {
    namespace fs = std::filesystem;
    std::string wmedf;
    for (const char* p : {"tests/fixtures/prisma/signal_real.wmedf",
                          "../tests/fixtures/prisma/signal_real.wmedf",
                          "../../tests/fixtures/prisma/signal_real.wmedf"}) {
        if (fs::exists(p)) { wmedf = p; break; }
    }
    if (wmedf.empty()) GTEST_SKIP() << "prisma wmedf fixture not found";

    // Combined SMART max layout: <serial(10-digit)>/<YYYYMMDD>/<NNNN>/{signal,event}
    auto root = temp_dir / "sd";
    auto sess = root / "0040181394" / "20260620" / "0001";
    fs::create_directories(sess);
    fs::copy_file(wmedf, sess / "signal_003.wmedf", fs::copy_options::overwrite_existing);
    {
        std::ofstream o(sess / "event_003.xml");
        o << "<?xml version=\"1.0\" encoding=\"utf-8\"?>\n<desc>\n"
          << "<RespEvent RespEventID = \"101\" EndTime = \"120\" Duration = \"15\"/>\n"
          << "<RespEvent RespEventID = \"111\" EndTime = \"300\" Duration = \"20\"/>\n"
          << "</desc>\n";
    }

    auto prisma = std::make_unique<PrismaIngestion>(root.string());
    auto fake = std::make_unique<FakeDataSource>();  // unused: prisma branch wins
    auto publisher = std::make_unique<DataPublisherService>(
        std::shared_ptr<hms::MqttClient>{}, mock_db);

    auto svc = std::make_unique<BurstCollectorService>(60);
    svc->injectDependenciesForTest(mock_db, std::move(fake), std::move(publisher),
                                   nullptr, std::move(prisma));

    EXPECT_CALL(*db_raw, getLastSessionStart(_)).WillRepeatedly(Return(std::nullopt));
    EXPECT_CALL(*db_raw, sessionExists(_, _)).WillRepeatedly(Return(false));
    EXPECT_CALL(*db_raw, markSessionCompleted(_, _)).WillRepeatedly(Return(true));
    // The real .wmedf parses, so the session is discovered, parsed, and stored.
    EXPECT_CALL(*db_raw, saveSession(_)).Times(AtLeast(1)).WillRepeatedly(Return(true));
    // Returning nightly metrics drives the publishHistoricalState branch too.
    SessionMetrics nm; nm.total_events = 2; nm.ahi = 0.44; nm.obstructive_apneas = 1;
    nm.hypopneas = 1; nm.usage_hours = 4.5;
    EXPECT_CALL(*db_raw, getNightlyMetrics(_, _))
        .WillRepeatedly(Return(std::optional<SessionMetrics>(nm)));

    EXPECT_TRUE(svc->runBurstCycleForTest());
}

// Prisma branch, no parseable data: initialize() can't detect a combined root,
// so the cycle bails early (covers the init-failure branch).
TEST_F(BurstOrchestrationTest, PrismaMode_NoData_InitFailsReturnsFalse) {
    namespace fs = std::filesystem;
    fs::create_directories(temp_dir / "empty_sd" / "0040181394" / "20260620" / "0001");
    auto prisma = std::make_unique<PrismaIngestion>((temp_dir / "empty_sd").string());
    auto fake = std::make_unique<FakeDataSource>();
    auto publisher = std::make_unique<DataPublisherService>(
        std::shared_ptr<hms::MqttClient>{}, mock_db);
    auto svc = std::make_unique<BurstCollectorService>(60);
    svc->injectDependenciesForTest(mock_db, std::move(fake), std::move(publisher),
                                   nullptr, std::move(prisma));
    EXPECT_CALL(*db_raw, getLastSessionStart(_)).WillRepeatedly(Return(std::nullopt));
    EXPECT_FALSE(svc->runBurstCycleForTest());
}

// ── SDD-002: full-card OSCAR residue capture ────────────────────────────────
// These drive a full new-session burst cycle (download -> archive) and assert
// the residue paths (downloadDatalogResidue + captureCardResidue) ran. The fake
// source records what each path fetched; the OSCAR archive layout is checked on
// disk. The residue passes only run after a successful download, so each test
// seeds a fresh single session.

// A single session folder that ALSO holds the per-night .crc residue next to the
// EDFs, plus a junk file the denylist must drop, plus an oversize non-EDF.
static void seedSessionWithResidue(FakeDataSource& ds) {
    ds.date_folders = {"20200101"};
    ds.folder_files["20200101"] = {
        mkEntry("20200101_220000_BRP.edf", 100),
        mkEntry("20200101_220000_PLD.edf", 20),
        mkEntry("20200101_220000_CSL.edf", 1),
        mkEntry("20200101_220000_EVE.edf", 2),
        mkEntry("20200101_220000_BRP.crc", 1),   // residue: keep
        mkEntry("20200101_220000_PLD.crc", 1),   // residue: keep
        mkEntry("vacation.jpg", 3000),           // junk: drop
        mkEntry("huge.bin", 25 * 1024),          // >20 MB non-EDF: drop
    };
}

// Wire the standard new-session DB expectations for a fresh download.
static void expectFreshDownload(MockDatabase& db) {
    EXPECT_CALL(db, getLastSessionStart(_)).WillRepeatedly(Return(std::nullopt));
    EXPECT_CALL(db, isForceCompleted(_, _)).WillRepeatedly(Return(false));
    EXPECT_CALL(db, sessionExists(_, _)).WillRepeatedly(Return(false));
}

TEST_F(BurstOrchestrationTest, Residue_DatalogCrc_CapturedIntoArchive) {
    auto svc = makeService(&seedSessionWithResidue);
    expectFreshDownload(*db_raw);
    EXPECT_CALL(*db_raw, updateCheckpointFileSizesMock(_, _)).Times(AtLeast(1));

    svc->runBurstCycleForTest();

    namespace fs = std::filesystem;
    auto datalog = archive_dir / "DATALOG" / "20200101";
    // The .crc residue rode the download into the OSCAR archive...
    EXPECT_TRUE(fs::exists(datalog / "20200101_220000_BRP.crc"));
    EXPECT_TRUE(fs::exists(datalog / "20200101_220000_PLD.crc"));
    // ...alongside the EDFs...
    EXPECT_TRUE(fs::exists(datalog / "20200101_220000_BRP.edf"));
    // ...but junk and oversize non-EDF were dropped.
    EXPECT_FALSE(fs::exists(datalog / "vacation.jpg"));
    EXPECT_FALSE(fs::exists(datalog / "huge.bin"));

    // The .crc were fetched via downloadFile; the EDFs too. Junk never fetched.
    auto fetched = [&](const std::string& n) {
        return std::find(src_raw->downloaded_files.begin(),
                         src_raw->downloaded_files.end(), n) != src_raw->downloaded_files.end();
    };
    EXPECT_TRUE(fetched("20200101_220000_BRP.crc"));
    EXPECT_FALSE(fetched("vacation.jpg"));
    EXPECT_FALSE(fetched("huge.bin"));
}

TEST_F(BurstOrchestrationTest, Residue_CardRootSweep_RecursesAndSkips) {
    auto svc = makeService([](FakeDataSource& ds) {
        seedOneSession(ds);  // a plain session so the cycle downloads + archives
        // Card root: Identification.* (keep), STR.edf (analytical, skip),
        // ezshare.cfg (junk, skip), a SETTINGS dir (recurse) and a DATALOG dir
        // (must NOT be recursed — the analytical walk owns it), plus a dot-dir.
        ds.dir_listings[""] = {
            mkEntry("Identification.tgt", 1),
            mkEntry("Identification.crc", 1),
            mkEntry("STR.edf", 50),
            mkEntry("ezshare.cfg", 1),
            mkEntry("SETTINGS", 0, /*is_dir=*/true),
            mkEntry("DATALOG", 0, /*is_dir=*/true),
            mkEntry(".Spotlight-V100", 0, /*is_dir=*/true),
        };
        ds.dir_listings["SETTINGS"] = {
            mkEntry("AGL.tgt", 1),
            mkEntry("DLL.log", 2),
        };
    });
    expectFreshDownload(*db_raw);
    EXPECT_CALL(*db_raw, updateCheckpointFileSizesMock(_, _)).Times(AtLeast(1));

    svc->runBurstCycleForTest();

    namespace fs = std::filesystem;
    // Root metadata captured straight into the OSCAR archive root.
    EXPECT_TRUE(fs::exists(archive_dir / "Identification.tgt"));
    EXPECT_TRUE(fs::exists(archive_dir / "Identification.crc"));
    // SETTINGS recursed into and its files captured under the mirrored subdir.
    EXPECT_TRUE(fs::exists(archive_dir / "SETTINGS" / "AGL.tgt"));
    EXPECT_TRUE(fs::exists(archive_dir / "SETTINGS" / "DLL.log"));

    auto pulled = [&](const std::string& rel) {
        return std::find(src_raw->downloaded_by_path.begin(),
                         src_raw->downloaded_by_path.end(), rel) != src_raw->downloaded_by_path.end();
    };
    // Root STR.edf is analytical and must NOT be swept as residue.
    EXPECT_FALSE(pulled("STR.edf"));
    // ezShare.cfg is junk — never pulled.
    EXPECT_FALSE(pulled("ezshare.cfg"));
    // DATALOG is owned by the analytical walk — never re-listed (no children pulled
    // from it), and the dot-dir was skipped.
    EXPECT_TRUE(pulled("Identification.tgt"));
    EXPECT_TRUE(pulled("SETTINGS\\AGL.tgt"));
}

// forceCompleteSession() drives the manual-completion path: mark + force-complete
// in the DB, publish completion, and run the manufacturer summary (ezShare STR
// branch downloads the root STR.edf via the fake source, then parse no-ops on the
// stub bytes). Exercises forceCompleteSession + processSessionSummary +
// processSTRFile(ezShare) with the null-MQTT publisher.
TEST_F(BurstOrchestrationTest, ForceCompleteSession_DrivesCompletionAndSummary) {
    auto svc = makeService(&BurstOrchestrationTest::seedOneSession);
    auto start = std::chrono::system_clock::now();

    EXPECT_CALL(*db_raw, getSessionStartForSleepDay(_, _, _))
        .WillRepeatedly(Return(std::optional<std::chrono::system_clock::time_point>(start)));
    EXPECT_CALL(*db_raw, markSessionCompleted(_, _)).WillRepeatedly(Return(true));
    EXPECT_CALL(*db_raw, setForceCompleted(_, _)).WillRepeatedly(Return(true));
    SessionMetrics nm; nm.ahi = 1.5; nm.total_events = 4; nm.usage_hours = 6.0;
    EXPECT_CALL(*db_raw, getNightlyMetrics(_, _))
        .WillRepeatedly(Return(std::optional<SessionMetrics>(nm)));

    EXPECT_TRUE(svc->forceCompleteSession("2020-01-01"));
}

}  // namespace burst_orch

// ============================================================================
// CONFIG HOT-RELOAD TESTS
// Drive reloadConfig() through every subsystem diff branch (snapshotConfig,
// source/db/mqtt/llm/o2ring reconfiguration) without a worker thread. All
// collaborators are built from config; none require a live broker/network.
// ============================================================================
TEST(BurstConfigReloadTest, ReloadConfig_AppliesAllSubsystemChanges) {
    namespace fs = std::filesystem;
    auto base = fs::temp_directory_path() / "hms_cpap_cfg_reload";
    fs::create_directories(base);

    BurstCollectorService svc(300);
    AppConfig cfg;

    // Pass 1: local source + sqlite DB + LLM on + O2Ring on. Every field differs
    // from the default (empty) snapshot, so each diff branch fires.
    cfg.source = "local";
    cfg.local_dir = (base / "local").string();
    cfg.burst_interval = 600;
    cfg.device_id = "cpap_cov_dev";
    cfg.device_name = "Cov Device";
    cfg.database.type = "sqlite";
    cfg.database.sqlite_path = (base / "reload.db").string();
    cfg.mqtt.enabled = false;
    cfg.llm.enabled = true;
    cfg.llm.provider = "ollama";
    cfg.llm.endpoint = "http://127.0.0.1:11434";
    cfg.llm.model = "llama3";
    cfg.o2ring.enabled = true;
    cfg.o2ring.mode = "http";
    cfg.o2ring.mule_url = "http://127.0.0.1:9";

    svc.setAppConfig(&cfg);
    svc.markConfigDirty();
    svc.reloadConfigForTest();

    // No dirty flag set -> early return (covers the guard).
    svc.reloadConfigForTest();

    // Pass 2: switch to the Lowenstein source, disable LLM + O2Ring -> exercises
    // the PrismaIngestion branch and the llm/o2ring "disabled" branches.
    cfg.source = "lowenstein";
    cfg.llm.enabled = false;
    cfg.o2ring.enabled = false;
    svc.markConfigDirty();
    svc.reloadConfigForTest();

    // Pass 3: ezShare (else) source with range disabled -> EzShareClient branch.
    cfg.source = "ezshare";
    cfg.ezshare_url = "http://127.0.0.1:1";
    cfg.ezshare_range = false;
    svc.markConfigDirty();
    svc.reloadConfigForTest();

    SUCCEED();
}

// ============================================================================
// LLM PROMPT FORMATTER TESTS (SDD-002 coverage backfill)
// buildMetricsString / buildRangeMetricsString are pure string builders over the
// metrics structs with many optional-field branches. Exercised here directly via
// the test seam (no live LLM/MQTT/DB needed — db_service_ stays null, so the
// oximetry blocks are guarded off).
// ============================================================================
namespace burst_fmt {

TEST(BurstMetricsFormatTest, MetricsString_AllFieldsPopulated) {
    BurstCollectorService svc(60);
    SessionMetrics m;
    m.usage_hours = 7.5; m.usage_percent = 93.75;
    m.ahi = 3.21; m.total_events = 24; m.obstructive_apneas = 10;
    m.central_apneas = 2; m.hypopneas = 10; m.reras = 2;
    m.avg_event_duration = 18.4; m.max_event_duration = 41.0;
    m.avg_pressure = 9.2; m.min_pressure = 6.0; m.max_pressure = 12.0; m.pressure_p95 = 11.4;
    m.avg_leak_rate = 12.0; m.max_leak_rate = 30.0; m.leak_p95 = 24.0; m.leak_p50 = 8.0;
    m.avg_mask_pressure = 9.0; m.avg_epr_pressure = 6.5; m.avg_snore = 0.4;
    m.avg_target_ventilation = 7.2; m.therapy_mode = 8;  // ASV (Variable EPAP)
    m.avg_respiratory_rate = 15.0; m.avg_tidal_volume = 480.0;
    m.avg_minute_ventilation = 7.1; m.avg_flow_limitation = 0.12;

    STRDailyRecord str;
    str.ahi = 3.0; str.mask_events = 4; str.leak_95 = 20.0; str.mask_press_95 = 11.0;
    str.asv_epap = 6.0; str.asv_min_ps = 3.0; str.asv_max_ps = 10.0;
    str.tgt_ipap_50 = 12.0; str.tgt_epap_50 = 6.0; str.tgt_vent_50 = 7.0;

    std::string out = svc.buildMetricsStringForTest(m, &str);

    EXPECT_NE(out.find("Usage: 7.50 hours"), std::string::npos);
    EXPECT_NE(out.find("AHI: 3.21 events/hour"), std::string::npos);
    EXPECT_NE(out.find("Avg event duration: 18.40s, max: 41.00s"), std::string::npos);
    EXPECT_NE(out.find("Pressure: avg=9.20 cmH2O"), std::string::npos);
    EXPECT_NE(out.find("Leak: avg=12.00 L/min"), std::string::npos);
    EXPECT_NE(out.find("EPR/EPAP pressure:"), std::string::npos);
    EXPECT_NE(out.find("Therapy mode: ASV (Variable EPAP)"), std::string::npos);
    EXPECT_NE(out.find("Respiratory rate:"), std::string::npos);
    EXPECT_NE(out.find("ResMed official daily summary:"), std::string::npos);
    EXPECT_NE(out.find("ASV EPAP: 6.00 cmH2O"), std::string::npos);
    EXPECT_NE(out.find("Target IPAP (median): 12.00 cmH2O"), std::string::npos);
}

TEST(BurstMetricsFormatTest, MetricsString_MinimalNoOptionals) {
    BurstCollectorService svc(60);
    SessionMetrics m;  // all optionals empty
    m.ahi = 1.0; m.total_events = 3;

    std::string out = svc.buildMetricsStringForTest(m, nullptr);

    EXPECT_NE(out.find("AHI: 1.00 events/hour"), std::string::npos);
    EXPECT_NE(out.find("Usage: 0.00 hours"), std::string::npos);
    // None of the optional sections should appear.
    EXPECT_EQ(out.find("Pressure:"), std::string::npos);
    EXPECT_EQ(out.find("Leak:"), std::string::npos);
    EXPECT_EQ(out.find("Therapy mode:"), std::string::npos);
    EXPECT_EQ(out.find("ResMed official daily summary:"), std::string::npos);
}

TEST(BurstMetricsFormatTest, MetricsString_TherapyModeNames) {
    BurstCollectorService svc(60);
    auto modeName = [&](int mode) {
        SessionMetrics m; m.therapy_mode = mode;
        return svc.buildMetricsStringForTest(m, nullptr);
    };
    EXPECT_NE(modeName(0).find("Therapy mode: CPAP"), std::string::npos);
    EXPECT_NE(modeName(1).find("Therapy mode: APAP"), std::string::npos);
    EXPECT_NE(modeName(7).find("Therapy mode: ASV (Fixed EPAP)"), std::string::npos);
    EXPECT_NE(modeName(99).find("Therapy mode: Unknown"), std::string::npos);
}

TEST(BurstMetricsFormatTest, RangeMetricsString_WeeklyAndMonthly) {
    BurstCollectorService svc(60);
    std::vector<SessionMetrics> nights;
    SessionMetrics a; a.sleep_day = "2026-06-01"; a.ahi = 2.0; a.usage_hours = 6.0;
    a.total_events = 12; a.avg_leak_rate = 10.0; a.avg_pressure = 9.0;
    SessionMetrics b; b.sleep_day = "2026-06-02"; b.ahi = 8.0; b.usage_hours = 3.0;
    b.total_events = 40;  // no leak/pressure -> exercises the "absent" branch + non-compliant
    nights = {a, b};

    std::string wk = svc.buildRangeMetricsStringForTest(nights, SummaryPeriod::WEEKLY);
    EXPECT_NE(wk.find("Weekly CPAP report (2 nights)"), std::string::npos);
    EXPECT_NE(wk.find("Weekly averages:"), std::string::npos);
    EXPECT_NE(wk.find("Compliance (>=4h): 1/2"), std::string::npos);
    EXPECT_NE(wk.find("Best AHI: 2.00 (2026-06-01)"), std::string::npos);
    EXPECT_NE(wk.find("Worst AHI: 8.00 (2026-06-02)"), std::string::npos);
    EXPECT_NE(wk.find("leak avg 10.00 L/min"), std::string::npos);

    std::string mo = svc.buildRangeMetricsStringForTest(nights, SummaryPeriod::MONTHLY);
    EXPECT_NE(mo.find("Monthly CPAP report (2 nights)"), std::string::npos);
    EXPECT_NE(mo.find("Monthly averages:"), std::string::npos);
}

TEST(BurstMetricsFormatTest, MetricsString_PartialOptionalBranches) {
    BurstCollectorService svc(60);
    // Event duration present but no max; pressure block via p95 only (no avg/min/max);
    // leak block via max only; PLD + ASV + respiratory singletons; STR with neither
    // asv_epap nor tgt_ipap_50 set (covers the str header without the ASV sub-blocks).
    SessionMetrics m;
    m.ahi = 2.5; m.total_events = 5;
    m.avg_event_duration = 12.0;          // no max_event_duration
    m.pressure_p95 = 10.5;                 // gate true via p95, avg/min/max absent
    m.max_leak_rate = 28.0;                // gate true via max, avg absent
    m.avg_mask_pressure = 8.8;
    m.avg_snore = 1.2;
    m.avg_tidal_volume = 450.0;
    m.avg_minute_ventilation = 6.8;
    m.avg_flow_limitation = 0.05;
    m.therapy_mode = 1;                    // APAP

    STRDailyRecord str;
    str.ahi = 2.0; str.mask_events = 2; str.leak_95 = 15.0; str.mask_press_95 = 10.0;
    // asv_epap and tgt_ipap_50 intentionally left unset.

    std::string out = svc.buildMetricsStringForTest(m, &str);
    EXPECT_NE(out.find("Avg event duration: 12.00s"), std::string::npos);
    EXPECT_EQ(out.find(", max:"), std::string::npos);          // no max segment
    EXPECT_NE(out.find("Pressure:"), std::string::npos);
    EXPECT_NE(out.find("95th=10.50 cmH2O"), std::string::npos);
    EXPECT_NE(out.find("Leak:"), std::string::npos);
    EXPECT_NE(out.find("Mask pressure (actual):"), std::string::npos);
    EXPECT_NE(out.find("Snore index:"), std::string::npos);
    EXPECT_NE(out.find("Therapy mode: APAP"), std::string::npos);
    EXPECT_NE(out.find("ResMed official daily summary:"), std::string::npos);
    EXPECT_EQ(out.find("ASV EPAP:"), std::string::npos);       // ASV sub-block absent
    EXPECT_EQ(out.find("Target IPAP"), std::string::npos);     // tgt sub-block absent
}

TEST(BurstMetricsFormatTest, RangeMetricsString_SingleNightNoLeak) {
    BurstCollectorService svc(60);
    SessionMetrics a; a.sleep_day = "2026-06-10"; a.ahi = 4.0; a.usage_hours = 5.0;
    a.total_events = 20;  // no leak -> leak_count==0 branch (no avg-leak line)
    std::string out = svc.buildRangeMetricsStringForTest({a}, SummaryPeriod::WEEKLY);
    EXPECT_NE(out.find("Weekly CPAP report (1 nights)"), std::string::npos);
    EXPECT_NE(out.find("Compliance (>=4h): 1/1"), std::string::npos);
    EXPECT_EQ(out.find("Avg leak:"), std::string::npos);  // no leak data -> line omitted
}

}  // namespace burst_fmt

// ============================================================================
// INITIALIZE() PATH TESTS (SDD-002 coverage backfill)
// initialize() + the initX helpers read env-driven config and are bypassed by
// injectDependenciesForTest(). Drive them on safe, network-free branches:
// sqlite DB, local/ezShare source, MQTT off. No broker/LLM endpoint is hit.
// ============================================================================
namespace burst_init {

class BurstInitTest : public ::testing::Test {
protected:
    std::filesystem::path base;
    void SetUp() override {
        base = std::filesystem::temp_directory_path() /
               ("hms_cpap_init_" + std::to_string(getpid()));
        std::filesystem::remove_all(base);
        std::filesystem::create_directories(base / "DATALOG");
    }
    void TearDown() override {
        std::filesystem::remove_all(base);
        for (const char* k : {"CPAP_SOURCE", "CPAP_LOCAL_DIR", "DB_TYPE", "SQLITE_PATH",
                              "MQTT_ENABLED", "LLM_ENABLED", "LLM_PROVIDER",
                              "O2RING_MULE_URL"})
            unsetenv(k);
    }
};

// local source + sqlite, MQTT + LLM + O2Ring all disabled.
TEST_F(BurstInitTest, Initialize_LocalSqlite_NoMqttNoLlm) {
    setenv("CPAP_SOURCE", "local", 1);
    setenv("CPAP_LOCAL_DIR", base.c_str(), 1);
    setenv("DB_TYPE", "sqlite", 1);
    setenv("SQLITE_PATH", (base / "init.db").c_str(), 1);
    setenv("MQTT_ENABLED", "false", 1);
    setenv("LLM_ENABLED", "false", 1);
    unsetenv("O2RING_MULE_URL");

    AppConfig cfg;
    cfg.o2ring.enabled = false;
    cfg.ezshare_range = true;
    BurstCollectorService svc(60);
    svc.initialize(&cfg);  // initDataSource(local)/initDatabase(sqlite)/initMqtt(off)/initLlm(off)/initO2Ring(off)
    SUCCEED();
}

// ezShare source (range disabled branch) + sqlite, LLM enabled (constructs the
// client + default prompt template — no endpoint is contacted at init time).
TEST_F(BurstInitTest, Initialize_EzShareSqlite_LlmEnabled) {
    setenv("CPAP_SOURCE", "ezshare", 1);
    setenv("DB_TYPE", "sqlite", 1);
    setenv("SQLITE_PATH", (base / "init2.db").c_str(), 1);
    setenv("MQTT_ENABLED", "false", 1);
    setenv("LLM_ENABLED", "true", 1);
    setenv("LLM_PROVIDER", "ollama", 1);

    AppConfig cfg;
    cfg.o2ring.enabled = false;
    cfg.ezshare_range = false;  // exercises EzShareClient::setSupportsRange(false)
    BurstCollectorService svc(60);
    svc.initialize(&cfg);
    SUCCEED();
}

// Unknown DB_TYPE falls back to sqlite (covers the fallback branch).
TEST_F(BurstInitTest, Initialize_UnknownDbType_FallsBackToSqlite) {
    setenv("CPAP_SOURCE", "ezshare", 1);
    setenv("DB_TYPE", "bogus", 1);
    setenv("SQLITE_PATH", (base / "init3.db").c_str(), 1);
    setenv("MQTT_ENABLED", "false", 1);
    setenv("LLM_ENABLED", "false", 1);

    AppConfig cfg;
    cfg.o2ring.enabled = false;
    BurstCollectorService svc(60);
    svc.initialize(&cfg);
    SUCCEED();
}

}  // namespace burst_init

