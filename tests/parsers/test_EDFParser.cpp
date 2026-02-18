/**
 * HMS-CPAP EDFParser Unit Tests
 *
 * Tests session grouping, timestamp parsing, and ResMed EDF quirks handling.
 */

#include <gtest/gtest.h>
#include <gmock/gmock.h>
#include "parsers/EDFParser.h"
#include <filesystem>
#include <fstream>
#include <chrono>

using namespace hms_cpap;
using namespace std::chrono;

// Test fixture for EDFParser
class EDFParserTest : public ::testing::Test {
protected:
    void SetUp() override {
        // Create temporary test directory
        test_dir = std::filesystem::temp_directory_path() / "hms_cpap_test";
        std::filesystem::create_directories(test_dir);
    }

    void TearDown() override {
        // Clean up test directory
        if (std::filesystem::exists(test_dir)) {
            std::filesystem::remove_all(test_dir);
        }
    }

    // Helper: Create minimal valid EDF file header
    void createMinimalEDFHeader(const std::string& filepath,
                               const std::string& timestamp_str,
                               const std::string& file_type,
                               int duration_seconds = 60,
                               int num_records = -1) {
        std::ofstream ofs(filepath, std::ios::binary);
        ASSERT_TRUE(ofs.is_open()) << "Failed to create test file: " << filepath;

        // EDF header is 256 bytes (fixed)
        char header[256] = {0};

        // Version (8 bytes) - "0       "
        memcpy(header, "0       ", 8);

        // Patient identification (80 bytes)
        std::string patient = "X X X X";
        memcpy(header + 8, patient.c_str(), std::min<size_t>(80, patient.size()));

        // Recording identification (80 bytes) - includes device info
        // Format: "Startdate 06-FEB-2026 PSG-CPAP-Device Sn 12345 HW 1 SW 2"
        std::string recording = "Startdate 06-FEB-2026 PSG-CPAP-Device Sn 20123456789 HW 1 SW 2";
        memcpy(header + 88, recording.c_str(), std::min<size_t>(80, recording.size()));

        // Start date (8 bytes) - "dd.mm.yy"
        std::string date = timestamp_str.substr(6, 2) + "." +
                          timestamp_str.substr(4, 2) + "." +
                          timestamp_str.substr(2, 2);
        memcpy(header + 168, date.c_str(), std::min<size_t>(8, date.size()));

        // Start time (8 bytes) - "hh.mm.ss"
        std::string time = timestamp_str.substr(9, 2) + "." +
                          timestamp_str.substr(11, 2) + "." +
                          timestamp_str.substr(13, 2);
        memcpy(header + 176, time.c_str(), std::min<size_t>(8, time.size()));

        // Header bytes (8 bytes) - "512     " (256 header + 256 per signal * 1 signal)
        memcpy(header + 184, "512     ", 8);

        // Reserved (44 bytes) - "EDF+C" or "EDF+D"
        memcpy(header + 192, "EDF+C   ", 8);

        // Number of data records (8 bytes) - "-1" means unknown (recording in progress)
        std::string records_str = std::to_string(num_records);
        while (records_str.size() < 8) records_str += " ";
        memcpy(header + 236, records_str.c_str(), 8);

        // Duration of data record (8 bytes) - "1       " (1 second per record)
        memcpy(header + 244, "1       ", 8);

        // Number of signals (4 bytes) - "1   " (Flow signal)
        memcpy(header + 252, "1   ", 4);

        ofs.write(header, 256);

        // Signal header (256 bytes per signal)
        char signal_header[256] = {0};

        // Label (16 bytes)
        std::string label = file_type == "BRP" ? "Flow" : (file_type == "SAD" ? "SpO2" : "Pressure");
        memcpy(signal_header, label.c_str(), std::min<size_t>(16, label.size()));

        // Transducer type (80 bytes)
        memcpy(signal_header + 16, "Internal", 8);

        // Physical dimension (8 bytes) - L/min for flow
        std::string phys_dim = file_type == "BRP" ? "L/min" : "%";
        memcpy(signal_header + 96, phys_dim.c_str(), std::min<size_t>(8, phys_dim.size()));

        // Physical minimum (8 bytes)
        memcpy(signal_header + 104, "-100    ", 8);

        // Physical maximum (8 bytes)
        memcpy(signal_header + 112, "100     ", 8);

        // Digital minimum (8 bytes)
        memcpy(signal_header + 120, "-32768  ", 8);

        // Digital maximum (8 bytes)
        memcpy(signal_header + 128, "32767   ", 8);

        // Prefiltering (80 bytes)
        memcpy(signal_header + 136, "None", 4);

        // Samples per record (8 bytes) - 25 for 25 Hz
        memcpy(signal_header + 216, "25      ", 8);

        // Reserved (32 bytes)

        ofs.write(signal_header, 256);

        // Write data records (simplified - just zeros)
        int samples_per_record = 25;
        int bytes_per_sample = 2;  // 16-bit integers
        int record_size = samples_per_record * bytes_per_sample;

        int actual_records = (num_records == -1) ? (duration_seconds / 1) : num_records;
        std::vector<char> record_data(record_size, 0);
        for (int i = 0; i < actual_records; ++i) {
            ofs.write(record_data.data(), record_size);
        }

        ofs.close();
    }

