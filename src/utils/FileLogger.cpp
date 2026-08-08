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
    // a pump thread. That hung the Windows installer for five hours, aborted
    // --preflight, and stopped the tray from starting the service. There is no
    // console to preserve in the case this exists for.
    if (!std::freopen(path.c_str(), "a", stdout)) return false;
    if (!std::freopen(path.c_str(), "a", stderr)) return false;

    // Line buffered so a crash does not swallow the last thing said, which is
    // the part a support log exists for.
    std::setvbuf(stdout, nullptr, _IOLBF, 0);
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
