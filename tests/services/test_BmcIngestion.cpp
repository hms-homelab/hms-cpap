// SDD-049: a BMC / React Health Luna card, read from a folder and by upload.
//
// Synthetic cards only, written to the layout the parser documents (one
// 256-byte packet per second, a .evt of 32-byte records after a 2048-byte
// region), with an invented serial and invented dates. The donor card is
// exercised end to end, outside the repository.
#include <gtest/gtest.h>
#include "utils/TimeCompat.h"

#include "clients/FlashAirClient.h"
#include "database/SQLiteDatabase.h"
#include "services/BmcCardMirror.h"
#include "services/BmcIngestion.h"
#include "services/BurstCollectorService.h"
#include "services/CardUpload.h"
#include "services/DataPublisherService.h"
#include "utils/AppConfig.h"

#include "FakeFlashAir.h"
#include "PosixCompat.h"

#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#ifndef _WIN32
#include <unistd.h>
#else
#include <process.h>
#endif
#include <vector>

using namespace hms_cpap;
namespace fs = std::filesystem;
namespace bmc = cpapdash::parser;

namespace {

constexpr const char* kSerial = "11002233";

void putLe16(std::vector<uint8_t>& b, size_t at, uint16_t v) {
    b[at] = static_cast<uint8_t>(v & 0xFF);
    b[at + 1] = static_cast<uint8_t>(v >> 8);
}
void putLe32(std::vector<uint8_t>& b, size_t at, uint32_t v) {
    for (int i = 0; i < 4; ++i) b[at + i] = static_cast<uint8_t>((v >> (8 * i)) & 0xFF);
}

struct Clock { int y, mo, d, h, mi, s; };

/// [seconds] packets from [at], one per second, EPAP 8 / IPAP 12 cmH2O, and
/// the 25 Hz pressure channel at [pressure_code] (code / 10 cmH2O on a G3).
std::vector<uint8_t> run(int session, Clock at, int seconds, uint16_t pressure_code = 0) {
    std::vector<uint8_t> out;
    std::tm tm{};
    tm.tm_year = at.y - 1900; tm.tm_mon = at.mo - 1; tm.tm_mday = at.d;
    tm.tm_hour = at.h; tm.tm_min = at.mi; tm.tm_sec = at.s;
    for (int i = 0; i < seconds; ++i) {
        std::tm t = tm;
        t.tm_sec += i;
        timegm_utc(&t);   // normalise the fields, no zone involved
        std::vector<uint8_t> p(bmc::kBmcPacketBytes, 0);
        putLe16(p, 2 * bmc::bmc_word::kSync, bmc::kBmcSync);
        putLe16(p, 2 * bmc::bmc_word::kSessionNumber, static_cast<uint16_t>(session));
        putLe16(p, 2 * bmc::bmc_word::kEpap, 16);
        putLe16(p, 2 * bmc::bmc_word::kIpap, 24);
        putLe16(p, 2 * bmc::bmc_word::kLeak, 50);
        putLe16(p, 2 * bmc::bmc_word::kRespRate, 15);
        putLe16(p, 2 * bmc::bmc_word::kIeRatio, 20);
        for (int k = 0; k < bmc::kBmcWaveHz; ++k)
            putLe16(p, 2 * (bmc::bmc_word::kPressure + k), pressure_code);
        putLe16(p, 248, static_cast<uint16_t>(t.tm_year + 1900));
        p[250] = static_cast<uint8_t>(t.tm_mon + 1);
        p[251] = static_cast<uint8_t>(t.tm_mday);
        p[252] = static_cast<uint8_t>(t.tm_hour);
        p[253] = static_cast<uint8_t>(t.tm_min);
        p[254] = static_cast<uint8_t>(t.tm_sec);
        out.insert(out.end(), p.begin(), p.end());
    }
    return out;
}

/// An events file holding one obstructive apnea of session 2 at 23:45, the
/// onset counted in seconds from noon of the sleep day.
std::vector<uint8_t> evt() {
    std::vector<uint8_t> b(bmc::kBmcEventRegionBytes, 0xFF);
    std::vector<uint8_t> r(bmc::kBmcEventRecordBytes, 0);
    putLe16(r, 0, bmc::kBmcSync);
    putLe16(r, 2, 2);
    putLe16(r, 4, static_cast<uint16_t>(bmc::bmc_event::kObstructive));
    putLe32(r, 8, 11 * 3600 + 45 * 60);
    putLe32(r, 12, 12);
    b.insert(b.end(), r.begin(), r.end());
    return b;
}

void write(const fs::path& p, const std::vector<uint8_t>& bytes) {
    fs::create_directories(p.parent_path());
    std::ofstream(p, std::ios::binary).write(reinterpret_cast<const char*>(bytes.data()),
                                             static_cast<std::streamsize>(bytes.size()));
}

class BmcCardTest : public ::testing::Test {
protected:
    fs::path root_;