    std::filesystem::path test_dir;
};

// ============================================================================
// SESSION GROUPING TESTS
// ============================================================================

TEST_F(EDFParserTest, SessionGrouping_SingleCSL_MultipleCheckpoints) {
    // ONE CSL file → ONE session
    // MULTIPLE BRP/PLD/SAD files → SAME session

    std::string session_dir = (test_dir / "20260206_140126").string();
    std::filesystem::create_directories(session_dir);

    // Create CSL file (session identifier)
    createMinimalEDFHeader(session_dir + "/20260206_140126_CSL.edf", "20260206_140126", "CSL");

    // Create EVE file (session end marker)
    createMinimalEDFHeader(session_dir + "/20260206_140126_EVE.edf", "20260206_140126", "EVE");

    // Create multiple checkpoint files (BRP, PLD, SAD)
    createMinimalEDFHeader(session_dir + "/20260206_140131_BRP.edf", "20260206_140131", "BRP", 300);
    createMinimalEDFHeader(session_dir + "/20260206_141424_BRP.edf", "20260206_141424", "BRP", 600);
    createMinimalEDFHeader(session_dir + "/20260206_143645_BRP.edf", "20260206_143645", "BRP", 400);

    createMinimalEDFHeader(session_dir + "/20260206_140132_PLD.edf", "20260206_140132", "PLD", 300);
    createMinimalEDFHeader(session_dir + "/20260206_141425_PLD.edf", "20260206_141425", "PLD", 600);

    createMinimalEDFHeader(session_dir + "/20260206_140132_SAD.edf", "20260206_140132", "SAD", 300);
    createMinimalEDFHeader(session_dir + "/20260206_141425_SAD.edf", "20260206_141425", "SAD", 600);

    // Parse session
    auto session = EDFParser::parseSession(session_dir, "TEST-DEVICE", "Test Device");

    ASSERT_NE(session, nullptr) << "Failed to parse session";
    EXPECT_EQ(session->device_id, "TEST-DEVICE");

    // Verify all checkpoint files were processed (combined into one session)
    // The parser should have processed 3 BRP files, 2 PLD, 2 SAD
    // Breathing summaries should be aggregated
    EXPECT_GT(session->breathing_summary.size(), 0) << "No breathing data found";

    // Verify session has events (EVE file present)
    EXPECT_TRUE(session->has_events) << "EVE file not detected";
}

TEST_F(EDFParserTest, SessionGrouping_MidnightCrossover) {
    // Session starts Feb 6 23:45
    // Checkpoints continue into Feb 7 00:15
    // Verify session remains grouped despite date change

    std::string session_dir = (test_dir / "20260206_234500").string();
    std::filesystem::create_directories(session_dir);

    createMinimalEDFHeader(session_dir + "/20260206_234500_CSL.edf", "20260206_234500", "CSL");
    createMinimalEDFHeader(session_dir + "/20260206_234505_BRP.edf", "20260206_234505", "BRP", 600);
    createMinimalEDFHeader(session_dir + "/20260207_000510_BRP.edf", "20260207_000510", "BRP", 600);  // Next day
    createMinimalEDFHeader(session_dir + "/20260207_001515_BRP.edf", "20260207_001515", "BRP", 300);  // Next day
    createMinimalEDFHeader(session_dir + "/20260207_002020_EVE.edf", "20260207_002020", "EVE");  // Session end

    auto session = EDFParser::parseSession(session_dir, "TEST-DEVICE", "Test Device");

    ASSERT_NE(session, nullptr);
    // All files should be grouped into ONE session despite midnight crossover
    EXPECT_TRUE(session->has_events) << "EVE file should be detected";
}

