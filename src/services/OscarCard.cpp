#include "services/OscarCard.h"

#include <algorithm>
#include <cctype>
#include <cerrno>
#include <chrono>
#include <condition_variable>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <mutex>
#include <thread>

#ifdef _WIN32
#include <windows.h>
#else
#include <csignal>
#include <fcntl.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>
#endif

namespace hms_cpap::oscar {

namespace fs = std::filesystem;

namespace {

std::string upper(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(),
                   [](unsigned char c) { return static_cast<char>(std::toupper(c)); });
    return s;
}

std::string homeDir() {
#ifdef _WIN32
    const char* h = std::getenv("USERPROFILE");
#else
    const char* h = std::getenv("HOME");
#endif
    return h ? h : "";
}

constexpr size_t kOutputKept = 8192;

void keepTail(std::string& out, const char* buf, size_t n) {
    out.append(buf, n);
    if (out.size() > 2 * kOutputKept) out.erase(0, out.size() - kOutputKept);
}

}  // namespace

std::vector<std::string> programCandidates(const std::string& home) {
    std::vector<std::string> out;
#if defined(_WIN32)
    // The Win64 installer's folder; the portable zip is wherever the user put it.
    for (const char* var : {"ProgramFiles", "ProgramW6432", "ProgramFiles(x86)"}) {
        if (const char* pf = std::getenv(var)) out.push_back((fs::path(pf) / "OSCAR" / "OSCAR.exe").string());
    }
    if (const char* la = std::getenv("LOCALAPPDATA")) out.push_back((fs::path(la) / "OSCAR" / "OSCAR.exe").string());
#elif defined(__APPLE__)
    out.push_back("/Applications/OSCAR.app/Contents/MacOS/OSCAR");
    if (!home.empty()) out.push_back((fs::path(home) / "Applications/OSCAR.app/Contents/MacOS/OSCAR").string());
#else
    // The Debian and Ubuntu packages install /usr/bin/OSCAR20.
    out.push_back("/usr/bin/OSCAR20");
    out.push_back("/usr/local/bin/OSCAR20");
    out.push_back("/usr/bin/OSCAR");
#endif
    (void)home;
    // Unique, in order.
    std::vector<std::string> unique;
    for (auto& p : out)
        if (std::find(unique.begin(), unique.end(), p) == unique.end()) unique.push_back(std::move(p));
    return unique;
}

std::string findProgram() {
    std::error_code ec;
    for (const auto& p : programCandidates(homeDir()))
        if (fs::is_regular_file(p, ec)) return p;
    return "";
}

std::vector<std::string> importCommand(const Settings& s, const std::string& card_root) {
    return {s.program, "--datadir", s.data_dir, "--profile", s.profile, "--import", kLoader, card_root};
}

std::vector<std::string> helpCommand(const std::string& program) { return {program, "--help"}; }

int jobLimitSeconds(uint64_t card_bytes) {
    const unsigned long long mb = card_bytes / (1024ull * 1024ull) + 1;
    const unsigned long long nodata = std::min<unsigned long long>(std::max<unsigned long long>(120, mb * 43 / 10), 3600);
    return static_cast<int>(nodata + 600);
}

std::string cardRootOf(const std::string& card_dir) {
    const fs::path d(card_dir);
    const fs::path parent = d.parent_path();
    if (!parent.empty() && upper(parent.filename().string()) == "P-SERIES") return parent.parent_path().string();
    return card_dir;
}

uint64_t cardBytes(const std::string& card_root) {
    uint64_t total = 0;
    std::error_code ec;
    const fs::path ps = fs::path(card_root) / "P-SERIES";
    const fs::path base = fs::is_directory(ps, ec) ? ps : fs::path(card_root);
    for (fs::recursive_directory_iterator it(base, fs::directory_options::skip_permission_denied, ec), end;
         !ec && it != end; it.increment(ec)) {
        if (it->is_regular_file(ec)) total += it->file_size(ec);
    }
    return total;
}

bool isSessionClose(const std::string& rel) {
    const auto u = upper(rel);
    if (u.size() < 4 || u.compare(u.size() - 4, 4, ".B01") != 0) return false;
    return u.rfind("P-SERIES/", 0) == 0 || u.find("/P-SERIES/") != std::string::npos;
}

