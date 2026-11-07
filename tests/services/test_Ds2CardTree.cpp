// SDD-065 D8: a DreamStation 2's card tree, placed from the files a Mule &
// Miner hands back already in their card form. Made-up bytes only: this host
// never opens them, so the tests need nothing a card would hold.

#include "services/Ds2CardTree.h"

#include <gtest/gtest.h>

#include <chrono>
#include <filesystem>
#include <fstream>
#include <sstream>

using namespace hms_cpap;
namespace fs  = std::filesystem;
namespace ds2 = cpapdash::ds2ble;

namespace {

constexpr const char* kSerial = "TEST0000000077";   // made up

ds2::ReadSealed item(uint8_t stream, uint32_t session, uint8_t bm, const std::string& path, const std::string& bytes) {
    ds2::ReadSealed s;
    s.stream   = stream;
    s.session  = session;
    s.bookmark = ds2::Bookmark{stream, bm, 0, 0, 0, 0, 0, 0, 0, 1};
    s.path     = path;
    s.card.assign(bytes.begin(), bytes.end());
    return s;
}

std::string readAll(const fs::path& p) {
    std::ifstream in(p, std::ios::binary);
    std::stringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

class CardTree : public ::testing::Test {
protected:
    fs::path dir_;
    void SetUp() override {
        dir_ = fs::temp_directory_path() /
               ("ds2_card_tree_" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
        fs::create_directories(dir_);
    }
    void TearDown() override {
        std::error_code ec;
        fs::remove_all(dir_, ec);
    }
};

}  // namespace

TEST(Ds2CardTreeFolder, IsEightHexDigitsEndingInTwoZerosAndTheSameForASerial) {
    const std::string f = Ds2CardTree::folderFor(kSerial);
    ASSERT_EQ(f.size(), 8u);
    EXPECT_EQ(f.substr(6), "00");
    for (const char c : f) EXPECT_TRUE((c >= '0' && c <= '9') || (c >= 'A' && c <= 'F')) << f;
    EXPECT_EQ(f, Ds2CardTree::folderFor(kSerial));
    EXPECT_NE(f, Ds2CardTree::folderFor("TEST0000000078"));
    // FNV-1a 32's offset basis (the empty string) is 811C9DC5 everywhere.
    EXPECT_EQ(Ds2CardTree::folderFor(""), "811C9D00");
}

TEST_F(CardTree, EachFileGoesToItsCardPathAsItCame) {
    Ds2CardTree t(dir_.string());
    const auto a = t.place(kSerial, item(1, 0x1F4, 1, "P0/000001F4.B01", "summary bytes"));
    ASSERT_TRUE(a.ok) << a.error;
    EXPECT_EQ(a.rel, "P0/000001F4.B01");
    ASSERT_TRUE(t.place(kSerial, item(0, 0, 0, "PROP.BIN", "prop")).ok);
    ASSERT_TRUE(t.place(kSerial, item(0, 0, 0, "LOG.SEQ", "log")).ok);
    const fs::path m = t.machineDir(kSerial);
    EXPECT_EQ(m, dir_ / "ds2card" / kSerial / "P-SERIES" / Ds2CardTree::folderFor(kSerial));
    EXPECT_EQ(readAll(m / "P0" / "000001F4.B01"), "summary bytes");
    EXPECT_EQ(readAll(m / "PROP.BIN"), "prop");
    EXPECT_EQ(readAll(m.parent_path() / "LAST.TXT"), Ds2CardTree::folderFor(kSerial));   // no line end
}

TEST_F(CardTree, LogFilesAreNumberedOnByBookmarkAndAReadAgainKeepsItsName) {
    Ds2CardTree t(dir_.string());
    EXPECT_EQ(t.place(kSerial, item(3, 0, 1, "D/000.B03", "d1")).rel, "D/000.B03");
    EXPECT_EQ(t.place(kSerial, item(3, 0, 2, "D/001.B03", "d2")).rel, "D/001.B03");
    // The next read numbers from 000 again.
    EXPECT_EQ(t.place(kSerial, item(3, 0, 3, "D/000.B03", "d3")).rel, "D/002.B03");
    EXPECT_EQ(t.place(kSerial, item(4, 0, 1, "E/000.B04", "e1")).rel, "E/000.B04");
    // The same record again (its bookmark) keeps its number.
    EXPECT_EQ(t.place(kSerial, item(3, 0, 2, "D/000.B03", "d2")).rel, "D/001.B03");
    EXPECT_EQ(readAll(t.machineDir(kSerial) / fs::path("D/002.B03")), "d3");
    const auto c = t.counts(kSerial);
    EXPECT_EQ(c.dfn, 3);
    EXPECT_EQ(c.efn, 1);
    // The numbering outlives the process.
    Ds2CardTree again(dir_.string());
    EXPECT_EQ(again.place(kSerial, item(3, 0, 9, "D/000.B03", "d9")).rel, "D/003.B03");
}

TEST_F(CardTree, APathOutsideTheCardsShapeIsRefused) {
    Ds2CardTree t(dir_.string());
    for (const auto& [stream, path] : std::vector<std::pair<uint8_t, std::string>>{
             {1, "../evil.B01"}, {1, "P0/../../x.B01"}, {1, "P0/000001F4.B02"}, {2, "P0/000001F4.B01"},
             {1, "P0/1F4.B01"}, {1, "/abs/000001F4.B01"}, {3, "E/000.B04"}, {4, "D/000.B03"},
             {0, "LAST.TXT"}, {0, "P0/000001F4.B01"}, {1, "P0\\000001F4.B01"}}) {
        const auto r = t.place(kSerial, item(stream, 1, 1, path, "x"));
        EXPECT_FALSE(r.ok) << path;
        EXPECT_EQ(r.error, "bad_path") << path;
    }
    EXPECT_FALSE(fs::exists(dir_ / "evil.B01"));
}

TEST_F(CardTree, CountsAreTheCurrentPFolderAndTheLogsAndFdTheOldestStart) {
    Ds2CardTree t(dir_.string());
    EXPECT_EQ(t.counts(kSerial).pfn, 0);
    ASSERT_TRUE(t.place(kSerial, item(1, 1, 1, "P0/00000001.B01", "a")).ok);
    ASSERT_TRUE(t.place(kSerial, item(2, 1, 1, "P0/00000001.B02", "b")).ok);
    ASSERT_TRUE(t.place(kSerial, item(1, 2, 2, "P0/00000002.B01", "c")).ok);
    EXPECT_EQ(t.counts(kSerial).pfn, 3);
    EXPECT_EQ(t.counts(kSerial).p_folder, 0);
    ASSERT_TRUE(t.place(kSerial, item(1, 3, 3, "P1/00000003.B01", "d")).ok);
    EXPECT_EQ(t.counts(kSerial).pfn, 1);
    EXPECT_EQ(t.counts(kSerial).p_folder, 1);
    t.noteSessionStart(kSerial, 2000);
    t.noteSessionStart(kSerial, 1000);
    t.noteSessionStart(kSerial, 3000);
    EXPECT_EQ(t.counts(kSerial).fd, 1000);
}
