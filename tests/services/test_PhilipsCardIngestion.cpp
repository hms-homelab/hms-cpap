// A Philips DreamStation card whose files are plain, read from a folder or
// through an ez Share, and its session charted live while it is recorded.
//
// Synthetic cards only (tests/fixtures/philips/gen/make_fixtures.py): session
// 0x40 holds four 240 s flow blocks in its .B05, then its .B01 and .B02. The
// machine appends the open session's .B05 a block at a time and writes the
// .B01 and .B02 at mask-off, so a test that stages the first k blocks alone
// stands for a session being recorded.
#include <gtest/gtest.h>

#include "clients/IDataSource.h"
#include "database/SQLiteDatabase.h"
#include "services/BurstCollectorService.h"
#include "services/DataPublisherService.h"
#include "services/PhilipsCardIngestion.h"
#include "services/PhilipsCardMirror.h"

#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>
#ifndef _WIN32
#include <unistd.h>
#else
#include <process.h>
#endif

using namespace hms_cpap;
namespace fs = std::filesystem;

namespace {

fs::path fixtures() {
    for (const fs::path p : {fs::path(__FILE__).parent_path() / ".." / "fixtures" / "philips",
                             fs::path("tests/fixtures/philips"),
                             fs::path("../tests/fixtures/philips"),
                             fs::path("../../tests/fixtures/philips")}) {
        std::error_code ec;
        if (fs::is_directory(p / "ds2_live", ec)) return p;
    }
    return {};
}

std::vector<char> bytesOf(const fs::path& p) {
    std::ifstream in(p, std::ios::binary);
    return {std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
}

void put(const fs::path& p, const std::vector<char>& bytes) {
    fs::create_directories(p.parent_path());
    std::ofstream(p, std::ios::binary).write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
}

constexpr int kBlocks = 4;   // blocks in the fixture's .B05

class PhilipsCardTest : public ::testing::Test {
protected:
    fs::path root_;
    fs::path src_;

    void SetUp() override {
        src_ = fixtures();
        if (src_.empty()) GTEST_SKIP() << "tests/fixtures/philips not found";
        root_ = fs::temp_directory_path() /
                ("hms_cpap_philips_" + std::to_string(::getpid()) + "_" +
                 ::testing::UnitTest::GetInstance()->current_test_info()->name());
        fs::remove_all(root_);
    }
    void TearDown() override {
        if (!root_.empty()) fs::remove_all(root_);
    }

    /// The card at [card] as the machine has it after [blocks] blocks of the
    /// session; [ended]: its .B01 and .B02 are written too.
    void stage(const fs::path& card, int blocks, bool ended) {
        const fs::path from = src_ / "ds2_live" / "P-SERIES" / "PXLIVE01";
        const fs::path to = card / "P-SERIES" / "PXLIVE01";
        put(to / "PROP.BIN", bytesOf(from / "PROP.BIN"));
        auto wave = bytesOf(from / "P0" / "00000040.B05");
        wave.resize(wave.size() / kBlocks * static_cast<size_t>(blocks));
        put(to / "P0" / "00000040.B05", wave);
        std::error_code ec;
        for (const char* ext : {"B01", "B02"}) {
            const fs::path f = to / "P0" / (std::string("00000040.") + ext);
            if (ended) put(f, bytesOf(from / "P0" / f.filename()));
            else fs::remove(f, ec);
        }
    }
};

}  // namespace

TEST_F(PhilipsCardTest, TheCardIsFoundUnderItsPSeriesFolder) {
    stage(root_, 1, false);
    const auto dir = PhilipsCardIngestion::findCardDir(root_.string());
    ASSERT_TRUE(dir.has_value());
    EXPECT_EQ(fs::path(*dir).filename(), "PXLIVE01");
}

TEST_F(PhilipsCardTest, AnOpenSessionIsFlowOnlyWithItsBreathing) {
    stage(root_, 2, false);
    const auto read = PhilipsCardIngestion(root_.string()).readSessions("dev", "DreamStation 2");
    ASSERT_TRUE(read.ok) << read.error;
    ASSERT_EQ(read.sessions.size(), 1u);
    const auto& one = read.sessions[0];
    EXPECT_EQ(one.number, 0x40u);
    EXPECT_FALSE(one.closed);
    const auto& s = *one.parsed;
    EXPECT_EQ(s.duration_seconds.value_or(0), 480);   // two 240 s blocks
    ASSERT_GE(s.breathing_summary.size(), 7u);
    for (const auto& m : s.breathing_summary) {
        EXPECT_FALSE(m.pressure_measured) << "no pressure is claimed before mask-off";
        EXPECT_TRUE(m.flow_measured);
    }
    // What the flow gives: the breathing rate and its timing.
    int with_rate = 0, with_ie = 0;
    for (const auto& m : s.breathing_summary) {
        if (m.respiratory_rate.has_value()) ++with_rate;
        if (m.ie_ratio.has_value()) ++with_ie;
    }
    EXPECT_GT(with_rate, 0);
    EXPECT_GT(with_ie, 0);
}

TEST_F(PhilipsCardTest, AnEndedSessionKeepsItsStartAndBringsItsPressure) {
    stage(root_, 2, false);
    const auto open = PhilipsCardIngestion(root_.string()).readSessions("dev", "DreamStation 2");
    ASSERT_TRUE(open.ok);
    ASSERT_EQ(open.sessions.size(), 1u);

    stage(root_, kBlocks, true);
    const auto ended = PhilipsCardIngestion(root_.string()).readSessions("dev", "DreamStation 2");
    ASSERT_TRUE(ended.ok);
    ASSERT_EQ(ended.sessions.size(), 1u);
    EXPECT_TRUE(ended.sessions[0].closed);
    // The same session, the same start: the stored row is updated, not doubled.
    EXPECT_EQ(*ended.sessions[0].parsed->session_start, *open.sessions[0].parsed->session_start);
    // The machine's two-minute pressure arrives with the events.
    int with_pressure = 0;
    for (const auto& m : ended.sessions[0].parsed->breathing_summary)
        if (m.epr_pressure.has_value()) ++with_pressure;
    EXPECT_GT(with_pressure, 0);
}

TEST_F(PhilipsCardTest, AnEncryptedCardGivesNothingAndSaysSo) {
    const auto read =
        PhilipsCardIngestion((src_ / "ds2_encrypted").string()).readSessions("dev", "DreamStation 2");
    EXPECT_FALSE(read.ok);
    EXPECT_TRUE(read.encrypted);
    EXPECT_TRUE(read.sessions.empty());
    EXPECT_NE(read.error.find("encrypted"), std::string::npos);
}

// The session being recorded is open, so its night reads live, and grows; the
// .B01 and .B02 landing end it at once, on its data, with its pressure.
TEST_F(PhilipsCardTest, TheBurstChartsASessionLiveAndEndsItAtMaskOff) {
    auto db = std::make_shared<SQLiteDatabase>((root_ / "cpap.db").string());
    ASSERT_TRUE(db->connect());
    BurstCollectorService svc(60);
    svc.injectDependenciesForTest(
        db, nullptr,
        std::make_unique<DataPublisherService>(std::shared_ptr<hms::MqttClient>{}, db));
    const fs::path card = root_ / "card";
    stage(card, 1, false);
    svc.usePhilipsCardForTest(card.string());

    const auto value = [&](const std::string& sql) {
        const Json::Value rows = db->executeQuery(sql, {svc.deviceIdForTest()});
        if (!rows.isArray() || rows.empty() || rows[0]["v"].isNull()) return -1.0;
        const Json::Value& v = rows[0]["v"];
        return v.isString() ? std::stod(v.asString()) : v.asDouble();
    };
    const std::string mine = "(SELECT id FROM cpap_sessions WHERE device_id = ?)";
    const auto sessions = [&] {
        return value("SELECT COUNT(*) AS v FROM cpap_sessions WHERE device_id = ?");
    };
    const auto open = [&] {
        return value("SELECT COUNT(*) AS v FROM cpap_sessions WHERE device_id = ?"
                     " AND session_end IS NULL");
    };
    const auto minutes = [&] {
        return value("SELECT COUNT(*) AS v FROM cpap_breathing_summary WHERE session_id IN " + mine);
    };
    const auto pressured = [&](const char* column) {
        return value(std::string("SELECT COUNT(") + column +
                     ") AS v FROM cpap_breathing_summary b LEFT JOIN cpap_calculated_metrics c"
                     " ON c.session_id = b.session_id AND c.timestamp = b.timestamp"
                     " WHERE b.session_id IN " + mine);
    };

    ASSERT_TRUE(svc.runBurstCycleForTest());
    EXPECT_EQ(sessions(), 1);
    EXPECT_EQ(open(), 0) << "the first look imports what was already written";

    stage(card, 3, false);                     // two more blocks landed
    ASSERT_TRUE(svc.runBurstCycleForTest());
    EXPECT_EQ(open(), 1) << "a session growing now is open: its night reads live";
    const double live_minutes = minutes();
    EXPECT_GE(live_minutes, 11);               // 720 s of flow
    EXPECT_EQ(pressured("b.avg_pressure"), 0) << "no pressure before mask-off, not even 0";
    EXPECT_EQ(pressured("c.epr_pressure"), 0);
    EXPECT_GT(pressured("c.respiratory_rate"), 0);

    svc.setCardMaskQuietForTest(std::chrono::hours(1));
    stage(card, kBlocks, true);                // mask-off: the .B01 and .B02 land
    ASSERT_TRUE(svc.runBurstCycleForTest());
    EXPECT_EQ(sessions(), 1) << "the same session, not a second row";
    EXPECT_EQ(open(), 0) << "the card says it ended: closed now, no quiet window";
    EXPECT_GT(minutes(), live_minutes);
    EXPECT_GT(pressured("c.epr_pressure"), 0) << "the delivered pressure arrives at mask-off";
    EXPECT_EQ(pressured("b.avg_pressure"), 0) << "a DreamStation's waveform holds no pressure";
}

TEST_F(PhilipsCardTest, AnEncryptedCardInTheFolderStoresNothing) {
    auto db = std::make_shared<SQLiteDatabase>((root_ / "cpap.db").string());
    ASSERT_TRUE(db->connect());
    BurstCollectorService svc(60);
    svc.injectDependenciesForTest(
        db, nullptr,
        std::make_unique<DataPublisherService>(std::shared_ptr<hms::MqttClient>{}, db));
    svc.usePhilipsCardForTest((src_ / "ds2_encrypted").string());
    EXPECT_FALSE(svc.runBurstCycleForTest());
    const Json::Value rows = db->executeQuery(
        "SELECT COUNT(*) AS v FROM cpap_sessions WHERE device_id = ?", {svc.deviceIdForTest()});
    const Json::Value& v = rows[0]["v"];
    EXPECT_EQ(v.isString() ? std::stoi(v.asString()) : v.asInt(), 0);
}

// ── Through an ez Share, or a Mule & Miner in proxy mode ────────────────────

namespace {

/// A folder served as an ez Share serves a card: listings in KB with the card
/// path's backslashes, every file dated 1980.
class FolderAsCard : public IDataSource {
public:
    explicit FolderAsCard(fs::path dir) : dir_(std::move(dir)) {}
    std::vector<EzShareFileEntry> listDir(const std::string& p) override {
        ++listings;
        std::vector<EzShareFileEntry> out;
        std::error_code ec;
        for (fs::directory_iterator it(dir_ / slashes(p), ec), end; !ec && it != end; it.increment(ec)) {
            EzShareFileEntry f;
            f.name = it->path().filename().string();
            f.is_dir = it->is_directory();
            if (!f.is_dir) f.size_kb = static_cast<int>((fs::file_size(it->path()) + 1023) / 1024);
            f.year = 1980;
            out.push_back(f);
        }
        return out;
    }
    bool downloadByPath(const std::string& p, const std::string& local) override {
        ++downloads;
        std::error_code ec;
        fs::copy_file(dir_ / slashes(p), local, fs::copy_options::overwrite_existing, ec);
        return !ec;
    }
    std::vector<std::string> listDateFolders() override { return {}; }
    std::vector<EzShareFileEntry> listFiles(const std::string&) override { return {}; }
    bool downloadFile(const std::string&, const std::string&, const std::string&) override {
        return false;
    }
    bool downloadFileRange(const std::string&, const std::string&, const std::string&, size_t,
                           size_t&) override {
        return false;
    }
    bool downloadRootFile(const std::string&, const std::string&) override { return false; }

