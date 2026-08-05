#include "EquipmentStubs.h"
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
    HMS_CPAP_STUB_EQUIPMENT_METHODS
    HMS_CPAP_STUB_SYNC_FOLDER_METHODS
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
    // SDD-010: local_dir is the card ROOT, so the date folder lives under its
    // DATALOG, not directly inside it.
    auto tmp = std::filesystem::temp_directory_path() / "cpap_test_backfill";
    auto datalog = tmp / "DATALOG" / "20260206";
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
    auto datalog = tmp / "DATALOG" / "20260207";   // SDD-010: root/DATALOG/<date>
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

// ── Single-day reparse is folder-scoped ─────────────────────────────────
// A reparse of sleep-day D must operate on folder D ONLY, and must NOT touch
// the adjacent (previous) night's folder — even though that folder may hold
// files timestamped on day D (cross-midnight early-AM sessions belong to the
// night that began the evening before, and live under that earlier folder).
// This is the guarantee the per-session UI reparse depends on.
TEST(BackfillServiceTest, SingleDayReparseTouchesOnlyThatFolder) {
    auto tmp = std::filesystem::temp_directory_path() / "cpap_test_backfill_scope";
    // Previous night's folder (must be left alone): contains a file whose
    // *filename* is dated 20260522 but which belongs to the 20260521 night.
    // SDD-010: both live under the card root's DATALOG.
    auto prev = tmp / "DATALOG" / "20260521";
    // Target folder for the reparse.
    auto target = tmp / "DATALOG" / "20260522";
    std::filesystem::create_directories(prev);
    std::filesystem::create_directories(target);

    writeMinimalEDF(prev / "20260522_043426_BRP.edf", "20260522_043426", "BRP", 300);
    writeMinimalEDF(target / "20260522_234902_BRP.edf", "20260522_234902", "BRP", 300);

    auto mock_db = std::make_shared<MockDatabase>();

    BackfillService::Config cfg;
    cfg.local_dir = tmp.string();
    cfg.device_id = "test_device";
    cfg.device_name = "Test CPAP";

    // ONLY the target folder may be deleted/reparsed.
    EXPECT_CALL(*mock_db, deleteSessionsByDateFolder("test_device", "20260522"))
        .WillOnce(Return(0));
    // The previous night's folder must NEVER be touched.
    EXPECT_CALL(*mock_db, deleteSessionsByDateFolder("test_device", "20260521"))
        .Times(0);

    EXPECT_CALL(*mock_db, saveSession(_)).WillOnce(Return(true));
    EXPECT_CALL(*mock_db, markSessionCompleted("test_device", _))
        .Times(1).WillOnce(Return(true));
    EXPECT_CALL(*mock_db, updateCheckpointFileSizesMock("test_device", _))
        .WillOnce(Return(true));

    BackfillService svc(cfg, mock_db);
    svc.start();
    // Reparse only sleep-day 2026-05-22 → folder 20260522.
    svc.trigger("2026-05-22", "2026-05-22");

    for (int i = 0; i < 30; ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
        auto status = svc.getStatus();
        if (status["status"].asString() != "running" &&
            status["status"].asString() != "idle") break;
    }
    svc.stop();

    auto status = svc.getStatus();
    EXPECT_EQ(status["status"].asString(), "complete");
    EXPECT_EQ(status["folders_total"].asInt(), 1);
    EXPECT_EQ(status["sessions_saved"].asInt(), 1);

    std::filesystem::remove_all(tmp);
}

