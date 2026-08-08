#include "utils/FileLogger.h"

#include "utils/AppConfig.h"
#include "services/SetupService.h"

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <string>

namespace fs = std::filesystem;

namespace hms_cpap {
namespace {

std::mutex  g_mutex;
bool        g_active = false;
std::string g_path;

// Rotate on the way in rather than while running.
//
// Checking the size once, before anything is open, keeps this to plain file
// renames. Rotating a live stream would mean reopening it underneath whatever
// is writing, and this class has already caused enough trouble by being clever.
void rotateIfNeeded(const std::string& path, std::size_t max_bytes, int keep) {
    if (max_bytes == 0) return;

    std::error_code ec;
    const auto size = fs::file_size(path, ec);
    if (ec || size < max_bytes) return;

    if (keep <= 0) {
        fs::remove(path, ec);
        return;
    }
    fs::remove(path + "." + std::to_string(keep), ec);
    for (int i = keep - 1; i >= 1; --i) {
        fs::rename(path + "." + std::to_string(i),
                   path + "." + std::to_string(i + 1), ec);
    }
    fs::rename(path, path + ".1", ec);
}

bool directoryIsWritable(const fs::path& dir) {
    std::error_code ec;
    if (!fs::is_directory(dir, ec)) return false;
    const fs::path probe = dir / ".hms-cpap-write-test";
    std::ofstream f(probe);
    if (!f.good()) return false;
    f.close();
    fs::remove(probe, ec);
    return true;
}

}  // namespace

std::string FileLogger::defaultPath() {
    const std::string exe = SetupService::executablePath();
    if (!exe.empty()) {
        const fs::path dir = fs::path(exe).parent_path();
        if (directoryIsWritable(dir)) return (dir / "hms-cpap.log").string();
    }
    return (fs::path(AppConfig::dataDir()) / "hms-cpap.log").string();
}

bool FileLogger::start(const std::string& path, std::size_t max_bytes, int keep) {
#ifndef _WIN32
    // WINDOWS ONLY, on purpose.
    //
    // Everywhere else the output already lands somewhere a user can reach:
    // journald under systemd, the terminal when run by hand. Redirecting there
    // would take that away to solve a problem those platforms do not have.
    // Windows is the gap: the desktop tray starts the service with no console,
    // so its output goes nowhere at all and a user asked for "the log" has
    // nothing to send.
    (void)path; (void)max_bytes; (void)keep;
    return false;
#else
    std::lock_guard<std::mutex> lock(g_mutex);
    if (g_active) return true;
    if (path.empty()) return false;

    std::error_code ec;
    fs::create_directories(fs::path(path).parent_path(), ec);
    rotateIfNeeded(path, max_bytes, keep);

    // Reopening the standard streams onto the file is the whole mechanism, and
    // deliberately the whole mechanism.
    //
    // It catches everything this program logs through, because all of it reaches
    // the stdout/stderr FILE*: std::cout and std::cerr, the MQTT module's
    // spdlog, and the web server's trantor. No pipe, no reader thread, no
    // descriptor juggling, so there is nothing that can deadlock, fail to exit,
    // or take the process down with it.
    //
    // The version this replaces preserved a console by teeing through a pipe and
    // a pump thread. It is gone because there is no console to preserve in the
    // case this exists for, and because a pipe plus a reader thread has failure
    // modes — deadlock, a thread that will not join — that a freopen does not.
    //
    // It is NOT gone because it caused the Windows failures, which is what an
    // earlier version of this comment claimed. Measuring them on a real machine
    // put the blame elsewhere: the installer hang was hms_cpap.exe missing the
    // MSVC runtime and putting up a modal box no silent install could click, and
    // the dead service was the setvbuf call below. Both survived the rewrite,
    // which is exactly why the rewrite alone fixed nothing.
    if (!std::freopen(path.c_str(), "a", stdout)) return false;
    if (!std::freopen(path.c_str(), "a", stderr)) return false;

    // UNBUFFERED, and _IONBF specifically, because the obvious alternative is
    // fatal here.
    //
    // The previous line read `setvbuf(stdout, nullptr, _IOLBF, 0)`. On glibc a
    // size of 0 means "pick a size yourself" and that is fine. The UCRT instead
    // requires size >= 2 whenever the mode is _IOFBF or _IOLBF, and a violation
    // does not return an error: it calls the invalid parameter handler, which
    // ends the process through __fastfail(FAST_FAIL_INVALID_ARG). No return
    // value to check, no exception to catch, no output, exit code 0xC0000409.
    //
    // That killed hms_cpap.exe on Windows every time this function ran, which is
    // every start except --preflight. The tray spawned the service, the service
    // died in milliseconds, and CI reported "the tray never started hms_cpap" —
    // a message that points at the shell, which was innocent. Reproduced on a
    // clean Windows 11 machine; WER named it exactly (ucrtbase +0x1cba8,
    // 0xC0000409, fail code 5 = FAST_FAIL_INVALID_ARG).
    //
    // Nothing is lost by dropping line buffering: MSVC treats _IOLBF as _IOFBF,
    // so stdout was never line buffered on Windows anyway. _IONBF ignores size,
    // which removes the trap, and unbuffered is a stronger version of what the
    // old comment was reaching for — a crash cannot swallow the last thing said
    // if there was never anything held back to swallow.
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    std::setvbuf(stderr, nullptr, _IONBF, 0);

    g_path = path;
    g_active = true;
    return true;
#endif
}

void FileLogger::stop() {
    std::lock_guard<std::mutex> lock(g_mutex);
    if (!g_active) return;
    std::fflush(stdout);
    std::fflush(stderr);
    g_active = false;
    g_path.clear();
}

std::string FileLogger::activePath() {
    std::lock_guard<std::mutex> lock(g_mutex);
    return g_active ? g_path : std::string{};
}

}  // namespace hms_cpap
