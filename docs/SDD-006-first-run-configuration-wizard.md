# SDD-006: First-run configuration wizard

**Status:** Implemented (phases 1-4)
**Date:** 2026-07-29
**Repo:** `hms-cpap` (public, MIT)
**Version target:** 4.7.0, alongside SDD-005

## Trigger

A user who wants their CPAP data to stay on their own machine can already have
exactly that. What they cannot do is *set it up* without a text editor. Today
the path is: unzip, run the binary from a terminal, discover that
`~/.hms-cpap/config.json` was created with defaults, read
`config.json.example` to learn the key names, hand-edit JSON, restart.

Everything the wizard needs to configure is already implemented and already
reachable over the API. The gap is that nothing walks a person through it.

## Relationship to SDD-005

These two are complementary and must not overlap.

| | SDD-005 Desktop app | SDD-006 (this) |
|---|---|---|
| Owns | Getting the software onto the machine | Getting the software configured |
| Deliverables | Installer, `.app`/`.exe` shell, tray, signing, notarization, autostart | Wizard steps, DB provisioning, apply mechanism, setup gate |
| Runs | Once, at install | Once, at first launch |
| Also serves | Installed users | Installed users **and** zip users |

SDD-005 Phase 1 (LAN discovery, `POST /api/sync/now`, wizard step 2 rework)
**shipped in 4.6.3** and is a dependency of this SDD, not a duplicate of it.
This SDD takes that step 2 as given and builds the rest of the wizard around
it.

One genuine overlap needs resolving, and it is handled in Design §6: both
documents touch autostart.

## Principle

**The wizard configures; it does not implement.** Every setting it writes is
already honoured by the backend. If a step here requires new behaviour in
`BurstCollectorService`, the parsers, or the storage layer, it has scoped
wrong. The two exceptions are called out explicitly and are both plumbing:
database provisioning (§3) and the apply/restart mechanism (§5).

## What already works (do not rebuild)

Verified against 4.6.3 on 2026-07-29:

| Capability | Where | State |
|---|---|---|
| `/setup` route, 3-step wizard | `frontend/src/app/pages/setup/setup.component.ts` | shipping |
| M&M LAN discovery + pick | `DeviceDiscoveryService`, `GET /api/discover/devices` | shipping (4.6.3) |
| `setup_complete` flag persisted | `AppConfig`, `POST /api/setup` | shipping |
| Read whole config, secrets masked | `GET /api/config` → `AppConfig::toJson()` | shipping (fixed 4.6.3) |
| Write any config section | `PUT /api/config` | shipping (fixed 4.6.3) |
| ezShare reachability test | `GET /api/config/test-ezshare` | shipping |
| Config file is the source of truth, env is fallback | `AppConfig::applyEnvFallbacks()` | shipping |
| Partial hot-reload of a running collector | `BurstCollectorService::markConfigDirty()` | shipping |
| SQLite / PostgreSQL / MySQL all functional | all three backends, oximetry at parity | shipping (4.6.3) |
| Refuses to boot on a silently-downgraded backend | `main.cpp` startup guard | shipping (4.6.3) |

The 4.6.3 release matters here more than it looks. Before it, a wizard that
wrote `database.type = "mysql"` would have produced a system where the
collector wrote MySQL and every reader queried a SQLite file, and one that
enabled cloud sync would have destroyed the token on the next save. Both are
fixed. This SDD is only viable on top of that work.

## What is actually missing

1. **No database step.** The wizard never asks. Whatever `AppConfig` defaulted
   to is what you get, and changing it means editing JSON.
2. **No advanced options.** MQTT, LLM summaries, ML training, sleep staging,
   SleepHQ export, CpapDash cloud sync and the O2 Ring are all implemented and
   all invisible to a new user.
3. **Nothing forces you into the wizard.** `setup_complete` is written but
   never read by the frontend. A first-run user lands on an empty dashboard,
   not on `/setup`.
4. **Nothing opens a browser.** Double-clicking the binary prints a URL to a
   terminal the user may not be looking at.
5. **`static_dir` is CWD-relative** (`./static/browser`), so running the binary
   from `~/Downloads` serves no UI at all.
6. **The wizard cannot know what the binary supports.** `BUILD_WITH_MYSQL` is
   off by default and off in some builds; offering MySQL where it was not
   compiled in now produces a hard refusal to boot (4.6.3), which is correct
   behaviour and a terrible first-run experience.