// ── Dashboard is fed even when STR.edf is missing (issue #16) ────────────
// cpap_daily_summary is the ONLY table getDashboard() reads. STR.edf is its
// preferred source, but a card whose machine has not written one yet has no STR
// to parse. Before the fix the backfill finished "complete" with sessions saved
// and the dashboard blank, which reads as "it imported my data and then lost it".
//
// SDD-010 note: this used to be framed as "the user mounted DATALOG itself".
// That configuration is now REFUSED outright rather than tolerated (see
// CardLayoutTest and PreflightServiceTest) because it silently costs the user
// every root file. What remains, and what this test now covers, is the
// legitimate case: a correct card ROOT that simply has no STR.edf in it yet.
TEST(BackfillServiceTest, BackfillWithoutStrDerivesDailySummaryFromSessions) {
    auto tmp = std::filesystem::temp_directory_path() / "cpap_test_backfill_nostr";
    std::filesystem::remove_all(tmp);
    auto datalog = tmp / "DATALOG";
    auto folder = datalog / "20260208";
    std::filesystem::create_directories(folder);

    writeMinimalEDF(folder / "20260208_230000_BRP.edf", "20260208_230000", "BRP", 300);

    // A valid root: DATALOG present, no STR anywhere.
    ASSERT_FALSE(std::filesystem::exists(tmp / "STR.edf"));
    ASSERT_FALSE(std::filesystem::exists(datalog / "STR.edf"));

    auto mock_db = std::make_shared<MockDatabase>();

    BackfillService::Config cfg;
    cfg.local_dir = tmp.string();          // SDD-010: the ROOT, not the DATALOG
    cfg.device_id = "test_device";
    cfg.device_name = "Test CPAP";

    EXPECT_CALL(*mock_db, deleteSessionsByDateFolder("test_device", "20260208"))
        .WillOnce(Return(0));
    EXPECT_CALL(*mock_db, saveSession(_)).WillOnce(Return(true));
    EXPECT_CALL(*mock_db, markSessionCompleted("test_device", _))
        .Times(1).WillOnce(Return(true));
    EXPECT_CALL(*mock_db, updateCheckpointFileSizesMock("test_device", _))
        .WillOnce(Return(true));

    // No STR to save...
    EXPECT_CALL(*mock_db, saveSTRDailyRecords(_)).Times(0);
    // ...so the summary MUST be derived from the sessions instead.
    EXPECT_CALL(*mock_db, aggregateDailySummaryFromSessions("test_device"))
        .Times(1).WillOnce(Return(true));

    BackfillService svc(cfg, mock_db);
    svc.start();
    svc.trigger("2026-02-08", "2026-02-08");

    for (int i = 0; i < 30; ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
        auto status = svc.getStatus();
        if (status["status"].asString() != "running" &&
            status["status"].asString() != "idle") break;
    }
    svc.stop();

    EXPECT_EQ(svc.getStatus()["sessions_saved"].asInt(), 1);

    std::filesystem::remove_all(tmp);
}

