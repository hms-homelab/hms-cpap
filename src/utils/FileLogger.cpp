#include "utils/FileLogger.h"

#include "utils/AppConfig.h"
#include "services/SetupService.h"

#include <chrono>
#include <cstdio>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#ifndef _WIN32
#include <unistd.h>
#endif

namespace fs = std::filesystem;

namespace hms_cpap {
namespace {

std::mutex  g_mutex;
bool        g_active = false;
std::string g_path;

#ifndef _WIN32
// POSIX tee state. Touched by start()/stop() under g_mutex; the file-side
// members below are then owned exclusively by the pump thread.
int         g_saved_stdout = -1;
int         g_saved_stderr = -1;
int         g_pipe_read    = -1;
int         g_pipe_write   = -1;
std::thread g_pump;

std::FILE*  g_file       = nullptr;
std::size_t g_written    = 0;
std::size_t g_max_bytes  = 0;
int         g_keep       = 0;
bool        g_line_start = true;

std::string stamp() {
    const auto now = std::chrono::system_clock::now();
    const auto t = std::chrono::system_clock::to_time_t(now);
    const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                        now.time_since_epoch()) % 1000;
    std::tm tm{};
    ::localtime_r(&t, &tm);
    char buf[32];
    std::snprintf(buf, sizeof(buf), "[%04d-%02d-%02d %02d:%02d:%02d.%03d] ",
                  tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday,
                  tm.tm_hour, tm.tm_min, tm.tm_sec,
                  static_cast<int>(ms.count()));
    return buf;
}

// Rotate the live file, from the pump thread, between chunks.
void rotateLive() {
    if (g_file) { std::fclose(g_file); g_file = nullptr; }

    std::error_code ec;
    if (g_keep > 0) {
        fs::remove(g_path + "." + std::to_string(g_keep), ec);
        for (int i = g_keep - 1; i >= 1; --i) {
            fs::rename(g_path + "." + std::to_string(i),
                       g_path + "." + std::to_string(i + 1), ec);
        }
        fs::rename(g_path, g_path + ".1", ec);
    } else {
        fs::remove(g_path, ec);
    }

    g_file = std::fopen(g_path.c_str(), "ab");
    g_written = 0;
    g_line_start = true;
}

// Does this line already begin with a "[YYYY-MM-DD " stamp of its own?
//
// spdlog writes its own timestamp, and the MQTT module and BackfillService log
// through it. Stamping those again produced lines reading
// "[2026-08-13 20:35:17.304] [2026-08-13 20:35:17.304] [info] ..." in the
// support log, which is noise in exactly the file someone is asked to read.
// std::cout lines have no stamp of their own and still get one.
bool alreadyStamped(const char* p, std::size_t avail) {
    if (avail < 12) return false;   // cannot tell yet; stamping is the safe guess
    if (p[0] != '[') return false;
    for (int i = 1; i <= 4; ++i)
        if (p[i] < '0' || p[i] > '9') return false;
    return p[5] == '-';
}

// Copy one chunk to the log file, prefixing a timestamp at each line start.
// The console copy is left untouched: it already looks the way it always has.
void writeToFile(const char* data, std::size_t n) {
    if (!g_file) return;

    std::size_t i = 0;
    while (i < n) {
        if (g_line_start && alreadyStamped(data + i, n - i)) {
            g_line_start = false;   // it brought its own
        }
        if (g_line_start) {
            const std::string s = stamp();
            std::fwrite(s.data(), 1, s.size(), g_file);
            g_written += s.size();
            g_line_start = false;
        }
        std::size_t j = i;
        while (j < n && data[j] != '\n') ++j;
        if (j < n) { ++j; g_line_start = true; }  // include the newline
        std::fwrite(data + i, 1, j - i, g_file);
        g_written += (j - i);
        i = j;
    }
    std::fflush(g_file);

    if (g_max_bytes > 0 && g_written >= g_max_bytes) rotateLive();
}

void pump() {
    std::vector<char> buf(8192);
    for (;;) {
        const auto n = ::read(g_pipe_read, buf.data(), buf.size());
        if (n <= 0) break;  // write end closed, or error

        // Console first: if the file write ever misbehaves, the terminal and
        // journald have already seen the line.
        if (g_saved_stdout >= 0) (void)::write(g_saved_stdout, buf.data(), n);
        writeToFile(buf.data(), static_cast<std::size_t>(n));
    }
    if (g_file) { std::fflush(g_file); std::fclose(g_file); g_file = nullptr; }
}
#endif  // !_WIN32

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
    std::lock_guard<std::mutex> lock(g_mutex);
    if (g_active) return true;
    if (path.empty()) return false;

    std::error_code ec;
    fs::create_directories(fs::path(path).parent_path(), ec);

#ifdef _WIN32
    rotateIfNeeded(path, max_bytes, keep);

    // WINDOWS: REPLACE the standard streams. There is no console to preserve —
    // the desktop tray starts the service without one, which is the entire
    // reason a file log exists on this platform — so a tee would buy nothing
    // and cost a pipe and a pump thread that can deadlock or refuse to join.
    //
    // It catches everything this program logs through, because all of it reaches
    // the stdout/stderr FILE*: std::cout and std::cerr, the MQTT module's
    // spdlog, and the web server's trantor.
    if (!std::freopen(path.c_str(), "a", stdout)) return false;
    if (!std::freopen(path.c_str(), "a", stderr)) return false;

