// SDD-031 (#28): a Sefam S.Box card through an ez Share, copied into the archive.
//
// A fake card serves an in-memory tree through the same two calls the real
// ez Share client uses (listDir, downloadByPath), so what is tested is what the
// mirror asks for and what it writes: the whole card on the first burst, then
// only the last two nights (D4), and nothing that has not changed.
#include <gtest/gtest.h>

#include "services/SefamCardMirror.h"

#include <filesystem>
#include <fstream>
#include <map>
#include <string>
#include <unistd.h>
#include <vector>

using namespace hms_cpap;
namespace fs = std::filesystem;

namespace {

class FakeCard : public IDataSource {
public:
    struct File { std::string body; int size_kb; int y, mo, d, h, mi; };
    std::map<std::string, std::vector<std::string>> dirs;   // card dir -> child dir names
    std::map<std::string, std::map<std::string, File>> files;  // card dir -> name -> file
    std::map<std::string, EzShareFileEntry> dir_stamps;      // "dir\\child" -> stamp
    std::vector<std::string> listed, downloaded;
    bool dead = false;

    void addFile(const std::string& dir, const std::string& name, const std::string& body,
                 int y, int mo, int d, int h, int mi) {
        files[dir][name] = File{body, static_cast<int>(body.size() / 1024 + 1), y, mo, d, h, mi};
    }
    void addDir(const std::string& parent, const std::string& name, int y = 0, int mo = 0,
                int d = 0, int h = 0, int mi = 0) {
        dirs[parent].push_back(name);
        EzShareFileEntry e;
        e.name = name; e.is_dir = true;
        e.year = y; e.month = mo; e.day = d; e.hour = h; e.minute = mi;
        dir_stamps[parent + "\\" + name] = e;
    }

    std::vector<EzShareFileEntry> listDir(const std::string& p) override {
        listed.push_back(p);
        if (dead) return {};
        std::vector<EzShareFileEntry> out;
        for (const auto& d : dirs[p]) out.push_back(dir_stamps[p + "\\" + d]);
        for (const auto& [n, f] : files[p]) {
            EzShareFileEntry e;
            e.name = n; e.size_kb = f.size_kb;
            e.year = f.y; e.month = f.mo; e.day = f.d; e.hour = f.h; e.minute = f.mi;
            out.push_back(e);
        }
        return out;
    }
    bool downloadByPath(const std::string& card_rel, const std::string& local) override {
        downloaded.push_back(card_rel);
        const auto cut = card_rel.rfind('\\');
        const std::string dir = cut == std::string::npos ? "" : card_rel.substr(0, cut);
        const std::string name = cut == std::string::npos ? card_rel : card_rel.substr(cut + 1);
        const auto it = files[dir].find(name);
        if (it == files[dir].end()) return false;
        fs::create_directories(fs::path(local).parent_path());
        std::ofstream(local, std::ios::binary) << it->second.body;
        return true;
    }
    // The ResMed calls, unused by the mirror.
    std::vector<std::string> listDateFolders() override { return {}; }
    std::vector<EzShareFileEntry> listFiles(const std::string&) override { return {}; }
    bool downloadFile(const std::string&, const std::string&, const std::string&) override { return false; }
    bool downloadFileRange(const std::string&, const std::string&, const std::string&, size_t,
                           size_t&) override { return false; }
    bool downloadRootFile(const std::string&, const std::string&) override { return false; }
};

/// The reporter's layout: 1263R/<serial>/DATA_<n>/DATA_<n>.INI plus channel
/// files, and a .RAM beside the session folders. Nights 10, 11 and 12 (the
/// 12th holding two sessions).
void buildCard(FakeCard& c) {
    c.addDir("", "1263R");
    c.addDir("1263R", "00000000");
    const std::string serial = "1263R\\00000000";
    c.addFile(serial, "00000000.RAM", "ram", 2025, 11, 13, 7, 0);
    struct S { int n, d, h; };
    for (const S s : {S{1, 10, 22}, S{2, 11, 23}, S{3, 12, 22}, S{4, 13, 3}}) {
        const std::string name = "DATA_" + std::to_string(s.n);
        c.addDir(serial, name, 2025, 11, s.d, s.h, 0);
        c.addFile(serial + "\\" + name, name + ".INI", "manifest " + name, 2025, 11, s.d, s.h, 0);
        c.addFile(serial + "\\" + name, name + ".DAT", std::string(3000, 'x'), 2025, 11, s.d, s.h, 5);
    }
}

class SefamMirrorTest : public ::testing::Test {
protected:
    void SetUp() override {
        archive_ = fs::temp_directory_path() / ("hms_sefam_mirror_" + std::to_string(::getpid()) +
                                                "_" + std::to_string(counter_++));
        fs::remove_all(archive_);
        buildCard(card_);
    }
    void TearDown() override { std::error_code ec; fs::remove_all(archive_, ec); }
    fs::path archive_;
    FakeCard card_;
    static int counter_;
};
int SefamMirrorTest::counter_ = 0;

}  // namespace

