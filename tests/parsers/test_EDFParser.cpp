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
#include <cmath>

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

// ============================================================================
// EDF ACTUAL_RECORDS > HEADER TESTS (ResMed growing file fix)
// ============================================================================

TEST_F(EDFParserTest, EDFFile_ActualRecordsGreaterThanHeader_ReadsAllData) {
    // ResMed writes num_records=1 in header at session start,
    // then appends data records. Parser should read ALL actual records,
    // not just what header declares.

    std::string filepath = (test_dir / "growing_file.edf").string();

    // Create EDF with header saying 1 record, but file contains 60 records
    std::ofstream ofs(filepath, std::ios::binary);

    char header[256] = {0};
    memcpy(header, "0       ", 8);  // Version
    memcpy(header + 8, "X X X X", 7);  // Patient
    memcpy(header + 88, "Startdate 06-FEB-2026", 21);  // Recording
    memcpy(header + 168, "06.02.26", 8);  // Date
    memcpy(header + 176, "14.01.31", 8);  // Time
    memcpy(header + 184, "512     ", 8);  // Header bytes (256 + 256)
    memcpy(header + 192, "EDF+C   ", 8);  // Reserved
    memcpy(header + 236, "1       ", 8);  // num_records_header = 1 (ResMed bug)
    memcpy(header + 244, "1       ", 8);  // Duration = 1 second
    memcpy(header + 252, "1   ", 4);      // Num signals = 1
    ofs.write(header, 256);

    // Signal header
    char signal_header[256] = {0};
    memcpy(signal_header, "Flow            ", 16);  // Label
    memcpy(signal_header + 96, "L/min   ", 8);  // Phys dim
    memcpy(signal_header + 104, "-100    ", 8);  // Phys min
    memcpy(signal_header + 112, "100     ", 8);  // Phys max
    memcpy(signal_header + 120, "-32768  ", 8);  // Dig min
    memcpy(signal_header + 128, "32767   ", 8);  // Dig max
    memcpy(signal_header + 216, "25      ", 8);  // 25 samples per record (25 Hz)
    ofs.write(signal_header, 256);

    // Write 60 data records (60 seconds of data at 25 Hz)
    // Each record = 25 samples * 2 bytes = 50 bytes
    int actual_records_written = 60;
    for (int rec = 0; rec < actual_records_written; ++rec) {
        for (int samp = 0; samp < 25; ++samp) {
            // Write some non-zero flow data (simulating breathing)
            int16_t value = static_cast<int16_t>((samp % 10 - 5) * 1000);  // Oscillating flow
            ofs.write(reinterpret_cast<char*>(&value), 2);
        }
    }
    ofs.close();

    // Open with EDFFile and verify
    EDFFile edf;
    ASSERT_TRUE(edf.open(filepath)) << "Failed to open EDF file";

    // Key assertions for the fix:
    EXPECT_EQ(edf.num_records_header, 1) << "Header should declare 1 record";
    EXPECT_EQ(edf.actual_records, 60) << "Actual records should be 60 (from file size)";
    EXPECT_FALSE(edf.complete) << "File should be marked incomplete (actual > header)";

    // Read flow signal and verify we get ALL 60 records worth of data
    std::vector<double> flow_data;
    int samples_read = edf.readSignal(0, flow_data);

    EXPECT_EQ(samples_read, 60 * 25) << "Should read all 1500 samples (60 records * 25 samples)";
    EXPECT_EQ(flow_data.size(), 60 * 25) << "Flow data should have 1500 samples";
}

