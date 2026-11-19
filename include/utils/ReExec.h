#pragma once

// The in-place restart (Settings > restart, the wizard's restart) without a
// supervisor.
//
// POSIX: execv replaces this image in the same process, so the listening
// sockets close with the old image and the new one binds them again.
//
// Windows has no such call. _execv STARTS a second process and then ends this
// one, so for a moment both exist and the new one's preflight finds the web
// port in use, refuses to start and exits: the restart left no process at all
// (every version since the preflight, 5.2.0; seen on 6.4.2 and 6.4.3,
// CpapDash-Win, 2026-11-18). So on Windows the new process is started with no
// handles inherited, told the old one's pid, and waits for that process to
// END (an event, not a timer) before it checks or binds anything.

#include <optional>
#include <string>

namespace hms_cpap::reexec {

/// The environment variable that carries the old process's pid to the new one.
inline constexpr const char* kParentEnv = "HMS_CPAP_REEXEC_PARENT";

/// The pid in `value`, or nullopt for null, empty or anything not all digits.
std::optional<unsigned long> parentPidFrom(const char* value);

/// Replace this process with a fresh `exe`. Returns only on failure.
/// POSIX: execv. Windows: start `exe` with no inherited handles and
/// kParentEnv set to this pid, then end this process at once.
bool replaceSelf(const std::string& exe);

/// Called first thing in main, before the preflight. On Windows, when started
/// by replaceSelf, waits until the old process has exited (its port is free
/// then) and clears kParentEnv. Does nothing otherwise, and on POSIX.
void waitForReplacedProcess();

}  // namespace hms_cpap::reexec