const char* outcomeName(Outcome o) {
    switch (o) {
        case Outcome::NotSetUp:     return "not_set_up";
        case Outcome::NoProgram:    return "no_program";
        case Outcome::NotSupported: return "not_supported";
        case Outcome::NoProfile:    return "no_profile";
        case Outcome::Busy:         return "busy";
        case Outcome::Timeout:      return "timeout";
        case Outcome::ReadNothing:  return "read_nothing";
        case Outcome::Failed:       return "failed";
        case Outcome::NothingNew:   return "nothing_new";
        case Outcome::Imported:     return "imported";
    }
    return "failed";
}

Outcome classifyExit(const ProcessResult& r, bool db_changed) {
    if (!r.started) return Outcome::NoProgram;
    if (r.timed_out) return Outcome::Timeout;
    if (r.exit_code == 75) return Outcome::Busy;
    if (r.exit_code == 0) return db_changed ? Outcome::Imported : Outcome::NothingNew;
    // Exit 1 that still stored something: a part of the card was read.
    if (r.exit_code == 1) return db_changed ? Outcome::Imported : Outcome::ReadNothing;
    return Outcome::Failed;
}

// ── the process ─────────────────────────────────────────────────────────────

#ifdef _WIN32

namespace {

std::wstring widen(const std::string& s) {
    if (s.empty()) return {};
    const int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), nullptr, 0);
    std::wstring w(static_cast<size_t>(n), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), w.data(), n);
    return w;
}

/// One argument quoted the way CommandLineToArgvW reads it back.
std::wstring quoteArg(const std::wstring& a) {
    if (!a.empty() && a.find_first_of(L" \t\"") == std::wstring::npos) return a;
    std::wstring out = L"\"";
    size_t backslashes = 0;
    for (wchar_t c : a) {
        if (c == L'\\') {
            ++backslashes;
            continue;
        }
        if (c == L'"') out.append(backslashes * 2 + 1, L'\\');
        else out.append(backslashes, L'\\');
        backslashes = 0;
        out.push_back(c);
    }
    out.append(backslashes * 2, L'\\');
    out.push_back(L'"');
    return out;
}

}  // namespace

ProcessResult SystemProcessRunner::run(const std::vector<std::string>& argv, int limit_s) {
    ProcessResult r;
    if (argv.empty()) {
        r.error = "no program";
        return r;
    }
    std::wstring cmd;
    for (const auto& a : argv) {
        if (!cmd.empty()) cmd += L' ';
        cmd += quoteArg(widen(a));
    }
    // This process's environment plus QT_QPA_PLATFORM=offscreen, for the child only.
    std::wstring env;
    if (wchar_t* block = GetEnvironmentStringsW()) {
        for (const wchar_t* p = block; *p; p += wcslen(p) + 1) {
            std::wstring kv(p);
            if (_wcsnicmp(kv.c_str(), L"QT_QPA_PLATFORM=", 16) == 0) continue;
            env += kv;
            env.push_back(L'\0');
        }
        FreeEnvironmentStringsW(block);
    }
    env += L"QT_QPA_PLATFORM=offscreen";
    env.push_back(L'\0');
    env.push_back(L'\0');

    SECURITY_ATTRIBUTES sa{sizeof(sa), nullptr, TRUE};
    HANDLE rd = nullptr, wr = nullptr;
    if (!CreatePipe(&rd, &wr, &sa, 0)) {
        r.error = "cannot make a pipe";
        return r;
    }
    SetHandleInformation(rd, HANDLE_FLAG_INHERIT, 0);
    STARTUPINFOW si{};
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESTDHANDLES | STARTF_USESHOWWINDOW;
    si.wShowWindow = SW_HIDE;
    si.hStdOutput = wr;
    si.hStdError = wr;
    si.hStdInput = GetStdHandle(STD_INPUT_HANDLE);
    PROCESS_INFORMATION pi{};
    const std::wstring exe = widen(argv[0]);
    if (!CreateProcessW(exe.c_str(), cmd.data(), nullptr, nullptr, TRUE,
                        CREATE_UNICODE_ENVIRONMENT | CREATE_NO_WINDOW, env.data(), nullptr, &si, &pi)) {
        r.error = "could not start " + argv[0] + " (error " + std::to_string(GetLastError()) + ")";
        CloseHandle(rd);
        CloseHandle(wr);
        return r;
    }
    CloseHandle(wr);
    r.started = true;
    std::string out;
    std::thread reader([&] {
        char buf[4096];
        DWORD n = 0;
        while (ReadFile(rd, buf, sizeof(buf), &n, nullptr) && n > 0) keepTail(out, buf, n);
    });
    const DWORD wait = WaitForSingleObject(pi.hProcess, static_cast<DWORD>(limit_s) * 1000u);
    if (wait == WAIT_TIMEOUT) {
        r.timed_out = true;
        TerminateProcess(pi.hProcess, 124);
        WaitForSingleObject(pi.hProcess, INFINITE);
    }
    DWORD code = 0;
    GetExitCodeProcess(pi.hProcess, &code);
    r.exit_code = static_cast<int>(code);
    reader.join();
    CloseHandle(rd);
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    r.output = out.size() > kOutputKept ? out.substr(out.size() - kOutputKept) : out;
    return r;
}