    int listings = 0, downloads = 0;

private:
    static std::string slashes(std::string p) {
        for (auto& c : p) if (c == '\\') c = '/';
        return p;
    }
    fs::path dir_;
};

}  // namespace

TEST_F(PhilipsCardTest, ThroughAnEzShareTheCopyReadsAsTheCardAndFollowsTheSession) {
    const fs::path card = root_ / "card";
    const fs::path copy = root_ / "copy";
    stage(card, 2, false);
    // The machine's own logs, which nothing reads, are not copied.
    put(card / "P-SERIES" / "PXLIVE01" / "D" / "000.B03", {'x'});
    FolderAsCard ez(card);

    auto m = mirrorPhilipsCard(ez, copy.string());
    ASSERT_TRUE(m.ok) << m.error;
    EXPECT_FALSE(m.encrypted);
    EXPECT_EQ(m.fetched, 2);   // PROP.BIN and the .B05
    EXPECT_FALSE(fs::exists(copy / "P-SERIES" / "PXLIVE01" / "D"));
    auto read = PhilipsCardIngestion(copy.string()).readSessions("dev", "DreamStation 2");
    ASSERT_TRUE(read.ok) << read.error;
    ASSERT_EQ(read.sessions.size(), 1u);
    EXPECT_EQ(read.sessions[0].parsed->duration_seconds.value_or(0), 480);

    m = mirrorPhilipsCard(ez, copy.string());   // nothing moved on the card
    EXPECT_EQ(m.fetched, 0);

    stage(card, kBlocks, true);                 // grown, then ended
    m = mirrorPhilipsCard(ez, copy.string());
    EXPECT_EQ(m.fetched, 3);                    // the grown .B05, the .B01, the .B02
    read = PhilipsCardIngestion(copy.string()).readSessions("dev", "DreamStation 2");
    ASSERT_EQ(read.sessions.size(), 1u);
    EXPECT_TRUE(read.sessions[0].closed);
}

TEST_F(PhilipsCardTest, ThroughAnEzShareAnEncryptedCardStopsAtItsIdentity) {
    FolderAsCard ez(src_ / "ds2_encrypted");
    const auto m = mirrorPhilipsCard(ez, (root_ / "copy").string());
    EXPECT_TRUE(m.ok);
    EXPECT_TRUE(m.encrypted);
    EXPECT_EQ(ez.downloads, 1) << "PROP.BIN only: the session files are not fetched";
}