7. **`archive_dir` has no config field.** It exists only as the
   `CPAP_ARCHIVE_DIR` environment variable, defaulting to `dataDir()`. An M&M
   user needs a folder for reconstructed files and cannot set one from the UI.

## Scope decisions (approved 2026-07-29)

- **Apply DB changes by self-restart.** Not a live DB swap. Reasoning in §5.
- **The wizard provisions databases**, issuing `CREATE DATABASE` with a second,
  never-persisted set of administrative credentials. Reasoning in §3.
- **Autostart is login-level only, never elevated.** No `sudo`, no UAC, no
  system service. Reasoning and the SDD-005 overlap in §6.
- **All three backends are offered**, gated by what the running binary actually
  compiled in (§2).
- **The wizard always restarts**, even for a SQLite-only run. A first launch has
  no session in flight, so there is nothing to preserve, and one code path is
  worth more than a saved two seconds.
- **Administrative DB credentials are never written to disk**, never returned by
  any endpoint, and are held only for the duration of one request.
- **Config and data stay at `~/.hms-cpap/`**, per SDD-005 decision 1.
- **Port stays 8893**, per SDD-005 decision 2, so the post-restart URL the
  wizard redirects to is the same one it was served from.
- **No new frontend test infrastructure.** The repo has no `.spec.ts` files;
  per SDD-005 this is not the document that introduces them. All new logic
  lands in a testable service instead (§8).

## Design

### 1. Wizard shape

Six steps. Everything except Database and Source is collapsed or skippable,
because the shortest correct path through this wizard should be four clicks.

```
1  Welcome            what this is, where data lives, "nothing leaves this computer"
2  Database           SQLite (default) | Advanced: PostgreSQL / MySQL
3  Source             M&M (discovery) | Folder | ezShare (advanced)
4  Advanced options   collapsed; every integration, all off by default
5  Start at login     one checkbox, per-OS installer underneath
6  Applying           writes config, restarts, polls /health, opens dashboard
```

Step 2, Database:

```
  (o) SQLite  (recommended)
      One file at ~/.hms-cpap/cpap.db. No server to install.

  ( ) Advanced  v
      Backend:  [ PostgreSQL v ]        <- only compiled-in backends listed
      Host:     [ localhost      ]  Port: [ 5432 ]
      Database: [ cpap_monitoring ]
      User:     [ cpap           ]  Password: [ ******** ]

      ( ) Use an existing, empty database
      ( ) Create it for me
          Admin user:     [ postgres ]
          Admin password: [ ******** ]
          Not saved. Used once to create the database, then discarded.

      [ Test connection ]   Connected. Server 16.2, database is empty.
```

Step 4, Advanced options, collapsed by default, each independently toggleable:

| Group | Writes |
|---|---|
| CpapDash cloud sync | `cpapdash.enabled`, `api_url`, `token`, `auto_sync` |
| Home Assistant / MQTT | `mqtt.*` |
| AI summaries | `llm.*` (provider, endpoint, model, key) |
| ML insights | `ml_training.*` |
| Sleep staging | `sleep_stage.*` |
| SleepHQ export | `sleephq.*` |
| O2 Ring oximetry | `o2ring.*` |
| Collection interval | `burst_interval` |

Every one of these already round-trips through `PUT /api/config` as of 4.6.3.
The wizard writes them; it does not interpret them.

### 2. `GET /api/capabilities`

New, unauthenticated, no side effects. The wizard must not offer a backend the
running binary cannot open, because since 4.6.3 that configuration is a
refuse-to-boot condition.

```json
{
  "version": "4.7.0",
  "backends": ["sqlite", "postgresql"],
  "features": { "pdf_reports": true, "mdns_discovery": true },
  "platform": "darwin-arm64",
  "data_dir": "/Users/x/.hms-cpap"
}
```

`backends` is compiled from `WITH_POSTGRESQL` / `WITH_MYSQL`. `pdf_reports` is
`false` on Windows, where the report stack is `#ifndef _WIN32`, so the wizard
stops implying a feature that is not there. `data_dir` lets the welcome step
name the real path instead of guessing.

### 3. Database provisioning

Two endpoints, both thin passthroughs to a testable service (§8).

**`POST /api/setup/test-db`** takes a candidate `database` block, attempts a
connection, and reports success plus whether the target already contains
`cpap_sessions`. Distinguishing "empty" from "already has data" is what lets
the UI say *"this database already has 412 sessions, they will be reused"*
rather than leaving the user to guess whether they are about to merge into
someone else's data.

