#include <gtest/gtest.h>
#include <gmock/gmock.h>
#include "services/BackfillService.h"
#include "parsers/CpapdashBridge.h"

#include <filesystem>
#include <fstream>

using namespace hms_cpap;
using ::testing::_;
using ::testing::Return;
using ::testing::Invoke;

// ── Mock database ───────────────────────────────────────────────────────

class MockDatabase : public IDatabase {
public:
    DbType dbType() const override { return DbType::SQLITE; }

    MOCK_METHOD(bool, connect, (), (override));
    MOCK_METHOD(void, disconnect, (), (override));
    MOCK_METHOD(bool, isConnected, (), (const, override));

    MOCK_METHOD(bool, saveSession, (const CPAPSession&), (override));
    MOCK_METHOD(bool, sessionExists, (const std::string&, const std::chrono::system_clock::time_point&), (override));
    MOCK_METHOD((std::optional<std::chrono::system_clock::time_point>), getLastSessionStart, (const std::string&), (override));
    MOCK_METHOD((std::optional<SessionMetrics>), getSessionMetrics, (const std::string&, const std::chrono::system_clock::time_point&), (override));
    MOCK_METHOD(bool, markSessionCompleted, (const std::string&, const std::chrono::system_clock::time_point&), (override));
    MOCK_METHOD(bool, reopenSession, (const std::string&, const std::chrono::system_clock::time_point&), (override));
    MOCK_METHOD(int, deleteSessionsByDateFolder, (const std::string&, const std::string&), (override));
    MOCK_METHOD(bool, isForceCompleted, (const std::string&, const std::chrono::system_clock::time_point&), (override));
    MOCK_METHOD(bool, setForceCompleted, (const std::string&, const std::chrono::system_clock::time_point&), (override));
    MOCK_METHOD((std::map<std::string, int>), getCheckpointFileSizes, (const std::string&, const std::chrono::system_clock::time_point&), (override));

    // GMock needs parens around complex return/param types with commas
    bool updateCheckpointFileSizes(
        const std::string& device_id,
        const std::chrono::system_clock::time_point& session_start,
        const std::map<std::string, int>& file_sizes) override {
        return updateCheckpointFileSizesMock(device_id, session_start);
    }
    MOCK_METHOD(bool, updateCheckpointFileSizesMock, (const std::string&, const std::chrono::system_clock::time_point&));
    MOCK_METHOD(bool, updateDeviceLastSeen, (const std::string&), (override));
    MOCK_METHOD(bool, saveSTRDailyRecords, (const std::vector<STRDailyRecord>&), (override));
    MOCK_METHOD((std::optional<std::string>), getLastSTRDate, (const std::string&), (override));
    MOCK_METHOD((std::optional<SessionMetrics>), getNightlyMetrics, (const std::string&, const std::chrono::system_clock::time_point&), (override));
    MOCK_METHOD((std::vector<SessionMetrics>), getMetricsForDateRange, (const std::string&, int), (override));
    MOCK_METHOD(bool, saveSummary, (const std::string&, const std::string&, const std::string&, const std::string&, int, double, double, double, const std::string&), (override));
    MOCK_METHOD(void*, rawConnection, (), (override));
    MOCK_METHOD(bool, saveOximetrySession, (const std::string&, const cpapdash::parser::OximetrySession&), (override));
    MOCK_METHOD(bool, oximetrySessionExists, (const std::string&, const std::string&), (override));
};

// ── Status reporting ────────────────────────────────────────────────────

TEST(BackfillServiceTest, InitialStatusIsIdle) {
    BackfillService::Config cfg;
    cfg.local_dir = "/nonexistent";
    cfg.device_id = "test_device";
    cfg.device_name = "Test CPAP";

    // Use nullptr for DB — we won't actually run a backfill
    BackfillService svc(cfg, nullptr);

    auto status = svc.getStatus();
    EXPECT_EQ(status["status"].asString(), "idle");
    EXPECT_EQ(status["folders_total"].asInt(), 0);
    EXPECT_EQ(status["folders_done"].asInt(), 0);
    EXPECT_EQ(status["sessions_parsed"].asInt(), 0);
    EXPECT_EQ(status["sessions_saved"].asInt(), 0);
    EXPECT_EQ(status["errors"].asInt(), 0);
}