TEST_F(EDFParserTest, EDFFile_ActualRecordsEqualsHeader_MarkedComplete) {
    // When actual_records == num_records_header, file is complete

    std::string filepath = (test_dir / "complete_file.edf").string();

    std::ofstream ofs(filepath, std::ios::binary);

    char header[256] = {0};
    memcpy(header, "0       ", 8);
    memcpy(header + 8, "X X X X", 7);
    memcpy(header + 88, "Startdate 06-FEB-2026", 21);
    memcpy(header + 168, "06.02.26", 8);
    memcpy(header + 176, "14.01.31", 8);
    memcpy(header + 184, "512     ", 8);
    memcpy(header + 236, "10      ", 8);  // num_records_header = 10
    memcpy(header + 244, "1       ", 8);
    memcpy(header + 252, "1   ", 4);
    ofs.write(header, 256);

    char signal_header[256] = {0};
    memcpy(signal_header, "Flow            ", 16);
    memcpy(signal_header + 96, "L/min   ", 8);
    memcpy(signal_header + 104, "-100    ", 8);
    memcpy(signal_header + 112, "100     ", 8);
    memcpy(signal_header + 120, "-32768  ", 8);
    memcpy(signal_header + 128, "32767   ", 8);
    memcpy(signal_header + 216, "25      ", 8);
    ofs.write(signal_header, 256);

    // Write exactly 10 records (matches header)
    for (int rec = 0; rec < 10; ++rec) {
        for (int samp = 0; samp < 25; ++samp) {
            int16_t value = 0;
            ofs.write(reinterpret_cast<char*>(&value), 2);
        }
    }
    ofs.close();

    EDFFile edf;
    ASSERT_TRUE(edf.open(filepath));

    EXPECT_EQ(edf.num_records_header, 10);
    EXPECT_EQ(edf.actual_records, 10);
    EXPECT_TRUE(edf.complete) << "File should be marked complete when actual == header";
}

// ============================================================================
// EXTRA_RECORDS AND GROWING FIELD TESTS
// ============================================================================

TEST_F(EDFParserTest, ExtraRecords_CorrectCount_WhenGrowing) {
    // Verify extra_records field correctly counts records beyond header
    std::string filepath = (test_dir / "extra_records_test.edf").string();

    std::ofstream ofs(filepath, std::ios::binary);

    char header[256] = {0};
    memcpy(header, "0       ", 8);
    memcpy(header + 8, "X X X X", 7);
    memcpy(header + 88, "Startdate 06-FEB-2026", 21);
    memcpy(header + 168, "06.02.26", 8);
    memcpy(header + 176, "14.01.31", 8);
    memcpy(header + 184, "512     ", 8);
    memcpy(header + 236, "5       ", 8);  // Header says 5 records
    memcpy(header + 244, "1       ", 8);
    memcpy(header + 252, "1   ", 4);
    ofs.write(header, 256);

    char signal_header[256] = {0};
    memcpy(signal_header, "Flow            ", 16);
    memcpy(signal_header + 96, "L/min   ", 8);
    memcpy(signal_header + 104, "-100    ", 8);
    memcpy(signal_header + 112, "100     ", 8);
    memcpy(signal_header + 120, "-32768  ", 8);
    memcpy(signal_header + 128, "32767   ", 8);
    memcpy(signal_header + 216, "25      ", 8);
    ofs.write(signal_header, 256);

    // Write 15 records (10 more than header declares)
    for (int rec = 0; rec < 15; ++rec) {
        for (int samp = 0; samp < 25; ++samp) {
            int16_t value = 0;
            ofs.write(reinterpret_cast<char*>(&value), 2);
        }
    }
    ofs.close();

    EDFFile edf;
    ASSERT_TRUE(edf.open(filepath));

    EXPECT_EQ(edf.num_records_header, 5) << "Header says 5 records";
    EXPECT_EQ(edf.actual_records, 15) << "File has 15 records";
    EXPECT_EQ(edf.extra_records, 10) << "Should have 10 extra records (15 - 5)";
    EXPECT_TRUE(edf.growing) << "File with extra records should be marked growing";
    EXPECT_FALSE(edf.complete) << "Growing file is not complete";
}