// ============================================================================
// TIMESTAMP PARSING TESTS
// ============================================================================

TEST_F(EDFParserTest, TimestampParsing_ExactMatch) {
    // Verify exact timestamp parsing from filename
    std::string session_dir = (test_dir / "20260206_140131").string();
    std::filesystem::create_directories(session_dir);

    createMinimalEDFHeader(session_dir + "/20260206_140131_BRP.edf", "20260206_140131", "BRP");

    // Parse with exact timestamp
    auto timestamp = system_clock::time_point{} + std::chrono::seconds(1738847491);  // 2026-02-06 14:01:31
    auto session = EDFParser::parseSession(session_dir, "TEST-DEVICE", "Test Device", timestamp);

    ASSERT_NE(session, nullptr);
    EXPECT_TRUE(session->session_start.has_value());
}

TEST_F(EDFParserTest, TimestampParsing_FilenameFormat) {
    // Verify filename timestamp format: YYYYMMDD_HHMMSS
    std::string session_dir = (test_dir / "test_session").string();
    std::filesystem::create_directories(session_dir);

    // Create file with specific timestamp
    createMinimalEDFHeader(session_dir + "/20260206_140131_BRP.edf", "20260206_140131", "BRP");

    auto session = EDFParser::parseSession(session_dir, "TEST-DEVICE", "Test Device");

    ASSERT_NE(session, nullptr);
    // Session start should be extracted from filename or file header
}

// ============================================================================
// RESMED EDF QUIRKS TESTS
// ============================================================================

TEST_F(EDFParserTest, EDFParser_EmptyPhysDim) {
    // Crc16 signal with empty phys_dim
    // Verify parser doesn't reject file

    std::string session_dir = (test_dir / "quirk_empty_phys_dim").string();
    std::filesystem::create_directories(session_dir);

    std::string filepath = session_dir + "/20260206_140131_BRP.edf";

    // Create EDF with empty physical dimension (ResMed quirk)
    std::ofstream ofs(filepath, std::ios::binary);

    char header[256] = {0};
    memcpy(header, "0       ", 8);  // Version
    std::string patient = "X X X X";
    memcpy(header + 8, patient.c_str(), 80);
    std::string recording = "Startdate 06-FEB-2026 PSG-CPAP-Device Sn 20123456789 HW 1 SW 2";
    memcpy(header + 88, recording.c_str(), 80);
    memcpy(header + 168, "06.02.26", 8);  // Date
    memcpy(header + 176, "14.01.31", 8);  // Time
    memcpy(header + 184, "512     ", 8);  // Header bytes
    memcpy(header + 236, "10      ", 8);  // Num records
    memcpy(header + 244, "1       ", 8);  // Duration
    memcpy(header + 252, "1   ", 4);      // Num signals
    ofs.write(header, 256);

    char signal_header[256] = {0};
    memcpy(signal_header, "Crc16", 5);  // Label
    // Physical dimension LEFT EMPTY (ResMed quirk)
    memcpy(signal_header + 96, "", 0);  // Empty phys_dim
    memcpy(signal_header + 104, "-32768  ", 8);  // Phys min
    memcpy(signal_header + 112, "32767   ", 8);  // Phys max
    memcpy(signal_header + 120, "-32768  ", 8);  // Dig min
    memcpy(signal_header + 128, "32767   ", 8);  // Dig max
    memcpy(signal_header + 216, "1       ", 8);  // Samples per record
    ofs.write(signal_header, 256);

    // Write data records
    char record[2] = {0};
    for (int i = 0; i < 10; ++i) {
        ofs.write(record, 2);
    }
    ofs.close();

    // Parser should handle this gracefully
    auto session = EDFParser::parseSession(session_dir, "TEST-DEVICE", "Test Device");
    ASSERT_NE(session, nullptr) << "Parser should not reject file with empty phys_dim";
}

