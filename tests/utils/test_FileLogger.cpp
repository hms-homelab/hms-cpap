#include <gtest/gtest.h>

#include "utils/FileLogger.h"

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>

namespace fs = std::filesystem;
using hms_cpap::FileLogger;

// COVERAGE GAP, stated rather than hidden.
//
// FileLogger is deliberately Windows-only: everywhere else the output already
// reaches a user (journald under systemd, the terminal by hand), so redirecting
// it would take something away to solve a problem those platforms do not have.
//
// The consequence is that its actual behaviour — reopening stdout and stderr
// onto the log file — has NO automated coverage, because the Windows CI job
// builds with -DBUILD_TESTS=OFF and the test binary is never produced there.
// Guarding these tests with #ifdef _WIN32 would not help; they would simply run
// nowhere while looking like coverage.
//
// So what is left pins the PLATFORM DECISION, which is the part that can be
// checked here and the part that broke: an earlier version ran on every
// platform, hung the Windows installer, aborted --preflight, and stopped the
// tray from starting the service. Verifying the capture itself needs a Windows
// machine, not this suite.

namespace {

fs::path tempDir(const std::string& name) {
    const fs::path d = fs::temp_directory_path() / ("hms-filelogger-" + name);
    std::error_code ec;
    fs::remove_all(d, ec);
    fs::create_directories(d, ec);
    return d;
}

}  // namespace

TEST(FileLoggerTest, DefaultPathNamesALogFileInAnExistingDirectory) {
    const std::string p = FileLogger::defaultPath();
    ASSERT_FALSE(p.empty());
    EXPECT_NE(p.find("hms-cpap.log"), std::string::npos);
    // Whichever branch it took, the directory it chose must already exist.
    EXPECT_TRUE(fs::is_directory(fs::path(p).parent_path()));
}

#ifndef _WIN32

// The platform decision itself. If this ever starts passing a non-null logger on
// POSIX, journald and terminal output are about to be redirected out from under
// somebody, which is exactly the regression this file exists to prevent.
TEST(FileLoggerTest, IsInertOnPosixSoJournaldAndTerminalKeepTheirOutput) {
    const fs::path dir = tempDir("posix-inert");
    const fs::path log = dir / "hms-cpap.log";

    EXPECT_FALSE(FileLogger::start(log.string(), 0, 0))
        << "FileLogger must not engage off Windows";
    EXPECT_TRUE(FileLogger::activePath().empty());

    // And it must not have touched the streams on its way to declining.
    testing::internal::CaptureStdout();
    std::cout << "console-intact" << std::endl;
    EXPECT_NE(testing::internal::GetCapturedStdout().find("console-intact"),
              std::string::npos);

    EXPECT_FALSE(fs::exists(log)) << "declining to start must not create a file";
}

// stop() is called from main()'s exit path whether or not start() succeeded, so
// it has to be safe on a logger that never engaged.
TEST(FileLoggerTest, StopIsSafeWhenNothingWasStarted) {
    FileLogger::stop();
    FileLogger::stop();
    EXPECT_TRUE(FileLogger::activePath().empty());
}

#endif  // !_WIN32