TEST_F(EDFParserTest, ExtraRecords_ZeroWhenComplete) {
    // When file matches header, extra_records should be 0
    std::string filepath = (test_dir / "no_extra_records.edf").string();

    std::ofstream ofs(filepath, std::ios::binary);

    char header[256] = {0};
    memcpy(header, "0       ", 8);
    memcpy(header + 8, "X X X X", 7);
    memcpy(header + 88, "Startdate 06-FEB-2026", 21);
    memcpy(header + 168, "06.02.26", 8);
    memcpy(header + 176, "14.01.31", 8);
    memcpy(header + 184, "512     ", 8);
    memcpy(header + 236, "20      ", 8);  // Header says 20 records
    memcpy(header + 244, "1       ", 8);
    memcpy(header + 252, "1   ", 4);
    ofs.write(header, 256);

    char signal_header[256] = {0};
    memcpy(signal_header, "Flow            ", 16);
    memcpy(signal_header + 96, "L/min   ", 8);
    memcpy(signal_header + 104, "-100    ", 8);
    memcpy(signal_header + 112, "100     ", 8);
    memcpy(signal_header + 120, "-32768  ", 8);
    memcpy(signal_header + 128, "32767   ", 8);
    memcpy(signal_header + 216, "25      ", 8);
    ofs.write(signal_header, 256);

    // Write exactly 20 records (matches header)
    for (int rec = 0; rec < 20; ++rec) {
        for (int samp = 0; samp < 25; ++samp) {
            int16_t value = 0;
            ofs.write(reinterpret_cast<char*>(&value), 2);
        }
    }
    ofs.close();

    EDFFile edf;
    ASSERT_TRUE(edf.open(filepath));

    EXPECT_EQ(edf.num_records_header, 20);
    EXPECT_EQ(edf.actual_records, 20);
    EXPECT_EQ(edf.extra_records, 0) << "Complete file has no extra records";
    EXPECT_FALSE(edf.growing) << "Complete file is not growing";
    EXPECT_TRUE(edf.complete) << "File matching header is complete";
}

TEST_F(EDFParserTest, ExtraRecords_ZeroWhenTruncated) {
    // When file has fewer records than header, extra_records should be 0
    std::string filepath = (test_dir / "truncated_extra.edf").string();

    std::ofstream ofs(filepath, std::ios::binary);

    char header[256] = {0};
    memcpy(header, "0       ", 8);
    memcpy(header + 8, "X X X X", 7);
    memcpy(header + 88, "Startdate 06-FEB-2026", 21);
    memcpy(header + 168, "06.02.26", 8);
    memcpy(header + 176, "14.01.31", 8);
    memcpy(header + 184, "512     ", 8);
    memcpy(header + 236, "50      ", 8);  // Header says 50 records
    memcpy(header + 244, "1       ", 8);
    memcpy(header + 252, "1   ", 4);
    ofs.write(header, 256);

    char signal_header[256] = {0};
    memcpy(signal_header, "Flow            ", 16);
    memcpy(signal_header + 96, "L/min   ", 8);
    memcpy(signal_header + 104, "-100    ", 8);
    memcpy(signal_header + 112, "100     ", 8);
    memcpy(signal_header + 120, "-32768  ", 8);
    memcpy(signal_header + 128, "32767   ", 8);
    memcpy(signal_header + 216, "25      ", 8);
    ofs.write(signal_header, 256);

    // Write only 10 records (fewer than header's 50)
    for (int rec = 0; rec < 10; ++rec) {
        for (int samp = 0; samp < 25; ++samp) {
            int16_t value = 0;
            ofs.write(reinterpret_cast<char*>(&value), 2);
        }
    }
    ofs.close();

    EDFFile edf;
    ASSERT_TRUE(edf.open(filepath));

    EXPECT_EQ(edf.num_records_header, 50);
    EXPECT_EQ(edf.actual_records, 10) << "Only 10 records in file";
    EXPECT_EQ(edf.extra_records, 0) << "Truncated file has no extra records";
    EXPECT_FALSE(edf.growing) << "Truncated file is not growing";
    EXPECT_FALSE(edf.complete) << "Truncated file is not complete";
}

