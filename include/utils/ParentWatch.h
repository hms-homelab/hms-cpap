#pragma once

#include <functional>

namespace hms_cpap {

/**
 * SDD-016: exit when the supervisor that started us dies.
 *
 * The Windows tray already guarantees this with a Job Object marked
 * KILL_ON_JOB_CLOSE, and CI proved why it is not optional: force-killing the
 * shell left hms_cpap alive holding port 8893, so the NEXT launch failed
 * preflight with a conflict we had caused ourselves.
 *
 * The other two platforms have no equivalent. Linux has
 * prctl(PR_SET_PDEATHSIG), which the supervisor sets on its side. macOS has
 * neither job objects nor PDEATHSIG, and that gap is what this closes.
 *
 * The mechanism is a pipe. The supervisor holds the write end; we block reading
 * the read end. We never expect data -- the only event that matters is the pipe
 * closing, which happens when the supervisor exits for ANY reason including
 * SIGKILL, because the kernel closes its descriptors. EOF means the parent is
 * gone.
 *
 * The pipe is stdin, and the flag takes no argument. The SDD originally
 * specified `--exit-with-parent <fd>`, which is worse in three ways: it needs
 * fd-inheritance plumbing across exec that is fiddly to get right portably, it
 * only ever runs on macOS so it gets the least testing of any platform-specific
 * code we have, and it cannot be exercised on Linux where CI actually runs.
 * Reading stdin needs no plumbing (QProcess already gives the child a stdin
 * pipe), and it behaves identically everywhere, so the macOS-only mechanism is
 * the one that gets tested on every push.
 *
 * ONLY active when --exit-with-parent is passed. That matters: under systemd or
 * Docker stdin is /dev/null and reads EOF immediately, so a service that
 * watched it unconditionally would exit the instant it started.
 */

/**
 * Watch `fd` on a detached thread and call `on_parent_gone` when it reaches
 * EOF (or errors in a way that means the far end is gone).
 *
 * Returns immediately. Any data that does arrive is read and discarded -- the
 * contract is about the pipe closing, not about its contents, so a caller that
 * writes to it cannot accidentally kill the service.
 *
 * @param fd              descriptor to watch; 0 (stdin) in production
 * @param on_parent_gone  invoked once, on the watcher thread
 */
void watchParentPipe(int fd, std::function<void()> on_parent_gone);

}  // namespace hms_cpap
