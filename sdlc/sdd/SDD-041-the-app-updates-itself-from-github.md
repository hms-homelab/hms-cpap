# SDD-041: the app updates itself, from GitHub, through the supervisor

**Status:** Shipped in 5.4.0 (2026-09-18), with D8 to D10 and the as-built
notes in §7. Accepted 2026-09-18, all seven decisions (§4). D1 and D5 in Albin's
words ("yes to the automatic check with one click apply on the dashboard banner.
fully automatic as opt in as well with opt in off[.] docker and add on [does]nt
need update they have their own"); D2, D3, D4, D6 and D7 as proposed, chosen the
same day.
**Date:** 2026-09-18
**Repo:** `hms-cpap`. A new updater in the service, the swap in the supervisor,
one Settings section, one release-side manifest.
**Related:** SDD-005 (desktop app), SDD-006 (first-run wizard), SDD-012
(settings and restart), SDD-016 (the supervisor), SDD-025 (the Pi zip),
SDD-035 (the add-on installs by pulling)

## Trigger

A user runs whatever they installed, for ever. Both support cases this week
turned on which version was running: one rebuilt a container and kept an old
database, the other upgraded by hand between two builds and had to be talked
through it. The people this is for do not watch a releases page, and telling
them to "update first" is most of every support reply.

Albin, 2026-09-18: an auto updater, and it has to include the supervisor,
served from GitHub.

## 1. What is already here, and what it costs us

Verified in the tree rather than remembered:

- **Every tag publishes five assets** (`CpapDash.dmg`,
  `CpapDashDesktop-Setup.exe`, `hms-cpap-linux-armhf.zip`,
  `hms-cpap-macos-arm64.zip`, `hms-cpap-windows-x64.zip`) plus the ghcr images.
  So the artefacts an updater needs already exist; nothing about the release
  pipeline has to change to START.
- **`hms_cpap --preflight`** is the one validator the shell, the installer,
  systemd and launchd share (SDD-016). An update has to pass it before it is
  allowed to become the running binary.
- **`POST /api/config/restart`** answers 202 BEFORE restarting, so a response is
  not lost with the process (SDD-012). The same trick an update needs.
- **The Pi installer already keeps `<binary>.previous`** (`packaging/pi/install.sh`),
  which is the rollback primitive, and restarts through systemd.
- **The supervisor owns the child's lifecycle, and it is ONE app on both
  desktops.** The Qt tray in `desktop/qt` (`TrayShell`, `Supervisor`,
  `ChildProcess`, `Autostart`) ships as `CpapDash.dmg` on macOS and as
  `CpapDashDesktop.exe` inside `CpapDashDesktop-Setup.exe` on Windows. On Windows
  its `ChildProcess` holds a Job Object with `KILL_ON_JOB_CLOSE`, so a killed tray
  cannot leave an orphan on port 8893. The C# tray under
  `desktop/windows/CpapDashDesktop` is retired and nothing builds it. SDD-016's
  header still says Proposed; the code shipped, the status line did not follow
  it. The Pi has systemd.
- **Schema changes are idempotent `ALTER TABLE ... ADD COLUMN` at connect**, not
  gated on a stored schema version, so an older binary can usually open a newer
  database. The exceptions are the migrations that rewrite DATA (SDD-032's
  record-count repair, SDD-034's duplicate collapse). This is what makes
  rollback nearly free, and why "nearly" has to be written down.

## 2. Who this is for

Native installs: Windows, macOS, and Linux including the Pi.

**Docker and the Home Assistant add-on are out of scope** (D5). They update by
pulling an image, which is the platform's job: the add-on installs by pulling
(SDD-035) and Home Assistant already shows and applies add-on updates. An
updater that swapped a binary inside a container would be overwritten by the
next pull and would fight the thing that owns it. The service detects that it is
containerised and says so instead of offering an update.

## 3. Design

### 3.1 The check

Once a day, and on demand from Settings, the service asks the GitHub Releases
API for the latest non-prerelease of `hms-homelab/hms-cpap`, with the previous
ETag. Nothing is downloaded by a check, and a 304 costs one request.
Unauthenticated, because the repo is public and the budget (60 requests per
hour per address) is two orders of magnitude more than one call a day needs.

A check yields: the latest version, whether it is newer than `HMS_CPAP_VERSION`,
the asset for this platform, and its SHA-256.

### 3.2 Who decides to apply it (D1)

Proposed: **the user, in one click, with the check automatic.** The service
learns there is an update and says so in Settings and in the supervisor's menu;
applying it is a button. Fully automatic application is available as an opt-in
for someone who wants it (a Pi in a cupboard), off by default.

The reason for the default is the same one behind Push-C3's on-demand OTA: a
therapy device's data collector should not change underneath its owner while
they sleep. The reason for the opt-in is that some installs have nobody to
click.

### 3.3 What is downloaded, and what proves it

The platform's asset, to a temp file beside the install, then:

1. its size and **SHA-256 match the manifest** (3.7);
2. on macOS, `codesign --verify` against the Developer ID the release is signed
   with, and the notarisation ticket is stapled (both already true of
   `CpapDash.dmg`);
3. on Windows, the installer's Authenticode signature, **which does not exist
   today** and is a gap this SDD names rather than hides (D3).

A failure at any step leaves the running install untouched and logs which step
refused.

### 3.4 Who swaps the install: never the service

The service cannot replace the file it is executing (on Windows it cannot even
be renamed while running), and it must not try.

**What is replaced is the whole install, not one binary (D8).** Corrected
2026-09-18 while building it: on both desktops an install is the supervisor, the
service, the web UI and the Qt libraries together. On macOS they are one signed
`CpapDash.app`, and editing a file inside a signed bundle breaks its seal. On
Windows they are one Inno Setup install. So the unit of an update is the unit of
a release: the DMG on macOS, `CpapDashDesktop-Setup.exe` on Windows, and the zip
on the Pi. Supervisor, service and UI therefore always match.

**The update replaces the supervisor too, so a helper that outlives it runs the
swap (D9).** The sequence on a desktop:

1. the service downloads the platform's file into `<data_dir>/update/`,
   checks its size and SHA-256 against the manifest (3.3), writes
   `pending.json`, answers the request, and exits with the dedicated code
   **42**;
2. the supervisor sees 42, not a failure. It re-checks the SHA-256 itself,
   copies the helper script out of the install (the install is about to be
   replaced), starts it detached, and quits;
3. the helper waits for the supervisor and the service to be gone. It moves the
   current install to `<install>.previous` and installs the new one: on macOS
   the verified `CpapDash.app` from the DMG, whose signature is checked
   against team `9JYJU98VQ3`; on Windows `Setup.exe /VERYSILENT` into the same
   folder, which needs no UAC because the install is per-user;
4. it runs the new `hms_cpap --preflight`;
5. it launches the new supervisor and waits for `/health` to report the NEW
   version;
6. if 3, 4 or 5 fails, it restores `.previous`, launches that, and writes
   `<data_dir>/update/result.json` naming the step that failed. Settings shows
   that result on the next start.

**On the Pi, systemd is the supervisor and root is needed (D10).**
`install.sh` also installs `hms-cpap-update.path` and a root oneshot
`hms-cpap-update.service`. The service writes a request naming the version it
wants into `~/.hms-cpap/update/`. That folder is writable by the service user,
so the root unit trusts nothing in it except the version: it downloads that
release's `manifest.json` and zip itself, from GitHub over HTTPS, and checks the
zip against the manifest. It never executes anything from the zip as root. Then
it runs the same steps: stop the service, keep `hms_cpap.previous` and the old UI,
install, preflight as the service user, start, verify `/health`, and roll back on
failure.

### 3.5 What an update must never do silently

- Run while a night is being collected. The updater waits for the collector to
  be idle, the same condition SleepHQ's debounced export already waits for
  (SDD-003), or for the user to say "now".
- Cross a DATA migration without a database backup. The binary swap is
  reversible; SDD-032 and SDD-034's rewrites are not. Before applying, the
  service copies the SQLite file (or names the Postgres/MySQL database in the
  log and refuses to promise a rollback, D6).

### 3.6 Where the user sees it (D1)

**A banner on the dashboard**, because that is the page a user actually opens:
one line naming the new version and a button that applies it. It appears only
when there is an update, and it is dismissible for that version.

Settings carries the detail behind the same fact: running version, latest, the
release's own notes, the same button, and the opt-in switch for fully
automatic. The supervisor's menu shows the same line and the same button, since
it is the thing still running when the service is not.

One log line per check that changes anything, and nothing at all for "still
current".

### 3.7 The manifest, and why it is not just the release JSON

The release API gives names, sizes and URLs, but no checksum. So the release
workflow publishes one extra asset, `manifest.json`: version, and per platform
the asset name, size and SHA-256. It is the file the updater trusts, it is
produced by the same workflow that builds the assets, and it makes "what is the
latest version for my platform" one request instead of a heuristic over asset
names.

## 4. Decisions (Albin's)

- **D1, who applies an update? ACCEPTED 2026-09-18**: the check is automatic,
  applying is one click, and the click is **on the dashboard banner** (3.6), not
  buried in Settings. Fully automatic stays available as an opt-in, off by
  default.
- **D2, what cadence? ACCEPTED**: daily, plus a "check now" on demand. One
  request a day with an ETag, so an unchanged answer is a 304.
- **D3, Windows signing. ACCEPTED**: ship the Windows updater with checksum
  verification (size and SHA-256 from the manifest, over HTTPS), and treat
  Authenticode signing of the installer as its own piece of work. The installer
  is unsigned today, so this verifies integrity, not identity, and the SDD says
  so rather than implying more.
- **D4, the Pi and systemd. ACCEPTED**: the same flow, with systemd as the
  supervisor and `install.sh` as the step that swaps and restarts, so there is
  one implementation of "replace, preflight, restart, verify, roll back".
- **D5, Docker and the add-on. ACCEPTED 2026-09-18**: out of scope, detected and
  said so (2). Albin: "docker and add on [does]nt need update they have their
  own."
- **D6, databases that are not SQLite. ACCEPTED**: SQLite is backed up
  automatically before an update that runs a data migration. For PostgreSQL and
  MySQL the banner names the database that will be migrated and says the
  rollback of that data is the user's; the binary rollback still works either
  way.
- **D7, what "latest" means. ACCEPTED**: the newest non-prerelease on GitHub,
  stable only. No channel switch, so a pre-release tag never reaches someone who
  did not ask for it.
- **D8, what an update replaces. ACCEPTED 2026-09-18**: the whole install on
  macOS and Windows (the app bundle and the installer), never one binary
  inside it, so the signature stays whole and supervisor, service and UI match
  (3.4).
- **D9, who runs the swap. ACCEPTED 2026-09-18**: a detached helper script
  (bash on macOS, PowerShell on Windows), started by the supervisor when the
  service exits with code 42. The helper replaces, preflights, launches,
  verifies, and restores `.previous` on failure.
- **D10, root on the Pi. ACCEPTED 2026-09-18**: a systemd path unit and a root
  oneshot installed by `install.sh`. No sudo rights for the service user. The
  root unit fetches and verifies the release itself rather than trusting a file
  the service user could have written.

## 5. Tests

- The check: a newer version is offered, the same version is not, a
  pre-release is ignored, a 304 changes nothing, and a network failure is one
  quiet log line rather than a banner.
- The manifest: a mismatched SHA-256 refuses the download; a truncated download
  refuses; a manifest for another platform is not offered.
- The swap, on each platform: preflight failure, start failure, and a health
  check reporting the OLD version all restore `.previous` and leave a working
  install.
- Refusals: containerised (out of scope), a night in progress, a pending data
  migration on a server database when D6 says refuse.
- E2E, per platform: install version N, publish N+1 into a fake release
  endpoint, apply it, and confirm `/health` reports N+1 with the database
  intact; then force a failure at each of the three steps and confirm the
  rollback.

## 6. Release

Albin's number, and its own release: the updater changes what every future
release does to an install, so a regression in it must have one obvious suspect.

Every platform in scope already has the thing that does the swap: the Qt tray on
macOS and Windows, systemd on the Pi (D4). So nothing waits on a new supervisor,
and the desktop swap is written ONCE, in `desktop/qt`, with the per-platform
file handling behind it. The order is the manifest in the release workflow, then
the service's check and the dashboard banner (which already tell a user an
update exists), then the swap in the Qt supervisor and in `install.sh`.

## 7. As built (2026-09-18)

Where the build differs from, or adds to, the text above:

- **Idle, concretely (3.5).** "A night is being collected" is: the collector
  stored session data within the last 20 minutes
  (`BurstCollectorService::dataArrivedWithin`). That covers a live night and a
  first import. The banner then offers "Update anyway", the user's "now".
- **The service leaves through the orderly shutdown, never `std::exit` from a
  thread.** The first macOS run found `std::exit(42)` from the apply thread
  tearing Drogon down under its own loop. trantor aborted and the supervisor
  saw exit 1, not 42. `main` now returns the requested code after the normal
  shutdown. The same fault was already in the supervised "Restart now"
  (SDD-012): it died with 139, so the supervisor never restarted it. That path
  now uses the same shutdown and exits 0.
- **Three checks of the file.** The service checks size and SHA-256 against the
  manifest; the supervisor hashes it again before acting (the folder is
  user-writable); the helper checks it a third time. On macOS the helper
  also requires `codesign --verify --deep --strict` and team `9JYJU98VQ3`. No
  stapler check: `xcrun` is not on users' Macs.
- **`.previous` on success.** The desktop helpers delete it once the new version
  answers `/health`, so /Applications does not keep a second CpapDash. It is
  named `CpapDash.app.previous`, not `*.app`, so it is never launchable. The Pi
  keeps `hms_cpap.previous` and `static/browser.previous`, as `install.sh`
  always has. The SQLite backup (`update/backup-<from>.db`) is kept everywhere.
- **The Pi unit also sets `HMS_CPAP_SUPERVISED=1`**, so `HMS_CPAP_UPDATER=systemd`
  is checked first. Read the other way, a Pi would exit 42 for a helper it
  does not have, and systemd would restart it into the same download for ever.
- **The Pi's root script does every file operation in the user's home as that
  user**, so a symlink planted there cannot become a root write or delete. It
  runs nothing from the zip as root, and it does not run the new zip's
  `install.sh`, so a release that adds a runtime library fails preflight and
  rolls back rather than installing it. Such a release needs the manual
  `install.sh`.
- **Auto-update** is `auto_update` in config.json (off). When the collector is
  busy it retries every 30 minutes instead of the next day.
- **Settings shows the helper's `result.json`**: "updated to X", or "rolled back
  at step S: why".
