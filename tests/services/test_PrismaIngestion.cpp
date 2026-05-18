#include <gtest/gtest.h>
#include "services/PrismaIngestion.h"

#include <filesystem>
#include <fstream>

using namespace hms_cpap;
namespace fs = std::filesystem;

class PrismaIngestionTest : public ::testing::Test {
protected:
    std::string tmp_dir;

    void SetUp() override {
        tmp_dir = "/tmp/prisma_ingestion_test_" + std::to_string(getpid());
        fs::create_directories(tmp_dir);
    }

    void TearDown() override {
        fs::remove_all(tmp_dir);
        fs::remove_all(fs::temp_directory_path() / "prisma_staged");
    }

    void createRawTree() {
        fs::create_directories(tmp_dir + "/events/20260514");
        fs::create_directories(tmp_dir + "/events/20260515");
        fs::create_directories(tmp_dir + "/signals/20260514");
        fs::create_directories(tmp_dir + "/signals/20260515");

        std::ofstream(tmp_dir + "/events/20260514/event_000370.xml") << "<desc></desc>";
        std::ofstream(tmp_dir + "/events/20260514/event_000371.xml") << "<desc></desc>";
        std::ofstream(tmp_dir + "/events/20260515/event_000380.xml") << "<desc></desc>";

        std::ofstream(tmp_dir + "/signals/20260514/signal_000370.wmedf") << "dummy";
        std::ofstream(tmp_dir + "/signals/20260514/signal_000371.wmedf") << "dummy";
        std::ofstream(tmp_dir + "/signals/20260515/signal_000380.wmedf") << "dummy";
    }

    void createNestedTree() {
        std::string base = tmp_dir + "/mnt/flash/data/therapy";
        fs::create_directories(base + "/events/20260514");
        fs::create_directories(base + "/signals/20260514");

        std::ofstream(base + "/events/20260514/event_000370.xml") << "<desc></desc>";
        std::ofstream(base + "/signals/20260514/signal_000370.wmedf") << "dummy";

        fs::create_directories(tmp_dir + "/mnt/flash/conf");
        std::ofstream(tmp_dir + "/mnt/flash/conf/device.xml")
            << "<DeviceSerialNumber value=\"12345678\"/>";
    }
};

// ── Raw directory detection (Prisma Smart) ──────────────────────────────────

TEST_F(PrismaIngestionTest, DetectsRawDirectoryStructure) {
    createRawTree();

    PrismaIngestion ingestion(tmp_dir);
    ASSERT_TRUE(ingestion.initialize());

    auto sessions = ingestion.discoverSessions(std::nullopt);
    EXPECT_EQ(sessions.size(), 3u);
}

TEST_F(PrismaIngestionTest, DiscoverSessionsPairsEventAndSignal) {
    createRawTree();

    PrismaIngestion ingestion(tmp_dir);
    ingestion.initialize();
    auto sessions = ingestion.discoverSessions(std::nullopt);

    ASSERT_EQ(sessions.size(), 3u);

    EXPECT_EQ(sessions[0].sequence_number, 370);
    EXPECT_EQ(sessions[0].date_folder, "20260514");
    EXPECT_FALSE(sessions[0].event_path.empty());
    EXPECT_FALSE(sessions[0].signal_path.empty());

    EXPECT_EQ(sessions[1].sequence_number, 371);
    EXPECT_EQ(sessions[2].sequence_number, 380);
    EXPECT_EQ(sessions[2].date_folder, "20260515");
}

TEST_F(PrismaIngestionTest, SkipsSignalWithoutPair) {
    fs::create_directories(tmp_dir + "/events/20260514");
    fs::create_directories(tmp_dir + "/signals/20260514");

    // Signal only, no event
    std::ofstream(tmp_dir + "/signals/20260514/signal_000370.wmedf") << "dummy";
    // Event only, no signal
    std::ofstream(tmp_dir + "/events/20260514/event_000371.xml") << "<desc></desc>";

    PrismaIngestion ingestion(tmp_dir);
    ingestion.initialize();
    auto sessions = ingestion.discoverSessions(std::nullopt);

    // signal_000370 has no event but still has a signal -> included (event is optional)
    // event_000371 has no signal -> excluded
    EXPECT_EQ(sessions.size(), 1u);
    EXPECT_EQ(sessions[0].sequence_number, 370);
    EXPECT_TRUE(sessions[0].event_path.empty());
}

TEST_F(PrismaIngestionTest, FiltersByLastSessionStart) {
    createRawTree();

    PrismaIngestion ingestion(tmp_dir);
    ingestion.initialize();

    // All sessions use fallback date-based timestamps since dummy files
    // aren't valid WMEDF. May 14 sessions will be at midnight 2026-05-14.
    // Filter should exclude sessions <= given timestamp.

    // Get all sessions first
    auto all = ingestion.discoverSessions(std::nullopt);
    ASSERT_GE(all.size(), 2u);

    // Use the first session's start as filter — should exclude it
    auto after_first = ingestion.discoverSessions(all[0].session_start);
    EXPECT_LT(after_first.size(), all.size());
}

