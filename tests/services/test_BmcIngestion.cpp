// SDD-049: a BMC / React Health Luna card, read from a folder and by upload.
//
// Synthetic cards only, written to the layout the parser documents (one
// 256-byte packet per second, a .evt of 32-byte records after a 2048-byte
// region), with an invented serial and invented dates. The donor card is
// exercised end to end, outside the repository.
#include <gtest/gtest.h>

#include "database/SQLiteDatabase.h"
#include "services/BmcIngestion.h"
#include "services/BurstCollectorService.h"
#include "services/CardUpload.h"
#include "services/DataPublisherService.h"

#include <filesystem>
#include <fstream>
#include <string>
#include <unistd.h>
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

/// [seconds] packets from [at], one per second, EPAP 8 / IPAP 12 cmH2O.
std::vector<uint8_t> run(int session, Clock at, int seconds) {
    std::vector<uint8_t> out;
    std::tm tm{};
    tm.tm_year = at.y - 1900; tm.tm_mon = at.mo - 1; tm.tm_mday = at.d;
    tm.tm_hour = at.h; tm.tm_min = at.mi; tm.tm_sec = at.s;
    for (int i = 0; i < seconds; ++i) {
        std::tm t = tm;
        t.tm_sec += i;
        timegm(&t);   // normalise the fields, no zone involved
        std::vector<uint8_t> p(bmc::kBmcPacketBytes, 0);
        putLe16(p, 2 * bmc::bmc_word::kSync, bmc::kBmcSync);
        putLe16(p, 2 * bmc::bmc_word::kSessionNumber, static_cast<uint16_t>(session));
        putLe16(p, 2 * bmc::bmc_word::kEpap, 16);
        putLe16(p, 2 * bmc::bmc_word::kIpap, 24);
        putLe16(p, 2 * bmc::bmc_word::kLeak, 50);
        putLe16(p, 2 * bmc::bmc_word::kRespRate, 15);
        putLe16(p, 2 * bmc::bmc_word::kIeRatio, 20);
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
    void TearDown() override { fs::remove_all(root_); }

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

}  // namespace

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
