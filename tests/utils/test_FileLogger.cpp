#include <gtest/gtest.h>

#include "utils/FileLogger.h"

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>

namespace fs = std::filesystem;
using hms_cpap::FileLogger;

// FileLogger writes a support log on EVERY platform now, not just Windows.
//
// It used to be Windows-only, on the reasoning that everywhere else the output
// already reaches a user through journald or a terminal. That is true of the
// machine and false of the person: a macOS or Raspberry Pi user asked to "send
// the log" has no more access to journalctl than a Windows user has to a
// console. So the file is written everywhere, and the two platforms get there
// differently:
//
//   Windows  freopen, REPLACING stdout/stderr. There is no console to keep --
//            the tray starts the service without one -- so a tee would buy
//            nothing and cost a pipe and a thread that can fail to join.
//   POSIX    a pipe + pump thread that TEES: the file gets a timestamped copy
//            and the original stdout still gets everything, so journald,
//            `docker logs` and a terminal all keep working.
//
// The POSIX half is covered below. The WINDOWS half still has no automated
// coverage here, because the Windows CI job builds with -DBUILD_TESTS=OFF and
// this binary is never produced there. That gap is real and worth naming: the
// Windows failures this class is known for -- a setvbuf call the UCRT treats as
// fatal, and a missing MSVC runtime that hung the installer -- were invisible to
// every test in this file and stayed invisible across a full rewrite of the
// mechanism. What caught them was running the shipped installer on a clean
// Windows 11 machine. Assume that is still the only thing that will.

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

// The platform decision itself.
//
// This used to assert the opposite: that start() DECLINED on POSIX, so that
// journald and the terminal could not be redirected out from under anyone. That
// was the right guard for a Windows-only feature and the wrong shape for what a
// user actually needs, which is one file in one known place on every platform we
// ship to -- a macOS or Raspberry Pi user has no more access to journalctl than a
// Windows user has to a console.
//
// So POSIX now TEES rather than declining or replacing: the file is written AND
// the original stdout still receives everything. Both halves are asserted here,
// because dropping either one is a regression -- no file means no support log,
// and no console means journald and `docker logs` go quiet.
TEST(FileLoggerTest, TeesOnPosixSoTheFileAndTheConsoleBothGetOutput) {
    const fs::path dir = tempDir("posix-tee");
    const fs::path log = dir / "hms-cpap.log";

    // Capture the real stdout BEFORE the logger dups it, so what the pump
    // writes back to the saved descriptor lands in the capture.
    testing::internal::CaptureStdout();

    ASSERT_TRUE(FileLogger::start(log.string(), 0, 0))
        << "FileLogger must engage on POSIX now, not just Windows";
    EXPECT_EQ(FileLogger::activePath(), log.string());

    std::cout << "tee-probe-line" << std::endl;

    // stop() joins the pump, so everything written above has reached both sides
    // by the time it returns.
    FileLogger::stop();
    const std::string console = testing::internal::GetCapturedStdout();

    EXPECT_NE(console.find("tee-probe-line"), std::string::npos)
        << "the console copy is gone: journald and `docker logs` would go quiet";

    ASSERT_TRUE(fs::exists(log)) << "no log file: nothing for a user to send";
    std::ifstream in(log);
    const std::string body((std::istreambuf_iterator<char>(in)),
                           std::istreambuf_iterator<char>());
    EXPECT_NE(body.find("tee-probe-line"), std::string::npos);
    // Lines are stamped in the file, and only in the file.
    EXPECT_NE(body.find("["), std::string::npos);
    EXPECT_EQ(console.find("["), std::string::npos)
        << "the console copy must look exactly as it always has";

    EXPECT_TRUE(FileLogger::activePath().empty());
}

// Rotation happens on the POSIX pump thread, between chunks, so it is only
// reachable through a real run. A support log that grows without bound is the
// thing rotation exists to prevent, and nothing else exercises rotateLive().
TEST(FileLoggerTest, RotatesOnceTheActiveFilePassesItsCap) {
    const fs::path dir = tempDir("posix-rotate");
    const fs::path log = dir / "hms-cpap.log";

    testing::internal::CaptureStdout();
    ASSERT_TRUE(FileLogger::start(log.string(), 4096, 2));   // 4 KB, keep 2
    for (int i = 0; i < 400; ++i)
        std::cout << "rotation-filler-line-" << i
                  << "-padding-padding-padding-padding" << std::endl;
    FileLogger::stop();
    testing::internal::GetCapturedStdout();

    EXPECT_TRUE(fs::exists(log)) << "rotation left no active log";
    EXPECT_TRUE(fs::exists(log.string() + ".1")) << "nothing was rotated out";
    // keep=2, so .3 must never appear.
    EXPECT_FALSE(fs::exists(log.string() + ".3"));
}

// spdlog brings its own "[YYYY-MM-DD HH:MM:SS.mmm]" prefix, and the MQTT module
// and BackfillService log through it. Stamping those a second time produced
// doubled timestamps in the support log -- caught in the macOS e2e, not by a
// test, which is why there is one now.
TEST(FileLoggerTest, DoesNotStampALineThatAlreadyCarriesOne) {
    const fs::path dir = tempDir("posix-stamp");
    const fs::path log = dir / "hms-cpap.log";

    testing::internal::CaptureStdout();
    ASSERT_TRUE(FileLogger::start(log.string(), 0, 0));
    std::cout << "[2026-08-13 20:35:17.304] [info] already stamped" << std::endl;
    std::cout << "plain line with no stamp" << std::endl;
    FileLogger::stop();
    testing::internal::GetCapturedStdout();

    std::ifstream in(log);
    std::string body((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());

    EXPECT_NE(body.find("[2026-08-13 20:35:17.304] [info] already stamped"), std::string::npos);
    EXPECT_EQ(body.find("] [2026-08-13 20:35:17.304]"), std::string::npos)
        << "the line was stamped twice";
    // The unstamped line still gets one, which is the reason to stamp at all.
    const auto plain = body.find("plain line with no stamp");
    ASSERT_NE(plain, std::string::npos);
    EXPECT_GT(plain, 0u);
    EXPECT_EQ(body[body.rfind('\n', plain) + 1], '[');
}

// stop() is called from main()'s exit path whether or not start() succeeded, so
// it has to be safe on a logger that never engaged.
TEST(FileLoggerTest, StopIsSafeWhenNothingWasStarted) {
    FileLogger::stop();
    FileLogger::stop();
    EXPECT_TRUE(FileLogger::activePath().empty());
}

#endif  // !_WIN32