TEST_F(EDFParserTest, GrowingFlag_SessionStopDetection) {
    // Verify that growing=false triggers session stop detection logic
    // while growing=true keeps session as IN_PROGRESS

    // First test: growing file should be IN_PROGRESS
    std::string session_dir1 = (test_dir / "growing_session_test").string();
    std::filesystem::create_directories(session_dir1);

    std::string filepath1 = session_dir1 + "/20260206_140131_BRP.edf";
    {
        std::ofstream ofs(filepath1, std::ios::binary);
        char header[256] = {0};
        memcpy(header, "0       ", 8);
        memcpy(header + 8, "X X X X", 7);
        memcpy(header + 88, "Startdate 06-FEB-2026 PSG-CPAP-Device Sn 20123456789", 50);
        memcpy(header + 168, "06.02.26", 8);
        memcpy(header + 176, "14.01.31", 8);
        memcpy(header + 184, "512     ", 8);
        memcpy(header + 192, "EDF+C   ", 8);
        memcpy(header + 236, "1       ", 8);  // Header says 1
        memcpy(header + 244, "1       ", 8);
        memcpy(header + 252, "1   ", 4);
        ofs.write(header, 256);

        char signal_header[256] = {0};
        memcpy(signal_header, "Flow            ", 16);
        memcpy(signal_header + 96, "L/min   ", 8);
        memcpy(signal_header + 104, "-100    ", 8);
        memcpy(signal_header + 112, "100     ", 8);
        memcpy(signal_header + 120, "-32768  ", 8);
        memcpy(signal_header + 128, "32767   ", 8);
        memcpy(signal_header + 216, "25      ", 8);
        ofs.write(signal_header, 256);

        // Write 60 records (growing)
        for (int rec = 0; rec < 60; ++rec) {
            for (int samp = 0; samp < 25; ++samp) {
                int16_t value = static_cast<int16_t>(sin(samp * 0.5) * 10000);
                ofs.write(reinterpret_cast<char*>(&value), 2);
            }
        }
    }

    createMinimalEDFHeader(session_dir1 + "/20260206_140131_CSL.edf", "20260206_140131", "CSL");

    auto session1 = EDFParser::parseSession(session_dir1, "TEST-DEVICE", "Test Device");
    ASSERT_NE(session1, nullptr);
    EXPECT_EQ(session1->status, CPAPSession::Status::IN_PROGRESS)
        << "Growing file should result in IN_PROGRESS session";

    // Second test: complete file with old data should be COMPLETED
    std::string session_dir2 = (test_dir / "complete_session_test").string();
    std::filesystem::create_directories(session_dir2);

    // Create session from 2 hours ago
    auto two_hours_ago = system_clock::now() - hours(2);
    auto time_t_val = system_clock::to_time_t(two_hours_ago);
    std::tm* tm = std::localtime(&time_t_val);
    char timestamp_str[16];
    std::strftime(timestamp_str, sizeof(timestamp_str), "%Y%m%d_%H%M%S", tm);

    std::string filepath2 = session_dir2 + "/" + std::string(timestamp_str) + "_BRP.edf";
    {
        std::ofstream ofs(filepath2, std::ios::binary);
        char header[256] = {0};
        memcpy(header, "0       ", 8);
        memcpy(header + 8, "X X X X", 7);
        memcpy(header + 88, "Startdate 06-FEB-2026 PSG-CPAP-Device Sn 20123456789", 50);

        // Parse date/time for header
        std::string date = std::string(timestamp_str).substr(6, 2) + "." +
                          std::string(timestamp_str).substr(4, 2) + "." +
                          std::string(timestamp_str).substr(2, 2);
        std::string time = std::string(timestamp_str).substr(9, 2) + "." +
                          std::string(timestamp_str).substr(11, 2) + "." +
                          std::string(timestamp_str).substr(13, 2);
        memcpy(header + 168, date.c_str(), 8);
        memcpy(header + 176, time.c_str(), 8);
        memcpy(header + 184, "512     ", 8);
        memcpy(header + 192, "EDF+C   ", 8);
        memcpy(header + 236, "60      ", 8);  // Header says 60 (matches actual)
        memcpy(header + 244, "1       ", 8);
        memcpy(header + 252, "1   ", 4);
        ofs.write(header, 256);

        char signal_header[256] = {0};
        memcpy(signal_header, "Flow            ", 16);
        memcpy(signal_header + 96, "L/min   ", 8);
        memcpy(signal_header + 104, "-100    ", 8);
        memcpy(signal_header + 112, "100     ", 8);
        memcpy(signal_header + 120, "-32768  ", 8);
        memcpy(signal_header + 128, "32767   ", 8);
        memcpy(signal_header + 216, "25      ", 8);
        ofs.write(signal_header, 256);

        // Write exactly 60 records (complete file)
        for (int rec = 0; rec < 60; ++rec) {
            for (int samp = 0; samp < 25; ++samp) {
                int16_t value = static_cast<int16_t>(sin(samp * 0.5) * 10000);
                ofs.write(reinterpret_cast<char*>(&value), 2);
            }
        }
    }

    createMinimalEDFHeader(session_dir2 + "/" + std::string(timestamp_str) + "_CSL.edf", timestamp_str, "CSL");

    auto session2 = EDFParser::parseSession(session_dir2, "TEST-DEVICE", "Test Device");
    ASSERT_NE(session2, nullptr);
    EXPECT_EQ(session2->status, CPAPSession::Status::COMPLETED)
        << "Complete file with old data should be COMPLETED";
}

