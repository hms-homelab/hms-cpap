#pragma once

#include <cstddef>
#include <string>

namespace hms_cpap {

/**
 * Tees everything the process writes to stdout and stderr into a log file,
 * without taking it away from the console.
 *
 * Why a file descriptor tee rather than redirecting std::cout: this program
 * logs through three different mechanisms. Most of it is std::cout and
 * std::cerr, the MQTT module logs through spdlog, and the web server logs
 * through trantor. spdlog and trantor write to the stdout FILE* directly, so
 * swapping the std::cout streambuf would silently miss them and a support log
 * that is missing the web server's complaints is worse than no support log,
 * because it looks complete. Duplicating fd 1 and 2 catches all three, and
 * anything added later, at one place.
 *
 * Console output is preserved so that running the binary in a terminal still
 * shows output and, on the hub, journald still receives it.
 */
class FileLogger {
public:
    /**
     * Begin teeing. Safe to call when the file cannot be opened: it reports
     * false and leaves the process logging to the console exactly as before,
     * because failing to open a log is never a reason to refuse to run.
     *
     * @param path       file to append to; parent directories are created
     * @param max_bytes  rotate once the active file passes this size
     * @param keep       how many rotated files to retain (hms-cpap.log.1, .2 …)
     */
    static bool start(const std::string& path, std::size_t max_bytes, int keep);

    /** Restore the original stdout/stderr and close the file. Idempotent. */
    static void stop();

    /** Path currently being written to, or empty when inactive. */
    static std::string activePath();

    /**
     * Where the log goes when nothing is configured.
     *
     * Prefers the directory the executable lives in, so that a Windows user who
     * is asked for "the log next to the program" finds it exactly there. That
     * directory is not writable for a packaged Unix install (/usr/local/bin is
     * root-owned and the service does not run as root), so it falls back to the
     * data directory beside config.json, which is both writable and already the
     * folder the desktop tray opens.
     */
    static std::string defaultPath();
};

}  // namespace hms_cpap