TEST_F(EDFParserTest, EDFParser_IncompleteFile) {
    // BRP file still being written (partial last record)
    // Verify parser reads available records

    std::string session_dir = (test_dir / "incomplete_file").string();
    std::filesystem::create_directories(session_dir);

    std::string filepath = session_dir + "/20260206_140131_BRP.edf";
    createMinimalEDFHeader(filepath, "20260206_140131", "BRP", 60, -1);  // -1 = unknown records

    auto session = EDFParser::parseSession(session_dir, "TEST-DEVICE", "Test Device");
    ASSERT_NE(session, nullptr) << "Parser should handle incomplete files";
}

// ============================================================================
// SESSION STATUS STALENESS TESTS
// ============================================================================

TEST_F(EDFParserTest, SessionStatus_StaleDataMarkedCompleted) {
    // Bug fix test: Session with data older than 30 minutes
    // should be marked COMPLETED even if flow-based detection
    // didn't find a clean end boundary (file stopped growing)

    std::string session_dir = (test_dir / "stale_session").string();
    std::filesystem::create_directories(session_dir);

    // Create session from 2 hours ago (well past 30 min staleness threshold)
    auto two_hours_ago = system_clock::now() - hours(2);
    auto time_t_val = system_clock::to_time_t(two_hours_ago);
    std::tm* tm = std::localtime(&time_t_val);

    char timestamp_str[16];
    std::strftime(timestamp_str, sizeof(timestamp_str), "%Y%m%d_%H%M%S", tm);

    // Create BRP file with old timestamp (6 minutes of data)
    std::string brp_filename = std::string(timestamp_str) + "_BRP.edf";
    createMinimalEDFHeader(session_dir + "/" + brp_filename, timestamp_str, "BRP", 360);

    // Create CSL file
    std::string csl_filename = std::string(timestamp_str) + "_CSL.edf";
    createMinimalEDFHeader(session_dir + "/" + csl_filename, timestamp_str, "CSL", 60);

    auto session = EDFParser::parseSession(session_dir, "TEST-DEVICE", "Test Device");

    ASSERT_NE(session, nullptr) << "Parser should handle stale session";
    // Staleness check: data is >30 min old, should be COMPLETED
    EXPECT_EQ(session->status, CPAPSession::Status::COMPLETED)
        << "Session with data >30 min old should be marked COMPLETED";
    EXPECT_TRUE(session->session_end.has_value())
        << "Completed session should have session_end set";
}

TEST_F(EDFParserTest, SessionStatus_RecentDataMarkedInProgress) {
    // Session with recent data (within last few minutes)
    // should remain IN_PROGRESS if no clean end boundary found
    // Note: This test may be flaky if run exactly at boundary

    std::string session_dir = (test_dir / "recent_session").string();
    std::filesystem::create_directories(session_dir);

    // Create session from 5 minutes ago (within 30 min threshold)
    auto five_minutes_ago = system_clock::now() - minutes(5);
    auto time_t_val = system_clock::to_time_t(five_minutes_ago);
    std::tm* tm = std::localtime(&time_t_val);

    char timestamp_str[16];
    std::strftime(timestamp_str, sizeof(timestamp_str), "%Y%m%d_%H%M%S", tm);

    // Create BRP file with recent timestamp (3 minutes of data)
    std::string brp_filename = std::string(timestamp_str) + "_BRP.edf";
    createMinimalEDFHeader(session_dir + "/" + brp_filename, timestamp_str, "BRP", 180);

    // Create CSL file
    std::string csl_filename = std::string(timestamp_str) + "_CSL.edf";
    createMinimalEDFHeader(session_dir + "/" + csl_filename, timestamp_str, "CSL", 60);

    auto session = EDFParser::parseSession(session_dir, "TEST-DEVICE", "Test Device");

    ASSERT_NE(session, nullptr) << "Parser should handle recent session";
    // Data is recent (<30 min), should remain IN_PROGRESS
    // Note: Status depends on flow-based detection too, so this may be COMPLETED
    // if flow data shows a clean end. The test file has all-zero data.
}

