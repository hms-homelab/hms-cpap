// SDD-031 (#28): an uploaded card is read by what it is.
//
// Synthetic cards only: Sefam manifests the discovery accepts (no channel data,
// so the parser refuses to vouch for them, which is itself counted), a ResMed
// DATALOG, and the Löwenstein markers. The real S.Box card is exercised end to
// end, outside the repository.
#include <gtest/gtest.h>

#include "database/SQLiteDatabase.h"
#include "services/CardUpload.h"
#include "services/SefamIngestion.h"
#include "utils/AppConfig.h"

#include <chrono>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <memory>
#include <string>
#include <unistd.h>

using namespace hms_cpap;
namespace fs = std::filesystem;

namespace {

/// The manifest test_SefamIngestion.cpp builds: what the INI reader needs to
/// call it valid and give it a start time. CRLF, as the device writes it.
std::string manifest(int year, int month, int day, int hour, int min) {
    std::string s;
    auto line = [&s](const std::string& t) { s += t + "\r\n"; };
    line("[Create Info]");
    line("Created By=S.Box_AUTO ");
    line("Serial Number=1263R00000000");
    line("Version=VER :A020400");
    line("Date=10/11/25 22:25:16");
    line("[Start Record]");
    line("Hour=" + std::to_string(hour));
    line("Min=" + std::to_string(min));
    line("Sec=0");
    line("Day=" + std::to_string(day));
    line("Month=" + std::to_string(month));
    line("Year=" + std::to_string(year));
    line("Programmed Record Duration=28800");
    line("Real Record Duration=28800");
    line("[Chan0]");
    line("Name=FLW");
    line("Description=NO");
    line("Type=4");
    line("Unit=lpm");
    line("Min=-180");
    line("Max=280");
    line("Freq=25");
    line("Bit=8");
    return s;
}

class CardUploadTest : public ::testing::Test {
protected:
    void SetUp() override {
        const auto tag = std::to_string(::getpid()) + "_" + std::to_string(counter_++);
        root_ = fs::temp_directory_path() / ("hms_card_upload_" + tag);
        fs::remove_all(root_);
        fs::create_directories(root_);
        path_ = (fs::temp_directory_path() / ("hms_card_upload_" + tag + ".db")).string();
        fs::remove(path_);
        auto lite = std::make_unique<SQLiteDatabase>(path_);
        ASSERT_TRUE(lite->connect());
        db_ = std::move(lite);
    }
    void TearDown() override {
        db_.reset();
        std::error_code ec;
        fs::remove_all(root_, ec);
        fs::remove(path_, ec);
    }
    void put(const std::string& rel, const std::string& body) {
        fs::create_directories((root_ / rel).parent_path());
        std::ofstream(root_ / rel, std::ios::binary) << body;
    }

    fs::path root_;
    std::string path_;
    std::unique_ptr<IDatabase> db_;
    static int counter_;
};
int CardUploadTest::counter_ = 0;

}  // namespace

TEST_F(CardUploadTest, ASefamCardIsRecognisedEvenInsideAWrapperFolder) {
    // The reporter's zip wraps the card in "cpap files/": four levels down,
    // one more than the parser's own Sefam detection looks.
    put("cpap files/1263R/00000000/DATA_1/DATA_1.INI", manifest(2025, 11, 10, 22, 25));
    EXPECT_EQ(classifyUploadedCard(root_.string()), UploadedCard::Sefam);
    EXPECT_EQ(cardNights(UploadedCard::Sefam, root_.string()).size(), 1u);
}

TEST_F(CardUploadTest, AStrayDataFolderWithoutAManifestIsNotASefamCard) {
    put("DATA_1/notes.txt", "x");
    EXPECT_NE(classifyUploadedCard(root_.string()), UploadedCard::Sefam);
}

TEST_F(CardUploadTest, AResmedCardIsRecognisedByItsDatalog) {
    fs::create_directories(root_ / "DATALOG" / "20260912");
    put("DATALOG/20260912/20260912_221000_BRP.edf", "x");
    EXPECT_EQ(classifyUploadedCard(root_.string()), UploadedCard::ResMed);
}

TEST_F(CardUploadTest, APrismaLineCardIsRecognisedByItsTherapyPdat) {
    // Its sessions are inside the .pdat, where the parser's detection cannot see.
    put("PRISMA/therapy.pdat", "PK");
    EXPECT_EQ(classifyUploadedCard(root_.string()), UploadedCard::Lowenstein);
}

TEST_F(CardUploadTest, APrismaSmartCardIsRecognisedByItsWmedf) {
    put("20260912/0001/signal_0001.wmedf", "x");
    EXPECT_EQ(classifyUploadedCard(root_.string()), UploadedCard::Lowenstein);
}

TEST_F(CardUploadTest, AZipOfSomethingElseIsNotACard) {
    put("photos/cat.jpg", "x");
    EXPECT_EQ(classifyUploadedCard(root_.string()), UploadedCard::Unknown);
    EXPECT_EQ(classifyUploadedCard((root_ / "nope").string()), UploadedCard::Unknown);
}