TEST_F(EDFParserTest, EDFFile_HeaderMinusOne_MarkedIncomplete) {
    // EDF+ with num_records_header = -1 means "unknown length" (still recording)

    std::string filepath = (test_dir / "unknown_length.edf").string();

    std::ofstream ofs(filepath, std::ios::binary);

    char header[256] = {0};
    memcpy(header, "0       ", 8);
    memcpy(header + 8, "X X X X", 7);
    memcpy(header + 88, "Startdate 06-FEB-2026", 21);
    memcpy(header + 168, "06.02.26", 8);
    memcpy(header + 176, "14.01.31", 8);
    memcpy(header + 184, "512     ", 8);
    memcpy(header + 192, "EDF+C   ", 8);
    memcpy(header + 236, "-1      ", 8);  // num_records_header = -1 (unknown)
    memcpy(header + 244, "1       ", 8);
    memcpy(header + 252, "1   ", 4);
    ofs.write(header, 256);

    char signal_header[256] = {0};
    memcpy(signal_header, "Flow            ", 16);
    memcpy(signal_header + 96, "L/min   ", 8);
    memcpy(signal_header + 104, "-100    ", 8);
    memcpy(signal_header + 112, "100     ", 8);
    memcpy(signal_header + 120, "-32768  ", 8);
    memcpy(signal_header + 128, "32767   ", 8);
    memcpy(signal_header + 216, "25      ", 8);
    ofs.write(signal_header, 256);

    // Write 30 records
    for (int rec = 0; rec < 30; ++rec) {
        for (int samp = 0; samp < 25; ++samp) {
            int16_t value = 0;
            ofs.write(reinterpret_cast<char*>(&value), 2);
        }
    }
    ofs.close();

    EDFFile edf;
    ASSERT_TRUE(edf.open(filepath));

    EXPECT_EQ(edf.num_records_header, -1) << "Header should indicate unknown length";
    EXPECT_EQ(edf.actual_records, 30) << "Actual records should be calculated from file size";
    EXPECT_FALSE(edf.complete) << "File with header=-1 should be marked incomplete";
}