**`POST /api/setup/create-db`** takes the same block plus
`admin_user` / `admin_password`, connects to the maintenance database
(`postgres`, or `mysql`), and runs the provisioning statements. Then it
reconnects using the *ordinary* credentials to prove the result is actually
usable, because a successful `CREATE DATABASE` followed by a failed `GRANT`
otherwise looks like success and fails at first boot.

Security properties, all deliberate:

- Administrative credentials are request-scoped. They are never written to
  `config.json`, never returned by any endpoint, never logged.
- Identifiers are validated against `^[A-Za-z_][A-Za-z0-9_]{0,62}$` before
  interpolation. `CREATE DATABASE` cannot take a bound parameter for its
  target name in either engine, so the guard has to be a whitelist rather
  than a placeholder. Passwords *are* bound.
- The endpoints refuse to run once `setup_complete` is true, so a finished
  install does not expose database provisioning on the LAN forever.

### 4. Source step and `archive_dir`

Step 2's source selection shipped in 4.6.3 and is reused unchanged. Two
additions:

- Choosing **M&M** now also requires a folder for reconstructed files, which
  means promoting `archive_dir` into `AppConfig` as a real field with an
  `applyEnvFallbacks()` bridge to the existing `CPAP_ARCHIVE_DIR`, so anyone
  currently setting the env var keeps working.
- Choosing **Folder** keeps the existing `local_dir` behaviour.

### 5. Applying the configuration

The wizard cannot hot-reload a database change, and pretending otherwise would
be the single biggest source of "it says configured but shows nothing" reports.

**Why.** `main.cpp` builds the `IDatabase` once at startup and hands it to five
separate consumers, and most of the config reaches services through
`portableSetenv()` before any of them are constructed. What
`markConfigDirty()` actually re-reads is the source, the ezShare URL, the
local directory and the burst interval. Everything else is frozen at boot:

| Setting | Live | Needs restart |
|---|---|---|
| `source`, `ezshare_url`, `local_dir`, `burst_interval` | yes | |
| `database.*` | | yes |
| `web_port` | | yes (listener already bound) |
| `mqtt.*`, `llm.*`, `agent.*` | | yes (env-bridged at startup) |
| `ml_training.*`, `sleep_stage.*`, `fysetc.*` | | yes (services constructed at startup) |
| `cpapdash.*` | | yes today, see below |

A true live swap means threading a swappable holder through the collector, the
web layer, reports, ML and backfill. That is weeks of work in the highest-risk
part of the codebase, to save a two-second restart on a screen the user sees
once. Rejected.

**Mechanism.** `POST /api/setup/apply` writes `config.json`, sets
`setup_complete`, responds `202 Accepted`, and only then triggers the restart,
so the response is not lost with the process.

Restarting has two cases, and the second is why this integrates with SDD-005
rather than duplicating it:

- **Supervised** (the SDD-005 shell set `HMS_CPAP_SUPERVISED=1`): exit `0`. The
  shell already spawns and restarts the child with backoff. Nothing new.
- **Standalone** (zip users): re-exec self via `execv` on POSIX, `_execv` on
  Windows, using the executable path resolved in §7.

The frontend then polls `GET /health` until it answers with `setup_complete`
true, and routes to the dashboard. Because the port is fixed by SDD-005
decision 2, that is the same origin the wizard is already loaded from. From
the user's side this is a spinner that says "Applying your settings" for about
two seconds, which is what "hot reload" meant in the original request.

**A related bug this exposes.** `CpapDashSyncService` receives its settings
once, in `main.cpp`. A token changed later through `PUT /api/config` updates
the file and the in-memory `AppConfig` but never reaches the running service.
The wizard's restart hides this, but the Settings page does not restart, so
this needs fixing in the same phase: `updateConfig()` should re-apply the sync
settings to the live instance.

### 6. Start at login, and the SDD-005 overlap

Both documents install autostart, and they must not fight.

**Resolution: the installer owns it when there is an installer.** SDD-005 §2
writes a LaunchAgent or a `HKCU\...\Run` key as part of installation, and the
shell supervises the binary. When `hms_cpap` sees `HMS_CPAP_SUPERVISED=1`, the
wizard's step 5 **reads that state and disables the checkbox** with the note
"managed by CpapDash Desktop", rather than installing a second, competing
autostart entry.

