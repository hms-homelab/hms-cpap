//
// test_LocalDataSource.cpp: SDD-040, a local card root behind IDataSource.
//
// This is the half that lets the burst's second branch be deleted, so it is
// pinned against the contract the ez Share path already relies on: sizes in KB,
// date folders under DATALOG, a range that appends from an offset, and the two
// answers that make the no-op rule possible (filesAreInPlace/rootPath).
//
#include <gtest/gtest.h>

#include "clients/LocalDataSource.h"
#include "services/SessionDiscoveryService.h"

#include <chrono>
#include <filesystem>
#include <fstream>
#include <string>
#include <unistd.h>

using namespace hms_cpap;
namespace fs = std::filesystem;

namespace {

class LocalDataSourceTest : public ::testing::Test {
protected:
    fs::path root;

    void SetUp() override {
        root = fs::temp_directory_path() / ("hms_lds_" + std::to_string(::getpid()));
        fs::remove_all(root);
        fs::create_directories(root / "DATALOG" / "20260328");
        fs::create_directories(root / "DATALOG" / "20260329");
        fs::create_directories(root / "SETTINGS");
        write(root / "DATALOG" / "20260328" / "20260328_220000_BRP.edf", 3 * 1024);
        write(root / "DATALOG" / "20260328" / "20260328_220000_CSL.edf", 800);   // under 1 KB
        write(root / "DATALOG" / "20260329" / "20260329_213000_BRP.edf", 2 * 1024);
        write(root / "STR.edf", 5 * 1024);
        write(root / "SETTINGS" / "SET1.tgt", 64);
    }

    void TearDown() override { fs::remove_all(root); }

    static void write(const fs::path& p, size_t bytes, char fill = 'a') {
        std::ofstream o(p, std::ios::binary);
        o << std::string(bytes, fill);
    }

    static std::string read(const fs::path& p) {
        std::ifstream in(p, std::ios::binary);
        return std::string(std::istreambuf_iterator<char>(in), {});
    }
};

}  // namespace

TEST_F(LocalDataSourceTest, ItSaysItsFilesAreAlreadyWhereTheArchiveWouldPutThem) {
    LocalDataSource src(root.string());
    // SDD-040 D1: this is what lets the cycle skip staging, the archive mirror
    // and the residual walk for a local source instead of copying a user's card
    // onto itself.
    EXPECT_TRUE(src.filesAreInPlace());
    EXPECT_EQ(src.rootPath(), root.string());
    EXPECT_TRUE(src.supportsRange());
}

TEST_F(LocalDataSourceTest, DateFoldersComeFromDatalogSortedAndNothingElseDoes) {
    LocalDataSource src(root.string());
    const auto folders = src.listDateFolders();
    ASSERT_EQ(folders.size(), 2u);
    EXPECT_EQ(folders[0], "20260328");
    EXPECT_EQ(folders[1], "20260329");   // SETTINGS and STR.edf are not nights
}

TEST_F(LocalDataSourceTest, FilesAreListedWithKilobyteSizesLikeTheCard) {
    LocalDataSource src(root.string());
    const auto files = src.listFiles("20260328");
    ASSERT_EQ(files.size(), 2u);
    EXPECT_EQ(files[0].name, "20260328_220000_BRP.edf");
    EXPECT_EQ(files[0].size_kb, 3);
    EXPECT_FALSE(files[0].is_dir);
    // Under a kilobyte reads as 0, exactly as an ez Share listing reports it.
    EXPECT_EQ(files[1].name, "20260328_220000_CSL.edf");
    EXPECT_EQ(files[1].size_kb, 0);
}

TEST_F(LocalDataSourceTest, AnUnknownFolderIsEmptyNotAnError) {
    LocalDataSource src(root.string());
    EXPECT_TRUE(src.listFiles("20991231").empty());
    EXPECT_TRUE(LocalDataSource((root / "nope").string()).listDateFolders().empty());
}

TEST_F(LocalDataSourceTest, TheCardRootAndItsSubdirectoriesWalkLikeACard) {
    LocalDataSource src(root.string());
    const auto at_root = src.listDir("");      // SDD-002: "" is the card root
    bool saw_str = false, saw_settings_dir = false;
    for (const auto& e : at_root) {
        if (e.name == "STR.edf") { saw_str = true; EXPECT_EQ(e.size_kb, 5); }
        if (e.name == "SETTINGS") saw_settings_dir = e.is_dir;
    }
    EXPECT_TRUE(saw_str);
    EXPECT_TRUE(saw_settings_dir);

    const auto in_settings = src.listDir("SETTINGS");
    ASSERT_EQ(in_settings.size(), 1u);
    EXPECT_EQ(in_settings[0].name, "SET1.tgt");
}