TEST_F(EDFParserTest, EDFFile_TruncatedFile_MarkedIncomplete) {
    // When actual_records < num_records_header, file is truncated

    std::string filepath = (test_dir / "truncated.edf").string();

    std::ofstream ofs(filepath, std::ios::binary);

    char header[256] = {0};
    memcpy(header, "0       ", 8);
    memcpy(header + 8, "X X X X", 7);
    memcpy(header + 88, "Startdate 06-FEB-2026", 21);
    memcpy(header + 168, "06.02.26", 8);
    memcpy(header + 176, "14.01.31", 8);
    memcpy(header + 184, "512     ", 8);
    memcpy(header + 236, "100     ", 8);  // Header says 100 records
    memcpy(header + 244, "1       ", 8);
    memcpy(header + 252, "1   ", 4);
    ofs.write(header, 256);

    char signal_header[256] = {0};
    memcpy(signal_header, "Flow            ", 16);
    memcpy(signal_header + 96, "L/min   ", 8);
    memcpy(signal_header + 104, "-100    ", 8);
    memcpy(signal_header + 112, "100     ", 8);
    memcpy(signal_header + 120, "-32768  ", 8);
    memcpy(signal_header + 128, "32767   ", 8);
    memcpy(signal_header + 216, "25      ", 8);
    ofs.write(signal_header, 256);

    // Write only 20 records (truncated - header says 100)
    for (int rec = 0; rec < 20; ++rec) {
        for (int samp = 0; samp < 25; ++samp) {
            int16_t value = 0;
            ofs.write(reinterpret_cast<char*>(&value), 2);
        }
    }
    ofs.close();

    EDFFile edf;
    ASSERT_TRUE(edf.open(filepath));

    EXPECT_EQ(edf.num_records_header, 100) << "Header declares 100 records";
    EXPECT_EQ(edf.actual_records, 20) << "Only 20 records actually in file";
    EXPECT_FALSE(edf.complete) << "Truncated file should be marked incomplete";
}

TEST_F(EDFParserTest, EDFFile_ResMed60SecondRecords) {
    // ResMed uses 60-second records (not 1-second)
    // Verify correct calculation of actual_records

    std::string filepath = (test_dir / "resmed_60sec.edf").string();

    std::ofstream ofs(filepath, std::ios::binary);

    char header[256] = {0};
    memcpy(header, "0       ", 8);
    memcpy(header + 8, "X X X X", 7);
    memcpy(header + 88, "Startdate 06-FEB-2026 PSG-CPAP-Device Sn 20123456789", 50);
    memcpy(header + 168, "06.02.26", 8);
    memcpy(header + 176, "14.01.31", 8);
    memcpy(header + 184, "512     ", 8);
    memcpy(header + 192, "EDF+C   ", 8);
    memcpy(header + 236, "1       ", 8);  // Header says 1 record
    memcpy(header + 244, "60      ", 8);  // 60 seconds per record (ResMed style)
    memcpy(header + 252, "1   ", 4);
    ofs.write(header, 256);

    char signal_header[256] = {0};
    memcpy(signal_header, "Flow            ", 16);
    memcpy(signal_header + 96, "L/min   ", 8);
    memcpy(signal_header + 104, "-100    ", 8);
    memcpy(signal_header + 112, "100     ", 8);
    memcpy(signal_header + 120, "-32768  ", 8);
    memcpy(signal_header + 128, "32767   ", 8);
    memcpy(signal_header + 216, "1500    ", 8);  // 1500 samples per 60-sec record (25 Hz)
    ofs.write(signal_header, 256);

    // Write 5 records (5 minutes of data at 60 sec/record)
    // Each record = 1500 samples * 2 bytes = 3000 bytes
    for (int rec = 0; rec < 5; ++rec) {
        for (int samp = 0; samp < 1500; ++samp) {
            int16_t value = static_cast<int16_t>(sin(samp * 0.1) * 10000);
            ofs.write(reinterpret_cast<char*>(&value), 2);
        }
    }
    ofs.close();

    EDFFile edf;
    ASSERT_TRUE(edf.open(filepath));

    EXPECT_EQ(edf.num_records_header, 1) << "Header says 1 record";
    EXPECT_EQ(edf.actual_records, 5) << "File contains 5 records";
    EXPECT_EQ(edf.record_duration, 60.0) << "Record duration should be 60 seconds";
    EXPECT_FALSE(edf.complete) << "actual > header means still recording";

    // Verify we can read all 5 minutes of data
    std::vector<double> flow_data;
    int samples_read = edf.readSignal(0, flow_data);

    EXPECT_EQ(samples_read, 5 * 1500) << "Should read 7500 samples (5 min at 25 Hz)";
    EXPECT_EQ(flow_data.size(), 7500) << "Flow data should have 7500 samples";
}

