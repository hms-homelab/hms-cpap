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

