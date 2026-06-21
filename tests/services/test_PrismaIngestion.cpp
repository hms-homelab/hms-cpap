#include <gtest/gtest.h>
#include "services/PrismaIngestion.h"
#include "miniz.h"
#include <cpapdash/parser/PrismaParser.h>
#include <cpapdash/parser/Models.h>

#include <filesystem>
#include <fstream>
#include <string>
#include <vector>
#include <utility>

using namespace hms_cpap;
namespace fs = std::filesystem;

namespace {

// Build a ZIP archive at `zip_path` from a list of (in-archive-name, content)
// entries using miniz's writer API. Returns true on success.
bool buildZip(const std::string& zip_path,
              const std::vector<std::pair<std::string, std::string>>& entries) {
    mz_zip_archive zip{};
    if (!mz_zip_writer_init_file(&zip, zip_path.c_str(), 0)) return false;
    bool ok = true;
    for (const auto& [name, content] : entries) {
        if (!mz_zip_writer_add_mem(&zip, name.c_str(), content.data(),
                                   content.size(), MZ_DEFAULT_COMPRESSION)) {
            ok = false;
            break;
        }
    }
    if (!mz_zip_writer_finalize_archive(&zip)) ok = false;
    mz_zip_writer_end(&zip);
    return ok;
}

}  // namespace

class PrismaIngestionTest : public ::testing::Test {
protected:
    std::string tmp_dir;

    void SetUp() override {
        tmp_dir = "/tmp/prisma_ingestion_test_" + std::to_string(getpid());
        fs::create_directories(tmp_dir);
        // ZIP mode extracts into a shared cache dir keyed only by temp path;
        // start each test from a clean slate so caching behaviour is testable.
        fs::remove_all(fs::temp_directory_path() / "prisma_cache");
    }

