#include <gtest/gtest.h>

#include "utils/LogTail.h"

#include <chrono>
#include <filesystem>
#include <fstream>
#include <string>

namespace fs = std::filesystem;
namespace lt = hms_cpap::logtail;

// SDD-059: the log read by byte cursor. Every file here is written by the test
// in a temporary directory; nothing reads a real log.

namespace {

class LogTailTest : public ::testing::Test {
protected:
    fs::path dir_, file_;
    void SetUp() override {
        dir_ = fs::temp_directory_path() /
               ("logtail_" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
        fs::create_directories(dir_);
        file_ = dir_ / "hms-cpap.log";
    }
    void TearDown() override { fs::remove_all(dir_); }

    void write(const std::string& text, bool append = false) {
        std::ofstream out(file_, std::ios::binary | (append ? std::ios::app : std::ios::trunc));
        out << text;
    }
    std::string lines(int from, int to) {   // "line <from>\n" .. "line <to - 1>\n"
        std::string s;
        for (int i = from; i < to; ++i) s += "line " + std::to_string(i) + "\n";
        return s;
    }
};

} // namespace

TEST_F(LogTailTest, TailAnswersTheLastLinesWithTheirOffsets) {
    write(lines(0, 10));
    const auto c = lt::tail(file_.string(), 3);
    ASSERT_TRUE(c.readable);
    ASSERT_EQ(c.lines.size(), 3u);
    EXPECT_EQ(c.lines.front(), "line 7");
    EXPECT_EQ(c.lines.back(), "line 9");
    EXPECT_EQ(c.end, c.size);
    EXPECT_EQ(c.start, lines(0, 7).size());
    EXPECT_FALSE(c.file_id.empty());
}

TEST_F(LogTailTest, TailOfAShortFileIsAllOfIt) {
    write(lines(0, 2));
    const auto c = lt::tail(file_.string(), 100);
    ASSERT_EQ(c.lines.size(), 2u);
    EXPECT_EQ(c.start, 0u);
}

TEST_F(LogTailTest, APartialLastLineIsHeldBack) {
    write(lines(0, 3) + "half a li");
    const auto c = lt::tail(file_.string(), 10);
    ASSERT_EQ(c.lines.size(), 3u);
    EXPECT_EQ(c.lines.back(), "line 2");
    EXPECT_EQ(c.end, lines(0, 3).size());
    EXPECT_LT(c.end, c.size);
}

TEST_F(LogTailTest, CarriageReturnsAreStripped) {
    write("a\r\nb\r\n");
    const auto c = lt::tail(file_.string(), 10);
    ASSERT_EQ(c.lines.size(), 2u);
    EXPECT_EQ(c.lines[0], "a");
    EXPECT_EQ(c.lines[1], "b");
}

TEST_F(LogTailTest, AfterAtTheEndAnswersNothingAndAfterAnAppendTheNewLines) {
    write(lines(0, 5));
    const auto first = lt::tail(file_.string(), 5);
    auto c = lt::after(file_.string(), first.end, first.file_id, 5);
    EXPECT_TRUE(c.lines.empty());
    EXPECT_FALSE(c.rotated);
    EXPECT_EQ(c.end, first.end);

    write(lines(5, 8) + "unfinish", true);
    c = lt::after(file_.string(), first.end, first.file_id, 5);
    ASSERT_EQ(c.lines.size(), 3u);
    EXPECT_EQ(c.lines.front(), "line 5");
    EXPECT_EQ(c.lines.back(), "line 7");
    EXPECT_EQ(c.end, lines(0, 8).size());

    // The partial line arrives whole once it is finished.
    write("ed\n", true);
    const auto next = lt::after(file_.string(), c.end, c.file_id, 5);
    ASSERT_EQ(next.lines.size(), 1u);
    EXPECT_EQ(next.lines[0], "unfinished");
}

TEST_F(LogTailTest, AfterIsCappedAndSaysThereIsMore) {
    write("start\n");
    const auto first = lt::tail(file_.string(), 1);
    write(lines(0, static_cast<int>(lt::kAfterCap) + 10), true);
    const auto c = lt::after(file_.string(), first.end, first.file_id, 5);
    EXPECT_EQ(c.lines.size(), lt::kAfterCap);
    EXPECT_TRUE(c.more);
    const auto rest = lt::after(file_.string(), c.end, c.file_id, 5);
    EXPECT_EQ(rest.lines.size(), 10u);
    EXPECT_FALSE(rest.more);
    EXPECT_EQ(rest.lines.back(), "line " + std::to_string(lt::kAfterCap + 9));
}

TEST_F(LogTailTest, AnotherFileIsARotationAndAnswersTheTail) {
    write(lines(0, 50));
    const auto first = lt::tail(file_.string(), 5);
    fs::rename(file_, dir_ / "hms-cpap.log.1");
    write(lines(100, 104));
    const auto c = lt::after(file_.string(), first.end, first.file_id, 2);
    EXPECT_TRUE(c.rotated);
    ASSERT_EQ(c.lines.size(), 2u);
    EXPECT_EQ(c.lines.back(), "line 103");
}

TEST_F(LogTailTest, AnOffsetPastTheEndIsARotationToo) {
    write(lines(0, 3));
    const auto first = lt::tail(file_.string(), 3);
    const auto c = lt::after(file_.string(), first.size + 100, first.file_id, 3);
    EXPECT_TRUE(c.rotated);
    EXPECT_EQ(c.lines.size(), 3u);
}

TEST_F(LogTailTest, BeforeAnswersTheLinesEndingAtTheOffset) {
    write(lines(0, 20));
    const auto last = lt::tail(file_.string(), 5);          // lines 15..19
    auto c = lt::before(file_.string(), last.start, 5);     // lines 10..14
    ASSERT_EQ(c.lines.size(), 5u);
    EXPECT_EQ(c.lines.front(), "line 10");
    EXPECT_EQ(c.lines.back(), "line 14");
    EXPECT_EQ(c.end, last.start);
    EXPECT_EQ(c.start, lines(0, 10).size());

    c = lt::before(file_.string(), c.start, 100);           // the rest: 0..9
    ASSERT_EQ(c.lines.size(), 10u);
    EXPECT_EQ(c.lines.front(), "line 0");
    EXPECT_EQ(c.start, 0u);                                 // nothing older is left

    EXPECT_TRUE(lt::before(file_.string(), 0, 5).lines.empty());
}

TEST_F(LogTailTest, AMissingFileIsNotReadable) {
    EXPECT_FALSE(lt::tail((dir_ / "none.log").string(), 5).readable);
    EXPECT_FALSE(lt::after((dir_ / "none.log").string(), 0, "", 5).readable);
    EXPECT_FALSE(lt::before((dir_ / "none.log").string(), 10, 5).readable);
    EXPECT_EQ(lt::fileId((dir_ / "none.log").string()), "");
}

TEST_F(LogTailTest, ReadsAcrossBlockBoundaries) {
    // Longer than one 64 KB read block, so the backward and forward scans both
    // cross block edges.
    write(lines(0, 20000));
    const auto c = lt::tail(file_.string(), 15000);
    ASSERT_EQ(c.lines.size(), 15000u);
    EXPECT_EQ(c.lines.front(), "line 5000");
    EXPECT_EQ(c.lines.back(), "line 19999");
    const auto b = lt::before(file_.string(), c.start, 6000);
    ASSERT_EQ(b.lines.size(), 5000u);
    EXPECT_EQ(b.lines.back(), "line 4999");
}