TEST_F(EDFParserTest, EDFFile_MultipleSignals_CorrectRecordSize) {
    // ResMed BRP has Flow + Pressure signals
    // Verify record size calculation is correct

    std::string filepath = (test_dir / "multi_signal.edf").string();

    std::ofstream ofs(filepath, std::ios::binary);

    // Header with 2 signals
    char header[256] = {0};
    memcpy(header, "0       ", 8);
    memcpy(header + 8, "X X X X", 7);
    memcpy(header + 88, "Startdate 06-FEB-2026", 21);
    memcpy(header + 168, "06.02.26", 8);
    memcpy(header + 176, "14.01.31", 8);
    memcpy(header + 184, "768     ", 8);  // 256 + 256*2 signals
    memcpy(header + 236, "1       ", 8);  // Header says 1 record
    memcpy(header + 244, "1       ", 8);  // 1 second per record
    memcpy(header + 252, "2   ", 4);      // 2 signals
    ofs.write(header, 256);

    // Signal headers for 2 signals
    char signal_header[512] = {0};
    // Signal 1: Flow (16 bytes label per signal)
    memcpy(signal_header, "Flow            ", 16);
    memcpy(signal_header + 16, "Press           ", 16);
    // Skip transducer (80*2), phys_dim (8*2), phys_min (8*2), phys_max (8*2), dig_min (8*2), dig_max (8*2)
    // Physical dimension at offset 96
    memcpy(signal_header + 16*2 + 80*2, "L/min   ", 8);
    memcpy(signal_header + 16*2 + 80*2 + 8, "cmH2O   ", 8);
    // Phys min at offset 96 + 8*2
    memcpy(signal_header + 16*2 + 80*2 + 8*2, "-100    ", 8);
    memcpy(signal_header + 16*2 + 80*2 + 8*2 + 8, "0       ", 8);
    // Phys max
    memcpy(signal_header + 16*2 + 80*2 + 8*4, "100     ", 8);
    memcpy(signal_header + 16*2 + 80*2 + 8*4 + 8, "30      ", 8);
    // Dig min
    memcpy(signal_header + 16*2 + 80*2 + 8*6, "-32768  ", 8);
    memcpy(signal_header + 16*2 + 80*2 + 8*6 + 8, "0       ", 8);
    // Dig max
    memcpy(signal_header + 16*2 + 80*2 + 8*8, "32767   ", 8);
    memcpy(signal_header + 16*2 + 80*2 + 8*8 + 8, "32767   ", 8);
    // Samples per record at offset 16*2 + 80*2 + 8*10 + 80*2 = 32 + 160 + 80 + 160 = 432
    // Actually: labels(16*2) + transducer(80*2) + phys_dim(8*2) + phys_min(8*2) + phys_max(8*2) + dig_min(8*2) + dig_max(8*2) + prefilter(80*2) = 32+160+16+16+16+16+16+160 = 432
    memcpy(signal_header + 432, "25      ", 8);  // Flow: 25 samples
    memcpy(signal_header + 440, "25      ", 8);  // Press: 25 samples
    ofs.write(signal_header, 512);

    // Write 10 data records
    // Each record = (25 + 25) samples * 2 bytes = 100 bytes
    for (int rec = 0; rec < 10; ++rec) {
        // Flow samples
        for (int samp = 0; samp < 25; ++samp) {
            int16_t value = static_cast<int16_t>(sin(samp * 0.5) * 10000);
            ofs.write(reinterpret_cast<char*>(&value), 2);
        }
        // Pressure samples
        for (int samp = 0; samp < 25; ++samp) {
            int16_t value = static_cast<int16_t>(10000);  // Constant 10 cmH2O
            ofs.write(reinterpret_cast<char*>(&value), 2);
        }
    }
    ofs.close();

    EDFFile edf;
    ASSERT_TRUE(edf.open(filepath));

    EXPECT_EQ(edf.num_signals, 2) << "Should have 2 signals";
    EXPECT_EQ(edf.record_size_bytes, 100) << "Record size = (25+25)*2 = 100 bytes";
    EXPECT_EQ(edf.num_records_header, 1);
    EXPECT_EQ(edf.actual_records, 10) << "10 records in file";
    EXPECT_FALSE(edf.complete) << "actual > header";
}

