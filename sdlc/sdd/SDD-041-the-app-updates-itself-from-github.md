# SDD-041: the app updates itself, from GitHub, through the supervisor

**Status:** Accepted 2026-09-18, all seven decisions (§4). D1 and D5 in Albin's
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
- **The supervisor owns the child's lifecycle** and, on Windows, a Job Object
  with `KILL_ON_JOB_CLOSE` so a kill cannot leave an orphan holding port 8893.
  SDD-016 is still Proposed: today that supervisor exists only as the Windows
  C# tray.
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

### 3.4 Who swaps the binary: the supervisor, never the service

The service cannot replace the file it is executing (on Windows it cannot even
be renamed while running), and it must not try. The sequence, owned by the
supervisor:

1. the service downloads and verifies (3.3), then asks the supervisor to apply;
2. the supervisor stops the child, the way it already knows how;
3. it moves the current binary to `<binary>.previous` and puts the new one in
   place, the shape `install.sh` already uses on the Pi;
4. it runs `hms_cpap --preflight`;
5. it starts the child and waits for `/health` to answer with the NEW version;
6. if any of 4, 5 or the health check fails, it restores `.previous`, starts it
   again, and reports the failure with the step that failed.

On a Pi with no supervisor running, step 2 to 5 is systemd's, driven by the same
code path in `install.sh` (D4).

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
SDD-016 (the supervisor) is still Proposed on macOS and Linux; the Windows tray
is the only supervisor that exists, so either this lands Windows-first or the
supervisor lands first (D4 decides how much of it this needs).