#else

ProcessResult SystemProcessRunner::run(const std::vector<std::string>& argv, int limit_s) {
    ProcessResult r;
    if (argv.empty()) {
        r.error = "no program";
        return r;
    }
    int fds[2];
    if (::pipe(fds) != 0) {
        r.error = "cannot make a pipe";
        return r;
    }
    // A pipe end the program must not inherit beyond its own stdout/stderr.
    ::fcntl(fds[0], F_SETFD, FD_CLOEXEC);
    std::vector<char*> args;
    for (const auto& a : argv) args.push_back(const_cast<char*>(a.c_str()));
    args.push_back(nullptr);

    const pid_t pid = ::fork();
    if (pid < 0) {
        ::close(fds[0]);
        ::close(fds[1]);
        r.error = "cannot start a process";
        return r;
    }
    if (pid == 0) {
        // The child: its own process group, so the limit ends whatever it starts.
        ::setpgid(0, 0);
        ::setenv("QT_QPA_PLATFORM", "offscreen", 1);
        ::dup2(fds[1], STDOUT_FILENO);
        ::dup2(fds[1], STDERR_FILENO);
        ::close(fds[1]);
        const int devnull = ::open("/dev/null", O_RDONLY);
        if (devnull >= 0) ::dup2(devnull, STDIN_FILENO);
        ::execv(args[0], args.data());
        _exit(127);
    }
    ::close(fds[1]);

    std::string out;
    std::thread reader([&] {
        char buf[4096];
        for (;;) {
            const ssize_t n = ::read(fds[0], buf, sizeof(buf));
            if (n > 0) keepTail(out, buf, static_cast<size_t>(n));
            else if (n == 0 || errno != EINTR) break;
        }
    });

    // The exit is the event; the limit is the job's, past which it is killed.
    std::mutex mu;
    std::condition_variable cv;
    bool exited = false;
    int status = 0;
    std::thread waiter([&] {
        int st = 0;
        while (::waitpid(pid, &st, 0) < 0 && errno == EINTR) {
        }
        std::lock_guard<std::mutex> lk(mu);
        status = st;
        exited = true;
        cv.notify_all();
    });
    {
        std::unique_lock<std::mutex> lk(mu);
        if (!cv.wait_for(lk, std::chrono::seconds(limit_s), [&] { return exited; })) {
            r.timed_out = true;
            ::kill(-pid, SIGKILL);
            ::kill(pid, SIGKILL);
            cv.wait(lk, [&] { return exited; });
        }
    }
    waiter.join();
    reader.join();
    ::close(fds[0]);

    if (WIFEXITED(status)) {
        r.exit_code = WEXITSTATUS(status);
        // execv failed in the child: the program is not runnable.
        r.started = !(r.exit_code == 127 && out.empty());
        if (!r.started) r.error = "could not run " + argv[0];
    } else {
        r.started = true;
        r.exit_code = WIFSIGNALED(status) ? 128 + WTERMSIG(status) : -1;
    }
    r.output = out.size() > kOutputKept ? out.substr(out.size() - kOutputKept) : out;
    return r;
}

#endif

}  // namespace hms_cpap::oscar