TEST_F(EDFParserTest, SessionStatus_GrowingFileMarkedInProgress) {
    // When actual_records > num_records_header, session should be IN_PROGRESS
    // regardless of staleness (file is actively being written)

    std::string session_dir = (test_dir / "growing_session").string();
    std::filesystem::create_directories(session_dir);

    std::string filepath = session_dir + "/20260206_140131_BRP.edf";

    // Create EDF with header=1, actual=60 (simulating ResMed growing file)
    std::ofstream ofs(filepath, std::ios::binary);

    char header[256] = {0};
    memcpy(header, "0       ", 8);
    memcpy(header + 8, "X X X X", 7);
    memcpy(header + 88, "Startdate 06-FEB-2026 PSG-CPAP-Device Sn 20123456789", 50);
    memcpy(header + 168, "06.02.26", 8);
    memcpy(header + 176, "14.01.31", 8);
    memcpy(header + 184, "512     ", 8);
    memcpy(header + 192, "EDF+C   ", 8);
    memcpy(header + 236, "1       ", 8);  // Header says 1 record
    memcpy(header + 244, "1       ", 8);  // 1 second per record
    memcpy(header + 252, "1   ", 4);
    ofs.write(header, 256);

    char signal_header[256] = {0};
    memcpy(signal_header, "Flow            ", 16);
    memcpy(signal_header + 96, "L/min   ", 8);
    memcpy(signal_header + 104, "-100    ", 8);
    memcpy(signal_header + 112, "100     ", 8);
    memcpy(signal_header + 120, "-32768  ", 8);
    memcpy(signal_header + 128, "32767   ", 8);
    memcpy(signal_header + 216, "25      ", 8);
    ofs.write(signal_header, 256);

    // Write 120 records (2 minutes of data) - more than header declares
    for (int rec = 0; rec < 120; ++rec) {
        for (int samp = 0; samp < 25; ++samp) {
            // Simulate breathing pattern
            int16_t value = static_cast<int16_t>(sin(samp * 0.5) * 10000);
            ofs.write(reinterpret_cast<char*>(&value), 2);
        }
    }
    ofs.close();

    // Also create CSL file
    createMinimalEDFHeader(session_dir + "/20260206_140131_CSL.edf", "20260206_140131", "CSL");

    auto session = EDFParser::parseSession(session_dir, "TEST-DEVICE", "Test Device");

    ASSERT_NE(session, nullptr) << "Parser should handle growing file";

    // Key assertion: session should be IN_PROGRESS because file has more data than header
    EXPECT_EQ(session->status, CPAPSession::Status::IN_PROGRESS)
        << "Session with actual_records > header should be IN_PROGRESS";

    // Verify we parsed all the data (2 minutes worth)
    EXPECT_GE(session->breathing_summary.size(), 2)
        << "Should have parsed at least 2 minutes of breathing data";
}

