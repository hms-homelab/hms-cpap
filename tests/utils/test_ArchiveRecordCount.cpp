// SDD-032 (ticket 127): the archive carries the real EDF record count.
#include <gtest/gtest.h>

#include "services/BurstCollectorService.h"
#include "utils/ArchiveRecordCount.h"
#include "utils/CardImport.h"

#include <algorithm>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <unistd.h>

using namespace hms_cpap;
namespace fs = std::filesystem;

namespace {

/// A two-signal EDF with [records] records whose header states [stored] as
/// num-data-records, as ResMed leaves it while the file records.
std::string makeEdf(int records, const std::string& stored) {
    const int ns = 2, spr_a = 1500, spr_b = 25;
    const int header_bytes = 256 + 256 * ns;
    std::string h(header_bytes, ' ');
    auto field = [&](std::size_t off, std::size_t len, const std::string& v) {
        std::memcpy(&h[off], v.data(), std::min(v.size(), len));
    };
    field(0, 8, "0");
    field(184, 8, std::to_string(header_bytes));
    field(236, 8, stored);
    field(244, 8, "60");
    field(252, 4, std::to_string(ns));
    const std::size_t spr_off = 256 + ns * 216;
    field(spr_off, 8, std::to_string(spr_a));
    field(spr_off + 8, 8, std::to_string(spr_b));
    std::string data(static_cast<std::size_t>(records) * (spr_a + spr_b) * 2, '\0');
    for (std::size_t i = 0; i < data.size(); ++i) data[i] = static_cast<char>(i * 7);
    return h + data;
}

std::string slurp(const fs::path& p) {
    std::ifstream in(p, std::ios::binary);
    return std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
}

class ArchiveRecordCount : public ::testing::Test {
protected:
    void SetUp() override {
        root_ = fs::temp_directory_path() / ("hms_recount_" + std::to_string(::getpid()));
        fs::remove_all(root_);
        fs::create_directories(root_);
    }
    void TearDown() override { std::error_code ec; fs::remove_all(root_, ec); }

    fs::path put(const fs::path& rel, const std::string& bytes) {
        const fs::path p = root_ / rel;
        fs::create_directories(p.parent_path());
        std::ofstream(p, std::ios::binary) << bytes;
        return p;
    }
    static std::string countOf(const fs::path& p) { return slurp(p).substr(236, 8); }

    fs::path root_;
};

}  // namespace

TEST_F(ArchiveRecordCount, AStaleSignalFileIsRepairedInPlaceAndNothingElseMoves) {
    const std::string stale = makeEdf(245, "-1");
    const auto p = put("20260913_233206_BRP.edf", stale);
    EXPECT_TRUE(repairSignalEdfFile(p.string()));

    const std::string after = slurp(p);
    ASSERT_EQ(after.size(), stale.size()) << "the size never changes";
    EXPECT_EQ(after.substr(236, 8), "245     ");
    EXPECT_EQ(after.substr(0, 236), stale.substr(0, 236));
    EXPECT_EQ(after.substr(244), stale.substr(244)) << "only the 8 bytes at 236";
    // What the card itself holds once ResMed finalizes the file.
    EXPECT_EQ(after, makeEdf(245, "245"));

    EXPECT_FALSE(repairSignalEdfFile(p.string())) << "a second pass has nothing to do";
}

TEST_F(ArchiveRecordCount, OnlyResMedSignalFilesWithWholeRecordsAreTouched) {
    const auto eve = put("20260913_233200_EVE.edf", makeEdf(3, "-1"));
    const auto csl = put("20260913_233200_CSL.edf", makeEdf(3, "-1"));
    const auto str = put("STR.edf", makeEdf(3, "-1"));
    const auto partial = put("20260913_233206_PLD.edf", makeEdf(10, "-1") + "xyz");
    const auto garbage = put("20260913_233206_SAD.edf", "not an EDF at all");
    for (const auto& p : {eve, csl, str, partial, garbage}) {
        const std::string before = slurp(p);
        EXPECT_FALSE(repairSignalEdfFile(p.string())) << p;
        EXPECT_EQ(slurp(p), before) << p;
    }
    EXPECT_FALSE(repairSignalEdfFile((root_ / "missing_BRP.edf").string()));
}