Step 5 does real work only for **zip users**, who have no shell and no
installer. For them:

| OS | Mechanism | Notes |
|---|---|---|
| macOS | `~/Library/LaunchAgents/com.hms.cpap.cli.plist`, `RunAtLoad` + `KeepAlive` | distinct label from SDD-005's, so both can coexist without one clobbering the other |
| Windows | `HKCU\Software\Microsoft\Windows\CurrentVersion\Run` | same key style as SDD-005, distinct value name |
| Linux | `~/.config/systemd/user/hms-cpap.service` + `loginctl enable-linger` | SDD-005 ships no Linux installer, so this is the only path Linux gets |

**These start at login, not at boot**, and the checkbox label says so. A user
who wants headless-on-boot needs the Docker image or a hand-written system
unit, and the wizard says that in one line rather than implying otherwise.
Nothing here ever prompts for elevation; that is decision-level, not an
implementation detail.

### 7. Setup gate, browser launch, and `static_dir`

Three small fixes that decide whether any of the above is reachable.

**Gate.** An Angular guard reads `GET /api/config`; when `setup_complete` is
false it redirects every route to `/setup`. Today the flag is written and never
read, so a first-run user sees an empty dashboard and no prompt.

**Browser launch.** On startup, when `setup_complete` is false and a
`--no-browser` flag was not passed, open `http://localhost:<web_port>/setup`:
`open` on macOS, `ShellExecuteW` on Windows, `xdg-open` on Linux. Suppressed
when supervised (the shell owns presentation) and when not attached to an
interactive session, so a Docker or systemd run never tries.

**`static_dir`.** Default becomes the executable's directory plus
`static/browser`, resolved via `_NSGetExecutablePath` / `GetModuleFileNameW` /
`/proc/self/exe`, with the current CWD-relative value kept as a fallback and an
explicit config value still winning. Without this, double-clicking a binary in
`~/Downloads` serves no UI, which makes every other item in this document
unreachable.

### 8. Testability

`CLAUDE.md`: the test binary links all of `src/` **except** `main.cpp`,
controllers, `web/` and the CLI. So none of this can be tested through its
controller, exactly as SDD-005 Phase 1 found.

Therefore a new `SetupService` in `src/services/` owns the logic, and
`CpapController` stays a passthrough:

- capability enumeration (pure, reads compile-time flags)
- identifier validation for provisioning
- provisioning statement construction per engine
- autostart file/registry content generation, with the filesystem and registry
  behind a seam so unit tests assert on generated content, never on a real
  LaunchAgent

The restart mechanism itself is not unit-testable and is covered by the manual
matrix.

## Decisions

| # | Question | Decision |
|---|---|---|
| 1 | Apply DB changes live or restart | Self-restart. A live swap is weeks of work in the riskiest code to save two seconds once. |
| 2 | Who restarts | Shell when supervised (exit 0), self re-exec when standalone. |
| 3 | Provision databases | Yes, `CREATE DATABASE` with request-scoped admin credentials, never persisted. |
| 4 | Which backends to offer | Only those in `GET /api/capabilities`. Offering an uncompiled one is now a refuse-to-boot config. |
| 5 | Autostart scope | Login-level only. Never elevated, never a system service. |
| 6 | Autostart ownership | Installer owns it when present; the wizard's checkbox disables itself under the shell. |
| 7 | Restart even for SQLite | Yes. One code path beats a saved two seconds on a first-run screen. |
| 8 | Provisioning endpoints after setup | Refuse once `setup_complete` is true. |
| 9 | Browser auto-open | Yes, first run only, suppressible with `--no-browser`, never when supervised or non-interactive. |
| 10 | Frontend tests | None. Logic moves to `SetupService` instead; no new test infrastructure. |

## Remaining risk

**Self re-exec on Windows** is the least-trodden piece. `_execv` replaces the
process image but the semantics around inherited handles and an already-bound
listening socket are less forgiving than POSIX `execv`. If it misbehaves, the
fallback is spawn-then-exit (start a detached copy, exit the original), which
costs a briefly-doubled process and needs the listener released first. This
needs a real Windows check before Phase 2 is called done, not a `lipo`-style
assumption that the file looks right.

Secondary: **`CREATE DATABASE` privileges vary**. A managed PostgreSQL where
the admin account cannot create roles will fail at the `GRANT`, which is
precisely why §3 verifies with the ordinary credentials afterwards instead of
trusting the first statement's success.