    // UNBUFFERED, and _IONBF specifically, because the obvious alternative is
    // fatal here.
    //
    // `setvbuf(stdout, nullptr, _IOLBF, 0)` is fine on glibc, where a size of 0
    // means "pick one". The UCRT instead requires size >= 2 for _IOFBF/_IOLBF,
    // and a violation does not return an error: it calls the invalid parameter
    // handler, which ends the process through __fastfail(FAST_FAIL_INVALID_ARG).
    // No return value to check, no exception, no output, exit code 0xC0000409.
    // That killed hms_cpap.exe on every start except --preflight, and CI blamed
    // the tray. Reproduced on a clean Windows 11 machine (ucrtbase +0x1cba8,
    // fail code 5 = FAST_FAIL_INVALID_ARG).
    //
    // Nothing is lost: MSVC treats _IOLBF as _IOFBF, so stdout was never line
    // buffered on Windows anyway. _IONBF ignores size, which removes the trap.
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    std::setvbuf(stderr, nullptr, _IONBF, 0);

    g_path = path;
    g_active = true;
    return true;
#else
    // EVERYWHERE ELSE: TEE. The file has to exist here too — a user on macOS or
    // a Pi has no more shell access to journalctl than a Windows user has to a
    // console, and "send me your log" has to mean one file in one known place on
    // every platform we ship to. But replacing the streams the way Windows does
    // would take away the terminal when run by hand and journald under systemd,
    // which are real here and are not on Windows. So both: fd 1 and 2 go to a
    // pipe, and the pump writes every chunk to the original descriptor AND to
    // the file.
    std::FILE* f = std::fopen(path.c_str(), "ab");
    if (!f) return false;

    // Rotate before the pump owns the file, so start-up rotation stays plain
    // renames on a file nothing is holding open.
    std::fclose(f);
    rotateIfNeeded(path, max_bytes, keep);
    f = std::fopen(path.c_str(), "ab");
    if (!f) return false;

    int fds[2];
    if (::pipe(fds) != 0) { std::fclose(f); return false; }

    g_saved_stdout = ::dup(1);
    g_saved_stderr = ::dup(2);
    if (g_saved_stdout < 0 || g_saved_stderr < 0) {
        ::close(fds[0]); ::close(fds[1]); std::fclose(f);
        return false;
    }

    g_pipe_read  = fds[0];
    g_pipe_write = fds[1];
    g_file       = f;
    g_path       = path;
    g_max_bytes  = max_bytes;
    g_keep       = keep;
    g_written    = static_cast<std::size_t>(fs::file_size(path, ec));
    if (ec) g_written = 0;
    g_line_start = true;

    // Line buffering so a crash does not swallow the last thing said, which is
    // the part a support log exists for. Safe here: a size of 0 means "choose
    // one" on glibc and Apple libc. It is NOT safe on the UCRT, see above.
    std::setvbuf(stdout, nullptr, _IOLBF, 0);
    std::setvbuf(stderr, nullptr, _IONBF, 0);

    ::dup2(g_pipe_write, 1);
    ::dup2(g_pipe_write, 2);

    g_active = true;
    g_pump = std::thread(pump);
    return true;
#endif
}

void FileLogger::stop() {
    std::lock_guard<std::mutex> lock(g_mutex);
    if (!g_active) return;

    std::fflush(stdout);
    std::fflush(stderr);

#ifndef _WIN32
    // Order matters here, in two ways that are easy to get wrong.
    //
    // First, fd 1 and 2 are themselves copies of the pipe's write end, so the
    // pump only sees EOF once THOSE are closed too. Restoring them onto the
    // saved descriptors is what closes them, so it has to happen before the
    // explicit close below or the join never returns.
    //
    // Second, the pump writes the console copy to g_saved_stdout as it drains.
    // Closing that descriptor before the join throws away whatever was still in
    // flight, which is exactly the tail of the log a crash makes interesting.
    // So the saved descriptors stay open until the thread has finished.
    if (g_saved_stdout >= 0) ::dup2(g_saved_stdout, 1);
    if (g_saved_stderr >= 0) ::dup2(g_saved_stderr, 2);

    if (g_pipe_write >= 0) { ::close(g_pipe_write); g_pipe_write = -1; }
    if (g_pump.joinable()) g_pump.join();
    if (g_pipe_read >= 0) { ::close(g_pipe_read); g_pipe_read = -1; }

    if (g_saved_stdout >= 0) ::close(g_saved_stdout);
    if (g_saved_stderr >= 0) ::close(g_saved_stderr);
    g_saved_stdout = g_saved_stderr = -1;
#endif

    g_active = false;
    g_path.clear();
}

std::string FileLogger::activePath() {
    std::lock_guard<std::mutex> lock(g_mutex);
    return g_active ? g_path : std::string{};
}

}  // namespace hms_cpap
