#include "utils/FileLogger.h"

#include "utils/AppConfig.h"
#include "services/SetupService.h"

#include <atomic>
#include <chrono>
#include <cstdio>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <thread>
#include <vector>

#ifdef _WIN32
#include <io.h>
#include <fcntl.h>
#define HMS_PIPE(fds)      ::_pipe((fds), 65536, _O_BINARY)
#define HMS_DUP(fd)        ::_dup(fd)
#define HMS_DUP2(a, b)     ::_dup2((a), (b))
#define HMS_READ(f, b, n)  ::_read((f), (b), static_cast<unsigned>(n))
#define HMS_WRITE(f, b, n) ::_write((f), (b), static_cast<unsigned>(n))
#define HMS_CLOSE(fd)      ::_close(fd)
#else
#include <unistd.h>
#define HMS_PIPE(fds)      ::pipe(fds)
#define HMS_DUP(fd)        ::dup(fd)
#define HMS_DUP2(a, b)     ::dup2((a), (b))
#define HMS_READ(f, b, n)  ::read((f), (b), (n))
#define HMS_WRITE(f, b, n) ::write((f), (b), (n))
#define HMS_CLOSE(fd)      ::close(fd)
#endif

namespace fs = std::filesystem;

namespace hms_cpap {
namespace {

std::mutex g_mutex;
bool        g_active = false;
std::string g_path;
int         g_saved_stdout = -1;
int         g_saved_stderr = -1;
int         g_pipe_read    = -1;
int         g_pipe_write   = -1;
std::thread g_pump;

// Written and read only by the pump thread.
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
#ifdef _WIN32
    ::localtime_s(&tm, &t);
#else
    ::localtime_r(&t, &tm);
#endif
    char buf[32];
    std::snprintf(buf, sizeof(buf), "[%04d-%02d-%02d %02d:%02d:%02d.%03d] ",
                  tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday,
                  tm.tm_hour, tm.tm_min, tm.tm_sec,
                  static_cast<int>(ms.count()));
    return buf;
}

void rotate() {
    if (g_file) { std::fclose(g_file); g_file = nullptr; }

    std::error_code ec;
    // Drop the oldest, then shift each one down: .2 -> .3, .1 -> .2, log -> .1
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

// Copy one chunk to the log file, prefixing a timestamp at each line start.
// The console copy is left untouched: it already looks the way it always has.
void writeToFile(const char* data, std::size_t n) {
    if (!g_file) return;

    std::size_t i = 0;
    while (i < n) {
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

    if (g_max_bytes > 0 && g_written >= g_max_bytes) rotate();
}

void pump() {
    std::vector<char> buf(8192);
    for (;;) {
        const auto n = HMS_READ(g_pipe_read, buf.data(), buf.size());
        if (n <= 0) break;  // write end closed, or error

        // Console first: if the file write ever misbehaves, the terminal and
        // journald have already seen the line.
        if (g_saved_stdout >= 0) HMS_WRITE(g_saved_stdout, buf.data(), n);
        writeToFile(buf.data(), static_cast<std::size_t>(n));
    }
    if (g_file) { std::fflush(g_file); std::fclose(g_file); g_file = nullptr; }
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

    std::FILE* f = std::fopen(path.c_str(), "ab");
    if (!f) return false;

    int fds[2];
    if (HMS_PIPE(fds) != 0) { std::fclose(f); return false; }

    g_saved_stdout = HMS_DUP(1);
    g_saved_stderr = HMS_DUP(2);
    if (g_saved_stdout < 0 || g_saved_stderr < 0) {
        HMS_CLOSE(fds[0]); HMS_CLOSE(fds[1]); std::fclose(f);
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
    // the part a support log exists for.
    std::setvbuf(stdout, nullptr, _IOLBF, 0);
    std::setvbuf(stderr, nullptr, _IONBF, 0);

    HMS_DUP2(g_pipe_write, 1);
    HMS_DUP2(g_pipe_write, 2);

    g_active = true;
    g_pump = std::thread(pump);
    return true;
}

void FileLogger::stop() {
    std::lock_guard<std::mutex> lock(g_mutex);
    if (!g_active) return;

    std::fflush(stdout);
    std::fflush(stderr);

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
    if (g_saved_stdout >= 0) HMS_DUP2(g_saved_stdout, 1);
    if (g_saved_stderr >= 0) HMS_DUP2(g_saved_stderr, 2);

    if (g_pipe_write >= 0) { HMS_CLOSE(g_pipe_write); g_pipe_write = -1; }
    if (g_pump.joinable()) g_pump.join();
    if (g_pipe_read >= 0) { HMS_CLOSE(g_pipe_read); g_pipe_read = -1; }

    if (g_saved_stdout >= 0) HMS_CLOSE(g_saved_stdout);
    if (g_saved_stderr >= 0) HMS_CLOSE(g_saved_stderr);
    g_saved_stdout = g_saved_stderr = -1;
    g_active = false;
    g_path.clear();
}

std::string FileLogger::activePath() {
    std::lock_guard<std::mutex> lock(g_mutex);
    return g_active ? g_path : std::string{};
}

}  // namespace hms_cpap