## Non-goals

- Any change to ingest, parsing, storage, or the dashboard.
- Installers, code signing, notarization, the tray shell. SDD-005 owns those.
- Cloud account creation or login. Cloud sync is a pasted token, as today.
- Multi-user or multi-machine configuration. One user, one machine, per SDD-005.
- Migrating an existing SQLite database into PostgreSQL or MySQL. Choosing a
  different backend starts empty; moving data between engines is its own SDD.
- A settings-page redesign. The wizard writes the same config the existing
  Settings page already edits.
- Reconfiguring a running install without restart. Out of scope by decision 1.

## Phases

### Phase 1: make the wizard reachable

`static_dir` relative to the executable, the Angular setup guard, browser
auto-open, and `GET /api/capabilities`. Small, and independently valuable: it
fixes "I double-clicked it and got a blank page" for everyone on the current
3-step wizard, before any new step exists.

Tests: `SetupService` capability enumeration; executable-path resolution per
platform.

### Phase 2: database step and apply/restart

`POST /api/setup/test-db`, `POST /api/setup/create-db`,
`POST /api/setup/apply`, the restart mechanism, the wizard's Database step, and
the `CpapDashSyncService` re-apply fix from §5.

Tests: identifier validation (accepts ordinary names, rejects quotes,
semicolons, dashes, leading digits, over-length); provisioning statements per
engine; refusal once `setup_complete`; test-db reporting empty versus populated.

### Phase 3: advanced options and `archive_dir`

The collapsed advanced groups, plus `archive_dir` promoted into `AppConfig`
with its env fallback.

Tests: `AppConfig` round-trip for `archive_dir` including the
`CPAP_ARCHIVE_DIR` fallback, extending the existing suite that already guards
every section against the 4.6.3 save/load drift.

### Phase 4: start at login

Per-OS autostart generation, the supervised-mode disable, and the
label/value-name separation from SDD-005.

Tests: generated plist, unit file and registry value content for each OS, with
the filesystem behind a seam.

## Test plan

Unit tests are enumerated per phase above. What no test binary can cover:

- **Regression before tagging.** Full suite green, not a filtered run.
- **Fresh-machine walkthrough**, no `~/.hms-cpap/`: double-click the binary, a
  browser opens on `/setup`, complete it with SQLite, land on a working
  dashboard. Then repeat choosing PostgreSQL with "create it for me", and
  again with MySQL, and confirm the ingested data lands in the chosen engine
  and not in a stray `cpap.db`. That last check is the 4.6.3 split-brain
  regression, and it is worth repeating from the UI.
- **Capability honesty.** A build with `-DBUILD_WITH_MYSQL=OFF` must not offer
  MySQL anywhere in the wizard.
- **Restart under both modes.** Standalone re-exec, and supervised exit-0 with
  the SDD-005 shell restarting the child.
- **Autostart matrix.** Survives a reboot on each OS; uninstalling the entry
  leaves `~/.hms-cpap/` untouched; installing under the shell does not create a
  competing entry.
- **Negative, by hand.** Wrong DB password, unreachable DB host, admin
  credentials lacking `CREATE DATABASE`, a target database that already holds
  sessions, port 8893 already bound, and a config directory that is read-only.

## Migration note for existing installs

Anyone upgrading has `setup_complete: true` already, so the gate never fires
and nothing changes for them.

One item does need saying in the 4.7.0 release notes rather than the wizard:
oximetry rows written before 4.6.3 carry timestamps shifted by the host's UTC
offset. The repair is re-uploading the original Wellue CSV or `.vld` files
through `/upload`, which upserts on filename. The wizard cannot detect or fix
this, and should not pretend to.

## References

- `docs/SDD-005-desktop-app.md` (installer, tray, signing, autostart ownership)
- `include/utils/AppConfig.h` (config schema, `applyEnvFallbacks`, `toJson`)
- `src/main.cpp` (startup env bridge, `makeDatabaseFromConfig`, the five DB
  consumers, backend guard)
- `src/controllers/CpapController.cpp` (`getConfig`, `updateConfig`,
  `setupComplete`, `discoverDevices`)
- `frontend/src/app/pages/setup/setup.component.ts` (current 3-step wizard)
- `include/database/DatabaseFactory.h` (backend selection)
- `CHANGELOG.md` 4.6.3 (the fixes this SDD depends on)
