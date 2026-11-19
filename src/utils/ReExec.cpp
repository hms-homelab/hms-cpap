#include "utils/ReExec.h"

#include <cstdio>
#include <cstdlib>
#include <iostream>

#ifdef _WIN32
#include <windows.h>
#else
#include <unistd.h>
#endif

namespace hms_cpap::reexec {

std::optional<unsigned long> parentPidFrom(const char* value) {
    if (!value || !*value) return std::nullopt;
    // 64-bit while counting: unsigned long is 32 bits on Windows, where a long
    // string would wrap round before the range check saw it.
    unsigned long long pid = 0;
    for (const char* c = value; *c; ++c) {
        if (*c < '0' || *c > '9') return std::nullopt;
        pid = pid * 10 + static_cast<unsigned long long>(*c - '0');
        if (pid > 0xFFFFFFFFull) return std::nullopt;
    }
    if (pid == 0) return std::nullopt;
    return static_cast<unsigned long>(pid);
}

bool replaceSelf(const std::string& exe) {
#ifdef _WIN32
    // The child inherits this environment block, so the variable reaches it.
    _putenv_s(kParentEnv, std::to_string(GetCurrentProcessId()).c_str());
    std::string cmd = "\"" + exe + "\"";
    STARTUPINFOA si{};
    si.cb = sizeof(si);
    PROCESS_INFORMATION pi{};
    // bInheritHandles FALSE: the new process must not hold a copy of this
    // one's listening sockets, or the port stays taken after we are gone.
    if (!CreateProcessA(exe.c_str(), cmd.data(), nullptr, nullptr, FALSE, 0, nullptr,
                        nullptr, &si, &pi)) {
        std::cerr << "Restart: could not start " << exe << " (error " << GetLastError()
                  << ")" << std::endl;
        _putenv_s(kParentEnv, "");
        return false;
    }
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    std::fflush(nullptr);
    // At once and without destructors, as _execv ended the process: the new
    // one is waiting for exactly this exit to free the port.
    _exit(0);
#else
    char* const argv[] = {const_cast<char*>(exe.c_str()), nullptr};
    ::execv(exe.c_str(), argv);
    return false;
#endif
}

void waitForReplacedProcess() {
#ifdef _WIN32
    const auto pid = parentPidFrom(std::getenv(kParentEnv));
    // Cleared whatever it held, so a later restart sets its own and nothing
    // this process starts inherits a stale one.
    _putenv_s(kParentEnv, "");
    if (!pid) return;
    HANDLE h = OpenProcess(SYNCHRONIZE, FALSE, static_cast<DWORD>(*pid));
    if (!h) return;   // already gone: nothing holds the port
    std::cout << "Restart: waiting for the previous process (" << *pid << ") to exit"
              << std::endl;
    WaitForSingleObject(h, INFINITE);
    CloseHandle(h);
#endif
}

}  // namespace hms_cpap::reexec
