#include <gtest/gtest.h>

#include "utils/FileLogger.h"

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>

namespace fs = std::filesystem;
using hms_cpap::FileLogger;

namespace {

// Always stops the tee, including when an assertion fails mid-test. Leaving it
// running would send the rest of the suite's output into a temp file and make
// every later failure invisible.
struct TeeGuard {
    ~TeeGuard() { FileLogger::stop(); }
};

std::string readAll(const fs::path& p) {
    std::ifstream f(p, std::ios::binary);
    return std::string(std::istreambuf_iterator<char>(f), {});
}

fs::path tempDir(const std::string& name) {
    const fs::path d = fs::temp_directory_path() / ("hms-filelogger-" + name);
    std::error_code ec;
    fs::remove_all(d, ec);
    fs::create_directories(d, ec);
    return d;
}

}  // namespace

// The whole point of teeing at the file descriptor rather than swapping the
// std::cout streambuf: this program logs through std::cout, through C stdio
// (spdlog and trantor write to the stdout FILE* directly) and through stderr.
// A support log that quietly drops two of those three is worse than none.
TEST(FileLoggerTest, CapturesCoutCerrAndCStdioAlike) {
    const fs::path dir = tempDir("mixed");
    const fs::path log = dir / "hms-cpap.log";

    TeeGuard guard;
    ASSERT_TRUE(FileLogger::start(log.string(), 0, 0));

    std::cout << "via-cout" << std::endl;
    std::cerr << "via-cerr" << std::endl;
    std::printf("via-printf\n");
    std::fflush(stdout);
    std::fprintf(stderr, "via-fprintf\n");
    std::fflush(stderr);

    FileLogger::stop();

    const std::string body = readAll(log);
    EXPECT_NE(body.find("via-cout"), std::string::npos);
    EXPECT_NE(body.find("via-cerr"), std::string::npos);
    EXPECT_NE(body.find("via-printf"), std::string::npos);
    EXPECT_NE(body.find("via-fprintf"), std::string::npos);
}

// Support logs are read by a human hunting a moment in time, and most of this
// program's output carries no timestamp of its own.
TEST(FileLoggerTest, StampsEveryLine) {
    const fs::path dir = tempDir("stamp");
    const fs::path log = dir / "hms-cpap.log";

    TeeGuard guard;
    ASSERT_TRUE(FileLogger::start(log.string(), 0, 0));
    std::cout << "first" << std::endl;
    std::cout << "second" << std::endl;
    FileLogger::stop();

    std::ifstream f(log);
    std::string line;
    int stamped = 0, total = 0;
    while (std::getline(f, line)) {
        if (line.empty()) continue;
        ++total;
        // "[YYYY-MM-DD HH:MM:SS.mmm] "
        if (line.size() > 25 && line[0] == '[' && line[5] == '-' && line[24] == ']') ++stamped;
    }
    EXPECT_GT(total, 0);
    EXPECT_EQ(stamped, total);
}

// A service left running for weeks must not fill the disk, and the user still
// has to be able to send something bounded.
TEST(FileLoggerTest, RotatesPastTheSizeLimitAndKeepsTheOldOne) {
    const fs::path dir = tempDir("rotate");
    const fs::path log = dir / "hms-cpap.log";

    TeeGuard guard;
    ASSERT_TRUE(FileLogger::start(log.string(), 2048, 2));

    for (int i = 0; i < 400; ++i) {
        std::cout << "line " << i << " " << std::string(60, 'x') << std::endl;
    }
    FileLogger::stop();

    EXPECT_TRUE(fs::exists(log)) << "the active log should exist after rotating";
    EXPECT_TRUE(fs::exists(log.string() + ".1")) << "the previous log should be kept";
    // keep=2 means .3 must never appear.
    EXPECT_FALSE(fs::exists(log.string() + ".3"));
}

// Failing to open a log is never a reason to refuse to run, and it must not
// leave stdout wired to a half-built pipe.
TEST(FileLoggerTest, AnUnopenableFileIsNotFatal) {
    TeeGuard guard;
    const fs::path bad = fs::temp_directory_path() / "hms-filelogger-nope" / "x" / "";
    EXPECT_FALSE(FileLogger::start(bad.string(), 0, 0));
    EXPECT_TRUE(FileLogger::activePath().empty());

    // stdout still works; if the aborted start had left fd 1 on a dead pipe
    // this would go nowhere and the rest of the suite would print nothing.
    testing::internal::CaptureStdout();
    std::cout << "still-alive" << std::endl;
    EXPECT_NE(testing::internal::GetCapturedStdout().find("still-alive"),
              std::string::npos);
}

// After stop() the process must be back to plain console logging, otherwise a
// user who switches the log off in config keeps writing to a file they think
// is closed.
TEST(FileLoggerTest, StopEndsCapture) {
    const fs::path dir = tempDir("stop");
    const fs::path log = dir / "hms-cpap.log";

    TeeGuard guard;
    ASSERT_TRUE(FileLogger::start(log.string(), 0, 0));
    std::cout << "while-active" << std::endl;
    FileLogger::stop();

    EXPECT_TRUE(FileLogger::activePath().empty());
    std::cout << "after-stop" << std::endl;
    std::cout.flush();

    const std::string body = readAll(log);
    EXPECT_NE(body.find("while-active"), std::string::npos);
    EXPECT_EQ(body.find("after-stop"), std::string::npos);
}

TEST(FileLoggerTest, DefaultPathIsUsableAndNamesALogFile) {
    const std::string p = FileLogger::defaultPath();
    ASSERT_FALSE(p.empty());
    EXPECT_NE(p.find("hms-cpap.log"), std::string::npos);
    // Whichever branch it took, the directory it chose must already exist.
    EXPECT_TRUE(fs::is_directory(fs::path(p).parent_path()));
}