// ── SDD-010: STR is resolved at the ROOT and nowhere else ────────────────
// There used to be a fallback that searched inside DATALOG when the root probe
// missed. ResMed never writes STR there, so it could not succeed; all it did
// was let a misconfigured path masquerade as a missing file. This test pins the
// fallback as DELETED: an STR sitting in the wrong place must be ignored, not
// rescued.
TEST(BackfillServiceTest, AnStrInsideDatalogIsIgnoredBecauseThatIsNotWhereItLives) {
    auto tmp = std::filesystem::temp_directory_path() / "cpap_test_backfill_strindatalog";
    std::filesystem::remove_all(tmp);
    auto datalog = tmp / "DATALOG";
    auto folder = datalog / "20260209";
    std::filesystem::create_directories(folder);

    writeMinimalEDF(folder / "20260209_230000_BRP.edf", "20260209_230000", "BRP", 300);

    // An STR in the WRONG place. If the old fallback were still here this would
    // be picked up and parsed; it must be passed over entirely. Deliberately not
    // a valid EDF: reaching the parser at all is itself the failure.
    std::ofstream(datalog / "STR.edf") << "this must never be parsed";
    ASSERT_TRUE(std::filesystem::exists(datalog / "STR.edf"));
    ASSERT_FALSE(std::filesystem::exists(tmp / "STR.edf"));

    auto mock_db = std::make_shared<MockDatabase>();

    BackfillService::Config cfg;
    cfg.local_dir = tmp.string();
    cfg.device_id = "test_device";
    cfg.device_name = "Test CPAP";

    EXPECT_CALL(*mock_db, deleteSessionsByDateFolder("test_device", "20260209"))
        .WillOnce(Return(0));
    EXPECT_CALL(*mock_db, saveSession(_)).WillOnce(Return(true));
    EXPECT_CALL(*mock_db, markSessionCompleted("test_device", _))
        .Times(1).WillOnce(Return(true));
    EXPECT_CALL(*mock_db, updateCheckpointFileSizesMock("test_device", _))
        .WillOnce(Return(true));

    // THE ASSERTION: the misplaced STR is never parsed or saved...
    EXPECT_CALL(*mock_db, saveSTRDailyRecords(_)).Times(0);
    // ...so the summary falls back to per-session aggregation, exactly as it
    // would if no STR existed at all.
    EXPECT_CALL(*mock_db, aggregateDailySummaryFromSessions("test_device"))
        .Times(1).WillOnce(Return(true));

    BackfillService svc(cfg, mock_db);
    svc.start();
    svc.trigger("2026-02-09", "2026-02-09");

    for (int i = 0; i < 30; ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
        auto status = svc.getStatus();
        if (status["status"].asString() != "running" &&
            status["status"].asString() != "idle") break;
    }
    svc.stop();

    std::filesystem::remove_all(tmp);
}

// ── SDD-010: a DATALOG-pointed root imports NOTHING ──────────────────────
// The whole point of the hard fail. A path one level too deep would happily
// discover sessions and silently lose every root file, so it must be refused
// before any database work happens rather than half-imported.
TEST(BackfillServiceTest, ALocalDirPointedAtDatalogRefusesAndImportsNothing) {
    auto tmp = std::filesystem::temp_directory_path() / "cpap_test_backfill_isdatalog";
    std::filesystem::remove_all(tmp);
    auto datalog = tmp / "DATALOG";
    auto folder = datalog / "20260210";
    std::filesystem::create_directories(folder);

    writeMinimalEDF(folder / "20260210_230000_BRP.edf", "20260210_230000", "BRP", 300);

    auto mock_db = std::make_shared<MockDatabase>();

    BackfillService::Config cfg;
    cfg.local_dir = datalog.string();      // one level too deep, on purpose
    cfg.device_id = "test_device";
    cfg.device_name = "Test CPAP";

    // Nothing may be deleted, saved, completed or summarised.
    EXPECT_CALL(*mock_db, deleteSessionsByDateFolder(_, _)).Times(0);
    EXPECT_CALL(*mock_db, saveSession(_)).Times(0);
    EXPECT_CALL(*mock_db, markSessionCompleted(_, _)).Times(0);
    EXPECT_CALL(*mock_db, saveSTRDailyRecords(_)).Times(0);
    EXPECT_CALL(*mock_db, aggregateDailySummaryFromSessions(_)).Times(0);

    BackfillService svc(cfg, mock_db);
    svc.start();
    svc.trigger("2026-02-10", "2026-02-10");

    for (int i = 0; i < 30; ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
        auto status = svc.getStatus();
        if (status["status"].asString() != "running" &&
            status["status"].asString() != "idle") break;
    }
    svc.stop();

    auto status = svc.getStatus();
    EXPECT_EQ(status["status"].asString(), "error");
    EXPECT_EQ(status["sessions_saved"].asInt(), 0);
    // The message has to be actionable, not just "failed".
    EXPECT_NE(status["error_message"].asString().find("DATALOG"), std::string::npos)
        << "error should explain the layout problem, got: "
        << status["error_message"].asString();

    std::filesystem::remove_all(tmp);
}
