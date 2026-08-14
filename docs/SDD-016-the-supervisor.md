# SDD-016: The supervisor

**Status:** Proposed
**Date:** 2026-08-14
**Repo:** `hms-cpap`, as its own project inside it
**Version:** target TBD (Albin's call)
**Supersedes parts of:** SDD-005 (desktop app) — see "What this reverses"
**Depends on:** SDD-006 (first-run wizard), SDD-012 (settings + restart)

## Trigger

hms_cpap is a long-running service that a non-technical person is expected to
own. Today what they can do about it depends entirely on which computer they
bought:

| | Start/stop | Autostart | Sees why it failed | Restart after a settings change |
|---|---|---|---|---|
| Windows | tray shell | Run key | yes, preflight report | via the web UI |
| macOS | nothing | nothing | nothing | via the web UI |
| Linux | `systemctl`, by hand | `systemctl enable` | `journalctl` | via the web UI |

A macOS user has no way to start it at all beyond running a binary from a
terminal, and a Linux user needs `systemctl`. The web UI can restart the
process, but only while the process is alive to answer the request — which is
exactly not the case when someone needs help most.

So: one supervisor, native Qt, on all three desktops, that owns the service's
lifecycle and can say what state it is in.

## What already exists, and must not be thrown away

**`desktop/windows/CpapDashDesktop/`, 673 lines of C#.** It is not a prototype;
it is the thing that survived three shipping Windows bugs. Two pieces of it are
hard-won and must survive into whatever replaces them:

- **`ChildJob.cs` — a Job Object with `KILL_ON_JOB_CLOSE`.** `Stop()` handles a
  clean quit and only runs if we get to run at all. Task Manager's "End task", a
  crash, or any `TerminateProcess` skips every finally block and destructor in
  the runtime. CI caught exactly that: force-killing the tray left hms_cpap
  alive holding port 8893, so the next launch failed preflight with a port
  conflict *we* had caused. The guarantee has to live in the kernel, not in our
  shutdown path. **Qt has no equivalent abstraction; this is per-platform code
  whatever language the UI is written in.**

- **`Supervisor.cs` is deliberately NOT a retry loop.** An earlier version
  restarted with escalating backoff and inferred the failure cause from how fast
  the child died, so a busy port was "discovered" by a timing heuristic and the
  user was shown a guess. Configuration errors are deterministic: a taken port,
  a wrong password and an unwritable folder do not become correct on the second
  attempt. The order is validate, then run, and if it exits, say why and stop.

**`hms_cpap --preflight`** is the validator, and it is deliberately one
implementation shared by the shell, the installer, systemd and launchd rather
than four drifting copies. The supervisor calls it; it does not re-implement it.

**`POST /api/config/restart`** (SDD-012) restarts the process from the web UI,
answering 202 *before* restarting so the response is not lost with the process.

## What this reverses, and what it keeps

SDD-005 decided two relevant things. This reverses **one** of them.

| SDD-005 said | Decided here (Albin, 2026-08-14) |
|---|---|
| Native minimal tray per OS: Swift + `NSStatusItem`, C# + `NotifyIcon` | **REVERSED. One Qt application, and it replaces the C# tray on Windows.** Three shells means three implementations of one state machine, and two of them were never written: macOS and Linux users got nothing. |
| Per-user agent, not a system service | **KEPT.** An earlier draft of this spec proposed real system services; walked back deliberately. |

Keeping the per-user agent is what preserves SDD-005's no-elevation promise: the
Windows installer is `PrivilegesRequired=lowest` on purpose, and registering with
the SCM would have reversed that for every user in order to serve the rare one
who logs out overnight. The cost is stated plainly: **the agent stops when the
user logs out.** That is the accepted trade.

Replacing the C# tray means the two hard-won guarantees above must be
re-implemented and re-proved in the Qt shell before it ships. They are not
optional and they are not portable: see Tests.

## What "reload" and "restart" mean here

These are not synonyms in hms_cpap and the supervisor must not present them as
one button:

- **Reload** — settings that hot-apply. The web UI writes `config.json` and the
  running process picks them up (`markConfigDirty` → `reloadConfig` on the burst
  worker). No downtime, no lost burst cycle.
- **Restart** — settings marked `restart` in the settings page (web port, data
  directory, database, logging). The process must come down and back up.

The supervisor owns restart. It does **not** own reload: reload is the running
process's own business and asking a supervisor to mediate it would add a way for
the two to disagree about the config on disk.

## Design

### The supervisor OWNS hms_cpap. The OS only owns the supervisor.

This is the load-bearing decision and it collapses most of the complexity.

If the supervisor is the front end for settings and can restart on demand, it
cannot be asking a service manager to do that on its behalf — it would be
guessing at state it does not control. So:

```
  login item  ->  SUPERVISOR  ->  spawns and owns  ->  hms_cpap
  (per OS)        (Qt, one impl)                       (child process)
```

The OS never knows hms_cpap exists. It starts **the supervisor** at login and
that is the whole of its involvement. What was three service backends becomes
one process-ownership implementation plus three small autostart entries:

| Platform | Autostart entry (starts the SUPERVISOR) |
|---|---|
| Windows | the `HKCU\...\Run` key the installer already writes |
| macOS | `~/Library/LaunchAgents/com.hms.cpap.plist`, `RunAtLoad` |
| Linux | a `systemctl --user` unit, `WantedBy=default.target` |

`IServiceController` shrinks accordingly — the OS-facing half is now only:

```cpp
class IAutostartController {
    virtual bool isEnabled() = 0;
    virtual bool setEnabled(bool, std::string& err) = 0;
};
```

...and everything else (status, start, stop, restart) is the supervisor's own
child handle, which is authoritative rather than inferred. `systemctl show` and
`QueryServiceStatus` disappear from the design, and with them the whole class of
bug where the service manager and the actual process disagree.

**Status still never comes from a port probe.** "Something answers on 8893" is
not "our child is running", and on a machine with a stale process those differ.
The child handle knows; the port does not.

### The child must die with the parent, on all three platforms

This is the guarantee `ChildJob.cs` provides today and it is not optional: CI
proved that force-killing the shell left hms_cpap alive holding port 8893, so
the next launch failed preflight with a conflict we had caused. Qt offers
nothing for this. Each platform needs its own mechanism:

| Platform | Mechanism | Notes |
|---|---|---|
| Windows | Job Object, `KILL_ON_JOB_CLOSE` | proven; port the existing C# logic |
| Linux | `prctl(PR_SET_PDEATHSIG, SIGTERM)` in the child after fork | kernel-enforced, same guarantee |
| macOS | **no kernel equivalent exists** | see below |

macOS has no `PDEATHSIG` and no job objects. The portable mechanism is a pipe:
the supervisor holds the write end, the child watches the read end, and when the
supervisor dies for any reason the pipe closes and the child sees EOF and exits.

**That requires a small change in hms_cpap itself** — a flag such as
`--exit-with-parent <fd>` and a watcher on it. It is a handful of lines, it is
the only cooperative piece, and it must be specified here rather than discovered
during the build. Without it, a force-killed supervisor on macOS orphans the
service exactly the way Windows used to.

### Health is separate from liveness

Two independent questions, shown separately, because conflating them is how a
user gets told the wrong thing:

- **Is the service running?** The supervisor knows: it is the parent, and the
  child handle is authoritative.
- **Is it healthy?** `GET /health` on the configured port, and `--preflight` for
  configuration validity before a start is attempted.

A supervisor that shows "running" while `/health` fails is more useful than one
that shows a single fused traffic light, because those two states have different
remedies.

### Privilege

**No elevation, anywhere, ever.** That is the point of staying per-user:

- **Linux** — `systemctl --user`, no privilege.
- **macOS** — a LaunchAgent in `~/Library/LaunchAgents`, no privilege.
- **Windows** — the Run key the installer already writes, no privilege.

A supervisor that prompts for a password to restart a service is a supervisor
people stop using. If a system-wide install is ever wanted, it is a separate
decision with its own spec, not a flag hidden in this one.

## The installer, and where configuration should happen

**What the installer does today.** 130 lines of Inno Setup, and there is no
configuration form in it at all:

1. **Copies files** — `CpapDashDesktop.exe`, `hms_cpap.exe`, the MSVC CRT DLLs,
   `config.example.json`, and `static/` for the web UI.
2. **Two checkboxes** — "Start when I log in" and "Create a desktop shortcut".
   That is the entire user input.
3. **Writes the Run key** for autostart, with `uninsdeletevalue` so an uninstall
   cannot leave one behind.
4. **Runs `hms_cpap --preflight`** at `ssPostInstall` and shows the report if it
   fails, through `SuppressibleMsgBox` — a plain `MsgBox` ignores
   `/SUPPRESSMSGBOXES` and hangs a silent install waiting for a click nobody is
   there to give.

It deliberately never touches `%USERPROFILE%\.hms-cpap`: that folder holds the
config and the entire therapy database, and an uninstaller that removes it turns
"I am reinstalling" into permanent loss of someone's medical record.

So today: copy, two checkboxes, autostart, validate. Configuration happens later,
in the web wizard (SDD-006).

### Keep Inno as the installer. Move FIRST-RUN configuration into Qt.

Two separate questions, and they get different answers.

**Do not rebuild the installer in Qt.** Qt Installer Framework would replace a
working, signed, per-user installer that just earned its CRT bundling the hard
way, and buy nothing a user can see. The installer's job is to put files on disk
and register autostart; it does that.

**Do move first-run configuration into the Qt supervisor**, because the current
arrangement has a chicken-and-egg in it. Configuring hms_cpap means opening the
web wizard, which means the service must be running and healthy enough to serve
it — which is exactly what it is not when the database credentials are wrong or
the port is taken. Today the installer's answer to that is a message box telling
the user to go fix something, with no way to fix it.

The supervisor is the natural place: it is already the thing that runs before the
service does, already runs `--preflight`, and already has to show the result.

### One binary, two faces

```
launch -> setup_complete false ? -> CONFIGURATOR window
                                     user picks source + database
                                     --preflight
                                     write config.json, start the service
                                     |
                                     v
                                    TRAY / menubar

launch -> setup_complete true  ? -> TRAY directly
                                    (window on click: status, start/stop/restart,
                                     log tail, "open settings" -> the web UI)
```

The same executable, not a separate configurator. One thing to build, sign,
notarize and put in the installer.

### Settings live in the tray, and the file stays single-writer

The supervisor has a Settings menu. A user changes a setting there, the service
restarts, done — nobody opens `config.json` in a text editor, on any platform.

The part that makes this safe is HOW it writes, and it is different depending on
whether the service is up:

| Service state | How settings are changed | Who writes `config.json` |
|---|---|---|
| Running | supervisor calls `PUT /api/config`, then restarts it | the **service** |
| Stopped (first run, or preflight failing) | supervisor writes the file directly | the supervisor |

So while the service is alive the supervisor is a CLIENT of the same endpoint
the web UI uses, not a second writer. The service remains the only thing that
touches the file, hot-reload keeps working for the settings that support it, and
there is no window in which two processes can disagree about what is on disk.
Only when the service is down — when there is no API to call and no web UI to
reach — does the supervisor write the file itself, and then by definition
nothing else can be writing.

**The boundary, stated once:**

> The supervisor writes `config.json` DIRECTLY only when the service is **not
> running and cannot be started**. While it is running, settings go through the
> service's own API.

Liveness, not setup state, and that distinction is load-bearing. The obvious
rule -- "only while `setup_complete` is false", mirroring the gate `setupApply`
already has -- builds a trap. Someone changes the web port in the web UI to one
that is taken and restarts: the service will not start, the web UI is
unreachable *because* it will not start, and a supervisor that is read-only
after setup has nothing to offer. That state is reachable through ordinary use,
and rescuing it is the entire reason this program exists.

Phrased around liveness it covers both cases and cannot race by construction:

There is no state where two processes write the file: while the service runs it
is the only writer and the supervisor asks it; while it is down there is nothing
else running to write.

It writes through the same `AppConfig` shape the service reads and validates by
calling `--preflight`, rather than re-implementing the checks.

What that buys: install finishes, the supervisor opens, the user picks a data
source and a database, preflight passes, the service starts. No browser needed
before there is anything to browse to -- and no dead end later when a setting
stops the service from coming back up.

## Decided

1. **Qt replaces the C# tray on Windows.** One shell everywhere.
2. **Per-user agent, not a system service.** See above.
3. **Ships inside the existing installer**, not as a separate download. One
   thing to sign, one thing to notarize, one thing a user installs.
4. **The installer stays Inno**; first-run configuration moves into the Qt
   supervisor.
5. **The tray has a Settings menu on every platform, with FULL PARITY** with the
   web settings page: source, database, MQTT, LLM, SleepHQ, oximetry, ML,
   reports, equipment, logging. The browser becomes optional. With the service
   up it edits through `PUT /api/config` and restarts; with it down it writes
   the file directly. The user never opens `config.json`, before or after setup.
6. **The supervisor OWNS the hms_cpap process.** The OS starts the supervisor at
   login and knows nothing else. Needs `--exit-with-parent` in hms_cpap for the
   macOS half of the die-with-parent guarantee.

## Tests

- `IServiceController` is an interface, so each backend gets a suite that runs
  against the real OS mechanism on that OS and skips cleanly elsewhere — the
  same shape as the DB engine suites, which is what caught the SQLite/MySQL
  stubs that had been returning nothing for months.
- **Status is never inferred:** a test that puts a foreign process on the
  configured port and asserts the supervisor still reports Stopped.
- **The die-with-parent guarantee, per platform**: force-kill the supervisor
  (`TerminateProcess`, `SIGKILL` — never a clean quit, which proves nothing),
  then assert no orphan holds the port. This is the CI test that caught the
  original Windows orphan; it must exist on Windows, Linux AND macOS, because
  each uses a different mechanism and macOS's is the one we invented.
- Preflight failure shows the failing check and its remedy, not "could not
  start".
- Restart is idempotent when already stopped, and reports the OS's own reason on
  failure rather than a guess.
- **The supervisor never writes the file while the service is running** — it
  must go through `PUT /api/config`. That is the whole guarantee against a
  second writer, so it is asserted directly.
- A setting changed through the tray survives the restart and is visible in the
  web UI, which is what proves the two front ends share one source of truth.
- **Recovery works after setup:** with `setup_complete` true and a port that is
  taken, the supervisor still lets the user fix it. That path is the reason the
  gate is liveness rather than setup state, so it gets a test.
- A first-run config that fails preflight shows the failing check and does NOT
  start the service, so the user fixes it where they set it.

## Deliberately out of scope

- **Reload.** The running process owns hot-apply; see above.
- **Being the only place settings can be changed.** The web UI keeps its
  settings page; the supervisor is a second front end to the same endpoint, not
  a replacement for it.
- **Log viewing beyond a tail.** The Logs page exists and has a copy button.
- **Cloud or account features.** This supervises a local process.