    void SetUp() override {
        root_ = fs::temp_directory_path() / ("hms_cpap_bmc_" + std::to_string(::getpid()) + "_" +
                                             ::testing::UnitTest::GetInstance()->current_test_info()->name());
        fs::remove_all(root_);
    }
    void TearDown() override {
        std::error_code ec;
        fs::remove_all(root_, ec);
    }

    /// Two nights: a 2-minute try in the evening and a 1-hour session that
    /// crosses midnight, with the apnea inside it. [card] is where the files go.
    void writeCard(const fs::path& card, int second_session_seconds = 3600) {
        auto raw = run(1, {2025, 3, 10, 21, 0, 0}, 120);
        const auto night = run(2, {2025, 3, 10, 23, 30, 0}, second_session_seconds);
        raw.insert(raw.end(), night.begin(), night.end());
        write(card / (std::string(kSerial) + ".000"), raw);
        write(card / (std::string(kSerial) + ".evt"), evt());
        write(card / (std::string(kSerial) + ".USR"), std::vector<uint8_t>(64, 0x20));
    }
};

/// SDD-060: a folder served the way an ez Share serves a card root (sizes in
/// KB, every file dated 1980), with Range at the root as a Mule and Miner has.
class FolderAsEzShare : public IDataSource {
public:
    explicit FolderAsEzShare(fs::path dir) : dir_(std::move(dir)) {}
    std::vector<EzShareFileEntry> listDir(const std::string& p) override {
        std::vector<EzShareFileEntry> out;
        if (!p.empty()) return out;
        for (const auto& e : fs::directory_iterator(dir_)) {
            EzShareFileEntry f;
            f.name = e.path().filename().string();
            f.size_kb = static_cast<int>((fs::file_size(e.path()) + 1023) / 1024);
            f.year = 1980;
            out.push_back(f);
        }
        return out;
    }
    bool downloadByPath(const std::string& name, const std::string& local) override {
        std::error_code ec;
        fs::copy_file(dir_ / name, local, fs::copy_options::overwrite_existing, ec);
        return !ec;
    }
    bool downloadFileRange(const std::string& folder, const std::string& name,
                           const std::string& local, size_t start, size_t& got) override {
        if (!folder.empty()) return false;
        std::ifstream in(dir_ / name, std::ios::binary);
        in.seekg(static_cast<std::streamoff>(start));
        std::string rest((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
        std::ofstream(local, std::ios::binary | std::ios::app) << rest;
        got = rest.size();
        return !rest.empty();
    }
    std::vector<std::string> listDateFolders() override { return {}; }
    std::vector<EzShareFileEntry> listFiles(const std::string&) override { return {}; }
    bool downloadFile(const std::string&, const std::string&, const std::string&) override {
        return false;
    }
    bool downloadRootFile(const std::string&, const std::string&) override { return false; }

private:
    fs::path dir_;
};

}  // namespace

TEST_F(BmcCardTest, ThroughAnEzShareTheCopyReadsAsTheCardDoesAndFollowsASession) {
    // SDD-060: the card in a Luna's slot, served over the ez Share; the copy
    // the mirror keeps reads as the folder itself, and a night that grew after
    // the mask came off is appended and read longer.
    const fs::path card = root_ / "card";
    const fs::path copy = root_ / "archive" / "bmc";
    writeCard(card, 1800);
    FolderAsEzShare ez(card);

    ASSERT_TRUE(mirrorBmcCard(ez, copy.string()).ok);
    auto read = BmcIngestion(copy.string()).readSessions("dev", "Luna");
    ASSERT_TRUE(read.ok) << read.error;
    ASSERT_EQ(read.sessions.size(), 2u);
    EXPECT_EQ(read.sessions[1]->duration_seconds.value_or(0), 1800);

    writeCard(card, 3600);   // the same session, an hour long now
    const auto m = mirrorBmcCard(ez, copy.string());
    ASSERT_TRUE(m.ok);
    EXPECT_EQ(m.appended, 1) << "the Luna only appended, so only the rest was fetched";
    read = BmcIngestion(copy.string()).readSessions("dev", "Luna");
    ASSERT_TRUE(read.ok) << read.error;
    ASSERT_EQ(read.sessions.size(), 2u);
    EXPECT_EQ(read.sessions[1]->duration_seconds.value_or(0), 3600);
    EXPECT_EQ(read.sessions[1]->metrics->obstructive_apneas, 1);
}

TEST_F(BmcCardTest, TheCardIsFoundInsideTheFoldersAZipWrapsItIn) {
    writeCard(root_ / "my card" / "SD");
    const auto dir = BmcIngestion::findCardDir(root_.string());
    ASSERT_TRUE(dir.has_value());
    EXPECT_EQ(fs::path(*dir), root_ / "my card" / "SD");
}

TEST_F(BmcCardTest, DataFilesWithoutTheirEventsAndUserFileAreNotACard) {
    write(root_ / (std::string(kSerial) + ".000"), run(1, {2025, 3, 10, 21, 0, 0}, 60));
    EXPECT_FALSE(BmcIngestion::findCardDir(root_.string()).has_value());
    EXPECT_FALSE(BmcIngestion(root_.string()).readSessions("dev", "Luna").ok);
}

TEST_F(BmcCardTest, EverySessionOldestFirstWithTheBilevelModeInItsMetrics) {
    writeCard(root_);
    const auto read = BmcIngestion(root_.string()).readSessions("dev", "Luna");
    ASSERT_TRUE(read.ok) << read.error;
    ASSERT_EQ(read.sessions.size(), 2u);
    EXPECT_LT(*read.sessions[0]->session_start, *read.sessions[1]->session_start);
    EXPECT_EQ(read.sessions[0]->duration_seconds.value_or(0), 120);
    const auto& night = *read.sessions[1];
    EXPECT_EQ(night.duration_seconds.value_or(0), 3600);
    ASSERT_TRUE(night.metrics.has_value());
    EXPECT_EQ(night.metrics->obstructive_apneas, 1);
    // D2: the parser names the mode in the settings; what is stored and
    // published is the metrics, where nothing would read as 0 (CPAP).
    EXPECT_EQ(night.metrics->therapy_mode.value_or(0), 2);
    EXPECT_DOUBLE_EQ(night.metrics->avg_therapy_pressure.value_or(0), 12.0);
    EXPECT_DOUBLE_EQ(night.metrics->avg_epr_pressure.value_or(0), 8.0);
}

TEST_F(BmcCardTest, AnUploadIsRecognisedAndImportsEachSessionOnce) {
    writeCard(root_ / "upload");
    ASSERT_EQ(classifyUploadedCard(root_.string()), UploadedCard::Bmc);
    EXPECT_STREQ(uploadedCardName(UploadedCard::Bmc), "bmc");

    SQLiteDatabase db((root_ / "cpap.db").string());
    ASSERT_TRUE(db.connect());
    auto c = importCardSessions(db, UploadedCard::Bmc, root_.string(), "dev", "Luna");
    EXPECT_EQ(c.found, 2);
    EXPECT_EQ(c.imported, 2);
    c = importCardSessions(db, UploadedCard::Bmc, root_.string(), "dev", "Luna");
    EXPECT_EQ(c.imported, 0);
    EXPECT_EQ(c.already_stored, 2) << "a second upload of the same card adds nothing";
    EXPECT_EQ(cardNights(UploadedCard::Bmc, root_.string()).size(), 1u)
        << "both sessions belong to the sleep day of 2025-03-10";
}

// D4: every session not yet stored, each burst, and the newest read again only
// while it grows.
TEST_F(BmcCardTest, TheBurstImportsNewSessionsAndANightThatGrew) {
    writeCard(root_, 1800);
    auto db = std::make_shared<SQLiteDatabase>((root_ / "cpap.db").string());
    ASSERT_TRUE(db->connect());

    BurstCollectorService svc(60);
    svc.injectDependenciesForTest(
        db, nullptr,
        std::make_unique<DataPublisherService>(std::shared_ptr<hms::MqttClient>{}, db));
    svc.useBmcCardForTest(root_.string());

    const auto number = [&](const std::string& sql) {
        const Json::Value rows = db->executeQuery(sql, {svc.deviceIdForTest()});
        if (!rows.isArray() || rows.empty()) return -1.0;
        const Json::Value& v = rows[0]["v"];
        return v.isString() ? std::stod(v.asString()) : v.asDouble();
    };
    const auto count = [&] {
        return number("SELECT COUNT(*) AS v FROM cpap_sessions WHERE device_id = ?");
    };
    const auto seconds = [&] {
        return number("SELECT MAX(duration_seconds) AS v FROM cpap_sessions WHERE device_id = ?");
    };

    ASSERT_TRUE(svc.runBurstCycleForTest());
    EXPECT_EQ(count(), 2);
    EXPECT_EQ(seconds(), 1800);

    ASSERT_TRUE(svc.runBurstCycleForTest());   // unchanged card: nothing new
    EXPECT_EQ(count(), 2);

    writeCard(root_, 3600);                    // the night went on
    ASSERT_TRUE(svc.runBurstCycleForTest());
    EXPECT_EQ(count(), 2) << "the same night, longer, not a third session";
    EXPECT_EQ(seconds(), 3600);
}

// SDD-058 D2: a session growing while hms-cpap watches is open, so its night
// reads live; it is closed on its own data end once it stops growing.
TEST_F(BmcCardTest, ASessionGrowingWhileWatchedIsOpenAndClosesOnItsDataWhenQuiet) {
    writeCard(root_, 600);
    auto db = std::make_shared<SQLiteDatabase>((root_ / "cpap.db").string());
    ASSERT_TRUE(db->connect());

    BurstCollectorService svc(60);
    svc.injectDependenciesForTest(
        db, nullptr,
        std::make_unique<DataPublisherService>(std::shared_ptr<hms::MqttClient>{}, db));
    svc.useBmcCardForTest(root_.string());

    const auto open = [&] {
        const Json::Value rows = db->executeQuery(
            "SELECT COUNT(*) AS v FROM cpap_sessions WHERE device_id = ? AND session_end IS NULL",
            {svc.deviceIdForTest()});
        const Json::Value& v = rows[0]["v"];
        return v.isString() ? std::stoi(v.asString()) : v.asInt();
    };
    const auto end = [&] {
        const Json::Value rows = db->executeQuery(
            "SELECT session_end AS v FROM cpap_sessions WHERE device_id = ?"
            " ORDER BY session_start DESC LIMIT 1",
            {svc.deviceIdForTest()});
        return rows[0]["v"].isNull() ? std::string() : rows[0]["v"].asString();
    };

    ASSERT_TRUE(svc.runBurstCycleForTest());
    EXPECT_EQ(open(), 0) << "the first look imports what was already written";

    writeCard(root_, 1200);                    // the machine is writing it
    ASSERT_TRUE(svc.runBurstCycleForTest());
    EXPECT_EQ(open(), 1) << "a session growing now is open";
    EXPECT_EQ(end(), "");

    svc.setCardMaskQuietForTest(std::chrono::hours(1));
    ASSERT_TRUE(svc.runBurstCycleForTest());   // no growth, inside the window
    EXPECT_EQ(open(), 1);

    svc.setCardMaskQuietForTest(std::chrono::seconds(0));
    ASSERT_TRUE(svc.runBurstCycleForTest());   // quiet: closed on its data
    EXPECT_EQ(open(), 0);
    EXPECT_EQ(end().substr(0, 19), "2025-03-10 23:50:00") << "start plus 1200 s, not the clock";
}

// A night stored by an older pass, its minutes a flat 0 (a parser that read no
// Luna pressure) and its end the one it had a minute in, is read again once by
// a new process, every session and not only the newest: its minutes take the
// real pressure and its end the data's.
TEST_F(BmcCardTest, AnOlderFlatZeroNightIsReadAgainIntoRealValuesAndItsEnd) {
    auto raw = run(1, {2025, 3, 10, 21, 0, 0}, 600, 70);     // 7.0 cmH2O
    const auto later = run(2, {2025, 3, 10, 23, 30, 0}, 600, 70);
    raw.insert(raw.end(), later.begin(), later.end());
    write(root_ / (std::string(kSerial) + ".000"), raw);
    write(root_ / (std::string(kSerial) + ".evt"), evt());
    std::vector<uint8_t> usr(bmc::kBmcUsrModelOffset + bmc::kBmcUsrModelBytes, 0x20);
    const std::string model = "G3 A20";
    std::copy(model.begin(), model.end(), usr.begin() + bmc::kBmcUsrModelOffset);
    write(root_ / (std::string(kSerial) + ".USR"), usr);

    auto db = std::make_shared<SQLiteDatabase>((root_ / "cpap.db").string());
    ASSERT_TRUE(db->connect());
    std::string device;
    {
        BurstCollectorService before(60);
        before.injectDependenciesForTest(
            db, nullptr,
            std::make_unique<DataPublisherService>(std::shared_ptr<hms::MqttClient>{}, db));
        before.useBmcCardForTest(root_.string());
        ASSERT_TRUE(before.runBurstCycleForTest());
        device = before.deviceIdForTest();
    }

    // What an older pass left: flat zeros, and an end 62 s in.
    const std::string mine = "(SELECT id FROM cpap_sessions WHERE device_id = ?)";
    db->executeQuery("UPDATE cpap_breathing_summary SET avg_pressure = 0, max_pressure = 0,"
                     " min_pressure = 0 WHERE session_id IN " + mine, {device});
    db->executeQuery("UPDATE cpap_sessions SET session_end = '2025-03-10 21:01:02'"
                     " WHERE device_id = ? AND session_start < '2025-03-10 22:00:00'", {device});

    BurstCollectorService after(60);
    after.injectDependenciesForTest(
        db, nullptr,
        std::make_unique<DataPublisherService>(std::shared_ptr<hms::MqttClient>{}, db));
    after.useBmcCardForTest(root_.string());
    ASSERT_EQ(after.deviceIdForTest(), device);
    ASSERT_TRUE(after.runBurstCycleForTest());

    const Json::Value older = db->executeQuery(
        "SELECT MIN(b.avg_pressure) AS lo, MAX(b.avg_pressure) AS hi, COUNT(*) AS n,"
        " MAX(s.session_end) AS session_end"
        " FROM cpap_breathing_summary b JOIN cpap_sessions s ON s.id = b.session_id"
        " WHERE s.device_id = ? AND s.session_start < '2025-03-10 22:00:00'", {device});
    ASSERT_EQ(older.size(), 1u);
    const auto num = [](const Json::Value& v) {
        return v.isString() ? std::stod(v.asString()) : v.asDouble();
    };
    ASSERT_FALSE(older[0]["lo"].isNull()) << "the older night's minutes have a pressure";
    EXPECT_GT(num(older[0]["n"]), 0);
    EXPECT_NEAR(num(older[0]["lo"]), 7.0, 1e-6) << "no minute is left at 0";
    EXPECT_NEAR(num(older[0]["hi"]), 7.0, 1e-6);
    EXPECT_EQ(older[0]["session_end"].asString().substr(0, 19), "2025-03-10 21:10:00")
        << "start plus 600 s, not the end it had a minute in";
}

// SDD-058 D2: a session that begins while hms-cpap watches is live from its
// first minute, and the one before it is closed.
TEST_F(BmcCardTest, ASessionBegunWhileWatchedIsOpenAndThePreviousOneIsClosed) {
    auto first = run(1, {2025, 3, 10, 21, 0, 0}, 120);
    write(root_ / (std::string(kSerial) + ".000"), first);
    write(root_ / (std::string(kSerial) + ".evt"), evt());
    write(root_ / (std::string(kSerial) + ".USR"), std::vector<uint8_t>(64, 0x20));
    auto db = std::make_shared<SQLiteDatabase>((root_ / "cpap.db").string());
    ASSERT_TRUE(db->connect());

    BurstCollectorService svc(60);
    svc.injectDependenciesForTest(
        db, nullptr,
        std::make_unique<DataPublisherService>(std::shared_ptr<hms::MqttClient>{}, db));
    svc.useBmcCardForTest(root_.string());
    const auto open = [&] {
        const Json::Value rows = db->executeQuery(
            "SELECT COUNT(*) AS v FROM cpap_sessions WHERE device_id = ? AND session_end IS NULL",
            {svc.deviceIdForTest()});
        const Json::Value& v = rows[0]["v"];
        return v.isString() ? std::stoi(v.asString()) : v.asInt();
    };

    ASSERT_TRUE(svc.runBurstCycleForTest());
    EXPECT_EQ(open(), 0);

    writeCard(root_, 300);                     // session 2 began after the first look
    ASSERT_TRUE(svc.runBurstCycleForTest());
    EXPECT_EQ(open(), 1) << "the new session is live";
    const Json::Value earlier = db->executeQuery(
        "SELECT session_end AS v FROM cpap_sessions WHERE device_id = ?"
        " ORDER BY session_start ASC LIMIT 1", {svc.deviceIdForTest()});
    EXPECT_FALSE(earlier[0]["v"].isNull()) << "the earlier session is closed";
}

// ── A Luna card in a Toshiba FlashAir ───────────────────────────────────────
//
// The FlashAir in the Luna's slot, on the home WiFi: a loopback server that
// answers as the card does (op=100 listings of DIR=/, files at their card
// path, Range honoured with a 206, a missing file a 307 to http://flashair/),
// serving a folder. The collector reaches it through its own FlashAirClient.


// The mirror over a real FlashAirClient: the five root files copied, and a
// night that went on appended from the card ROOT, not looked for in a DATALOG.
TEST_F(BmcCardTest, ThroughAFlashAirTheCopyReadsAsTheCardDoesAndTheRawFileIsAppended) {
    const fs::path card = root_ / "card";
    const fs::path copy = root_ / "archive" / "bmc";
    writeCard(card, 1800);
    FakeFlashAir fa(card);
    FlashAirClient client;
    client.setBaseURL(fa.baseUrl());

    auto m = mirrorBmcCard(client, copy.string());
    ASSERT_TRUE(m.ok) << m.error;
    EXPECT_EQ(m.stem, kSerial);
    EXPECT_EQ(m.fetched, 3);
    auto read = BmcIngestion(copy.string()).readSessions("dev", "Luna");
    ASSERT_TRUE(read.ok) << read.error;
    ASSERT_EQ(read.sessions.size(), 2u);
    EXPECT_EQ(read.sessions[1]->duration_seconds.value_or(0), 1800);

    fa.clearRequests();
    writeCard(card, 3600);
    m = mirrorBmcCard(client, copy.string());
    ASSERT_TRUE(m.ok) << m.error;
    EXPECT_EQ(m.appended, 1) << "only the rest of the .000 was fetched";
    EXPECT_TRUE(fa.asked(std::string("GET /") + kSerial + ".000 range="));
    EXPECT_FALSE(fa.asked("/DATALOG")) << "a Luna card has no DATALOG";
    read = BmcIngestion(copy.string()).readSessions("dev", "Luna");
    ASSERT_TRUE(read.ok) << read.error;
    EXPECT_EQ(read.sessions[1]->duration_seconds.value_or(0), 3600);
    EXPECT_EQ(read.sessions[1]->metrics->obstructive_apneas, 1);
}

// transport flashair + format bmc, set in Settings: the burst copies the card
// off the FlashAir into <archive>/bmc and stores its nights, and a night that
// went on is read longer.
TEST_F(BmcCardTest, ALunaOnAFlashAirSetInSettingsIsCopiedAndItsNightsStored) {
    const fs::path card = root_ / "card";
    writeCard(card, 1800);
    FakeFlashAir fa(card);
    auto db = std::make_shared<SQLiteDatabase>((root_ / "cpap.db").string());
    ASSERT_TRUE(db->connect());

    BurstCollectorService svc(60);
    svc.injectDependenciesForTest(
        db, nullptr,
        std::make_unique<DataPublisherService>(std::shared_ptr<hms::MqttClient>{}, db));
    AppConfig cfg;
    cfg.transport = "flashair";
    cfg.format = "bmc";
    cfg.flashair_url = fa.baseUrl();
    cfg.archive_dir = (root_ / "archive").string();
    cfg.mqtt.enabled = false;
    svc.setAppConfig(&cfg);
    svc.markConfigDirty();
    svc.reloadConfigForTest();
    EXPECT_EQ(svc.dataSourceNameForTest(), "the FlashAir");
    EXPECT_EQ(svc.cpapSourceForTest(), "bmc") << "a BMC source to everything downstream";

    const auto number = [&](const std::string& sql) {
        const Json::Value rows = db->executeQuery(sql, {svc.deviceIdForTest()});
        if (!rows.isArray() || rows.empty()) return -1.0;
        const Json::Value& v = rows[0]["v"];
        return v.isString() ? std::stod(v.asString()) : v.asDouble();
    };
    ASSERT_TRUE(svc.runBurstCycleForTest());
    EXPECT_EQ(number("SELECT COUNT(*) AS v FROM cpap_sessions WHERE device_id = ?"), 2);
    EXPECT_TRUE(fs::exists(root_ / "archive" / "bmc" / (std::string(kSerial) + ".000")));

    writeCard(card, 3600);
    ASSERT_TRUE(svc.runBurstCycleForTest());
    EXPECT_EQ(number("SELECT COUNT(*) AS v FROM cpap_sessions WHERE device_id = ?"), 2);
    EXPECT_EQ(number("SELECT MAX(duration_seconds) AS v FROM cpap_sessions WHERE device_id = ?"), 3600);
    EXPECT_FALSE(fa.asked("/DATALOG"));
}

// The same Luna moved between an ez Share and a FlashAir in Settings: the
// client follows the transport, the source stays BMC, and no restart.
TEST(BmcFlashAirReload, ALunaSwitchesBetweenAnEzShareAndAFlashAirWithoutARestart) {
    const auto archive = fs::temp_directory_path() / ("hms_bmc_switch_" + std::to_string(::getpid()));
    BurstCollectorService svc(60);
    AppConfig cfg;
    cfg.transport = "ezshare";
    cfg.format = "bmc";
    cfg.ezshare_url = "http://127.0.0.1:1";
    cfg.flashair_url = "http://127.0.0.1:1";
    cfg.archive_dir = archive.string();
    cfg.mqtt.enabled = false;
    svc.setAppConfig(&cfg);
    svc.markConfigDirty();
    svc.reloadConfigForTest();
    EXPECT_EQ(svc.dataSourceNameForTest(), "ez Share");
    EXPECT_EQ(svc.cpapSourceForTest(), "bmc");

    cfg.transport = "flashair";
    svc.markConfigDirty();
    svc.reloadConfigForTest();
    EXPECT_EQ(svc.dataSourceNameForTest(), "the FlashAir");
    EXPECT_EQ(svc.cpapSourceForTest(), "bmc");

    cfg.transport = "ezshare";
    svc.markConfigDirty();
    svc.reloadConfigForTest();
    EXPECT_EQ(svc.dataSourceNameForTest(), "ez Share");
    EXPECT_EQ(svc.cpapSourceForTest(), "bmc");

    // A ResMed card on the FlashAir is still the ResMed path.
    cfg.transport = "flashair";
    cfg.format = "resmed";
    svc.markConfigDirty();
    svc.reloadConfigForTest();
    EXPECT_EQ(svc.dataSourceNameForTest(), "the FlashAir");
    EXPECT_EQ(svc.cpapSourceForTest(), "flashair");

    // And a Sefam card behind one is read through a copy, as behind an ez Share.
    cfg.format = "sefam";
    svc.markConfigDirty();
    svc.reloadConfigForTest();
    EXPECT_EQ(svc.dataSourceNameForTest(), "the FlashAir");
    EXPECT_EQ(svc.cpapSourceForTest(), "sefam");
    std::error_code ec;
    fs::remove_all(archive, ec);
}

// At start, from CPAP_TRANSPORT/CPAP_FORMAT, the same as on a reload.
TEST(BmcFlashAirReload, AStartWithALunaOnAFlashAirBuildsTheFlashAirCopy) {
    setenv("CPAP_TRANSPORT", "flashair", 1);
    setenv("CPAP_FORMAT", "bmc", 1);
    setenv("CPAP_ARCHIVE_DIR", (fs::temp_directory_path() / "hms_bmc_start").string().c_str(), 1);
    setenv("FLASHAIR_BASE_URL", "http://127.0.0.1:1", 1);
    BurstCollectorService svc(60);
    svc.initDataSourceForTest();
    EXPECT_EQ(svc.dataSourceNameForTest(), "the FlashAir");
    EXPECT_EQ(svc.cpapSourceForTest(), "bmc");

    setenv("CPAP_FORMAT", "sefam", 1);
    BurstCollectorService sefam(60);
    sefam.initDataSourceForTest();
    EXPECT_EQ(sefam.dataSourceNameForTest(), "the FlashAir");
    EXPECT_EQ(sefam.cpapSourceForTest(), "sefam");

    unsetenv("CPAP_TRANSPORT");
    unsetenv("CPAP_FORMAT");
    unsetenv("CPAP_ARCHIVE_DIR");
    unsetenv("FLASHAIR_BASE_URL");
}