TEST(BackfillServiceTest, StatusJsonHasAllFields) {
    BackfillService::Config cfg;
    cfg.local_dir = "/tmp";
    cfg.device_id = "test";
    cfg.device_name = "Test";

    BackfillService svc(cfg, nullptr);
    auto status = svc.getStatus();

    EXPECT_TRUE(status.isMember("status"));
    EXPECT_TRUE(status.isMember("folders_total"));
    EXPECT_TRUE(status.isMember("folders_done"));
    EXPECT_TRUE(status.isMember("sessions_parsed"));
    EXPECT_TRUE(status.isMember("sessions_saved"));
    EXPECT_TRUE(status.isMember("sessions_deleted"));
    EXPECT_TRUE(status.isMember("errors"));
}

TEST(BackfillServiceTest, TriggerSetsRequestedFlag) {
    BackfillService::Config cfg;
    cfg.local_dir = "/tmp";
    cfg.device_id = "test";
    cfg.device_name = "Test";

    BackfillService svc(cfg, nullptr);
    // Just verify trigger doesn't crash without start()
    svc.trigger("2025-08-15", "2025-08-20");

    // Status should still be idle (worker not running)
    auto status = svc.getStatus();
    EXPECT_EQ(status["status"].asString(), "idle");
}

TEST(BackfillServiceTest, StartAndStopCleanly) {
    BackfillService::Config cfg;
    cfg.local_dir = "/tmp";
    cfg.device_id = "test";
    cfg.device_name = "Test";

    BackfillService svc(cfg, nullptr);
    svc.start();
    svc.stop();

    auto status = svc.getStatus();
    EXPECT_EQ(status["status"].asString(), "idle");
}

// ── Backfill marks sessions completed ───────────────────────────────────

// Helper: write a minimal EDF file (same approach as test_EDFParser)
static void writeMinimalEDF(const std::string& filepath,
                            const std::string& timestamp_str,
                            const std::string& file_type,
                            int duration_seconds = 60) {
    std::ofstream ofs(filepath, std::ios::binary);

    char header[256] = {0};
    memcpy(header, "0       ", 8);
    std::string patient = "X X X X";
    memcpy(header + 8, patient.c_str(), std::min<size_t>(80, patient.size()));
    std::string recording = "Startdate 06-FEB-2026 PSG-CPAP-Device Sn 20123456789 HW 1 SW 2";
    memcpy(header + 88, recording.c_str(), std::min<size_t>(80, recording.size()));
    std::string date = timestamp_str.substr(6, 2) + "." +
                       timestamp_str.substr(4, 2) + "." +
                       timestamp_str.substr(2, 2);
    memcpy(header + 168, date.c_str(), std::min<size_t>(8, date.size()));
    std::string time = timestamp_str.substr(9, 2) + "." +
                       timestamp_str.substr(11, 2) + "." +
                       timestamp_str.substr(13, 2);
    memcpy(header + 176, time.c_str(), std::min<size_t>(8, time.size()));
    memcpy(header + 184, "512     ", 8);
    memcpy(header + 192, "EDF+C   ", 8);
    std::string records_str = std::to_string(duration_seconds);
    while (records_str.size() < 8) records_str += " ";
    memcpy(header + 236, records_str.c_str(), 8);
    memcpy(header + 244, "1       ", 8);
    memcpy(header + 252, "1   ", 4);
    ofs.write(header, 256);

    char signal_header[256] = {0};
    std::string label = file_type == "BRP" ? "Flow" : "Pressure";
    memcpy(signal_header, label.c_str(), std::min<size_t>(16, label.size()));
    memcpy(signal_header + 16, "Internal", 8);
    memcpy(signal_header + 96, "L/min   ", 8);
    memcpy(signal_header + 104, "-100    ", 8);
    memcpy(signal_header + 112, "100     ", 8);
    memcpy(signal_header + 120, "-32768  ", 8);
    memcpy(signal_header + 128, "32767   ", 8);
    memcpy(signal_header + 136, "None", 4);
    memcpy(signal_header + 216, "25      ", 8);
    ofs.write(signal_header, 256);

    int record_size = 25 * 2;  // 25 samples * 2 bytes
    std::vector<char> record_data(record_size, 0);
    for (int i = 0; i < duration_seconds; ++i) {
        ofs.write(record_data.data(), record_size);
    }
}

