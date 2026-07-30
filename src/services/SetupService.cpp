#include "services/SetupService.h"

#include "utils/AppConfig.h"

#include <cstdlib>
#include <filesystem>
#include <string>
#include <vector>

#ifdef _WIN32
  #include <windows.h>
#else
  #include <climits>
  #include <unistd.h>   // isatty/STDIN_FILENO on every POSIX, not just Linux
  #ifdef __APPLE__
    #include <mach-o/dyld.h>
  #endif
#endif

namespace hms_cpap {

namespace {

// Reported so the wizard can label the download the user is running and, more
// usefully, so a support ticket carries it without asking. Kept coarse on
// purpose: this is a label, not a feature test. Anything behaviour-changing
// belongs in Capabilities as its own flag.
const char* platformName() {
#if defined(_WIN32)
  #if defined(_M_ARM64)
    return "windows-arm64";
  #else
    return "windows-x64";
  #endif
#elif defined(__APPLE__)
  #if defined(__aarch64__) || defined(__arm64__)
    return "darwin-arm64";
  #else
    return "darwin-x64";
  #endif
#else
  #if defined(__aarch64__)
    return "linux-arm64";
  #else
    return "linux-x64";
  #endif
#endif
}

}  // namespace

SetupService::Capabilities SetupService::capabilities() {
    Capabilities c;
    c.version = HMS_CPAP_VERSION;

    // SQLite is unconditional: it is the embedded default and has no build flag.
    c.backends.emplace_back("sqlite");
#ifdef WITH_POSTGRESQL
    c.backends.emplace_back("postgresql");
#endif
#ifdef WITH_MYSQL
    c.backends.emplace_back("mysql");
#endif

    // gnuplot + libharu are wired only on POSIX (see main.cpp), so a Windows
    // build genuinely has no reports. Saying so lets the wizard stop implying a
    // feature that is not there.
#ifndef _WIN32
    c.pdf_reports = true;
#endif

    // Discovery ships whenever the binary does; it is not behind a build flag.
    c.mdns_discovery = true;

    c.platform = platformName();
    c.data_dir = AppConfig::dataDir();
    return c;
}

bool SetupService::supportsBackend(const std::string& type) {
    const auto caps = capabilities();
    for (const auto& b : caps.backends) {
        if (b == type) return true;
    }
    return false;
}

std::string SetupService::executablePath() {
#if defined(_WIN32)
    // MAX_PATH is not the ceiling on modern Windows; grow until it fits rather
    // than truncating a long install path into something that does not exist.
    std::vector<wchar_t> buf(MAX_PATH);
    for (;;) {
        const DWORD n = ::GetModuleFileNameW(nullptr, buf.data(),
                                             static_cast<DWORD>(buf.size()));
        if (n == 0) return {};
        if (n < buf.size() - 1) {
            return std::filesystem::path(std::wstring(buf.data(), n)).string();
        }
        if (buf.size() > 65536) return {};   // refuse to grow without bound
        buf.resize(buf.size() * 2);
    }
#elif defined(__APPLE__)
    // _NSGetExecutablePath reports the required size when the buffer is short,
    // and may hand back a path containing symlinks or "..", so canonicalise.
    uint32_t size = 0;
    _NSGetExecutablePath(nullptr, &size);
    std::vector<char> buf(size + 1, '\0');
    if (_NSGetExecutablePath(buf.data(), &size) != 0) return {};
    std::error_code ec;
    auto canonical = std::filesystem::weakly_canonical(
        std::filesystem::path(buf.data()), ec);
    return ec ? std::string(buf.data()) : canonical.string();
#else
    std::error_code ec;
    auto p = std::filesystem::read_symlink("/proc/self/exe", ec);
    if (ec) return {};
    return p.string();
#endif
}

std::string SetupService::executableDir() {
    const auto exe = executablePath();
    if (exe.empty()) return {};
    std::error_code ec;
    auto dir = std::filesystem::path(exe).parent_path();
    if (dir.empty()) return {};
    (void)ec;
    return dir.string();
}

std::string SetupService::resolveStaticDir(const std::string& configured) {
    // An explicit choice always wins. Empty and the historical default both
    // mean "the user never set this", since neither can be told apart from an
    // untouched config.
    if (!configured.empty() && configured != kLegacyStaticDir) return configured;

    const auto dir = executableDir();
    if (!dir.empty()) {
        std::error_code ec;
        auto candidate = std::filesystem::path(dir) / "static" / "browser";
        if (std::filesystem::exists(candidate, ec) && !ec) {
            return candidate.string();
        }
    }

    // Nothing next to the binary: fall back to the historical behaviour so a
    // developer running from the build directory is unaffected.
    return kLegacyStaticDir;
}

bool SetupService::shouldOpenBrowser(const LaunchContext& ctx) {
    if (ctx.setup_complete)  return false;   // already configured
    if (ctx.no_browser_flag) return false;   // explicitly declined
    if (ctx.supervised)      return false;   // the shell owns presentation
    if (!ctx.interactive)    return false;   // no human, no browser
    return true;
}

std::string SetupService::setupUrl(int web_port) {
    // localhost, not the configured bind address: 0.0.0.0 is not a thing a
    // browser can navigate to, and this URL is only ever opened on the machine
    // the service is running on.
    return "http://localhost:" + std::to_string(web_port) + "/setup";
}

bool SetupService::isInteractiveSession() {
#ifdef _WIN32
    // A service or a detached process has no console window.
    return ::GetConsoleWindow() != nullptr;
#else
    // stdin being a tty is the closest portable proxy. A Docker run without
    // -t, a systemd unit and a cron job all fail this, which is exactly the
    // set we want to skip.
    if (::isatty(STDIN_FILENO) == 1) return true;
    // A desktop session started from a launcher may have no tty but does have
    // a display, so treat that as interactive too.
    return std::getenv("DISPLAY") != nullptr ||
           std::getenv("WAYLAND_DISPLAY") != nullptr;
#endif
}

bool SetupService::openInBrowser(const std::string& url) {
    // The URL is built by setupUrl() from an int port, never from user input,
    // so there is nothing here to quote-escape. If that ever stops being true
    // this needs revisiting before it reaches a shell.
#if defined(_WIN32)
    const auto res = reinterpret_cast<INT_PTR>(
        ::ShellExecuteA(nullptr, "open", url.c_str(), nullptr, nullptr, SW_SHOWNORMAL));
    return res > 32;   // ShellExecute's documented success threshold
#elif defined(__APPLE__)
    return std::system(("open " + url + " >/dev/null 2>&1 &").c_str()) == 0;
#else
    return std::system(("xdg-open " + url + " >/dev/null 2>&1 &").c_str()) == 0;
#endif
}

}  // namespace hms_cpap
