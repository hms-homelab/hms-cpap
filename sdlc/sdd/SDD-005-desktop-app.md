# SDD-005: Desktop app (installable local sync service for Windows and macOS)

**Status:** Proposed
**Date:** 2026-07-23
**Repo:** `hms-cpap` (public, MIT). No new repo.
**Version target:** 4.7.0

## Trigger

Users are asking for an all-local experience: their CPAP data stays on their
own machine and never reaches a cloud. `hms-cpap` already delivers exactly
that, but only to someone willing to unzip a release and run a binary from a
terminal. The product gap is distribution, not capability.

The pitch this enables: "install HMS-CPAP, it runs quietly in your menu bar,
your CPAP data syncs to your own computer, nothing leaves your house."

## Principle

**This SDD adds no data-path features.** Every byte of ingest, parsing,
storage and UI already ships. If a design step here requires changing
`EzShareClient`, `BurstCollectorService`, or the parser, it has scoped wrong.

## What already works (do not rebuild)

Verified against v4.6.2 on 2026-07-23:

| Capability | Where | State |
|---|---|---|
| SQLite as default DB, no server needed | `config.json.example` `database.type=sqlite` | shipping |
| First-run wizard, 3 steps | `frontend/src/app/pages/setup/setup.component.ts` | shipping |
| Full local UI on :8893 | dashboard, sessions, session-detail, compare, reports, equipment, upload, settings | shipping |
| Ingest from a mounted SD card or folder | `source=local`, `local_dir` | shipping |
| Ingest over ezShare-style HTTP | `EzShareClient.cpp`, `GET /dir?dir=A:DATALOG%5C<date>`, `GET /download?file=...` | shipping |
| Windows x64 + macOS arm64 builds | `.github/workflows/docker-build.yml` jobs `windows-build`, `macos-build` | shipping since v4.4.10 |

**The Mule and Miner is already reachable with zero new code.** The mule
serves `/dir` and `/download` on port 80 (`CONTROL_HTTP_PORT` is defined as
`PROXY_HTTP_PORT`, one always-on server) from `mule/main/ezshare_proxy.c`,
proxying over SPI to the miner. Those are byte-for-byte the paths
`EzShareClient` already requests. Setting `source=ezshare` and `ezshare_url`
to the mule's host makes an M&M work today, unmodified.

The mule also already advertises itself: `mdns_hostname_set("cpapdash")` plus
service `_cpapdash._tcp` on `CONTROL_HTTP_PORT`, carrying TXT records
`serial`, `fw`, and `mode` (`proxy` or `cloud`), added per-unit so several
units on one LAN can be told apart (`mule/main/wifi_manager.c:99-113`).

## What is actually missing

1. No installer. The user unzips and runs a binary.
2. No autostart. Nothing survives a reboot.
3. No signing. macOS Gatekeeper hard-blocks an unsigned binary from a zip;
   Windows SmartScreen shows a full-screen "unrecognized app" wall.
4. No visible presence. Nothing says it is running or lets a user force a sync.
5. No discovery. The wizard's ezShare field is a host box pre-filled with
   `http://192.168.4.1`. It works, but it cannot find an M&M for you.

Items 1 to 4 are packaging. Item 5 is the only new code in the sync path.

## Scope decisions (approved 2026-07-23)

- **Local only for v1.** No cloud pull. `CpapDashSyncService` stays as it is
  (equipment mirror, opt-in, push-up) and is not touched.
- **Windows and macOS both**, shipped together.
- **Same public MIT `hms-cpap` repo.** No branded fork.
- **Native minimal tray per OS.** Swift + `NSStatusItem`, C# + `NotifyIcon`.
  No Tauri, no Electron, no Rust in the release pipeline.
- **Per-user agent, not a system service.** LaunchAgent on macOS, per-user
  autostart on Windows.
- **Buy an OV certificate for Windows.** Developer ID plus notarization on
  macOS using the existing Apple Developer account.
- **v1 data sources: local folder, and Mule and Miner over the LAN.**
  Direct ezShare-card entry stays supported in config and settings, but is
  not promoted in the wizard.