TEST_F(SefamMirrorTest, TheFirstBurstCopiesTheWholeCard) {
    const auto r = mirrorSefamCard(card_, archive_.string(), /*whole_card=*/true);
    ASSERT_TRUE(r.ok) << r.error;
    EXPECT_EQ(r.fetched, 9);   // 4 sessions x 2 files + the .RAM
    EXPECT_EQ(r.failed, 0);
    EXPECT_TRUE(fs::exists(archive_ / "1263R/00000000/DATA_1/DATA_1.INI"));
    EXPECT_TRUE(fs::exists(archive_ / "1263R/00000000/DATA_4/DATA_4.DAT"));
    EXPECT_TRUE(fs::exists(archive_ / "1263R/00000000/00000000.RAM"));
}

TEST_F(SefamMirrorTest, ASecondPassFetchesNothingThatHasNotChanged) {
    ASSERT_TRUE(mirrorSefamCard(card_, archive_.string(), true).ok);
    card_.downloaded.clear();
    const auto r = mirrorSefamCard(card_, archive_.string(), true);
    ASSERT_TRUE(r.ok);
    EXPECT_EQ(r.fetched, 0);
    EXPECT_EQ(r.unchanged, 9);
    EXPECT_TRUE(card_.downloaded.empty());
}

TEST_F(SefamMirrorTest, AFileWhoseSizeOrStampChangedIsFetchedAgain) {
    ASSERT_TRUE(mirrorSefamCard(card_, archive_.string(), true).ok);
    card_.addFile("1263R\\00000000\\DATA_4", "DATA_4.DAT", std::string(9000, 'y'),
                  2025, 11, 13, 7, 30);   // the night grew
    const auto r = mirrorSefamCard(card_, archive_.string(), false);
    ASSERT_TRUE(r.ok);
    EXPECT_EQ(r.fetched, 1);
    EXPECT_EQ(fs::file_size(archive_ / "1263R/00000000/DATA_4/DATA_4.DAT"), 9000u);
}

TEST_F(SefamMirrorTest, WithHistoryOnlyTheLastTwoNightsAreListed) {
    // D4: once the device has history, the card's last two nights only.
    // Nights by the folders' own stamps: DATA_1 night 10, DATA_2 night 11,
    // DATA_3 night 12, DATA_4 (03:00 on the 13th) night 12 too.
    const auto r = mirrorSefamCard(card_, archive_.string(), /*whole_card=*/false);
    ASSERT_TRUE(r.ok);
    EXPECT_TRUE(fs::exists(archive_ / "1263R/00000000/DATA_2/DATA_2.INI"));
    EXPECT_TRUE(fs::exists(archive_ / "1263R/00000000/DATA_3/DATA_3.INI"));
    EXPECT_TRUE(fs::exists(archive_ / "1263R/00000000/DATA_4/DATA_4.INI"));
    EXPECT_FALSE(fs::exists(archive_ / "1263R/00000000/DATA_1")) << "older than two nights";
    for (const auto& p : card_.listed)
        EXPECT_EQ(p.find("DATA_1"), std::string::npos) << "listed " << p;
}

TEST_F(SefamMirrorTest, ACardThatDoesNotAnswerIsAFailureNotAnEmptyCard) {
    card_.dead = true;
    const auto r = mirrorSefamCard(card_, archive_.string(), true);
    EXPECT_FALSE(r.ok);
    EXPECT_FALSE(r.error.empty());
}

TEST_F(SefamMirrorTest, NoArchiveFolderIsRefused) {
    const auto r = mirrorSefamCard(card_, "", true);
    EXPECT_FALSE(r.ok);
}

TEST(SefamMirrorNames, SessionFoldersAreDataNOrADay) {
    EXPECT_TRUE(isSefamSessionFolderName("DATA_229"));
    EXPECT_TRUE(isSefamSessionFolderName("data_7"));
    EXPECT_TRUE(isSefamSessionFolderName("260913"));
    EXPECT_FALSE(isSefamSessionFolderName("DATA_"));
    EXPECT_FALSE(isSefamSessionFolderName("1263R"));
    EXPECT_FALSE(isSefamSessionFolderName("24337476"));   // a serial, eight digits
    EXPECT_FALSE(isSefamSessionFolderName("DATALOG"));
}

TEST(SefamMirrorNights, WithoutStampsTheLastFourByNumber) {
    std::vector<EzShareFileEntry> dirs;
    for (int n : {3, 10, 1, 7, 2, 9}) {
        EzShareFileEntry e;
        e.name = "DATA_" + std::to_string(n);
        e.is_dir = true;
        dirs.push_back(e);
    }
    const auto out = lastTwoNights(dirs);
    ASSERT_EQ(out.size(), 4u);
    EXPECT_EQ(out.front().name, "DATA_3");
    EXPECT_EQ(out.back().name, "DATA_10");
}