    void TearDown() override {
        fs::remove_all(tmp_dir);
        fs::remove_all(fs::temp_directory_path() / "prisma_staged");
        fs::remove_all(fs::temp_directory_path() / "prisma_cache");
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

    // SMART max (fw 3.17) combined layout: <serial>/<YYYYMMDD>/<NNNN>/ holding
    // event_NNN.xml + signal_NNN.wmedf together, with 3-digit sequence numbers.
    void createCombinedTree() {
        std::string serial = tmp_dir + "/0040181394";
        auto pair = [](const std::string& dir, int i) {
            char e[40], s[40];
            std::snprintf(e, sizeof e, "/event_%03d.xml", i);
            std::snprintf(s, sizeof s, "/signal_%03d.wmedf", i);
            std::ofstream(dir + e) << "<desc></desc>";
            std::ofstream(dir + s) << "dummy";
        };
        fs::create_directories(serial + "/20260607/0000");
        for (int i = 0; i < 3; ++i) pair(serial + "/20260607/0000", i);   // 3 pairs
        std::ofstream(serial + "/20260607/0000/trendCurves.tc") << "tc";

        fs::create_directories(serial + "/20260620/0001");
        for (int i = 3; i < 5; ++i) pair(serial + "/20260620/0001", i);   // 2 pairs
        std::ofstream(serial + "/20260620/0001/trendCurves.tc") << "tc";

        // stray log dir at the serial root must be ignored
        fs::create_directories(serial + "/log");
        std::ofstream(serial + "/log/service.log") << "log";
    }
};

// ── Combined layout detection (Prisma SMART max, fw 3.17) ───────────────────

TEST_F(PrismaIngestionTest, DetectsCombinedSmartMaxLayout) {
    createCombinedTree();

    // data_dir is the SD root that holds the <serial>/ folder.
    PrismaIngestion ingestion(tmp_dir);
    ASSERT_TRUE(ingestion.initialize());

    auto sessions = ingestion.discoverSessions(std::nullopt);
    EXPECT_EQ(sessions.size(), 5u);   // 3 pairs on 0607 + 2 pairs on 0620
}

TEST_F(PrismaIngestionTest, CombinedLayoutPairsThreeDigitSequence) {
    createCombinedTree();

    PrismaIngestion ingestion(tmp_dir);
    ingestion.initialize();
    auto sessions = ingestion.discoverSessions(std::nullopt);

    ASSERT_EQ(sessions.size(), 5u);
    for (const auto& s : sessions) {
        EXPECT_FALSE(s.event_path.empty());
        EXPECT_FALSE(s.signal_path.empty());
        EXPECT_EQ(s.date_folder.size(), 8u);   // YYYYMMDD, not the NNNN subfolder
    }
}

TEST_F(PrismaIngestionTest, CombinedLayoutWorksWhenRootIsSerialFolder) {
    createCombinedTree();

    // data_dir points straight at the serial folder (no SD-root wrapper).
    PrismaIngestion ingestion(tmp_dir + "/0040181394");
    ASSERT_TRUE(ingestion.initialize());
    EXPECT_EQ(ingestion.discoverSessions(std::nullopt).size(), 5u);
}

TEST_F(PrismaIngestionTest, CombinedLayoutRespectsLastSessionFilter) {
    createCombinedTree();

    PrismaIngestion ingestion(tmp_dir);
    ingestion.initialize();
    auto all = ingestion.discoverSessions(std::nullopt);
    ASSERT_EQ(all.size(), 5u);

    // Everything on/before the first night should be skipped on the next pass.
    auto cutoff = all.front().session_start;
    auto rest = ingestion.discoverSessions(cutoff);
    EXPECT_LT(rest.size(), all.size());
}

// SMART max event XML uses spaced RespEvent attributes (RespEventID = "101").
// Validates, through hms-cpap's linked shared parser, that (a) spaced attributes
// parse (was 0 events) and (b) AHI counts apneas + hypopneas only — not RERA or
// flow limitation.
TEST_F(PrismaIngestionTest, SmartMaxSpacedEventsParseAndAhiExcludesNonRespiratory) {
    std::string xml = tmp_dir + "/event_003.xml";
    {
        std::ofstream o(xml);
        o << "<?xml version=\"1.0\" encoding=\"utf-8\"?>\n<desc>\n"
          << "<DeviceEvent  DeviceEventID=\"0\" Time=\"0\" ParameterID=\"1003\" NewValue=\"2\"/>\n"
          << "<RespEvent RespEventID = \"101\" EndTime = \"120\" Duration = \"15\"/>\n"  // OA
          << "<RespEvent RespEventID = \"102\" EndTime = \"200\" Duration = \"12\"/>\n"  // CA
          << "<RespEvent RespEventID = \"111\" EndTime = \"300\" Duration = \"20\"/>\n"  // hypopnea
          << "<RespEvent RespEventID = \"151\" EndTime = \"400\" Duration = \"5\"/>\n"   // flow limit (not AHI)
          << "<RespEvent RespEventID = \"121\" EndTime = \"500\" Duration = \"8\"/>\n"   // RERA (not AHI)
          << "</desc>\n";
    }

    cpapdash::parser::ParsedSession session;
    auto now = std::chrono::system_clock::now();
    ASSERT_TRUE(cpapdash::parser::PrismaParser::parseEventXml(xml, session, now));

    // Spaced attributes parsed: all 5 mapped RespEvents recorded.
    EXPECT_EQ(session.events.size(), 5u);

    session.duration_seconds = 3600.0;  // 1 hour
    session.calculateMetrics();
    ASSERT_TRUE(session.metrics.has_value());
    // AHI = (OA 1 + CA 1 + hypopnea 1) / 1h = 3.0; RERA + flow limitation excluded.
    EXPECT_NEAR(session.metrics->ahi, 3.0, 0.01);
    EXPECT_EQ(session.metrics->total_events, 5);  // all still recorded for display
}

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

// ── ZIP mode (Prisma Line: therapy.pdat + config.pcfg) ──────────────────────

// Builds therapy.pdat / config.pcfg archives whose extracted contents form a
// nested mnt/flash tree, then verifies discovery works end-to-end from ZIPs.
TEST_F(PrismaIngestionTest, ZipModeExtractsAndDiscovers) {
    ASSERT_TRUE(buildZip(tmp_dir + "/therapy.pdat", {
        {"mnt/flash/data/therapy/events/20260601/event_000500.xml", "<desc></desc>"},
        {"mnt/flash/data/therapy/signals/20260601/signal_000500.wmedf", "dummy"},
        {"mnt/flash/data/therapy/events/20260601/event_000501.xml", "<desc></desc>"},
        {"mnt/flash/data/therapy/signals/20260601/signal_000501.wmedf", "dummy"},
    }));
    ASSERT_TRUE(buildZip(tmp_dir + "/config.pcfg", {
        {"mnt/flash/conf/device.xml", "<DeviceSerialNumber value=\"99887766\"/>"},
    }));

    PrismaIngestion ingestion(tmp_dir);
    ASSERT_TRUE(ingestion.initialize());

    auto sessions = ingestion.discoverSessions(std::nullopt);
    ASSERT_EQ(sessions.size(), 2u);
    EXPECT_EQ(sessions[0].sequence_number, 500);
    EXPECT_EQ(sessions[1].sequence_number, 501);
    EXPECT_EQ(sessions[0].date_folder, "20260601");
    EXPECT_FALSE(sessions[0].signal_path.empty());
}

// device.xml lives only inside config.pcfg; deviceXmlPath() must find it in the
// extracted config cache (config_dir_), not just in data_dir_.
TEST_F(PrismaIngestionTest, ZipModeDeviceXmlFromConfigArchive) {
    ASSERT_TRUE(buildZip(tmp_dir + "/therapy.pdat", {
        {"mnt/flash/data/therapy/events/20260601/event_000500.xml", "<desc></desc>"},
        {"mnt/flash/data/therapy/signals/20260601/signal_000500.wmedf", "dummy"},
    }));
    ASSERT_TRUE(buildZip(tmp_dir + "/config.pcfg", {
        {"mnt/flash/conf/device.xml", "<DeviceSerialNumber value=\"99887766\"/>"},
    }));

    PrismaIngestion ingestion(tmp_dir);
    ASSERT_TRUE(ingestion.initialize());

    std::string dev = ingestion.deviceXmlPath();
    ASSERT_FALSE(dev.empty());
    EXPECT_TRUE(fs::exists(dev));
    // Should resolve into the extracted config cache, not the raw data dir.
    EXPECT_NE(dev.find("prisma_cache"), std::string::npos);

    // And stageSession should pull it in as device.xml.
    auto sessions = ingestion.discoverSessions(std::nullopt);
    ASSERT_EQ(sessions.size(), 1u);
    std::string staged = ingestion.stageSession(sessions[0]);
    EXPECT_TRUE(fs::exists(fs::path(staged) / "device.xml"));
    fs::remove_all(staged);
}

// therapy.pdat present but no config.pcfg: ZIP mode still triggers and discovery
// works from the extracted therapy tree.
TEST_F(PrismaIngestionTest, ZipModeTherapyOnlyNoConfig) {
    ASSERT_TRUE(buildZip(tmp_dir + "/therapy.pdat", {
        {"mnt/flash/data/therapy/events/20260601/event_000500.xml", "<desc></desc>"},
        {"mnt/flash/data/therapy/signals/20260601/signal_000500.wmedf", "dummy"},
    }));

    PrismaIngestion ingestion(tmp_dir);
    ASSERT_TRUE(ingestion.initialize());

    auto sessions = ingestion.discoverSessions(std::nullopt);
    EXPECT_EQ(sessions.size(), 1u);
    EXPECT_EQ(sessions[0].sequence_number, 500);
}

// Re-running initialize() on a fresh instance must reuse the extraction cache
// (no re-extract) and still discover the same sessions deterministically.
TEST_F(PrismaIngestionTest, ZipModeReusesExtractionCache) {
    ASSERT_TRUE(buildZip(tmp_dir + "/therapy.pdat", {
        {"mnt/flash/data/therapy/events/20260601/event_000500.xml", "<desc></desc>"},
        {"mnt/flash/data/therapy/signals/20260601/signal_000500.wmedf", "dummy"},
    }));

    {
        PrismaIngestion first(tmp_dir);
        ASSERT_TRUE(first.initialize());
        EXPECT_EQ(first.discoverSessions(std::nullopt).size(), 1u);
    }

    // Cache now exists; a second instance must reuse it and still work.
    std::string therapy_cache =
        (fs::temp_directory_path() / "prisma_cache" / "therapy").string();
    ASSERT_TRUE(fs::is_directory(therapy_cache));

    PrismaIngestion second(tmp_dir);
    ASSERT_TRUE(second.initialize());
    auto sessions = second.discoverSessions(std::nullopt);
    ASSERT_EQ(sessions.size(), 1u);
    EXPECT_EQ(sessions[0].sequence_number, 500);
}

// A corrupt/non-ZIP therapy.pdat triggers ZIP mode but extraction fails; with no
// usable events/signals tree, initialize() must fail.
TEST_F(PrismaIngestionTest, ZipModeCorruptArchiveFailsInit) {
    std::ofstream(tmp_dir + "/therapy.pdat") << "this is not a zip file";

    PrismaIngestion ingestion(tmp_dir);
    EXPECT_FALSE(ingestion.initialize());
}

// ── deviceXmlPath fallbacks ─────────────────────────────────────────────────

// device.xml sitting directly in the data dir (no conf/ subdir) is the last
// fallback in deviceXmlPath().
TEST_F(PrismaIngestionTest, DeviceXmlDirectInDataDir) {
    createRawTree();
    std::ofstream(tmp_dir + "/device.xml") << "<DeviceSerialNumber value=\"11112222\"/>";

    PrismaIngestion ingestion(tmp_dir);
    ASSERT_TRUE(ingestion.initialize());

    std::string dev = ingestion.deviceXmlPath();
    ASSERT_FALSE(dev.empty());
    EXPECT_EQ(dev, tmp_dir + "/device.xml");
}

// conf/device.xml (without the mnt/flash prefix) is a recognised fallback.
TEST_F(PrismaIngestionTest, DeviceXmlInConfSubdir) {
    createRawTree();
    fs::create_directories(tmp_dir + "/conf");
    std::ofstream(tmp_dir + "/conf/device.xml") << "<DeviceSerialNumber value=\"33334444\"/>";

    PrismaIngestion ingestion(tmp_dir);
    ASSERT_TRUE(ingestion.initialize());

    std::string dev = ingestion.deviceXmlPath();
    EXPECT_EQ(dev, tmp_dir + "/conf/device.xml");
}

// No device.xml anywhere -> empty path, and stageSession copies no device.xml.
TEST_F(PrismaIngestionTest, DeviceXmlAbsentReturnsEmpty) {
    createRawTree();

    PrismaIngestion ingestion(tmp_dir);
    ASSERT_TRUE(ingestion.initialize());

    EXPECT_TRUE(ingestion.deviceXmlPath().empty());

    auto sessions = ingestion.discoverSessions(std::nullopt);
    ASSERT_GE(sessions.size(), 1u);
    std::string staged = ingestion.stageSession(sessions[0]);
    EXPECT_FALSE(fs::exists(fs::path(staged) / "device.xml"));
    fs::remove_all(staged);
}

// ── File pairing / discovery edge cases ─────────────────────────────────────

// A date folder in events/ with no matching folder in signals/ is skipped.
TEST_F(PrismaIngestionTest, EventDateFolderWithoutSignalFolderSkipped) {
    fs::create_directories(tmp_dir + "/events/20260514");
    fs::create_directories(tmp_dir + "/events/20260515");
    fs::create_directories(tmp_dir + "/signals/20260514");
    // No signals/20260515 directory at all.

    std::ofstream(tmp_dir + "/events/20260514/event_000370.xml") << "<desc></desc>";
    std::ofstream(tmp_dir + "/signals/20260514/signal_000370.wmedf") << "dummy";
    std::ofstream(tmp_dir + "/events/20260515/event_000380.xml") << "<desc></desc>";

    PrismaIngestion ingestion(tmp_dir);
    ASSERT_TRUE(ingestion.initialize());
    auto sessions = ingestion.discoverSessions(std::nullopt);

    ASSERT_EQ(sessions.size(), 1u);
    EXPECT_EQ(sessions[0].date_folder, "20260514");
    EXPECT_EQ(sessions[0].sequence_number, 370);
}

// Non-8-digit date folders and files that don't match the seq regex are ignored.
TEST_F(PrismaIngestionTest, NonMatchingFoldersAndFilesIgnored) {
    fs::create_directories(tmp_dir + "/events/20260514");
    fs::create_directories(tmp_dir + "/signals/20260514");
    // Bogus date folders that must be ignored (wrong length / non-numeric).
    fs::create_directories(tmp_dir + "/events/2026051");      // 7 digits
    fs::create_directories(tmp_dir + "/events/notadate1");    // non-numeric
    fs::create_directories(tmp_dir + "/signals/2026051");
    fs::create_directories(tmp_dir + "/signals/notadate1");

    // Valid pair.
    std::ofstream(tmp_dir + "/events/20260514/event_000370.xml") << "<desc></desc>";
    std::ofstream(tmp_dir + "/signals/20260514/signal_000370.wmedf") << "dummy";
    // Files that don't match the (event|signal)_<digits> pattern -> ignored.
    // (Sequence width varies by model: 6 digits on Prisma Smart, 3 on SMART max,
    // so only a non-numeric sequence is reliably "not a session file".)
    std::ofstream(tmp_dir + "/events/20260514/readme.txt") << "ignore";
    std::ofstream(tmp_dir + "/signals/20260514/signal_abc.wmedf") << "ignore";

    PrismaIngestion ingestion(tmp_dir);
    ASSERT_TRUE(ingestion.initialize());
    auto sessions = ingestion.discoverSessions(std::nullopt);

    ASSERT_EQ(sessions.size(), 1u);
    EXPECT_EQ(sessions[0].sequence_number, 370);
    EXPECT_FALSE(sessions[0].event_path.empty());
}

// discoverSessions() lazily initialises when initialize() was not called first.
TEST_F(PrismaIngestionTest, DiscoverSessionsLazyInitializes) {
    createRawTree();

    PrismaIngestion ingestion(tmp_dir);
    // No explicit initialize() call.
    auto sessions = ingestion.discoverSessions(std::nullopt);
    EXPECT_EQ(sessions.size(), 3u);
}

// discoverSessions() on an uninitialisable tree returns empty (lazy init fails).
TEST_F(PrismaIngestionTest, DiscoverSessionsReturnsEmptyWhenInitFails) {
    // tmp_dir has no events/signals and no ZIPs.
    PrismaIngestion ingestion(tmp_dir);
    auto sessions = ingestion.discoverSessions(std::nullopt);
    EXPECT_TRUE(sessions.empty());
}

// last_session_start in the past keeps all date folders (date >= last_date) and
// the timestamp filter does not drop sessions strictly after it.
TEST_F(PrismaIngestionTest, PastFilterKeepsAllSessions) {
    createRawTree();

    PrismaIngestion ingestion(tmp_dir);
    ASSERT_TRUE(ingestion.initialize());

    // A clearly-past timestamp: 2000-01-01.
    std::tm tm{};
    tm.tm_year = 2000 - 1900;
    tm.tm_mon = 0;
    tm.tm_mday = 1;
    auto past = std::chrono::system_clock::from_time_t(mktime(&tm));

    auto sessions = ingestion.discoverSessions(past);
    EXPECT_EQ(sessions.size(), 3u);
}

// A future last_session_start filters out everything (date folders < last_date).
TEST_F(PrismaIngestionTest, FutureFilterDropsAllSessions) {
    createRawTree();

    PrismaIngestion ingestion(tmp_dir);
    ASSERT_TRUE(ingestion.initialize());

    std::tm tm{};
    tm.tm_year = 2099 - 1900;
    tm.tm_mon = 11;
    tm.tm_mday = 31;
    auto future = std::chrono::system_clock::from_time_t(mktime(&tm));

    auto sessions = ingestion.discoverSessions(future);
    EXPECT_TRUE(sessions.empty());
}

// stageSession with an empty event_path copies only the signal (event optional).
TEST_F(PrismaIngestionTest, StageSessionWithoutEventCopiesSignalOnly) {
    fs::create_directories(tmp_dir + "/events/20260514");
    fs::create_directories(tmp_dir + "/signals/20260514");
    std::ofstream(tmp_dir + "/signals/20260514/signal_000370.wmedf") << "dummy";

    PrismaIngestion ingestion(tmp_dir);
    ASSERT_TRUE(ingestion.initialize());
    auto sessions = ingestion.discoverSessions(std::nullopt);
    ASSERT_EQ(sessions.size(), 1u);
    ASSERT_TRUE(sessions[0].event_path.empty());

    std::string staged = ingestion.stageSession(sessions[0]);
    bool has_signal = false, has_event = false;
    for (const auto& entry : fs::directory_iterator(staged)) {
        std::string name = entry.path().filename().string();
        if (name.rfind("signal_", 0) == 0) has_signal = true;
        if (name.rfind("event_", 0) == 0) has_event = true;
    }
    EXPECT_TRUE(has_signal);
    EXPECT_FALSE(has_event);
    fs::remove_all(staged);
}

// stageSession is idempotent: re-staging clears stale contents and re-copies.
TEST_F(PrismaIngestionTest, StageSessionClearsStaleContents) {
    createRawTree();

    PrismaIngestion ingestion(tmp_dir);
    ASSERT_TRUE(ingestion.initialize());
    auto sessions = ingestion.discoverSessions(std::nullopt);
    ASSERT_GE(sessions.size(), 1u);

    std::string staged = ingestion.stageSession(sessions[0]);
    // Plant a stale file that should be wiped on the next stage.
    std::ofstream(fs::path(staged) / "stale_marker.txt") << "old";
    ASSERT_TRUE(fs::exists(fs::path(staged) / "stale_marker.txt"));

    std::string staged2 = ingestion.stageSession(sessions[0]);
    EXPECT_EQ(staged, staged2);
    EXPECT_FALSE(fs::exists(fs::path(staged) / "stale_marker.txt"));

    bool has_signal = false;
    for (const auto& entry : fs::directory_iterator(staged)) {
        if (entry.path().filename().string().rfind("signal_", 0) == 0) has_signal = true;
    }
    EXPECT_TRUE(has_signal);
    fs::remove_all(staged);
}