// ── Nested directory detection (from ZIP-extracted tree) ────────────────────

TEST_F(PrismaIngestionTest, DetectsNestedMntFlashStructure) {
    createNestedTree();

    PrismaIngestion ingestion(tmp_dir);
    ASSERT_TRUE(ingestion.initialize());

    auto sessions = ingestion.discoverSessions(std::nullopt);
    EXPECT_EQ(sessions.size(), 1u);
    EXPECT_EQ(sessions[0].sequence_number, 370);
}

TEST_F(PrismaIngestionTest, FindsDeviceXmlInConf) {
    createNestedTree();

    PrismaIngestion ingestion(tmp_dir);
    ingestion.initialize();

    std::string dev = ingestion.deviceXmlPath();
    EXPECT_FALSE(dev.empty());
    EXPECT_TRUE(fs::exists(dev));
}

// ── Session staging ─────────────────────────────────────────────────────────

TEST_F(PrismaIngestionTest, StageSessionCopiesFiles) {
    createRawTree();

    PrismaIngestion ingestion(tmp_dir);
    ingestion.initialize();
    auto sessions = ingestion.discoverSessions(std::nullopt);
    ASSERT_GE(sessions.size(), 1u);

    std::string staged = ingestion.stageSession(sessions[0]);
    EXPECT_TRUE(fs::is_directory(staged));

    bool has_signal = false, has_event = false;
    for (const auto& entry : fs::directory_iterator(staged)) {
        std::string name = entry.path().filename().string();
        if (name.find("signal_") == 0) has_signal = true;
        if (name.find("event_") == 0) has_event = true;
    }
    EXPECT_TRUE(has_signal);
    EXPECT_TRUE(has_event);

    fs::remove_all(staged);
}

TEST_F(PrismaIngestionTest, StageSessionIncludesDeviceXml) {
    createNestedTree();

    PrismaIngestion ingestion(tmp_dir);
    ingestion.initialize();
    auto sessions = ingestion.discoverSessions(std::nullopt);
    ASSERT_GE(sessions.size(), 1u);

    std::string staged = ingestion.stageSession(sessions[0]);
    EXPECT_TRUE(fs::exists(fs::path(staged) / "device.xml"));

    fs::remove_all(staged);
}

// ── Edge cases ──────────────────────────────────────────────────────────────

TEST_F(PrismaIngestionTest, EmptyDirectoryReturnsNoSessions) {
    fs::create_directories(tmp_dir + "/events");
    fs::create_directories(tmp_dir + "/signals");

    PrismaIngestion ingestion(tmp_dir);
    ingestion.initialize();
    auto sessions = ingestion.discoverSessions(std::nullopt);
    EXPECT_TRUE(sessions.empty());
}

TEST_F(PrismaIngestionTest, NoEventsSignalsDirsFailsInit) {
    // Empty dir with no events/ or signals/
    PrismaIngestion ingestion(tmp_dir);
    EXPECT_FALSE(ingestion.initialize());
}

TEST_F(PrismaIngestionTest, SessionsAreSortedByStart) {
    createRawTree();

    PrismaIngestion ingestion(tmp_dir);
    ingestion.initialize();
    auto sessions = ingestion.discoverSessions(std::nullopt);

    for (size_t i = 1; i < sessions.size(); ++i) {
        EXPECT_LE(sessions[i - 1].session_start, sessions[i].session_start);
    }
}

// ── Real fixture test (if available) ────────────────────────────────────────

TEST_F(PrismaIngestionTest, RealSampleData) {
    std::string sample_dir = "../lowenstein_samples/extracted_therapy/mnt/flash/data/therapy";
    // Also try relative to source dir
    if (!fs::is_directory(sample_dir)) {
        sample_dir = "../../lowenstein_samples/extracted_therapy/mnt/flash/data/therapy";
    }
    if (!fs::is_directory(sample_dir)) {
        GTEST_SKIP() << "Lowenstein sample data not found";
    }

    PrismaIngestion ingestion(sample_dir);
    ASSERT_TRUE(ingestion.initialize());

    auto sessions = ingestion.discoverSessions(std::nullopt);
    EXPECT_GT(sessions.size(), 0u);

    for (const auto& s : sessions) {
        EXPECT_GT(s.sequence_number, 0);
        EXPECT_FALSE(s.signal_path.empty());
        EXPECT_NE(s.session_start, std::chrono::system_clock::time_point{});
    }
}
