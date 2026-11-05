// SDD-060: a BMC / React Health Luna card through an ez Share (or a Mule and
// Miner in local mode), copied into a folder the BMC ingestion reads.
//
// A fake card serves the root of a Luna card the way the bench card did
// (2026-11-03): five serial-named files, every entry dated 1980-0-0, sizes in
// KB. What is tested is what the mirror asks for and what it leaves on disk.
#include <gtest/gtest.h>

#include "services/BmcCardMirror.h"

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <map>
#include <string>
#ifndef _WIN32
#include <unistd.h>
#else
#include <process.h>
#endif
#include <vector>

using namespace hms_cpap;
namespace fs = std::filesystem;

namespace {

class FakeLuna : public IDataSource {
public:
    std::map<std::string, std::string> files;   // root name -> bytes
    std::vector<std::string> whole, ranged;
    bool dead = false;
    bool range = true;
    bool ignore_range = false;   // answers a Range with the whole file
    int stamp_year = 1980;       // the one bench Luna's 1980-0-0, unless a test dates it

    bool supportsRange() const override { return range; }

    std::vector<EzShareFileEntry> listDir(const std::string& p) override {
        if (dead || !p.empty()) return {};
        std::vector<EzShareFileEntry> out;
        for (const auto& [n, b] : files) {
            EzShareFileEntry e;
            e.name = n;
            e.size_kb = static_cast<int>((b.size() + 1023) / 1024);
            e.year = stamp_year;
            out.push_back(e);
        }
        EzShareFileEntry cfg;
        cfg.name = "ezshare.cfg";
        cfg.year = 2016;
        out.push_back(cfg);
        return out;
    }
    bool downloadByPath(const std::string& name, const std::string& local) override {
        whole.push_back(name);
        const auto it = files.find(name);
        if (it == files.end()) return false;
        std::ofstream(local, std::ios::binary) << it->second;
        return true;
    }
    bool downloadFileRange(const std::string& folder, const std::string& name,
                           const std::string& local, size_t start, size_t& got) override {
        if (!folder.empty()) return false;
        ranged.push_back(name);
        const auto it = files.find(name);
        if (it == files.end() || ignore_range) return false;
        if (start >= it->second.size()) return false;
        std::ofstream(local, std::ios::binary | std::ios::app) << it->second.substr(start);
        got = it->second.size() - start;
        return true;
    }
    std::vector<std::string> listDateFolders() override { return {}; }
    std::vector<EzShareFileEntry> listFiles(const std::string&) override { return {}; }
    bool downloadFile(const std::string&, const std::string&, const std::string&) override {
        return false;
    }
    bool downloadRootFile(const std::string&, const std::string&) override { return false; }
};

/// 256-byte packets with a running counter, so every byte differs by place.
std::string packets(int n, int first = 0) {
    std::string s;
    for (int i = first; i < first + n; ++i)
        for (int b = 0; b < 256; ++b) s.push_back(static_cast<char>((i * 7 + b) & 0xff));
    return s;
}

void lunaCard(FakeLuna& c, int raw_packets) {
    c.files["22B39570.000"] = packets(raw_packets);
    c.files["22B39570.EVT"] = std::string(2880, 'e');
    c.files["22B39570.IDX"] = std::string(3584, 'i');
    c.files["22B39570.LOG"] = std::string(34000, 'l');
    c.files["22B39570.USR"] = std::string(5000, 'u');
}

std::string slurp(const fs::path& p) {
    std::ifstream in(p, std::ios::binary);
    return std::string(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
}

class BmcMirrorTest : public ::testing::Test {
protected:
    void SetUp() override {
        dir_ = fs::temp_directory_path() /
               ("hms_bmc_mirror_" + std::to_string(::getpid()) + "_" + std::to_string(counter_++));
        fs::remove_all(dir_);
        lunaCard(card_, 923);   // 236,288 B, the bench card's .000
    }
    void TearDown() override {
        std::error_code ec;
        fs::remove_all(dir_, ec);
    }
    bool anyPart() const {
        for (const auto& e : fs::directory_iterator(dir_)) {
            const auto ext = e.path().extension().string();
            if (ext == ".part" || ext == ".tail") return true;
        }
        return false;
    }
    fs::path dir_;
    FakeLuna card_;
    static int counter_;
};
int BmcMirrorTest::counter_ = 0;

}  // namespace

TEST(BmcStem, TheStemIsTheOneWithAUsrBesideItsRawFiles) {
    std::vector<EzShareFileEntry> root;
    for (const char* n : {"22B39570.000", "22B39570.EVT", "22B39570.USR", "ezshare.cfg",
                          "NOTES.TXT"}) {
        EzShareFileEntry e;
        e.name = n;
        root.push_back(e);
    }
    ASSERT_TRUE(bmcStemOf(root));
    EXPECT_EQ(*bmcStemOf(root), "22B39570");
}

TEST(BmcStem, NoUsrIsNoLunaCard) {
    std::vector<EzShareFileEntry> root;
    for (const char* n : {"22B39570.000", "22B39570.EVT"}) {
        EzShareFileEntry e;
        e.name = n;
        root.push_back(e);
    }
    EXPECT_FALSE(bmcStemOf(root));
}

TEST(BmcStem, TwoCardsPickTheOneWhoseRawFileIsLargest) {
    std::vector<EzShareFileEntry> root;
    auto add = [&](const char* n, int kb) {
        EzShareFileEntry e;
        e.name = n;
        e.size_kb = kb;
        root.push_back(e);
    };
    add("11111111.000", 40);
    add("11111111.USR", 1);
    add("22B39570.000", 312);
    add("22B39570.USR", 1);
    EXPECT_EQ(*bmcStemOf(root), "22B39570");
}

TEST_F(BmcMirrorTest, TheFirstPassCopiesTheCardsFiveFilesAndNothingElse) {
    const auto r = mirrorBmcCard(card_, dir_.string());
    ASSERT_TRUE(r.ok) << r.error;
    EXPECT_EQ(r.stem, "22B39570");
    EXPECT_EQ(r.fetched, 5);
    EXPECT_EQ(slurp(dir_ / "22B39570.000"), card_.files["22B39570.000"]);
    EXPECT_FALSE(fs::exists(dir_ / "ezshare.cfg")) << "only the card's own files (D2)";
    EXPECT_FALSE(anyPart());
}

TEST_F(BmcMirrorTest, AnUnchangedCardFetchesNothing) {
    ASSERT_TRUE(mirrorBmcCard(card_, dir_.string()).ok);
    card_.whole.clear();
    const auto r = mirrorBmcCard(card_, dir_.string());
    ASSERT_TRUE(r.ok);
    EXPECT_EQ(r.fetched + r.appended, 0);
    EXPECT_TRUE(card_.whole.empty());
    EXPECT_FALSE(r.raw_changed);
}

TEST_F(BmcMirrorTest, ASessionLaterTheRawFileIsAppendedAndTheSidecarsTakenAgain) {
    ASSERT_TRUE(mirrorBmcCard(card_, dir_.string()).ok);
    card_.whole.clear();
    // The bench: 236,288 B became 319,232 B, the old bytes an exact prefix.
    card_.files["22B39570.000"] += packets(324, 923);
    card_.files["22B39570.EVT"] += "x";   // under a KB: the KB alone cannot see it

    const auto r = mirrorBmcCard(card_, dir_.string());
    ASSERT_TRUE(r.ok);
    EXPECT_EQ(r.appended, 1);
    EXPECT_TRUE(r.raw_changed);
    EXPECT_EQ(slurp(dir_ / "22B39570.000"), card_.files["22B39570.000"]);
    EXPECT_EQ(fs::file_size(dir_ / "22B39570.000"), 319232u);
    EXPECT_EQ(slurp(dir_ / "22B39570.EVT"), card_.files["22B39570.EVT"])
        << "a raw file moved, so .EVT is taken again (D3)";
    std::vector<std::string> expect_whole{"22B39570.EVT", "22B39570.IDX", "22B39570.LOG"};
    EXPECT_EQ(card_.whole, expect_whole) << ".000 was ranged, .USR untouched";
    EXPECT_FALSE(anyPart());
}

TEST_F(BmcMirrorTest, AFileWhoseStampMovedAtTheSameKbIsTakenAgain) {
    // Size alone cannot say a file changed: the KB is rounded. A card that
    // dates its files is judged by the stamp as well.
    card_.stamp_year = 2026;
    ASSERT_TRUE(mirrorBmcCard(card_, dir_.string()).ok);
    card_.whole.clear();
    card_.files["22B39570.USR"][0] = 'X';   // same size, rewritten
    card_.stamp_year = 2027;                 // and the card says so

    const auto r = mirrorBmcCard(card_, dir_.string());
    ASSERT_TRUE(r.ok);
    EXPECT_EQ(slurp(dir_ / "22B39570.USR"), card_.files["22B39570.USR"]);
    EXPECT_NE(std::find(card_.whole.begin(), card_.whole.end(), "22B39570.USR"),
              card_.whole.end());
}

TEST_F(BmcMirrorTest, ACardThatNoLongerStartsWithWhatWeHoldIsTakenWhole) {
    ASSERT_TRUE(mirrorBmcCard(card_, dir_.string()).ok);
    card_.whole.clear();
    card_.files["22B39570.000"] = packets(1400, 5000);   // longer, different bytes

    const auto r = mirrorBmcCard(card_, dir_.string());
    ASSERT_TRUE(r.ok);
    EXPECT_EQ(r.appended, 0);
    EXPECT_EQ(slurp(dir_ / "22B39570.000"), card_.files["22B39570.000"]);
    EXPECT_EQ(card_.whole.front(), "22B39570.000");
}

TEST_F(BmcMirrorTest, AShorterRawFileIsTakenWhole) {
    ASSERT_TRUE(mirrorBmcCard(card_, dir_.string()).ok);
    card_.files["22B39570.000"] = packets(100);

    const auto r = mirrorBmcCard(card_, dir_.string());
    ASSERT_TRUE(r.ok);
    EXPECT_EQ(r.appended, 0);
    EXPECT_EQ(slurp(dir_ / "22B39570.000"), card_.files["22B39570.000"]);
}

TEST_F(BmcMirrorTest, ASourceThatIgnoresRangeFallsBackToTheWholeFile) {
    ASSERT_TRUE(mirrorBmcCard(card_, dir_.string()).ok);
    card_.ignore_range = true;
    card_.files["22B39570.000"] += packets(324, 923);

    const auto r = mirrorBmcCard(card_, dir_.string());
    ASSERT_TRUE(r.ok);
    EXPECT_EQ(r.appended, 0);
    EXPECT_EQ(slurp(dir_ / "22B39570.000"), card_.files["22B39570.000"]);
    EXPECT_FALSE(anyPart());
}

TEST_F(BmcMirrorTest, ACardThatDoesNotAnswerIsAFailureNotAnEmptyCard) {
    card_.dead = true;
    const auto r = mirrorBmcCard(card_, dir_.string());
    EXPECT_FALSE(r.ok);
    EXPECT_FALSE(r.error.empty());
}

TEST_F(BmcMirrorTest, ACardWithNoLunaFilesSaysSo) {
    card_.files.clear();
    card_.files["STR.edf"] = "resmed";
    const auto r = mirrorBmcCard(card_, dir_.string());
    EXPECT_FALSE(r.ok);
    EXPECT_NE(r.error.find("no Luna card"), std::string::npos);
}