TEST_F(ArchiveRecordCount, TheSweepRepairsEveryNightAndLeavesTheRestAlone) {
    put("DATALOG/20260913/20260913_233206_BRP.edf", makeEdf(245, "-1"));
    put("DATALOG/20260913/20260913_233206_PLD.edf", makeEdf(245, "187"));  // too low
    put("DATALOG/20260913/20260913_233206_SAD.edf", makeEdf(245, "245"));  // already right
    put("DATALOG/20260913/20260913_233200_EVE.edf", makeEdf(3, "-1"));
    put("DATALOG/20260914/20260914_232407_BRP.edf", makeEdf(40, "-1"));
    put("DATALOG/notes/x_BRP.edf", makeEdf(5, "-1"));                     // not a night folder
    put("STR.edf", makeEdf(3, "-1"));

    const auto r = sweepArchiveSignalEdfs(root_.string());
    EXPECT_EQ(r.checked, 4);
    EXPECT_EQ(r.repaired, 3);
    EXPECT_EQ(countOf(root_ / "DATALOG/20260913/20260913_233206_BRP.edf"), "245     ");
    EXPECT_EQ(countOf(root_ / "DATALOG/20260913/20260913_233206_PLD.edf"), "245     ");
    EXPECT_EQ(countOf(root_ / "DATALOG/20260914/20260914_232407_BRP.edf"), "40      ");
    EXPECT_EQ(countOf(root_ / "DATALOG/20260913/20260913_233200_EVE.edf"), "-1      ");
    EXPECT_EQ(countOf(root_ / "DATALOG/notes/x_BRP.edf"), "-1      ");
    EXPECT_EQ(countOf(root_ / "STR.edf"), "-1      ");

    const auto again = sweepArchiveSignalEdfs(root_.string());
    EXPECT_EQ(again.checked, 4);
    EXPECT_EQ(again.repaired, 0);

    EXPECT_EQ(sweepArchiveSignalEdfs("").checked, 0);
    EXPECT_EQ(sweepArchiveSignalEdfs((root_ / "absent").string()).checked, 0);
}

// Ticket 127 exactly: the archived copy has the size of the download it came
// from, so the size-only skip never copied the finalized bytes. The archive
// step repairs it anyway.
TEST_F(ArchiveRecordCount, TheArchiveStepRepairsASameSizeCopy) {
    const std::string stale = makeEdf(245, "-1");
    put("tmp/20260914/20260914_232407_BRP.edf", stale);            // the Range-grown download
    put("archive/DATALOG/20260914/20260914_232407_BRP.edf", stale); // archived earlier, same size
    put("tmp/20260914/20260914_232402_EVE.edf", makeEdf(3, "-1"));

    BurstCollectorService svc(60);
    ASSERT_TRUE(svc.archiveSessionFilesForTest("20260914", (root_ / "tmp").string(),
                                               (root_ / "archive").string()));
    const auto archived = root_ / "archive/DATALOG/20260914";
    EXPECT_EQ(countOf(archived / "20260914_232407_BRP.edf"), "245     ");
    EXPECT_EQ(countOf(archived / "20260914_232402_EVE.edf"), "-1      ");
    // The download copy is the Range resume's and the parser's; left as it is.
    EXPECT_EQ(countOf(root_ / "tmp/20260914/20260914_232407_BRP.edf"), "-1      ");
}

TEST_F(ArchiveRecordCount, TheZipMirrorRepairsWhatItCopies) {
    put("zip/DATALOG/20260913/20260913_233206_BRP.edf", makeEdf(12, "-1"));
    put("zip/DATALOG/20260913/20260913_233200_EVE.edf", makeEdf(3, "-1"));
    const auto r = mirrorCardInto((root_ / "zip").string(), (root_ / "card").string(),
                                  (root_ / "card/DATALOG").string());
    EXPECT_EQ(r.copied, 2);
    EXPECT_EQ(countOf(root_ / "card/DATALOG/20260913/20260913_233206_BRP.edf"), "12      ");
    EXPECT_EQ(countOf(root_ / "card/DATALOG/20260913/20260913_233200_EVE.edf"), "-1      ");
    // The upload's own extraction is not ours to rewrite either way.
    EXPECT_EQ(countOf(root_ / "zip/DATALOG/20260913/20260913_233206_BRP.edf"), "-1      ");
}
