//
// test_CardLayout.cpp (SDD-010).
//
// The bug these exist to prevent: `local_dir` was documented and used as the
// DATALOG folder, so STR resolution reached UP with parent_path() and then fell
// back to searching INSIDE DATALOG. ResMed never writes STR there, so the
// fallback could not succeed. Its only effect was to make a misconfigured path
// present itself as a missing file, which sent people looking for a data
// problem that did not exist.
//
// So the property under test is: the three possible shapes of a configured
// folder are told apart CORRECTLY and UNAMBIGUOUSLY, because everything
// downstream (whether we ingest at all, and what the banner says) is decided
// from this one answer.
//
#include <gtest/gtest.h>

#include "utils/CardLayout.h"

#include <filesystem>
#include <fstream>
#include <string>

using namespace hms_cpap;

namespace {

namespace fs = std::filesystem;

/// A scratch directory that removes itself. Named per-test so a crashed run
/// cannot leak state into the next one.
class TempTree {
public:
    explicit TempTree(const std::string& tag) {
        root_ = fs::temp_directory_path() /
                ("hms_cardlayout_" + tag + "_" +
                 std::to_string(reinterpret_cast<uintptr_t>(this)));
        std::error_code ec;
        fs::remove_all(root_, ec);
        fs::create_directories(root_, ec);
    }
    ~TempTree() {
        std::error_code ec;
        fs::remove_all(root_, ec);
    }

    TempTree(const TempTree&) = delete;
    TempTree& operator=(const TempTree&) = delete;

    const fs::path& path() const { return root_; }
    std::string str() const { return root_.string(); }