TEST_F(LocalDataSourceTest, AFileIsReadOutWhereverTheCallerAsksForIt) {
    LocalDataSource src(root.string());
    const auto dest = root.parent_path() / ("hms_lds_out_" + std::to_string(::getpid()) + ".edf");
    fs::remove(dest);

    EXPECT_TRUE(src.downloadFile("20260329", "20260329_213000_BRP.edf", dest.string()));
    EXPECT_EQ(fs::file_size(dest), 2u * 1024);

    EXPECT_TRUE(src.downloadRootFile("STR.edf", dest.string()));
    EXPECT_EQ(fs::file_size(dest), 5u * 1024);

    EXPECT_TRUE(src.downloadByPath("SETTINGS\\SET1.tgt", dest.string()));
    EXPECT_EQ(fs::file_size(dest), 64u);

    EXPECT_FALSE(src.downloadFile("20260329", "missing.edf", dest.string()));
    fs::remove(dest);
}

TEST_F(LocalDataSourceTest, CopyingAFileOntoItselfLeavesItAlone) {
    // The cycle will not ask (filesAreInPlace), but a caller that does must not
    // truncate the user's own card.
    LocalDataSource src(root.string());
    const auto self = root / "DATALOG" / "20260329" / "20260329_213000_BRP.edf";
    EXPECT_TRUE(src.downloadFile("20260329", "20260329_213000_BRP.edf", self.string()));
    EXPECT_EQ(fs::file_size(self), 2u * 1024);
}

// SDD-040: the whole point of this class is that ONE cycle can serve every
// transport. That is only true if the same folder groups into the same sessions
// whether it is read as a card or as a directory, so the two paths are compared
// on the same bytes. estimateCheckpointEnd() widens a checkpoint to its
// modification time, which is why listFiles() carries one.
TEST_F(LocalDataSourceTest, TheSameFolderGroupsTheSameWayThroughEitherPath) {
    const std::string datalog = (root / "DATALOG").string();

    LocalDataSource src(root.string());
    SessionDiscoveryService via_source(src);

    const auto through_the_interface = via_source.groupSessionsInFolder("20260328");
    const auto through_the_filesystem =
        SessionDiscoveryService::groupLocalFolder(datalog + "/20260328", "20260328");

    ASSERT_EQ(through_the_interface.size(), through_the_filesystem.size())
        << "a folder must resolve to the same number of nights on either path";
    for (size_t i = 0; i < through_the_interface.size(); ++i) {
        const auto& a = through_the_interface[i];
        const auto& b = through_the_filesystem[i];
        EXPECT_EQ(a.session_prefix, b.session_prefix);
        EXPECT_EQ(a.date_folder, b.date_folder);
        EXPECT_EQ(a.brp_files, b.brp_files);
        EXPECT_EQ(a.pld_files, b.pld_files);
        EXPECT_EQ(a.sad_files, b.sad_files);
        EXPECT_EQ(a.csl_files, b.csl_files);
        EXPECT_EQ(a.eve_files, b.eve_files);
        EXPECT_EQ(a.session_start, b.session_start);
    }
}

TEST_F(LocalDataSourceTest, ARangeAppendsFromTheOffsetAndEofIsNothingNew) {
    LocalDataSource src(root.string());
    const auto dest = root.parent_path() / ("hms_lds_range_" + std::to_string(::getpid()) + ".edf");
    fs::remove(dest);

    // The collector's shape: it holds the first bytes and asks for the rest.
    write(dest, 1024, 'a');
    size_t got = 0;
    ASSERT_TRUE(src.downloadFileRange("20260329", "20260329_213000_BRP.edf",
                                      dest.string(), 1024, got));
    EXPECT_EQ(got, 1024u);
    EXPECT_EQ(fs::file_size(dest), 2u * 1024);

    // From byte 0 the local copy is REPLACED, not appended to. Pinned against
    // EzShareClient, which opens with trunc for a zero offset: a LocalDataSource
    // that always appended would double the file.
    got = 0;
    ASSERT_TRUE(src.downloadFileRange("20260329", "20260329_213000_BRP.edf",
                                      dest.string(), 0, got));
    EXPECT_EQ(got, 2u * 1024);
    EXPECT_EQ(fs::file_size(dest), 2u * 1024);

    // At EOF: true, nothing appended. That is what the collector reads as
    // "unchanged", the signal a night settles on.
    got = 12345;
    ASSERT_TRUE(src.downloadFileRange("20260329", "20260329_213000_BRP.edf",
                                      dest.string(), 2 * 1024, got));
    EXPECT_EQ(got, 0u);
    EXPECT_EQ(fs::file_size(dest), 2u * 1024);

    fs::remove(dest);
}