- **One user, one machine, one M&M.** This is a single-machine ensemble for
  one person and their CPAP, not a fleet of IoT devices. No multi-unit
  ingest, no device inventory, no fleet concepts anywhere in this feature.
- **User-facing name is "CpapDash Desktop."** The repo, the license, the
  binary (`hms_cpap`) and the project identity stay HMS-CPAP / MIT. Only the
  shell's menu bar item, window title and About box carry the brand, so the
  desktop reads as one product family with CpapDash cloud and the phone app.
- **Config and data stay at `~/.hms-cpap/`** on both platforms. No move to
  platform-native directories, no migration.
- **Port conflict fails loudly.** No silent port hunting.
- **No self-update.** Notify only.

## Design

### 1. Install layout

Per-user install on both platforms, so neither installer needs elevation.

**macOS**, `HMS-CPAP.app` in `/Applications` (or `~/Applications`):

```
HMS-CPAP.app/Contents/
  MacOS/HMS-CPAP          menubar shell (Swift)
  Resources/hms_cpap      the existing binary, unchanged
  Resources/static/browser/   the Angular dist, unchanged
  Resources/config.example.json
```

**Windows**, `%LOCALAPPDATA%\Programs\HMS-CPAP\`:

```
HMS-CPAP.exe             tray shell (C#)
hms_cpap.exe             the existing binary, unchanged
static\browser\
config.example.json
```

Both layouts are the current release-zip layout with a shell binary added
alongside. The zips keep shipping unchanged for people who prefer them.

### 2. Autostart

**macOS:** `~/Library/LaunchAgents/com.hms.cpap.plist`, `RunAtLoad` plus
`KeepAlive`, launching the `.app` shell (not `hms_cpap` directly, so the
menu bar item is what gets supervised). Written on first launch, removed on
uninstall.

**Windows:** registry `HKCU\Software\Microsoft\Windows\CurrentVersion\Run`
entry pointing at the shell exe. Not Task Scheduler: the shell already
supervises the child process, so restart-on-failure buys nothing and a
scheduled task is harder to remove cleanly on uninstall.

Uninstall removes the LaunchAgent or the Run key. It never touches
`~/.hms-cpap/`.

The agent supervises the shell; the shell spawns and restarts `hms_cpap`.
`hms_cpap` itself gains no daemon logic and no new flags beyond what it has.

### 3. Tray / menubar shell

Deliberately dumb. It owns no state and parses no CPAP data.

```
  [icon] CpapDash Desktop
  ------------------------
  Status: Synced 4m ago
  Update available (4.8.0)      <- only when one exists
  Open Dashboard
  Sync Now
  ------------------------
  Start at Login   [x]
  Quit
```

- Spawns `hms_cpap` as a child, restarts it with backoff if it dies.
- Polls `GET /health` for liveness and version (already exists).
- "Open Dashboard" opens `http://localhost:8893` in the default browser.
- "Sync Now" calls `POST /api/sync/now` (new, see below).
- "Update available" appears only when a GitHub releases check finds a newer
  tag, and links to the release page. Nothing downloads or installs itself.
- Icon states: running, syncing, stopped, error.

**Port conflict is a hard failure, by decision.** If `hms_cpap` cannot bind
8893 it exits non-zero, and the shell surfaces a dialog. The dialog must be
actionable, not merely truthful:

```
  CpapDash Desktop could not start

  Port 8893 is already in use by another
  program on this computer.

  To use a different port, edit "web_port" in
  ~/.hms-cpap/config.json and start again.

     [ Open config folder ]      [ OK ]
```

### 3a. `POST /api/sync/now`

A dedicated route on `CpapController`, not a reuse of `/api/backfill`.
Backfill already means "go re-read historical data" in this codebase, and
overloading it would mislead the next reader. `sync/now` forces one burst
cycle immediately, returns whether a cycle was already running, and is a
no-op if the collector is mid-cycle. It is a trigger, not a new code path:
it wakes the existing loop.

New source lives in `desktop/macos/` and `desktop/windows/`, excluded from
the C++ build. Neither shell is required to run `hms_cpap`.

### 4. Discovery and the wizard

**Backend.** A new `include/services/DeviceDiscoveryService.h` performs a
one-shot mDNS browse for `_cpapdash._tcp`, returning instances with host,
port, and the `serial` / `fw` / `mode` TXT values. Exposed as
`GET /api/discover/devices` on `CpapController`, with a short timeout
(2 to 3 s) and no caching beyond the request.

Implementation: `mjansson/mdns`, compiled into the existing target, rather
than platform APIs (`dns_sd` on macOS, `DnsServiceBrowse` on Windows), which
would fork the code three ways and leave a Linux gap.

**License verified 2026-07-23:** the Unlicense. "This is free and unencumbered
software released into the public domain." No attribution requirement, no
copyleft, nothing that touches this repo's MIT terms or the OSCAR/GPL
constraint. Clean to vendor or depend on.

Vendored as a single self-contained header at `third_party/mdns/mdns.h`
(1621 lines, implementation inline), alongside its LICENSE. No `vcpkg.json`
entry: one public-domain header does not justify a manifest dependency across
three toolchains. On Windows it needs `ws2_32` and `iphlpapi` linked.

**The library owns the socket layer and the query encoding, and that division
is the point.** It binds 5353 with `SO_REUSEPORT`, joins 224.0.0.251, sets
`IP_MULTICAST_IF`, and picks a multicast question over a unicast one based on
the bound port. Most importantly it is built to be opened **once per
interface**, and that is what makes a scan reliable: a single `INADDR_ANY`
socket sends the query out whichever interface the kernel prefers, which on a
machine with Wi-Fi plus bridges plus VPN adapters is regularly not the one the
bridge is on. Measured: a hand-rolled single-socket version found the unit on
one scan and returned empty on the next while the unit was up and serving;
the per-interface version returned it 5 times out of 5.

**Record parsing stays ours**, in `DeviceDiscoveryService::parseResponse`.
The library parses inside `mdns_query_recv()`, which calls `recvfrom()`
itself and therefore cannot be driven by captured packets. Keeping our own
pure parser over a byte buffer is what makes the 19 offline fixture tests
possible, and CI never touches multicast.

Units reporting `mode=cloud` are listed but shown as not available for local
sync, since the proxy path is what serves `/dir`.

**Wizard.** `setup.component.ts` step 2 gains a first option:

```
  (o) CpapDash Mule and Miner      [Scan]
      Found CPD-0007  fw 4.1.3           [Use this]

  ( ) SD card or folder on this computer
      [ /Volumes/NO NAME/DATALOG      ] [Browse]

  ( ) ezShare card directly (advanced)
      [ http://192.168.4.1            ] [Test]
```

Choosing the discovered unit writes `source=ezshare` and
`ezshare_url=http://<host>` into config. That is the entire integration.
Manual entry remains for anyone whose network blocks multicast.

**One unit, by decision.** If a scan somehow returns more than one, the
wizard lists them so the user can pick the right one, and stores exactly
one. There is no multi-unit ingest, no saved device list, and no concept of
a fleet. A second M&M on the LAN is an edge case to disambiguate, not a
feature to support.

### 5. Signing

**macOS:** Developer ID Application certificate, `codesign` the shell, the
`hms_cpap` binary and the bundle, hardened runtime on, then `notarytool
submit --wait` and `stapler staple`. Ship a `.dmg`.

The bundle is **universal2** (arm64 + x86_64). The CPAP user base runs older
hardware, and an Intel Mac owner hitting "application cannot be opened" costs
more in support than the extra build minutes. This means building the C++
binary for both arches (Homebrew deps for both, then `lipo -create`) and
setting the Swift shell to both. This is the one place the build genuinely
gets harder, and it is the main risk in Phase 2.

**Windows:** OV code-signing certificate, `signtool` over the shell exe,
`hms_cpap.exe` and the installer. Installer is **Inno Setup**, per-user,
producing a clean Add/Remove Programs entry without the WiX toolchain.

Certificates live in GitHub Actions secrets. The release job signs; local
builds stay unsigned and are for development only.

### 6. CI

`windows-build` and `macos-build` each grow a packaging step after their
existing bundle step, producing an installer artifact alongside today's zip.
The `release` job (`docker-build.yml:403`) already downloads native artifacts
with `always()` so a failed native build does not block the release; the new
installers attach the same way. Signing steps are skipped when the secrets
are absent, so forks and PRs still build.

## Decisions

All resolved 2026-07-23. Recorded with the reasoning so they are not
re-litigated later.

| # | Question | Decision |
|---|---|---|
| 1 | Config and data directory | Keep `~/.hms-cpap/` on both platforms. No migration. |
| 2 | Port 8893 in use | Fail loudly with an actionable dialog. No port hunting. |
| 3 | macOS architecture | `universal2`, Intel and Apple Silicon. |
| 4 | Windows autostart | `HKCU\...\Run` key. |
| 5 | Sync Now | Dedicated `POST /api/sync/now`. |
| 6 | Multiple M&M units | One. Single-machine ensemble, not a fleet. |
| 7 | Uninstall | Never deletes data. Opt-in box, unchecked. |
| 8 | Self-update | Notify only, no auto-update. |
| 9 | Display name | "CpapDash Desktop" in the UI, HMS-CPAP stays the project. |
| 10 | Windows installer | Inno Setup, per-user. |

Two of these are worth spelling out, because the reasoning is not obvious
from the one-line summary.

**(2) The port failure is deliberate, and it is a trade.** Auto-picking a
free port would hide the problem from the user entirely, at the cost of the
dashboard URL no longer being a fixed, documentable `localhost:8893`. The
call went the other way: a stable, known port that a user can be told about
over email, and an explicit failure when something else has taken it. The
mitigation is that the dialog names the fix and offers to open the config
folder, rather than reporting a dead end. 8893 collisions are rare.

**(7) Uninstall never deletes sleep data.** Removing the binaries, the
LaunchAgent or Run key, and nothing else. The SQLite database and
`config.json` survive an uninstall and are found again on reinstall. The
Windows uninstaller may offer "also delete my sleep data" as an **unchecked**
box; macOS documents the path instead. Someone's therapy history is not
collateral of a reinstall.

## Remaining risk

The single largest unknown is the `universal2` build. Everything else in this
SDD is well-trodden. Building the C++ binary for x86_64 on a `macos-14`
runner means Homebrew dependencies for both architectures, and Drogon, paho,
libharu and jsoncpp all have to cooperate. If that fights back, the fallback
is arm64-only for v1 with Intel deferred, but that is a fallback and not the
plan, and it needs a call if it happens rather than a silent downgrade.

## Non-goals

- Any cloud communication. No pull, no push, no account, no login.
- Changes to `CpapDashSyncService`, `SleepHqExportService`, or MQTT.
- Any change to ingest, parsing, storage or the Angular UI beyond the
  wizard's step 2.
- Firmware changes in `cpapdash-push-c3`. None are needed.
- A Linux installer. Linux users have Docker and the existing build path.
- Mobile. The Flutter app is a separate product.

## Phases

Each phase is independently shippable. Phase 1 is the only phase that adds
C++, so it is the only phase that carries unit tests. Phases 2 to 4 are
packaging and are verified by the install matrix, not by a test binary.

### Phase 1: discovery and sync-now (the only new C++)

`DeviceDiscoveryService`, `GET /api/discover/devices`, `POST /api/sync/now`,
and the wizard step 2 rework. Ships inside the existing release zips and is
useful on its own, with or without any installer.

**Unit tests ship in the same PR, in `tests/services/`, per this repo's
convention that new service code comes with a suite.**

`tests/services/test_DeviceDiscoveryService.cpp`:

- mDNS response parsing from fixture byte buffers: well-formed PTR + SRV +
  TXT for one `_cpapdash._tcp` instance yields host, port, serial, fw, mode.
- Truncated packet, malformed length prefix, and a TXT record missing `mode`
  are each rejected without crashing or reading out of bounds.
- Compressed DNS name pointers resolve correctly, including a pointer loop,
  which must terminate rather than hang.
- A response for some other service type is ignored.
- Two responses for the same instance deduplicate to one entry.
- No responders yields an empty list and a success status, never an error.

`tests/services/test_SyncNow.cpp`:

- A trigger while no cycle is running requests one.
- A trigger while a cycle is already running reports that and does not queue
  or start a second cycle.
- Repeated triggers in quick succession collapse to one request.

**Design constraint that shapes both.** Per `CLAUDE.md`, the test binary
links all of `src/` **except** `main.cpp`, controllers, and `web/`. So
neither of these can be tested through its Drogon controller. The logic must
live in the service layer with the controller as a thin passthrough, and the
network must be injected, exactly as `CpapDashSyncService` injects its
`Transport` so its reconcile is testable with no network. `DeviceDiscovery`
takes a socket/transport seam; the fixtures are captured byte buffers, so
CI never touches multicast.

**No frontend tests.** The repo has zero `.spec.ts` files and no configured
frontend test infrastructure. The wizard step 2 change does not justify
standing that up, and this SDD is not the place to introduce it. The wizard
is covered by the manual integration check below.

**Integration, run by hand against a real mule on the LAN:** browse finds the
unit, TXT values match `devices.txt`, picking it writes `ezshare_url` into
config, and a burst cycle then pulls real files through `/dir` and
`/download`.

### Phase 2: macOS packaging

`.app` bundle, LaunchAgent, menubar shell, universal2, Developer ID signing,
notarization, `.dmg`. First because the signing identity already exists and
the test machine is on the desk. No unit tests: the shell is deliberately
dumb, and what could be tested (spawn, restart backoff, health poll) is
verified by the install matrix on real hardware, where it actually matters.

### Phase 3: Windows packaging

Inno Setup installer, Run-key autostart, tray shell, OV signing. Gated on the
certificate arriving, which is the only reason it is not Phase 2. Same
testing posture as Phase 2.

### Phase 4: CI

Both packaging jobs wired into the tagged release, signing secrets installed,
signing verified on a clean VM. The existing `release` job already tolerates
a failed native build via `always()`, so a signing misconfiguration degrades
to "no installer attached" rather than "no release."

## Test plan

Unit tests are enumerated per suite in **Phase 1** above, since that is the
only phase adding C++. What follows is the cross-phase verification that no
test binary can cover.

- **Regression, before anything is tagged.** The full suite green, not a
  filtered run. A filtered `--gtest_filter` pass is not a deploy gate in this
  repo.
- **Install matrix, clean VMs.** macOS on a machine that has never seen the
  app: install, no Gatekeeper prompt, survives reboot, tray reports running,
  uninstall leaves no LaunchAgent and leaves `~/.hms-cpap/` intact. Same on a
  clean Windows VM with no SmartScreen wall.
- **Architecture.** The `universal2` bundle verified on a real Intel Mac, not
  just `lipo -info`. Rosetta masks this class of failure, so the check is
  "does it launch and sync on Intel hardware", not "does the file contain two
  slices".
- **Reinstall.** Uninstall then reinstall finds the existing database and
  config, and the user's history is still there.
- **Negative, by hand.** Port already bound (the dialog appears and its
  "Open config folder" button works), no network, multicast blocked by the
  router, M&M powered off mid-sync, SD card yanked mid-read.

## References

- `mule/main/ezshare_proxy.c`, `mule/main/wifi_manager.c:99-113`
  (`cpapdash-push-c3`)
- `include/clients/EzShareClient.h:33-34`, `src/clients/EzShareClient.cpp`
- `frontend/src/app/pages/setup/setup.component.ts:320-355`
- `.github/workflows/docker-build.yml:202` (`windows-build`), `:294`
  (`macos-build`), `:403` (`release`)