TEST_F(CardUploadTest, UploadsMergeAndANewerCopyReplacesTheOlder) {
    const auto store = root_ / "store";
    put("a/1263R/x/DATA_1/DATA_1.INI", "first");
    std::string err;
    ASSERT_TRUE(mergeCardInto((root_ / "a").string(), store.string(), err)) << err;
    put("b/1263R/x/DATA_1/DATA_1.INI", "second");
    put("b/1263R/x/DATA_2/DATA_2.INI", "new");
    ASSERT_TRUE(mergeCardInto((root_ / "b").string(), store.string(), err)) << err;
    std::ifstream in(store / "1263R/x/DATA_1/DATA_1.INI");
    std::string body((std::istreambuf_iterator<char>(in)), {});
    EXPECT_EQ(body, "second");
    EXPECT_TRUE(fs::exists(store / "1263R/x/DATA_2/DATA_2.INI"));
}

TEST_F(CardUploadTest, TheRepliedNightsComeFromTheSessionsNotTheChannelData) {
    put("1263R/0/DATA_1/DATA_1.INI", manifest(2025, 11, 10, 22, 25));
    put("1263R/0/DATA_2/DATA_2.INI", manifest(2025, 11, 12, 1, 5));   // after midnight
    const auto nights = cardNights(UploadedCard::Sefam, root_.string());
    ASSERT_EQ(nights.size(), 2u);
    EXPECT_EQ(*nights.begin(), "2025-11-10");
    EXPECT_EQ(*nights.rbegin(), "2025-11-11") << "01:05 belongs to the night before";
}

TEST_F(CardUploadTest, TheImportCountsEverySessionAndSkipsWhatIsStored) {
    // D3: every session on the card, not only those newer than the last.
    put("1263R/0/DATA_1/DATA_1.INI", manifest(2025, 11, 10, 22, 25));
    put("1263R/0/DATA_2/DATA_2.INI", manifest(2025, 11, 11, 22, 30));
    put("1263R/0/DATA_3/DATA_3.INI", manifest(2025, 11, 12, 22, 35));

    // Night 11 is already in the database under the same start.
    SefamIngestion probe(root_.string());
    ASSERT_TRUE(probe.initialize());
    const auto sessions = probe.discoverSessions(std::nullopt);
    ASSERT_EQ(sessions.size(), 3u);
    CPAPSession stored;
    stored.device_id = "dev";
    stored.session_start = sessions[1].session_start;
    stored.duration_seconds = 60;
    ASSERT_TRUE(db_->saveSession(stored));

    int last_done = -1, last_total = -1;
    const auto c = importCardSessions(*db_, UploadedCard::Sefam, root_.string(), "dev", "S.Box",
                                      [&](int done, int total) { last_done = done; last_total = total; });
    EXPECT_EQ(c.found, 3);
    EXPECT_EQ(c.already_stored, 1);
    // Manifests without channel data: the parser will not vouch for them, and
    // a refusal is counted, not saved.
    EXPECT_EQ(c.refused, 2);
    EXPECT_EQ(c.imported, 0);
    EXPECT_EQ(last_done, 3);
    EXPECT_EQ(last_total, 3);
}

TEST_F(CardUploadTest, ARemovedNightStaysRemovedThroughAnUpload) {
    put("1263R/0/DATA_1/DATA_1.INI", manifest(2025, 11, 10, 22, 25));
    ASSERT_TRUE(db_->removeNight("dev", "20251110").ok);
    const auto c = importCardSessions(*db_, UploadedCard::Sefam, root_.string(), "dev", "S.Box");
    EXPECT_EQ(c.found, 1);
    EXPECT_EQ(c.removed, 1);
    EXPECT_EQ(c.refused, 0);
}

TEST_F(CardUploadTest, AFolderThatIsNotTheCardImportsNothing) {
    const auto c = importCardSessions(*db_, UploadedCard::Sefam, root_.string(), "dev", "S.Box");
    EXPECT_EQ(c.found, 0);
    EXPECT_EQ(c.imported, 0);
}

TEST(CollectorSource, TheTransportAndFormatPairMapsToWhatTheCollectorBuilds) {
    EXPECT_EQ(AppConfig::collectorSource("local", "resmed"), "local");
    EXPECT_EQ(AppConfig::collectorSource("local", "sefam"), "sefam");
    EXPECT_EQ(AppConfig::collectorSource("local", "lowenstein"), "lowenstein");
    EXPECT_EQ(AppConfig::collectorSource("ezshare", "resmed"), "ezshare");
    EXPECT_EQ(AppConfig::collectorSource("ezshare", "sefam"), "sefam_ezshare");
    // A Prisma answers the ez Share with error 601: not a pair; ResMed reading.
    EXPECT_EQ(AppConfig::collectorSource("ezshare", "lowenstein"), "ezshare");
    EXPECT_EQ(AppConfig::collectorSource("fysetc", "sefam"), "fysetc");
}
