// SDD-061: a .cpapcard upload. Its layout entry read out of the zip before the
// card is queued, bounded before a byte is inflated; and taken out of the
// extracted card, so it never lands in the archive.

#include "services/CpapCardZip.h"
#include "sleepbridge/LearnedCardTestUtil.h"

#include "services/CpapCard.h"
#include "services/PrismaIngestion.h"
#include "sleepbridge/LearnedCard.h"

#include <gtest/gtest.h>

#include <atomic>
#include <filesystem>
#include <fstream>
#include <unistd.h>

using namespace hms_cpap;
using cpapcardtest::Entry;
namespace fs = std::filesystem;

namespace {

struct Scratch {
    fs::path dir;
    Scratch() {
        static std::atomic<int> n{0};
        dir = fs::temp_directory_path() /
              ("hms_cpapcard_" + std::to_string(::getpid()) + "_" + std::to_string(n++));
        fs::remove_all(dir);
        fs::create_directories(dir);
    }
    ~Scratch() {
        std::error_code ec;
        fs::remove_all(dir, ec);
    }
    std::string path(const std::string& name) const { return (dir / name).string(); }
};

const std::string kStr = std::string(1024, 'S');
const std::string kBrp = std::string(2048, 'B');

}  // namespace

TEST(CpapCard, TheLayoutIsReadOutOfTheFileAsDesktopWritesIt) {
    Scratch s;
    const std::string cbl1 = lctest::card().cbl1();
    ASSERT_TRUE(cpapcardtest::writeZip(s.path("x.cpapcard"),
                                       {{"DATALOG/20261101/20261101_221500_BRP.edf", kBrp},
                                        {"STR.edf", kStr},
                                        {"Identification.tgt", "#SRN 00000000000\n"},
                                        {"baseline.cbl1", cbl1, true}}));
    std::string why;
    const auto got = readCardLayout(s.path("x.cpapcard"), &why);
    ASSERT_TRUE(got) << why;
    EXPECT_EQ(*got, cbl1);
    EXPECT_TRUE(sleepbridge::decodeLearnedCard(*got, &why)) << why;
}

TEST(CpapCard, APlainCardZipHasNoLayout) {
    Scratch s;
    ASSERT_TRUE(cpapcardtest::writeZip(s.path("card.zip"), {{"STR.edf", kStr}}));
    std::string why = "stale";
    EXPECT_FALSE(readCardLayout(s.path("card.zip"), &why));
    EXPECT_TRUE(why.empty());

    std::ofstream(s.path("junk.zip")) << "not a zip";
    EXPECT_FALSE(readCardLayout(s.path("junk.zip"), &why));
    EXPECT_TRUE(why.empty());
}

TEST(CpapCard, TheLayoutIsFoundInsideOneWrappingFolderOnly) {
    Scratch s;
    const std::string cbl1 = lctest::card().cbl1();
    ASSERT_TRUE(cpapcardtest::writeZip(s.path("wrapped.zip"),
                                       {{"MYCARD/STR.edf", kStr}, {"MYCARD/baseline.cbl1", cbl1, true}}));
    std::string why;
    EXPECT_TRUE(readCardLayout(s.path("wrapped.zip"), &why));

    ASSERT_TRUE(cpapcardtest::writeZip(s.path("deep.zip"), {{"A/B/baseline.cbl1", cbl1, true}}));
    EXPECT_FALSE(readCardLayout(s.path("deep.zip"), &why));
    ASSERT_TRUE(cpapcardtest::writeZip(s.path("named.zip"), {{"notbaseline.cbl1", cbl1, true}}));
    EXPECT_FALSE(readCardLayout(s.path("named.zip"), &why));
}

TEST(CpapCard, ALayoutOver32MbIsRefusedBeforeItIsInflated) {
    Scratch s;
    const std::string big(sleepbridge::kLearnedCardMax + 1, '\0');   // deflates to almost nothing
    ASSERT_TRUE(cpapcardtest::writeZip(s.path("big.cpapcard"), {{"baseline.cbl1", big, true}}));
    std::string why;
    EXPECT_FALSE(readCardLayout(s.path("big.cpapcard"), &why));
    EXPECT_NE(why.find("32 MB"), std::string::npos);
}

TEST(CpapCard, TheExtractedCardLosesTheLayoutAndNothingElse) {
    Scratch s;
    ASSERT_TRUE(cpapcardtest::writeZip(s.path("x.cpapcard"),
                                       {{"DATALOG/20261101/20261101_221500_BRP.edf", kBrp},
                                        {"STR.edf", kStr},
                                        {"baseline.cbl1", lctest::card().cbl1(), true}}));
    const fs::path out = s.dir / "out";
    ASSERT_TRUE(PrismaIngestion::extractZip(s.path("x.cpapcard"), out.string()));
    ASSERT_TRUE(fs::exists(out / "baseline.cbl1"));
    EXPECT_EQ(removeCardLayout(out.string()), 1);
    EXPECT_FALSE(fs::exists(out / "baseline.cbl1"));
    EXPECT_TRUE(fs::exists(out / "STR.edf"));
    EXPECT_TRUE(fs::exists(out / "DATALOG/20261101/20261101_221500_BRP.edf"));
    EXPECT_EQ(removeCardLayout(out.string()), 0);
}