TEST(BackfillServiceTest, BackfillMarksSessionsCompleted) {
    // Create a temp DATALOG directory with one date folder and one session
    auto tmp = std::filesystem::temp_directory_path() / "cpap_test_backfill";
    auto datalog = tmp / "20260206";
    std::filesystem::create_directories(datalog);

    // Write minimal BRP file (required for session discovery)
    writeMinimalEDF(datalog / "20260206_220000_BRP.edf", "20260206_220000", "BRP", 300);

    auto mock_db = std::make_shared<MockDatabase>();

    BackfillService::Config cfg;
    cfg.local_dir = tmp.string();
    cfg.device_id = "test_device";
    cfg.device_name = "Test CPAP";

    // deleteSessionsByDateFolder is called before re-parsing
    EXPECT_CALL(*mock_db, deleteSessionsByDateFolder("test_device", "20260206"))
        .WillOnce(Return(0));

    // saveSession should be called, and we return true so the code path continues
    EXPECT_CALL(*mock_db, saveSession(_))
        .WillOnce(Return(true));

    // THE KEY ASSERTION: markSessionCompleted must be called after saveSession
    EXPECT_CALL(*mock_db, markSessionCompleted("test_device", _))
        .Times(1)
        .WillOnce(Return(true));

    // updateCheckpointFileSizes is called after save
    EXPECT_CALL(*mock_db, updateCheckpointFileSizesMock("test_device", _))
        .WillOnce(Return(true));

    BackfillService svc(cfg, mock_db);

    // Run backfill synchronously (not via start/trigger which uses a thread)
    // We call trigger + start, then stop after completion
    svc.start();
    svc.trigger("2026-02-06", "2026-02-06");

    // Wait for backfill to complete (poll status)
    for (int i = 0; i < 30; ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
        auto status = svc.getStatus();
        if (status["status"].asString() != "running" &&
            status["status"].asString() != "idle") break;
    }
    svc.stop();

    auto status = svc.getStatus();
    EXPECT_EQ(status["status"].asString(), "complete");
    EXPECT_GE(status["sessions_saved"].asInt(), 1);

    // Cleanup
    std::filesystem::remove_all(tmp);
}

TEST(BackfillServiceTest, BackfillWithoutSaveDoesNotMarkCompleted) {
    // If saveSession fails, markSessionCompleted should NOT be called
    auto tmp = std::filesystem::temp_directory_path() / "cpap_test_backfill2";
    auto datalog = tmp / "20260207";
    std::filesystem::create_directories(datalog);

    writeMinimalEDF(datalog / "20260207_230000_BRP.edf", "20260207_230000", "BRP", 120);

    auto mock_db = std::make_shared<MockDatabase>();

    BackfillService::Config cfg;
    cfg.local_dir = tmp.string();
    cfg.device_id = "test_device";
    cfg.device_name = "Test CPAP";

    EXPECT_CALL(*mock_db, deleteSessionsByDateFolder("test_device", "20260207"))
        .WillOnce(Return(0));

    // saveSession FAILS
    EXPECT_CALL(*mock_db, saveSession(_))
        .WillOnce(Return(false));

    // markSessionCompleted should NOT be called when save fails
    EXPECT_CALL(*mock_db, markSessionCompleted(_, _)).Times(0);
    EXPECT_CALL(*mock_db, updateCheckpointFileSizesMock(_, _)).Times(0);

    BackfillService svc(cfg, mock_db);
    svc.start();
    svc.trigger("2026-02-07", "2026-02-07");

    for (int i = 0; i < 30; ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
        auto status = svc.getStatus();
        if (status["status"].asString() != "running" &&
            status["status"].asString() != "idle") break;
    }
    svc.stop();

    auto status = svc.getStatus();
    EXPECT_EQ(status["status"].asString(), "complete");
    EXPECT_EQ(status["sessions_saved"].asInt(), 0);

    std::filesystem::remove_all(tmp);
}