    void dir(const std::string& rel) const {
        std::error_code ec;
        fs::create_directories(root_ / rel, ec);
    }
    void file(const std::string& rel) const {
        std::error_code ec;
        fs::create_directories((root_ / rel).parent_path(), ec);
        std::ofstream(root_ / rel) << "x";
    }

private:
    fs::path root_;
};

// ---------------------------------------------------------------- Root

TEST(CardLayoutTest, ACardRootIsRecognisedByItsDatalogFolder) {
    TempTree t("root");
    t.dir("DATALOG");
    t.file("STR.edf");

    EXPECT_EQ(classifyLocalDir(t.str()), LocalDirLayout::Root);
}

TEST(CardLayoutTest, ARootIsStillARootWithNoStrYet) {
    // A card whose machine has not written STR yet is configured CORRECTLY.
    // Treating a missing STR as a layout problem would hard-fail a working
    // setup, and STR absence is explicitly not an error in SDD-010.
    TempTree t("root_no_str");
    t.dir("DATALOG");

    EXPECT_EQ(classifyLocalDir(t.str()), LocalDirLayout::Root);
}

TEST(CardLayoutTest, LowercaseDatalogIsAcceptedAsARoot) {
    // A card copied by hand onto a case-sensitive filesystem can arrive
    // lowercase. Same place, different spelling.
    TempTree t("root_lower");
    t.dir("datalog");

    EXPECT_EQ(classifyLocalDir(t.str()), LocalDirLayout::Root);
}

TEST(CardLayoutTest, DatalogWinsOverAStrayDateFolderInTheRoot) {
    // A root that ALSO happens to contain an eight-digit folder is still a
    // root. If date folders were checked first, one stray directory would
    // misclassify a perfectly good card as DATALOG and stop all ingestion.
    TempTree t("root_and_stray");
    t.dir("DATALOG/20260101");
    t.dir("20251231");

    EXPECT_EQ(classifyLocalDir(t.str()), LocalDirLayout::Root);
}

TEST(CardLayoutTest, AnEmptyDatalogStillMakesItARoot) {
    // No sessions recorded yet is a legitimate state, not a misconfiguration.
    TempTree t("root_empty_datalog");
    t.dir("DATALOG");

    EXPECT_EQ(classifyLocalDir(t.str()), LocalDirLayout::Root);
    EXPECT_EQ(datalogDirFor(t.str()), (t.path() / "DATALOG").string());
}

// ----------------------------------------------------------- IsDatalog

TEST(CardLayoutTest, APathPointedAtDatalogItselfIsDetected) {
    // THE case this whole file exists for. Sessions would import and STR never
    // would, so it must be caught rather than tolerated.
    TempTree t("is_datalog");
    t.dir("20260101");
    t.dir("20260102");

    EXPECT_EQ(classifyLocalDir(t.str()), LocalDirLayout::IsDatalog);
}

TEST(CardLayoutTest, ASingleDateFolderIsEnoughToRecogniseDatalog) {
    TempTree t("is_datalog_one");
    t.dir("20260101");

    EXPECT_EQ(classifyLocalDir(t.str()), LocalDirLayout::IsDatalog);
}

TEST(CardLayoutTest, TheDatalogRemedyPointsAtTheFolderAboveIt) {
    // A remedy that does not name the actual fix is how a user ends up filing
    // a support ticket, which is what SDD-010 is trying to prevent.
    TempTree t("is_datalog_remedy");
    t.dir("20260101");

    const auto remedy = localDirRemedy(LocalDirLayout::IsDatalog, t.str());
    EXPECT_NE(remedy.find(t.path().parent_path().string()), std::string::npos)
        << "remedy should name the parent directory, got: " << remedy;
}

// ------------------------------------------------------------ Unusable

TEST(CardLayoutTest, AnEmptyPathIsUnusable) {
    EXPECT_EQ(classifyLocalDir(""), LocalDirLayout::Unusable);
}

TEST(CardLayoutTest, AMissingPathIsUnusable) {
    // An unmounted share and a typo look identical from here, and both need
    // the same answer: stop, and say so.
    const auto missing = fs::temp_directory_path() / "hms_cardlayout_definitely_absent";
    std::error_code ec;
    fs::remove_all(missing, ec);

    EXPECT_EQ(classifyLocalDir(missing.string()), LocalDirLayout::Unusable);
}

TEST(CardLayoutTest, ADirectoryWithNeitherDatalogNorDateFoldersIsUnusable) {
    TempTree t("unusable");
    t.dir("Documents");
    t.file("readme.txt");

    EXPECT_EQ(classifyLocalDir(t.str()), LocalDirLayout::Unusable);
}

TEST(CardLayoutTest, AFileIsNotADirectoryAndIsUnusable) {
    TempTree t("unusable_file");
    t.file("STR.edf");

    EXPECT_EQ(classifyLocalDir((t.path() / "STR.edf").string()),
              LocalDirLayout::Unusable);
}

// -------------------------------------------- date-folder name matching

TEST(CardLayoutTest, NonDateFolderNamesNeverCountAsDateFolders) {
    // Eight characters is not eight DIGITS, and nine digits is not a date
    // folder either. A loose match here would classify ordinary directories as
    // DATALOG and refuse to ingest from a working root.
    TempTree t("not_dates");
    t.dir("STR");
    t.dir("2026");
    t.dir("202601011");   // nine digits
    t.dir("2026010");     // seven digits
    t.dir("2026-01-01");  // ten chars, has separators
    t.dir("abcdefgh");    // eight chars, not digits

    EXPECT_EQ(classifyLocalDir(t.str()), LocalDirLayout::Unusable);
}

TEST(CardLayoutTest, AFileNamedLikeADateDoesNotMakeItDatalog) {
    // Only directories are session folders.
    TempTree t("date_file");
    t.file("20260101");

    EXPECT_EQ(classifyLocalDir(t.str()), LocalDirLayout::Unusable);
}

// ------------------------------------------------------ datalogDirFor

TEST(CardLayoutTest, DatalogDirForResolvesToARealDirectoryWhateverTheCasing) {
    // Asserting an exact spelling here would be a test of the FILESYSTEM, not
    // of this code: macOS and Windows are case-insensitive, so a lowercase
    // "datalog" on disk is legitimately found and returned as "DATALOG". Linux
    // returns the lowercase form. Both are right.
    //
    // What callers actually depend on, and what must hold everywhere, is that
    // the returned path names a directory that exists.
    TempTree t("datalog_case");
    t.dir("datalog");

    const auto resolved = datalogDirFor(t.str());
    EXPECT_TRUE(fs::is_directory(resolved)) << "not a directory: " << resolved;
    EXPECT_EQ(classifyLocalDir(t.str()), LocalDirLayout::Root);
}

TEST(CardLayoutTest, DatalogDirForFallsBackToTheCanonicalNameWhenAbsent) {
    // Callers use the result in error messages, so it must always be a usable
    // path to name rather than an empty string.
    TempTree t("datalog_absent");

    EXPECT_EQ(datalogDirFor(t.str()), (t.path() / "DATALOG").string());
}

// ------------------------------------------------------------ messages

TEST(CardLayoutTest, ARootHasNoProblemAndNoRemedy) {
    TempTree t("root_msgs");
    t.dir("DATALOG");

    EXPECT_TRUE(localDirProblem(LocalDirLayout::Root, t.str()).empty());
    EXPECT_TRUE(localDirRemedy(LocalDirLayout::Root, t.str()).empty());
}

TEST(CardLayoutTest, EveryFailureNamesBothTheProblemAndAFix) {
    // The invariant behind the banner: a user who reads it knows what is wrong
    // AND what to do next.
    for (auto layout : {LocalDirLayout::IsDatalog, LocalDirLayout::Unusable}) {
        EXPECT_FALSE(localDirProblem(layout, "/some/path").empty())
            << "layout " << localDirLayoutString(layout) << " has no problem text";
        EXPECT_FALSE(localDirRemedy(layout, "/some/path").empty())
            << "layout " << localDirLayoutString(layout) << " has no remedy";
    }
}

TEST(CardLayoutTest, AnEmptyPathSaysNothingIsConfiguredRatherThanNamingAnEmptyPath) {
    const auto problem = localDirProblem(LocalDirLayout::Unusable, "");
    EXPECT_NE(problem.find("no local folder"), std::string::npos)
        << "got: " << problem;
}

TEST(CardLayoutTest, LayoutStringsAreStable) {
    // These travel to the UI over /api/capabilities, so they are an interface.
    EXPECT_STREQ(localDirLayoutString(LocalDirLayout::Root), "root");
    EXPECT_STREQ(localDirLayoutString(LocalDirLayout::IsDatalog), "is_datalog");
    EXPECT_STREQ(localDirLayoutString(LocalDirLayout::Unusable), "unusable");
}

}  // namespace
