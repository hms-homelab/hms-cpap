# Changelog

All notable changes to HMS-CPAP will be documented in this file.

The format is based on [Keep a Changelog](https://keepachangelog.com/en/1.0.0/),
and this project adheres to [Semantic Versioning](https://semver.org/spec/v2.0.0.html).

## [Unreleased]

## [4.9.4] - 2026-08-05: every setting, for every mode, in the Settings page

### Added
- **The Settings page now covers the whole configuration** (SDD-012). 26 of the
  63 settable keys had no field, so changing them meant editing
  `~/.hms-cpap/config.json` by hand. The one that hurt most was `archive_dir`:
  the Mule and Miner path requires it, and it was reachable only during the
  first-run wizard, so an onboarded user could not move where their nights land.
  It now appears for every source, alongside four sections that previously had
  no UI at all (Fysetc, Agent, Sleep Stage, CpapDash Cloud) and the loose keys
  `llm.max_tokens`, `llm.prompt_file`, `mqtt.client_id`, `sleephq.quiet_minutes`,
  `web_port` and `static_dir`.
- **Saved and "in effect" are now told apart.** `PUT /api/config` only ever
  re-applied the burst collector and the cloud mirror; everything else reached
  the file and never the running service, with no hint that a restart was owed.
  Fields that require one are marked, and a save that touches one raises a
  banner naming what changed, with a Restart now button. New endpoint
  `POST /api/config/restart` performs it, reusing the wizard's existing
  supervised/re-exec logic rather than a second copy. Where the process cannot
  restart itself, it says so instead of spinning.
- Changing the web port moves the open tab to the new port once the service
  answers there, rather than leaving a dead page behind.

### Fixed
- **A downloading source with no archive directory now says so** (ticket 67).
  `PreflightService::run()` only validated `archive_dir` when a value was
  already set, so the empty case, which is the one that breaks things, produced
  no check and no log line. `main.cpp` skips exporting `CPAP_ARCHIVE_DIR` for an
  empty value, so OSCAR archiving and SleepHQ export both switched themselves
  off while the dashboard kept filling in normally, which reads as data loss
  rather than a missing setting. Startup, `--preflight`, the desktop shell and
  the systemd `ExecStartPre` now all report it with the cause and the remedy,
  and the Settings field is marked required and explains itself when empty.
  Warning, not fatal: a bad path stops ingestion, never the dashboard that
  explains it. `local` and `lowenstein` read files in place and are exempt.
- **`archive_dir` changes now apply without a restart.** It is not part of the
  burst collector's config snapshot, so `markConfigDirty()` could not carry it;
  consumers read `CPAP_ARCHIVE_DIR` through ConfigManager, which `main.cpp` set
  once at startup. It is now re-exported on change and applies on the next burst.
- **`static_dir`, `mqtt.client_id` and `llm.prompt_file` are returned by
  `GET /api/config`.** `save()` had always written them but `toJson()` never
  emitted them. Harmless while nothing rendered them; once Settings did, they
  would have read back empty and saving the form would have overwritten real
  values.
- **Changing only the MQTT client ID now takes effect.** It was copied into the
  rebuilt client but left out of the comparison that decides whether to rebuild,
  so it did nothing until some other MQTT field also changed.
- **A database change no longer half-applies.** `reloadConfig()` swapped the
  burst collector's handle while `main.cpp` hands a separate connection to each
  other subsystem, none of which were rebuilt. Nights were written to the new
  database while the dashboard still read the old one, which looks like data
  loss rather than a misconfiguration. Every consumer now stays on the same
  database until the process restarts, which is what the Settings page says.

## [4.9.3] - 2026-08-05: the night that finally reaches the disk

### Fixed
- **A settled night now reaches the archive** (SDD-011, ticket 67). Skipping the
  download for a session whose files had stopped changing also skipped the
  archive step, because that step sits below the "nothing downloaded" early
  return. A session that went stable while still missing from disk could
  therefore never be archived, on any future burst: the only code that writes
  the archive was behind a branch that would never be taken for it again. The
  night stayed visible in the dashboard, because it was in SQLite, while OSCAR,
  the zip export and SleepHQ export all silently had nothing to read. The skip
  now requires the files to be present and non-empty under
  `<CPAP_ARCHIVE_DIR>/DATALOG/<date>`, and a settled night that is missing is
  fetched once to repair it. `captureCardResidue` (Identification.*, SETTINGS/,
  Journal.dat) was stranded by the same return and is fixed with it. Affects
  ezShare and Fysetc; local mode reads from disk and never archived.

## [4.9.2] - 2026-08-04: the card root, and local nights that can settle

### Changed
- **`local_dir` now means the SD card ROOT, not `DATALOG`** (SDD-010). `STR.edf`
  and `DATALOG/` are siblings at the root of a ResMed card, so that is the one
  layout the code depends on. STR is resolved at the root and nowhere else; the
  old fallback that searched inside `DATALOG` is gone, because ResMed never
  writes STR there and all it ever did was make a misconfigured path look like
  missing data. Applies everywhere, including `--reparse`.

  **This is a breaking configuration change.** A `local_dir` pointed at
  `DATALOG` is now refused rather than silently repaired: hms-cpap imports
  nothing, keeps serving the nights already in the database, and shows a red
  banner naming the folder to switch to. `--backfill` takes a path to a file and
  is unaffected. Lowenstein/Prisma trees are exempt, since they have no
  `DATALOG` at all.

### Added
- **Partial nights in local mode** (SDD-010, extending SDD-008). Local and SMB
  nights now join the folder ledger, so a night whose transfer settles without
  its `STR.edf` reports **Partial** instead of Complete-with-short-metrics, and
  its metrics and LLM summary are suppressed until the STR arrives. Previously
  `isNightPartial()` opted local mode out entirely on the assumption that a
  filesystem source cannot stall, which is untrue for amanuense: it streams from the
  CPAP into the share long after therapy ends. Lowenstein stays out of the
  ledger deliberately: it has no `STR.edf`, so an armed debt could never clear.
- `GET /api/capabilities` reports `config_error` (layout, path, problem, remedy)
  when the local folder is misconfigured, and the UI carries an app-wide banner.
- **The two most recent stored sessions are always re-checked**, on every source.
  Re-checking used to be anchored entirely on the current date: today's folder,
  or a session started within 48 hours. On a card that stops being written to, or
  an archive copied once off an old SD card, nothing matches either test, so no
  folder is ever observed a second time. Settling needs two observations of the
  same signature, so those nights sat at `Live` forever and could never resolve
  to Complete or Partial. Anchoring on what is persisted rather than on the clock
  is how hms-cpapdash keeps its dashboard populated regardless of data age. New
  `IDatabase::getNthLatestSessionStart()`; the change is additive, so the
  existing today/48h rules are untouched.

## [4.9.1] - 2026-08-04: the events table that cannot disappear

### Added
- **Events page** (SDD-009, support ticket 67). A searchable table of every
  respiratory event across all nights, read straight from `cpap_events`:
  filter by date range, event type, and minimum duration; rows link to the
  night's detail page. Unlike the dashboard's breakdown card this does not
  depend on `cpap_daily_summary`, so a missing STR cannot make it disappear.
  New endpoint: `GET /api/events`.

## [4.9.0] - 2026-08-04: O2-only nights are visible, and numeric dates land on the right month

### Added
- **Nights that exist only as an O2 recording now appear in the sessions
  list** (support ticket 67). A CSV upload for a night with no CPAP session
  used to import successfully and then have no UI surface at all: the
  sessions list is built from CPAP sessions and is the only route to the
  detail page, so the recording was invisible and the upload looked
  rejected. Those nights now get their own row, marked "O2 only" with SpO2
  and heart rate but no machine columns, and the detail page renders the
  oximetry charts without a CPAP session behind them.
- A golden-corpus convention for oximetry CSV exports
  (`tests/fixtures/oximetry_csv/README.md`): the Wellue apps have no stable
  export format, so real user files get pinned as fixtures instead of
  re-deriving the contract from memory each time it breaks.

### Fixed
- **Two more signals for ambiguous numeric CSV dates.** A US export where
  every date component is 12 or below and the filename stamp is gone parsed
  day-first, so `07/05/2026` filed under the 7th of May and July uploads
  looked like they were being refused (support ticket 67, follow-up to
  [issue #17](https://github.com/hms-homelab/hms-cpap/issues/17)). A
  midnight crossing inside the file now settles the order (only the day can
  tick up by one overnight), and failing that, a 12-hour AM/PM clock implies
  a US-style locale and therefore month-first. Files that resolve by the
  existing rules are unaffected.
- **Re-uploading a corrected O2 CSV now corrects the night on PostgreSQL.**
  The upsert kept the originally stored `start_time` (and `sample_interval`,
  `cpap_session_date`) on conflict, and 4.7.3 made `start_time` the source
  of the night assignment, so a misfiled recording could never be fixed by
  uploading again. SQLite and MySQL already updated those columns.
- **The dashboard and session-detail LIVE banners now follow the same
  night state as the sessions list.** SDD-008 tightened the list's live
  test to the transfer ledger but left both banners on the old
  open-session test, so they could keep blinking green after the list had
  moved on to Partial or Done.

## [4.8.2] - 2026-08-03: PostgreSQL can read its own data again

### Fixed
- **Every read the web layer makes was returning nothing on PostgreSQL, since
  4.6.3** ([issue #18](https://github.com/hms-homelab/hms-cpap/issues/18)).
  `IDatabase::executeQuery` is the one method on that interface with a default
  body rather than being pure virtual, and its default returns an empty array.
  4.6.3 switched every database handle in `main.cpp` from `PostgresDatabase`
  (which implements it) to the shared factory, which builds `DatabaseService`
  (which did not). So the collector kept writing correctly while every reader
  got `[]`, and nothing logged an error because nothing failed.
  - This was never a data problem. The rows were in PostgreSQL the whole time
    and are visible again on upgrade. There is nothing to re-import and no
    migration; if you deleted and re-added data trying to fix this, that was
    not the cause.
  - SQLite and MySQL were never affected. Both implement the method.
  - It is wider than the dashboard. On PostgreSQL this also silently emptied
    **PDF reports**, **CpapDash cloud sync**, **ML training data** and **live
    sleep staging**, all of which read through the same call. Worth re-checking
    anything you concluded from those between 4.6.3 and 4.8.1.
- **Wellue O2 Ring exports with numeric dates imported as "no O2 data found"**
  ([issue #17](https://github.com/hms-homelab/hms-cpap/issues/17)). The parser
  accepted only month-name timestamps (`06:53:07 Apr 12 2026`), but the Wellue
  app follows the phone's locale and also writes `21:56:34 02/08/2026`. Every
  row failed to parse, the session came out empty, and the upload was rejected
  as unreadable, for a file other tools accept.
  - Day-first vs month-first is resolved in the only order that can be right:
    a component above 12 settles it outright; otherwise the export filename's
    own timestamp is checked against the first row, since Wellue names the file
    after the first sample; failing both, day-first.

### Changed
- The oximetry `device_id` is now a single named constant instead of the same
  string literal written out at nine call sites in eight files. A mismatch
  between any writer and any reader would have produced an empty chart with
  nothing logged, which is the same silent-empty failure as #18. Deliberately
  NOT merged with the BLE name-match list (which matches what a ring
  advertises) or the `o2ring` key in config.json (a file-format contract).

## [4.8.1] - 2026-08-02: your configuration survives a restart, and the dashboard stops going blank

Both halves of [issue #16](https://github.com/hms-homelab/hms-cpap/issues/16),
where Docker setup was completed, data imported, and then the next start came
back to the setup wizard with an empty dashboard. They turned out to be two
unrelated bugs that happened to land on the same user.

### Fixed
- **`config.json` no longer lives in the container's writable layer.** It
  resolved under `$HOME`, which in an image is not a volume, so
  `docker compose down` destroyed it, along with `setup_complete` and every
  answer given to the setup wizard. The next start came up with defaults and
  demanded setup again. The image now keeps it in `/config` and declares that a
  volume, and `docker-compose.yml` mounts a named volume there.
  - The location is `HMS_CPAP_DATA_DIR` if set, otherwise `~/.hms-cpap` as
    before, so native installs are unaffected.
  - An existing `~/.hms-cpap/config.json` is migrated into the new location
    once, rather than being ignored in favour of a fresh one. Anyone who
    bind-mounted the home directory to work around this keeps their setup.
- **The dashboard is populated even when `STR.edf` is not reachable.**
  `cpap_daily_summary` is the only table the dashboard reads, and for ResMed the
  only thing that ever wrote it was `STR.edf`. Mount `DATALOG` instead of the SD
  card root that holds `STR.edf` beside it, an easy and reasonable mistake,
  and every session parsed, saved and listed correctly while the dashboard
  stayed empty. That reads as "it imported my data and then lost it".
  - When STR is unavailable the summary is now derived from the sessions
    themselves, via the aggregation that Lowenstein devices (which have no STR
    at all) already used.
  - STR remains preferred and upserts over the derived rows the moment it
    appears: it carries the machine's own nightly aggregates (mask on/off
    pairs, leak percentiles, therapy mode) that sessions alone cannot supply.
  - The same gap existed in the backfill, which logged "dashboard will be empty"
    and finished reporting success.
  - Both log lines now say what is being done about it, and name the SD card
    root as the mount that gives full detail.

## [4.8.0] - 2026-07-31: a desktop app for Windows, and configuration that fails honestly

### Added
- **CpapDash Desktop for Windows (SDD-005 phase 3).** An installer and a tray
  icon, so a private local install no longer means a zip and a terminal. The
  shell supervises `hms_cpap`, opens the dashboard, triggers a sync, and offers
  Start at Login. Per-user install, so nothing prompts for elevation.
  - The child runs inside a Windows **Job Object**, so it cannot outlive the
    tray even when the tray is force-killed. Without that, an orphan holds port
    8893 and the NEXT launch fails blaming a conflict we caused ourselves.
  - Uninstalling removes the program and the Run key and **never touches
    `~/.hms-cpap`**, which is the user's entire therapy history.
- **Start automatically, on every platform.** A LaunchAgent or LaunchDaemon on
  macOS, systemd user and system units on Linux, and the Run key on Windows.
  The boot services pin both the account and `HOME`: without that a daemon runs
  as root, resolves `~/.hms-cpap` somewhere the user cannot see, and builds a
  second empty database that looks exactly like data loss.
- **`hms_cpap --preflight`.** Validates the data directory, the web port, the
  database credentials and the source folder, and reports what is wrong AND what
  to change. Startup refuses to boot on a fatal failure, systemd runs it as
  `ExecStartPre`, and the installer runs it as its last step, so one
  implementation answers for every launcher.

### Changed
- **Configuration errors are found up front, not through retries.** A busy port,
  a wrong password and an unwritable folder do not become correct on a second
  attempt, so nothing retries them. The tray and both systemd units now report
  the cause and stop. The systemd units previously retried a failing
  `ExecStartPre` ELEVEN times in 27 seconds and kept going, because attempts
  spaced 5s apart never fill the default 10s start-limit window.

### Fixed
- **PostgreSQL could not open its own database from the wizard.** Utility
  statements take no bind parameters, so `CREATE ROLE ... PASSWORD $1` was a
  syntax error; the password is now escaped by the driver. `CREATEROLE` is also
  checked before existence, so pointing at an existing user no longer fails.
- **MSVC build breaks** that no macOS or Linux build could catch:
  `CpapController::sync_` declared inside a POSIX-only block, and a bare
  `<mysql.h>` where the client headers live in three different places.

### Testing
- The Windows installer is now **run**, not merely built: install, layout, Run
  key, preflight passing and failing, the tray serving `/health`, no orphaned
  child after either a force-kill or a normal close, and an uninstall that
  leaves the data directory intact.
- `QueryService` gained cover for all thirteen read paths, including every one
  against an empty database, which is what a fresh install actually hits.
  Coverage 78.0% to 80.3%.

## [4.7.3] - 2026-07-30: cover the oximetry night assignment

### Added
- **`QueryService` is now in the test binary.** It was excluded along with the
  rest of `src/web`, but it pulls in no Drogon, only `IDatabase`, `SqlDialect`
  and jsoncpp, so its read queries run against a temp SQLite file exactly like
  the database tests do. It hand-builds SQL for three dialects, which is
  precisely the code that should not be uncovered: that is how an unnarrowed
  `OR` shipped and served every recording on two consecutive nights.
- **Seven tests for oximetry night assignment**
  (`tests/web/test_QueryService_oximetry.cpp`), pinning the rule that a
  recording belongs to exactly one night, `date(start_time - 12 hours)`:
  an after-midnight recording files under the previous evening and is *not*
  also served for its own calendar date; recordings either side of midnight
  group into one night; noon is the boundary; the filename no longer
  influences the night; and a night with no recordings stays empty instead of
  borrowing from a neighbour.

  Verified as real regression cover rather than assumed: rebuilt against the
  pre-fix query, four of the seven fail, including the after-midnight case.

## [4.7.2] - 2026-07-30: an oximetry recording belongs to one night, not two

### Fixed
- **Every SpO2 recording was shown on two consecutive nights.** Opening one
  night and then the next showed the identical trace on both, so an imported
  CSV looked like it had been imported twice. Reported by an owner against the
  CSV upload, but it was never CSV-specific: BLE `.vld` sessions duplicated the
  same way, and it affected every session rather than an edge case.

  `getSessionOximetry` matched a session if *any* of six predicates held:
  `cpap_session_date` equal to the requested night **or the next one**, plus
  four `filename LIKE` patterns against both dates, one of which matched the
  date anywhere in the name. The widening was there because the ring labels a
  night with the morning's date, but nothing ever narrowed it back down, so a
  session dated the 6th was returned for both the night of the 5th and the
  night of the 6th.

  It now derives the night the same way everything else does,
  `date(start_time - 12 hours)` via `sql::sleepDay`, which yields exactly one
  night per recording and the same one the therapy chart uses. A 04:56 AM
  recording files under the previous evening, as it should. Verified against a
  real database: the session that previously returned its 4,964 samples for
  both the 5th and the 6th now returns them only for the 5th.

  This also drops the filename matching entirely, which mattered because
  `cpap_session_date` is empty on every `live_*.vld` row — those sessions were
  relying on `LIKE` alone to be found.

## [4.7.1] - 2026-07-30: first-run wizard database step, and the BLE scan stops being O2Ring-only

### Added
- **First-run wizard: database step and apply/restart (SDD-006 phase 2).**
  Setting up a private install no longer needs a text editor. The wizard now
  asks where the data should live before asking where it comes from, defaulting
  to the built in file and expanding to PostgreSQL or MySQL only if asked.
  - `POST /api/setup/test-db` connects and reports whether the target already
    holds sessions, so the wizard can say "this database already holds 412
    sessions, they will be reused" instead of leaving someone to guess whether
    they are merging into another person's data. It does NOT go through
    `IDatabase::connect()`, which creates and migrates the schema as a side
    effect: a typo in the database name would otherwise silently create twenty
    tables in whatever the user actually pointed at.
  - `POST /api/setup/create-db` provisions the database and its owner using
    administrator credentials that are request-scoped, never written to
    `config.json`, never returned and never logged. It then reconnects as the
    ordinary user, because a successful `CREATE DATABASE` followed by a failed
    `GRANT` otherwise looks like success and fails at first boot.
  - Identifiers are whitelisted (`letters, digits, underscore, not starting with
    a digit, at most 63 characters`) before interpolation, because neither
    engine accepts a bound parameter for the target of `CREATE DATABASE`.
    Passwords are always bound, never inlined. The MySQL grant is `@'%'` rather
    than `@'localhost'`: the whole point of the step is pointing at a database on
    another box, and a localhost grant authenticates during setup and then fails
    from the app host.
  - `POST /api/setup/apply` writes the config, marks setup complete, answers
    `202` and only THEN restarts, so the response is not lost with the process.
    A database change cannot be hot-reloaded, and pretending otherwise is the
    single biggest source of "it says configured but shows nothing".
  - All three refuse once `setup_complete` is true, so a finished install does
    not leave database provisioning exposed on the LAN forever.
  - The wizard only ever offers backends this build was actually compiled with.

### Fixed
- **The BLE scan only ever recognised one oximeter model.** `O2RingBleClient`
  matched a hardcoded `"O2Ring"` substring in two duplicated places, so every
  other device in the same family was invisible even though they all speak the
  identical GATT profile we already implement. It now matches the whole
  Viatom/Wellue line (`o2ring`, `checkme`, `checko2`, `viatom`, `wellue`,
  `sleepu`, `oxyring`, `band-wu`) case-insensitively, with a fallback to the
  advertised service UUID from BlueZ's `Device1.UUIDs` for models that drop
  their name from adverts once paired. Both call sites now share one
  `deviceMatches()` helper instead of carrying the same predicate twice.

  `band-wu` is the Checkme O2 Ultra. Confirmed from an owner's nRF Connect
  dump that it advertises as `Band-WU NNNN`, matching neither "checkme" nor
  anything else we looked for, and that its advertisement carries only 16-bit
  Heart Rate (`180D`) — the Viatom 128-bit service is a *connected* service
  and never appears in the advert, so the UUID fallback cannot see it either
  and the name is the only hook. Kept specific rather than a bare `band`,
  which would match any fitness tracker in range and have us connect to a
  stranger's device only to fail GATT discovery against it.

  Whether the Ultra's command set above that transport matches our block
  reads is still unverified against real hardware.

- **PostgreSQL never created its own core schema.** SQLite and MySQL both build
  their whole schema in `connect()`; this backend only ran incremental
  migrations, and its core tables lived in `scripts/schema.sql`, which nothing
  ships and nothing applies. Pointing at a FRESH PostgreSQL database therefore
  produced an install that connected happily, answered every read endpoint, and
  had no `cpap_sessions` to write to. That stayed invisible for as long as every
  PostgreSQL user ran that file by hand; the wizard's "create it for me" removes
  that step, so the gap would have become the default experience.

- **Cloud sync settings never reached the running service.**
  `CpapDashSyncService` took its settings once, in `main.cpp`, and nothing
  re-applied them. A token pasted into the Settings page updated the file and
  the in-memory config but not the live service, so the next sync still used the
  old one. The first-run wizard's restart hid this; the Settings page does not
  restart, so it did not.

- **First-run wizard: advanced options and start at login (SDD-006 phases 3-4).**
  The wizard is now the whole setup path, not just the database half.
  - `archive_dir` is a real config field with a `CPAP_ARCHIVE_DIR` env fallback,
    so anyone already exporting that keeps working. Choosing the Mule and Miner
    REQUIRES it, because the bridge hands over raw files that have to land
    somewhere before anything can parse them, and until now it was reachable
    only as an env var the wizard could not write.
  - An advanced step for MQTT, the LLM summaries, ML insights and the optional
    CpapDash mirror. Every group defaults OFF and is sent only when switched on,
    so an untouched section never overwrites what an upgrading user already has.
  - **Start at login**, which is the piece that makes a local install survive a
    reboot: a LaunchAgent on macOS, a systemd USER unit on Linux
    (`WantedBy=default.target`, because a user unit wanted by
    `multi-user.target` silently never runs). It starts at LOGIN, not at boot,
    and the wizard says so rather than implying otherwise. Under the SDD-005
    desktop shell the wizard reports "managed by CpapDash Desktop" and refuses,
    instead of installing a second entry racing the first.

- **Partial sessions (SDD-008).** A transfer from the Mule and Miner that starts
  and does not finish used to leave the night showing as live forever:
  `BurstCollectorService` closed a session exactly one way, when the checkpoint
  files stopped changing, and had no branch for the TRANSFER failing rather than
  the therapy continuing. The night is now reported honestly as **partial**.
  - New `cpap_sync_folders` ledger on **all three backends**, one row per date
    folder, plus every column in `MySQLDatabase::migrateSchema()`. The
    transition logic is a pure state machine with no DB, no clock and no I/O.
  - **No grace windows anywhere**, by decision. State advances only on observed
    facts: a stable signature, files stored, an STR parsed. An unreachable card
    simply does not advance the state, which is the honest answer.
  - Re-arming the STR/sidecar debt is **bounded** at the same signature. Upstream
    measured 339 re-pulls of one folder in three days without that bound.
  - Metrics and the LLM summary are **suppressed** for an incomplete night, which
    publishes only the partial fact. A truncated night's AHI and usage hours are
    wrong rather than uncertain, MQTT values are retained so Home Assistant keeps
    them in history, and both are hard to retract.
  - Partial nights are **excluded from compliance and trend aggregates** and shown
    with a badge instead. A stalled transfer undercounts usage hours, so
    including one can report someone below the 4-hour threshold on a night they
    actually met it.
  - Partial is never terminal: the night clears itself when its STR arrives.

- **The close edge never refetched the sidecars.** When every checkpoint file was
  unchanged the collector marked the session complete and returned without
  downloading anything, so an EVE or CSL file that grew inside a single KB
  bucket after the last checkpoint change was never pulled again and its events
  were lost silently. ezShare listings are KB-rounded, so a size comparison
  cannot see that growth. The sidecars are now refetched on close, in full from
  offset 0 -- never a ranged request, because an offset at or past the card's
  real EOF hangs the ezShare.

## [4.7.0] - 2026-07-30

### Added
- **Cleaning schedules (SDD-007).** Supplies answer when to *replace* a mask;
  nothing answered when to *wash* one, which is the daily and weekly half of
  CPAP care and the part people actually skip. Replacement and cleaning stay
  separate concepts on purpose: a mask is replaced every 90 days and wiped every
  day, and one interval cannot mean both.
  - Two tables and seven seeded presets on **all three backends**, with the
    catalog and the task keys taken verbatim from the cloud's SDD-043 so a user
    running both stacks sees one vocabulary.
  - `computeCleaningStatus` is ported from `hms-cpapdash-api` unchanged apart
    from the namespace, and its tests are that suite's vectors copied verbatim.
    Three implementations of this function now exist (the cloud's, this one, the
    Dart port in the phone app) and nothing but shared vectors stops them
    drifting into disagreeing about when a mask is dirty.
  - Seven local routes under `/api/cleaning`, mirroring the equipment surface.
    `status` is computed on read and never stored, exactly like supply wear.
    `/suggest` only offers what the setup actually holds, so a profile with no
    humidifier gets no water-tub tasks, and is idempotent on
    `(profile, task_key)`.
  - **Home Assistant sensors.** There is no phone and no notification planner
    here, so HA entities *are* the reminder mechanism: one retained sensor per
    enabled task plus a household-wide "Cleaning Due" binary sensor. This
    deliberately does less than the cloud spec, which fires notifications from
    the app's scheduler. hms-cpap publishes state and stops; when to be nudged
    is an automation the user owns.
  - **Optional cloud mirror**, against the new `POST /v1/cleaning/sync`
    (hms-cpapdash-api v2026.12.0). A second exchange with its own cursor, run
    after the equipment one because a task references a profile and the only
    shared handle is `client_uuid`, which the profile acquires during that pass.
  - A **Cleaning section** on the equipment page, below Accessories inside the
    selected setup's panel, reusing the existing wear colours so "due" reads in
    the same red as an overdue supply.

- **The first-run wizard is reachable (SDD-006 phase 1).** It had shipped a
  while ago and almost nobody could get to it.
  - `static_dir` defaulted to `./static/browser`, relative to the working
    directory, so double-clicking the binary anywhere other than its own folder
    served no UI at all. That one default made every other part of setup
    unreachable. It now resolves beside the executable, with the old value kept
    as a fallback and an explicit config value still winning.
  - `setup_complete` had been written since the wizard landed and was never read
    by anything, so a first-run user got an empty dashboard and no hint that
    configuration existed. A route guard now sends them to `/setup`.
  - On a genuine first run the binary opens the wizard rather than printing a
    URL to a terminal nobody is reading. Suppressed by `--no-browser`, under a
    supervising shell, and when not attached to a user session, so Docker and
    systemd never try.
  - `GET /api/capabilities` reports which storage backends the build actually
    has. Since 4.6.3 a config naming an uncompiled backend refuses to boot,
    which is correct and a miserable way to end a wizard.

### Fixed
- **`auto_sync` never fired, for anything.** `markDirty()` existed, was
  documented, and was called by nothing, so the debounce could never trigger and
  the only working path was the explicit `POST /api/equipment/cloud-sync`. Every
  mutation that changes mirrored data now marks the mirror stale. This affected
  equipment too, not only the tables this release adds.
- **Cleaning tasks were pushed to the cloud without a `client_uuid`.** The
  backfill covered profiles and items and silently skipped the new table, so a
  task went up with an empty uuid, came back with one, and the apply loop had
  nothing to match it to. Pushing looked perfectly healthy while nothing the
  cloud changed could ever come back down. Found by running the two services
  against each other over real HTTP, which no unit test could have done.
- **MSVC could not compile `DeviceDiscoveryService`**: it cast a `select()`
  timeout through `suseconds_t`, which Windows does not have.
- **`requestSyncNow` and `syncNowOutcomeString` were stranded** inside the
  `#ifndef _WIN32` block that exists for the Fysetc TCP server, so they vanished
  on MSVC and the controller failed to link. Their own unit tests could not
  catch it: they only ever ran where the guard was satisfied.
- **The Docker image build missed `third_party/`**, so it failed on a missing
  `mdns.h` while the native build, which has a full checkout, succeeded.
- MySQL on MSVC now links through vcpkg's exported target rather than a bare
  library path, which left ten `ZSTD_*` symbols unresolved.

## [4.6.3] - 2026-07-29

### Fixed
- **`database.type = "mysql"` wrote to MySQL and read from SQLite.** `main.cpp`
  hand-rolled backend selection in five places (the collector's DB, plus the
  separate connections for the web layer, reports, ML and backfill) and every
  one had only a `postgresql` branch, so `mysql` fell through to the SQLite
  default. Meanwhile `BurstCollectorService` used `makeDatabaseFromConfig()`,
  which honours `DB_TYPE` correctly. The collector therefore filled MySQL while
  every reader looked at a SQLite file it had just created, and the dashboard
  stayed permanently empty. All five sites now go through the factory, and
  startup refuses to continue when a requested non-SQLite backend silently
  downgraded, rather than logging therapy data somewhere the user never looks.

- **Saving any setting from the web UI destroyed the CpapDash sync token and
  the whole Fysetc block.** `AppConfig::save()` emitted the `cpapdash` object
  twice, the second time with the token masked to `"********"`, so the mask
  landed on disk and the next load read it back as the literal token. The same
  function never wrote the `fysetc` block that `load()` reads, so a UI save
  silently discarded it. Redaction now happens only in `toJson()`, which
  answers the API; `save()` persists secrets verbatim.

- **`GET /api/config` hid the `cpapdash` and `fysetc` sections entirely**, and
  `PUT /api/config` could not write `agent`, `sleep_stage`, `cpapdash`,
  `fysetc` or `web_port`. Cloud sync and Fysetc TCP were unreachable from the
  UI even though both were fully implemented.

- **Oximetry timestamps were shifted by the host's UTC offset.** Every
  timestamp column in this schema holds bare local wall clock, reached by a
  matched parse/render pair: CPAP goes `mktime` → `localtime`, and the oximetry
  parsers deliberately read the ring's printed time *as if* UTC (`timegm`), so
  they need a `gmtime` render. They were being written with `localtime`
  instead, so on an EDT host a ring row reading `23:00` was stored as `19:00`.
  Aggregates, MQTT sensors, PDF reports and range summaries were unaffected
  (delta-based, and oximetry is correlated to CPAP by date, not by timestamp),
  but the session-detail view drew the SpO2 and heart-rate charts four hours
  (five in winter) away from the flow and pressure charts for the same night.
  On PostgreSQL the samples disagreed with their own session header inside one
  table, because the header used `gmtime` and the sample loop used `localtime`.
  SQLite's live path had the mirror-image bug: `datetime('now')` is UTC there,
  so live samples were skewed the other way.
  This never showed up in the cloud because `mktime` and `timegm` are identical
  on a UTC host; it only affected local, non-UTC installs.
  **Existing oximetry rows written before this release stay shifted.** The
  offset in force at ingest was never recorded, so DST makes a blind arithmetic
  correction unreliable. Re-upload the original Wellue CSV or `.vld` files via
  `/upload`, which upserts on filename and so replaces rather than duplicates.

- **MySQL never gained columns added by later releases.** The backend's only
  schema management was `CREATE TABLE IF NOT EXISTS`, which is a no-op on a
  table that already exists, and it had no `ALTER TABLE` anywhere. An install
  created by an older build kept its original shape forever and every read of a
  newer column failed at runtime — the same failure class as 4.4.10. Added a
  `migrateSchema()` pass that runs on every connect, guarded by
  `information_schema` rather than `ADD COLUMN IF NOT EXISTS` (MariaDB supports
  that syntax, Oracle MySQL does not). Verified against a real four-column
  `cpap_sessions`: eleven columns restored, existing rows preserved, and a
  second start applies nothing.

- **MySQL truncated `checkpoint_files` at 256 bytes.** The JSON column was
  fetched with a binder capped to its inline buffer, so any night carrying more
  than a handful of files came back cut mid-key and parsed into garbage.

- **MySQL could not be built on macOS at all, and was silently disabled when
  it could not be found.** `MySQLDatabase.h` probed only `<mysql/mysql.h>` and
  `<mariadb/mysql.h>`, but pkg-config points `-I` straight at the directory
  holding `mysql.h`, so neither resolved under Homebrew; Debian only worked
  because `/usr/include` is always searched. Added a bare `<mysql.h>` probe, a
  `my_bool` shim for Oracle MySQL 8+ (which removed the typedef), and replaced
  a `std::vector<my_bool>` whose `vector<bool>` specialisation has no
  addressable element for `MYSQL_BIND::is_null`. On the CMake side, a missing
  client used to leave `BUILD_WITH_MYSQL=ON` in the cache while `WITH_MYSQL`
  went undefined, producing a binary that accepted `database.type = "mysql"`
  and then refused to start; that is now a hard configure error.

- **The test binary linked a different MySQL client than the shipped binary.**
  `run_tests` used bare `-l` names and resolved to MariaDB Connector/C while
  `hms_cpap` linked `libmysqlclient`. The two have different TLS defaults, so
  the suite validated a library that does not ship and failed against a server
  the real binary connects to.

### Added
- **MySQL oximetry support.** `oximetry_sessions` and `oximetry_samples` were
  missing from the MySQL schema entirely, and `saveOximetrySession`,
  `oximetrySessionExists` and `saveLiveOximetrySample` were stubs that returned
  failure. `POST /api/upload/oximetry` now works on MySQL.
- **`getOximetrySummary`, `getOximetryRangeSummary`, `getOximetryNightlySpo2`
  and `getCheckpointFilesByFolder` on SQLite and MySQL.** All four were
  inline header stubs returning `{}` on both backends; only PostgreSQL
  implemented them. On SQLite — the default backend — this meant O2 Ring data
  was written and then never reached the oximetry MQTT sensors, the PDF
  reports or the daily aggregation, and the SleepHQ export guard treated every
  folder as unparsed and queued it. PostgreSQL's `DISTINCT ON` has no
  equivalent elsewhere, so MySQL ranks with a window function and SQLite relies
  on its bare-column-with-`MAX()` rule.
- **MySQL enabled in the native macOS and Windows CI builds**, with a
  find_path/find_library fallback for MSVC and vcpkg where pkg-config is absent.
- 54 new tests (1233 total): oximetry and checkpoint parity across SQLite and
  MySQL, a MySQL schema-drift guard, and `AppConfig` secret-redaction and
  section round-trip regressions.
- **LAN discovery of Mule and Miner units** (`GET /api/discover/devices`) plus
  **`POST /api/sync/now`** to force a single burst cycle, wired into the setup
  page so a new install can find its unit instead of being told to type in an
  IP. First step of SDD-005 (docs/SDD-005-desktop-app.md); the installer and
  menu-bar pieces of that spec are still to come.

### Changed
- MySQL no longer logs a deprecation warning and three duplicate-key errors on
  every single connect. `MYSQL_OPT_RECONNECT` is version-guarded (Oracle removed
  it in 8.4) and indexes are declared inline as `KEY`, since MySQL has no
  `CREATE INDEX IF NOT EXISTS`. Two of those indexes were dropped outright as
  redundant against `UNIQUE KEY uq_device_session`.

## [4.6.2] - 2026-07-23

### Fixed
- **Lowenstein dashboard trends (AHI, usage) were always empty (#14).**
  `cpap_daily_summary` — the table `getDashboard`/`getTrend`/`getStatistics`
  query exclusively, with no fallback — was only ever populated by
  `processSTRFile()` (`STR.edf`, ResMed-only). Lowenstein has no `STR.edf`, so
  the table was never written for that source: `ahi_trend`/`usage_trend` came
  back `[]` and `compliance_pct` was always `0`, even though `cpap_sessions` /
  `cpap_session_metrics` had full data all along (the session list worked
  fine, since it reads those tables directly).
  Added `IDatabase::aggregateDailySummaryFromSessions()` (SQLite, PostgreSQL,
  MySQL) — an idempotent upsert that re-derives `cpap_daily_summary` from
  `cpap_sessions` + `cpap_session_metrics`, grouped by sleep day
  (`session_start - 12h`), same self-healing philosophy as
  `saveSTRDailyRecords`. Wired into the Lowenstein burst cycle after sessions
  are saved each pass.
  `ahi` is a **duration-weighted average of each session's own stored `ahi`**,
  not re-derived by summing the typed event columns: the shared parser counts
  a further generic "unclassified apnea" event type that is folded into
  `ahi` but never persisted to any typed column, so reconstructing it from
  `obstructive_apneas + central_apneas + hypopneas + clear_airway_apneas`
  alone silently undercounts it (confirmed against real production data — a
  real session came back 15.31 vs. the true 25.25). `ai` is then `ahi - hi`
  so `AHI = AI + HI` still holds for the row.
  Pressure/leak/SpO2/EPR columns store `NULL` rather than `0` for now
  (`avg_mask_pressure`/`avg_spo2`/`avg_epr_pressure` are always `0` in
  `cpap_session_metrics` until #15 — WMEDF signal aggregation — lands).
  6 new SQLite tests, including a direct regression test for the AHI
  undercount above.

## [4.6.1] - 2026-07-23

### Fixed
- **AHI aggregation double-counted non-apnea events (#13).** The per-session
  `ahi` computed by the shared `cpapdash-parser` library (apneas + hypopneas
  only, fixed in parser v2026.1.3) was already correct, but every SQL
  aggregation query that groups multiple mask-on/off segments into one
  sleep-night row — `QueryService::getSessions` (`/api/sessions`) and the
  nightly/trend queries in all three DB backends (PostgreSQL, MySQL, SQLite)
  — recomputed AHI itself as `total_events * 3600 / duration`, reintroducing
  the same bug one layer up. This inflated AHI badly on Löwenstein Prisma
  SMART max sessions, which flag many non-apnea `RespEvent` types (snoring,
  flow limitation, Cheyne-Stokes). Fixed to sum
  `obstructive_apneas + central_apneas + hypopneas + clear_airway_apneas`
  instead of `total_events` in all affected queries. Self-healing: the
  affected event-type counters were always correct, so this corrects
  historical sessions automatically on next query — no reparse needed.

## [4.6.0] - 2026-07-19

### Added
- **SDD-004: equipment profiles and supply reminders.** A profile is a named
  setup that owns exactly one machine plus its accessories; wear is COMPUTED
  from the in-use date and the replacement interval, never stored. Ships across
  all three database engines (PostgreSQL, SQLite, MySQL) with matching schema
  scripts and a parameterised drift guard that fails the build when an engine
  and its `scripts/schema*.sql` disagree. The one-machine-per-profile rule is a
  partial unique index on PostgreSQL and SQLite; MySQL has no partial indexes,
  so there it rests on the controller guard.
- **Supply state in Home Assistant.** Each tracked accessory publishes retained
  `days_left` and `wear_percent` sensors plus a household-wide `supplies_due`
  binary sensor, via MQTT discovery, on the existing burst cycle. Machines and
  undated accessories are deliberately skipped: a sensor pinned at 0 forever is
  worse than no sensor.
- **Supply transition events.** Retained state cannot express "this just went
  overdue" — the value looks identical on the cycle it crossed and on every
  cycle after. State changes now also emit a non-retained message on
  `cpap/<device>/supplies/event` carrying `from`, `state`, `days_left` and
  `replace_by`, so an automation fires once per crossing instead of once per
  burst. Crossings back to `fresh` are emitted too, so an automation can clear
  what it raised. A first sighting is silent when fresh but announced when
  already overdue. The last state per entity persists to
  `~/.hms-cpap/supply_events.json` (write-then-rename) because the publisher is
  rebuilt every cycle; without it every cycle would look like a first sighting.
  The ledger is kept out of the database on purpose: it is local notification
  bookkeeping, and a column would sync it to the cloud and fight last-write-wins.
- **Optional CpapDash cloud mirror.** Off by default. When enabled with a token,
  equipment syncs against the CpapDash API with uuid-matched last-write-wins;
  local stays the source of truth either way. `POST /api/equipment/cloud-sync`
  triggers it on demand.
- **Equipment page** in the web UI: profile chips, per-item wear bars, inline
  editing, and a catalog-driven add row.

### Fixed
- **Windows build broke on the supply publisher.** `SupplyPublisher.cpp` calls
  `gmtime_r`, which does not exist on MSVC; every other caller includes
  `utils/TimeCompat.h` for the `gmtime_s`-backed shim and this one did not.
  Linux and the tests never noticed. Include added.
- **Cloud sync could silently discard a genuine edit.** Applying a row from the
  CpapDash cloud stamped `updated_at = now()` — the moment we MIRRORED the row,
  not the moment the user CHANGED it. That makes a copy outrank its own original
  under last-write-wins: device A edits at T1, device B mirrors it at T2, then A
  makes a real edit at T3 where T1 < T3 < T2, and B's untouched copy beats A's
  genuine edit, which is dropped with no error. Mirrored rows now carry the
  origin row's timestamp. The server already worked this way
  (`COALESCE(NULLIF($7,'')::timestamptz, NOW())` guarded by `updated_at <= $7`),
  so the two sides are now symmetric.
- **Sync never settled.** The push watermark advanced only as far as the newest
  row PUSHED, so rows APPLIED from the cloud stayed above it permanently and were
  pushed straight back on every sweep — a standing loop with `auto_sync` on.
  Applied rows now advance the watermark too. Preserving the timestamp alone did
  not fix this; both changes are required.
- **Malformed timestamps behaved differently on every engine.** Given a bad
  override string, SQLite stored it verbatim (`NULLIF(?,'')` only catches the
  EMPTY string), PostgreSQL threw on the `::timestamptz` cast and failed the whole
  write, and MySQL silently fell back to `NOW()`. All three now gate through one
  shared `IDatabase::sanitizeUpdatedAtOverride()`, so they accept and reject
  exactly the same inputs.
- **SleepHQ export shipped no machine.** `BackfillService` passed the DATALOG
  directory as the card root, so `STR.edf` and `Identification.*` were never
  uploaded. SleepHQ received a therapy folder with no summary and no machine and
  processed it into nothing visible. It now passes the card root.
- **Nights that never parsed were never exported.** Export was only triggered
  from the parsed-session path, so a folder that failed to parse was never
  offered to SleepHQ. The burst cycle now also walks the archive and queues
  unparsed nights, skipping today and folders already checkpointed.
- **EDFParser accepted negative durations,** yielding sessions that ended before
  they started.

- **Windows build.** libpqxx 7.10+ yields `pqxx::row_ref` when iterating a result
  rather than `pqxx::row`, and the three equipment row-mapping helpers named
  `pqxx::row` concretely. MSVC rejected the conversion (Linux accepted it), so
  this failed only on the Windows runner. The helpers are templated on the row
  type and work with either.

### Tests
- **The PostgreSQL equipment implementation had no tests at all.** The
  `EquipmentBackend` fixture was hardcoded to SQLite, leaving roughly 450 lines of
  PG equipment code unexecuted by any test — which is what put the coverage
  ratchet under its 80% threshold. The fixture is now parameterized over SQLite
  and PostgreSQL, so all 20 cases run against both (40 total) with no assertion
  changed. PG runs against a uniquely-named throwaway schema whose `search_path`
  has NO `public` fallback, so a failed migration errors instead of silently
  pointing write-heavy tests at real user data, and skips cleanly when no server
  is reachable. MySQL is excluded on purpose: it cannot express the partial unique
  index behind the one-machine-per-profile rule.

### Changed
- Equipment UI says "profile" throughout; "setup" is gone from all user-facing
  copy. Rename and delete are compact icon buttons aligned right of the profile
  header, so they no longer compete visually with the profile chips themselves.
- README no longer describes the charts as "clinical-grade". The software is not
  cleared as anything of the sort, and the phrase contradicted the disclaimer.
- `AppConfig` example comment no longer carries a real private LAN address.
- README test count corrected from 425 to the actual suite size.
- README documents equipment profiles and supply reminders: a feature entry, a
  table-of-contents link, and a section covering profiles, why wear is computed
  rather than stored, the retained-sensor vs event distinction, and a worked
  Home Assistant automation.
- `upsertEquipmentProfile`, `upsertEquipmentItem`, `tombstoneEquipmentProfile`
  and `tombstoneEquipmentItem` take an explicit `updated_at_override` with **no
  default value**, so the compiler forces every call site to declare whether it
  is a local write (`""`, stamps now) or a mirror of a remote row (the origin
  timestamp). A defaulted argument would let a future apply-path silently
  reintroduce the bug above.

### Legal
- **`DISCLAIMER.md`** — not a medical device, not regulator-cleared, not a
  monitoring system; do not change therapy based on it, and the specific ways
  parser output can be wrong. Summarised in a banner at the top of the README.
- **`TERMS.md`** — terms of use covering permitted use, the not-a-medical-device
  condition, third-party services, warranty, liability, and contribution terms
  (including: do not contribute GPL-derived or manufacturer-proprietary code).
- **`PRIVACY.md`** — no telemetry, no analytics, no phone-home; what is stored
  and where; and an exhaustive list of every outbound path (MQTT broker, SleepHQ
  export, CpapDash sync, LLM endpoint), all off by default. Notes that tokens are
  stored in plaintext and that the service ships with no authentication and must
  not be exposed to the internet.
- **`NOTICE`** — trademark attribution and an explicit statement of independence
  from ResMed, Philips, Lowenstein, SleepHQ and others; a clean-room statement
  recording that OSCAR (GPLv3) was consulted for format understanding only and
  that no OSCAR code was copied or derived from, which is why MIT applies; and a
  full third-party dependency list with licenses, including the MySQL client's
  GPL-with-FOSS-exception caveat.

## [4.5.1] - 2026-07-19

> Version jumps past 4.4.11 on purpose. A stray `v4.5.0` tag already sits on an
> older commit (it points at "set version 4.4.1"), so 4.5.1 is the first number
> unambiguously ahead of everything published.

### Fixed
- **Unreachable-device log storm.** When the ez Share / Mule device is offline the
  poll loop retries about once a minute, and every attempt logged a full error —
  three identical lines per cycle, because `O2RingClient::getLive()` also re-logged
  the failure `httpGet()` had already reported. A single offline device produced
  hundreds of identical lines a day (102 for the `A:DATALOG` listing, 101 each for
  the O2Ring live read and its duplicate), burying real signal in the journal and
  keeping the health summary red with no added information. HTTP failures now go
  through a `FailureLogThrottle`: the first failure is logged in full (an outage
  stays immediately visible), identical repeats are counted and suppressed, one
  summary line is emitted per 15 minutes ("still failing: N consecutive failures
  over M min; K identical errors suppressed"), and recovery is announced with the
  streak length. A *different* error still logs immediately, so new information is
  never hidden. Measured: 2 lines per outage instead of ~15 per five minutes.
- **Startup banner reported the wrong version.** It hardcoded `Version: 2.2.0` and
  had drifted years behind the `VERSION` file, so startup logs were actively
  misleading about what was deployed. The banner now reads `HMS_CPAP_VERSION`.

## [4.4.11] - 2026-07-14

### Fixed
- **SleepHQ auto-export no longer ships partial nights.** Exports fired the
  moment any session was marked completed, which happens 1-2 minutes after
  ANY mask-off, including mid-night breaks: a 1-minute mask-on fragment could
  be uploaded as the entire night, and a single failed file upload silently
  killed the full-night export with no retry. Exports are now debounced
  (SDD-003): a completed session marks its date folder dirty, and the export
  runs only once the archive folder has been quiet for
  `sleephq.quiet_minutes` (default 15, `SLEEPHQ_QUIET_MINUTES`), so late EVE/
  CSL files and the delayed STR flush are included. Failed exports retry with
  exponential backoff, and a folder that changes mid-upload is re-exported in
  full. The manual per-night "Upload to SleepHQ" button still exports
  immediately.

## [4.4.10] - 2026-07-13

### Fixed
- **Schema scripts caught up with the runtime migrations.** The checked-in
  `scripts/schema{,_mysql,_sqlite}.sql` had drifted behind the v2.2.0 in-code
  migrations, so databases created from the scripts (e.g. an external
  PostgreSQL where the app user cannot ALTER) failed session saves with
  `column "spo2_drops" of relation "cpap_session_metrics" does not exist`.
  `spo2_drops` is now present with the integer type the code expects, and the
  also-missing `odi` column and `cpap_breaths` table were added to all three
  dialects. Thanks to @ToasterDEV for the report and initial fix (#10).
- Windows (MSVC) build portability: `O2RingCsvParser` used POSIX `timegm`, which
  MSVC lacks. Added a uniquely named `timegm_utc` helper to `utils/TimeCompat.h`
  (POSIX `timegm` / Win32 `_mkgmtime`), avoiding both the missing-identifier error
  and an `LNK2005` clash with Drogon's own `timegm`. No effect on Linux/macOS.

### Added
- CI: newly opened GitHub issues are auto-assigned to the maintainer.

## [4.4.9] - 2026-07-10

### Fixed
- **Session grouping now measures mask-off gaps end-to-start.** Gaps between
  checkpoint blocks were measured start-to-start from filename prefixes, so any
  recording block longer than `SESSION_GAP_MINUTES` looked like a gap: a 4-hour
  evening block followed by a 9-minute mask-off break split one night into two
  sessions, halving durations and doubling apparent AHI. Each checkpoint now
  carries an estimated write-close time (BRP size at ~6 KB/min, plus the file's
  modified time when plausible), and the split compares the next block's start
  against the running end of the current group. Applies to both ezShare and
  local-folder grouping. Re-run affected nights with a reparse to regroup them.
- **Card-root residue no longer re-downloads every burst.** The full-card
  residue sweep re-fetched every `SETTINGS/`, `Identification.*` and `Journal.dat`
  file on each session-bearing burst (40+ extra ezShare round-trips per cycle
  during an active night). Files already archived at the listed size are skipped.

## [4.4.6] - 2026-06-29

### Added
- Full-card OSCAR residue capture (SDD-002): the per-night `.crc` plus root
  `Identification.*`, `SETTINGS/`, and `JOURNAL` are now pulled into the OSCAR
  archive (ezShare transport), so `~/.hms-cpap/` is a complete, OSCAR-importable
  card image and SleepHQ uploads include the residue.

### Fixed
- SQLite / local-directory dashboard freeze (#8): in local-directory mode the STR
  daily summary (`cpap_daily_summary`, the dashboard's source) was only written on
  session completion, which never fires for static local sessions — so the
  dashboard froze on an old date. STR is now processed every burst cycle in local
  mode (idempotent upsert; self-healing).
- STR `--backfill` and `--reparse` honored `DB_TYPE` instead of hardcoding
  PostgreSQL (#8): they failed for SQLite users (Synology Docker) trying to reach
  `localhost:5432`. Backend selection now goes through a shared `DatabaseFactory`.
- `processSTRFile` persists the full STR history instead of only a trailing
  7-day window.

### Changed
- Coverage gate restored to 80% (test backfill); the unit CI gate skips the
  flaky / broker-dependent integration suites.

## [4.4.5] - 2026-06-27

### Added
- **Web upload page** (`/upload`, plus an "Upload" nav link) with two drop zones,
  mirroring the cpapdash-api upload UX:
  - **CPAP Data (.zip)** — `POST /api/upload/cpap`. Extracts the zip's `DATALOG`
    date folders into the configured archive and reparses them via the existing
    backfill pipeline (async; the page polls `/api/backfill/status`).
  - **O2 Ring Oximetry (.csv)** — `POST /api/upload/oximetry`. Parses a Wellue /
    Viatom "O2 Ring" CSV server-side and stores it under the `o2ring` device,
    returning the parsed summary synchronously.
- **`O2RingCsvParser`** — handles both Wellue export dialects (24-hour
  `06:53:07 Apr 12 2026` and the O2 Ring S 12-hour quoted, comma-after-day
  `"11:20:29PM Jun 19, 2026"`), auto-detects the sample interval from the
  timestamps, and maps sentinel "no reading" values (SpO₂ 255 / HR 65535) to
  `0xFF` so `OximetrySample::valid()` excludes them. Emits the shared
  `OximetrySession` and reuses `VLDParser::calculateMetrics`. 5 unit tests.

### Changed
- Raised the Drogon client max body size to 512 MB so multi-MB zip / CSV uploads
  aren't rejected with `413` (default was 1 MB).
- `PrismaIngestion::extractZip` is now public so the CPAP zip upload reuses it.

## [4.4.4] - 2026-06-22

### Changed
- **Sessions list is now recency-paginated instead of date-windowed.**
  `GET /api/sessions` takes `limit` + `offset` (was `days` + `limit`) and no
  longer filters by a 30-day window, so the full night history is reachable.
  The Sessions page loads the latest 20 nights with a "Load more" button that
  walks back through every night; the 30s auto-refresh keeps however many pages
  are already expanded. Dashboard "latest night" tile and the realtime
  live-session probe now fetch the most recent night regardless of its age.

## [4.4.3] - 2026-06-21

### Fixed
- **Green CI.** `CPAPModelsTest.Metrics_EventCounting_MixedEvents` still asserted
  the pre-fix AHI (counted a RERA). Updated to the correct apnea+hypopnea-only
  value (1.32) that the parser (2026.1.3) now computes. No code change — the AHI
  fix already shipped via the shared parser; this only realigns the stale test.

## [4.4.2] - 2026-06-21

### Fixed
- **SMART max events now parse + correct AHI** via the shared parser bump to
  2026.1.3: tolerate spaced `RespEvent` attributes (`RespEventID = "101"`, was 0
  events), and AHI counts apneas + hypopneas only (was inflated by flow
  limitation / RERA / snore / leak). Added an hms-cpap test covering both.
- **CI green again.** `PostgresEdgesTest` fixture was missing the `spo2_drops` /
  `odi` columns the metrics insert writes (failed only in the fresh-schema
  coverage job). Excluded the SleepHQ network glue (`SleepHqClient`,
  `SleepHqExportService`) from the coverage denominator, matching the existing
  Fysetc transport exclusion.

## [4.4.1] - 2026-06-21

### Added
- **Löwenstein Prisma SMART max support** (GitHub #6). Newer firmware (e.g.
  3.17) drops the split events//signals trees and nests everything under
  `<serial>/<YYYYMMDD>/<NNNN>/` with events, signals, and trendCurves together
  and 3-digit sequence numbers. `PrismaIngestion` now auto-detects this combined
  layout (root may be the SD root or the serial folder) and pairs event/signal
  by sequence within each session subfolder. Verified against a real SMART max
  sample (21/21 sessions discovered and parsed).

### Changed
- Prisma sequence-number matching relaxed from exactly 6 digits to any width, so
  both Prisma Smart (6-digit) and SMART max (3-digit) parse.

## [4.4.0] - 2026-06-21

### Added
- **SleepHQ auto-export.** Forward completed therapy nights to SleepHQ via their
  public API (OAuth password grant, create import, multipart upload with MD5
  content_hash, process_files). Shared design with the cloud (SDD-009).
  - Auto-triggers, each toggleable: on session complete (live collector) and on
    local-mode/backfill import.
  - Manual per-night export: "Upload to SleepHQ" item in the Sessions row menu
    (`POST /api/sleephq/export/{date}`), shown only when enabled.
  - Settings "SleepHQ Sync" card (enabled, client ID/secret, the two auto
    toggles); creds persist via the existing config save.
  - Verified end-to-end against the live SleepHQ API.

### Fixed
- **Build break:** `BackfillService::Config` referenced a `sleephq` field it
  never declared (the SleepHQ C++ had not been compiled until now). Added the
  nested gate, populated from `AppConfig`.

## [4.3.3] - 2026-06-03

### Added
- **Test coverage 27.4% → 75.0%** — ~470 new deterministic tests across parsers,
  DB layers (SQLite + Postgres via throwaway-schema fixtures), services, agent
  subsystem, ML training, and the burst-collector orchestration. Coverage CI job
  with a Postgres service + ratchet gate.
- **BurstCollectorService DI seam** — `injectDependenciesForTest()` /
  `runBurstCycleForTest()` (production-inert) make the orchestration unit-testable.

### Fixed
- **GnuplotService injection** — chart title/ylabel are now sanitized before
  entering the single-quoted gnuplot script piped to gnuplot (`sanitizeLabel`).
- **PG test schema isolation** — the DatabaseService Postgres suite now runs on a
  fresh DB in CI (create schema first, then connect with `search_path=<schema>,public`).

## [4.3.2] - 2026-06-03

### Added
- **Code coverage (gcov/lcov)** — `ENABLE_COVERAGE` CMake flag + `scripts/coverage.sh`
  (instrumented build → tests → lcov HTML report + ratchet gate). New CI
  `coverage` job (with a Postgres service so the pqxx suite runs) enforcing a
  ratchet `COVERAGE_MIN`.
- **Test coverage 27.4% → 60.6%** — ~330 new tests across SessionDiscovery,
  Fat32Parser, DataPublisher, SleepStageClassifier, a SQLite DB CRUD suite,
  InsightsEngine, EzShare/Prisma parsing, a Postgres DatabaseService suite
  (throwaway-schema, skips without PG), O2Ring decode, DiscoveryPublisher,
  the agent subsystem, and BurstCollectorService lifecycle. All deterministic
  (temp dirs, fakes, fixed epochs; no live broker/device/clock).

## [4.3.1] - 2026-06-03

### Fixed
- **Per-session reparse now works for sessions of any age** — the UI reparse button previously only cleared checkpoint sizes and reopened the session, relying on the next burst cycle to re-download and reparse. But the burst collector only revisits the last ~2 nights, so reparsing an older session silently never ran (and could leave the row stuck open). Reparse now delegates to a single-day archive backfill (group → delete that day's rows → parse → save → mark completed), which works regardless of session age.

### Changed
- **BackfillService wired in all source modes** — previously only `local`/`ezshare`. It reparses from the permanent archive, so it must be available in every mode; the per-session reparse depends on it.
- Reparse stays scoped to exactly one night's folder (`sleep_day → YYYYMMDD`), never touching the adjacent night, guarded by a new unit test (`SingleDayReparseTouchesOnlyThatFolder`).

### Removed
- Dead `BurstCollectorService::reparseSession()` (the broken clear-checkpoints-and-defer path that caused stale open sessions).

## [4.3.0] - 2026-05-18

### Added
- **Lowenstein Prisma support** — full session parsing for Prisma Line and Prisma Smart machines (WMEDF signals + XML events). Auto-detects raw directory trees (Prisma Smart) and therapy.pdat ZIP archives (Prisma Line). AHI, event metrics, breathing summaries, device info, and pressure/flow/SpO2 signal extraction.
- **PrismaIngestion service** — session discovery, file pairing (event+signal by sequence number), staging, ZIP extraction via miniz, and WMEDF header timestamp parsing. Integrated into BurstCollectorService as `CPAP_SOURCE=lowenstein`.
- **Manufacturer-aware session completion** — `processSessionSummary()` dispatches by source: ResMed runs STR.edf processing, Lowenstein stub ready for statistics_year.bin (future).
- **14 new tests** — PrismaIngestion unit tests (11) + PrismaE2E integration tests (3, 43/43 real sessions verified)
- **Updated screenshots** — refreshed dashboard, sessions, session-detail; new reports and settings page screenshots
- **Supported Devices table in README** — ResMed (live + import), Philips DreamStation 2 (import), Lowenstein Prisma (import)

### Changed
- **README overhaul** — multi-manufacturer framing, architecture diagram with shared data source paths, Lowenstein setup docs, generic feature descriptions, updated test count (425)
- **BurstCollectorService** — Lowenstein branch in `executeBurstCycle()` and `reloadConfig()`, hot-reload support for source switching

## [4.2.1] - 2026-05-11

### Added
- **O2Ring avg HR MQTT** — `publishOximetrySummary()` publishes retained `cpap/{device_id}/oximetry/avg_spo2` and `cpap/{device_id}/oximetry/avg_heart_rate` after each session completes; two new HA discovery sensors (65 total)
- **Sessions HR column** — sessions table shows avg heart rate with machine HR first, O2Ring fallback; fetches O2Ring data for all recent sessions (not just those missing machine SpO2)

## [4.2.0] - 2026-05-06

### Added
- **PDF Report Generation** — OOP class hierarchy: `BaseReportGenerator` + `RangeReportGenerator` (multi-night) + `DailyReportGenerator` (single-night per-minute detail)
- **Daily Detail Report** — 8 charts with data tables: mask pressure, respiratory rate, tidal volume, minute ventilation, leak rate, snore index, SpO2 and heart rate from O2Ring (only rendered when ring data exists for that session)
- **Range Report SpO2 chart** — O2Ring nightly SpO2 trend via `getOximetryNightlySpo2()` on `IDatabase`
- **Reports page** (`/reports`) — date range form, generate button, auto-refreshing status table with download
- **Sessions "Day PDF" action** — generates single-day report from session dropdown and auto-downloads on ready
- **Nav bar** split to separate `.html`/`.css` files; Reports link added between Sessions and Settings

### Fixed
- **30-min bucket averaging** — daily charts bucket per-second/per-minute signals into 30-min averages; `extractCol30` now correctly parses numeric-as-string JSON values from DB (was falling through to `0.0`)
- **Spurious zero filtering** — `vmin` threshold per signal skips invalid zero readings for pressure, RR, tidal volume, SpO2, HR
- **GnuplotService** — deletes partial output file when gnuplot exits non-zero, preventing libharu `ec=4155` stream overflow on corrupt PNG load
- **`std::isfinite` guards** — both in data extraction and before gnuplot data write to prevent NaN propagation
- **Windows MSVC build** — `FysetcDataSource.cpp` now excluded alongside `FysetcTcpServer.cpp`; `libhpdf`/gnuplot/report services gated behind `#ifndef _WIN32` / MSVC CMake exclusions

## [4.1.2] - 2026-05-04

### Refactored
- **BurstCollectorService lifecycle** — constructor is now minimal (device ID/name only); all subsystem init (data source, DB, MQTT, LLM, O2Ring) moved into explicit `initialize(AppConfig*)` method called after construction. Fixes `app_config_` being null during EzShareClient creation, which caused range config to never apply.

## [4.1.1] - 2026-05-04

### Fixed
- **Live session not shown in realtime API** — `has_live` is a count, not a boolean; fixed comparison from `== "1"` to `> 0` so nights with multiple open sessions display correctly
- **Range downloads ignored config** — `EZSHARE_SUPPORTS_RANGE=false` env var and `ezshare_range: false` config were set but stale binary still attempted range requests; rebuild enforces correct behavior
- **ezShare URL pointed at Fysetc** — service env had `EZSHARE_BASE_URL=http://192.168.2.75` (Fysetc) instead of real ezShare `.40`

## [4.0.6] - 2026-04-26

### Fixed
- **Dashboard crash on SQLite/Docker** -- `getRealtime()` null guard prevents `Cannot read properties of null (reading 'session')` error when no live device exists. Charts now render correctly in local/Docker mode.
- **Config file overwritten on startup** -- binary no longer overwrites user-provided config.json with defaults. Only creates config on first run.
- **MSVC C4456 warning** -- renamed shadowed lock variable in MLTrainingService
- **Local mode STR processing** -- processSTRFile() was not called after session completion in local/Docker mode, leaving daily_summary empty and trend charts blank

## [4.1.0] - 2026-04-25

### Added
- **HA-style dashboard** -- full redesign with 10 new section components matching Home Assistant Mushroom card layout
- **Key Metrics row** -- AHI score, usage, mask leak, total events, session status with color-coded FA icons
- **O2 Ring Oximetry section** -- SpO2, heart rate, ODI, ring status from Wellue O2Ring
- **AI Session Summary** -- LLM-generated markdown analysis with Overall/Events/Recommendations
- **Therapy Insights** -- InsightsEngine analysis (AHI trend, leak correlation, compliance, best/worst nights)
- **STR Daily Metrics** -- official ResMed indices (OAI/CAI/HI/RERA) from STR.edf
- **Sleep Events Breakdown** -- obstructive, central, hypopneas, RERAs with color coding
- **Therapy Pressure section** -- half-doughnut gauges for avg and P95 pressure
- **Respiratory Metrics** -- rate, tidal volume, minute ventilation
- **Real-Time Status** -- session info and pressure range
- **ML Intelligence section** -- predicted AHI, trend, hours, mask fit risk, anomaly detection
- **`/api/insights` endpoint** -- exposes InsightsEngine analysis via REST (QueryService + CpapController)
- **Font Awesome 6.5** -- all icons replaced with FA for consistency
- **O2Ring fallback in session detail** -- SpO2/HR cards pull from O2Ring when machine values are 0
- **O2Ring SpO2 in sessions table** -- fetches oximetry for recent sessions without machine SpO2
- **Sleep stage classification** -- HMM-smoothed 4-stage classifier (Wake/Light/Deep/REM) from CPAP signals
- **`/api/sessions/{date}/sleep_stages` endpoint** -- per-session hypnogram data
- **`/api/sleep-stages/status` endpoint** -- model status and configuration

### Changed
- **Dashboard Key Metrics** -- now uses session-aggregated data (matching sessions table) instead of STR for AHI/usage/events
- **MetricCardComponent** -- added optional FA icon and iconColor inputs
- **Session detail cards** -- FA icons with dynamic color coding (AHI, events, SpO2)

### Fixed
- **Insights query** -- removed non-existent `leak_70` column that caused silent SQL failure
- **JSON string-to-double parsing** -- added `jdouble()` helper for safe conversion of DB string values

## [4.0.4] - 2026-04-21

### Added
- **Range request toggle in settings UI** — checkbox under Data Source to enable/disable HTTP Range downloads for ezShare and Fysetc Poll modes
- **`ezshare_range` config field** — persisted to config.json, env fallback `EZSHARE_SUPPORTS_RANGE`, hot-reloadable

### Changed
- **BurstCollectorService** — reads range support from `AppConfig.ezshare_range` instead of environment variable

## [4.0.0] - 2026-04-21

### Added
- **Fysetc raw-sector TCP protocol** — new data acquisition path that reads raw SD sectors over TCP from the Fysetc WiFi SD Pro. No FAT mount on ESP32, no HTTP server. Device becomes a thin sector I/O service with <10ms bus hold times.
- **IDataSource interface** — extracted from EzShareClient. Both EzShareClient and FysetcDataSource implement it. SessionDiscoveryService and BurstCollectorService work through IDataSource, making data source swappable.
- **FysetcDataSource adapter** — translates FAT32 sector reads into IDataSource interface. Existing session discovery, checkpoint comparison, and download pipeline runs unchanged.
- **Fat32Parser** — read-only FAT32 parser with bulk 64-sector prefetch cache. BPB, cluster chains, LFN entries, byte-offset sector ranges.
- **FysetcTcpServer** — TCP listener (port 9000). HELLO handshake, SECTOR_READ request/response, LOG forwarding, TCP keepalive.
- **FysetcProtocol.h** — binary wire format codec, 10 message types, length-prefixed.
- **Firmware log forwarding** — ESP_LOG ring buffer drained over TCP. Crash diagnostics survive reboots.
- **WiFi RSSI + stats monitoring** — logged every 30s via forwarded LOG messages.
- **32 new tests** — Fat32Parser (17), FysetcTcpServer (8), FysetcProtocol (5), FysetcCollector (2).

### Changed
- **SessionDiscoveryService** — takes `IDataSource&` instead of `EzShareClient&`.
- **BurstCollectorService** — uses `unique_ptr<IDataSource>`. Source selected by config: `"ezshare"`, `"fysetc"`, or `"local"`.
- **EzShareClient** — inherits from `IDataSource`, `override` on 5 methods.

## [3.3.1] - 2026-04-18

### Added
- **ODI metric on dashboard** — replaces motion with Oxygen Desaturation Index (3% drops/hr)
- **SpO2 color coding** — green >=95%, orange 85-94%, red <85%
- **BLE adapter detection** — `/api/config/test-ble` endpoint, Angular settings shows adapter status
- **Service log warning** — clear message when no BLE adapter found

## [3.3.0] - 2026-04-18

### Added
- **Wellue O2 Ring oximetry integration** — overnight SpO2/HR/motion data correlated with CPAP therapy
- **Direct BLE mode** — native C++ BlueZ D-Bus client (sdbus-c++) connects directly to O2 Ring via USB Bluetooth adapter, no mule needed. Build with `-DBUILD_WITH_BLE=ON`
- **HTTP mule mode** — polls O2 Ring data via mule ESP32-C3 at `/o2ring/live` and `/o2ring/files`
- **State machine** — detects ring active/inactive transitions, polls live SpO2 when on finger, downloads .vld files when session ends (active→inactive edge)
- **VLD v3 parser** — shared C++ library (hms-cpapdash-parser) parses O2 Ring binary files: header, 5-byte records, timestamp reconstruction, ODI calculation
- **PostgreSQL storage** — `oximetry_sessions` + `oximetry_samples` tables (v2.2.0 migration), live samples with `source='live'` overwritten by VLD import
- **MQTT discovery** — 6 new HA sensors: spo2, heart_rate, motion, active (binary), last_spo2, last_heart_rate (retained)
- **Angular charts** — O2Ring SpO2 and Heart Rate in session detail overview grid + dashboard metric cards
- **LLM correlation** — daily/weekly/monthly summaries include oximetry metrics (ODI, time below 90%, SpO2 baseline), LLM detects AHI vs ODI discrepancies
- **Settings UI** — O2 Ring section: enabled toggle, mode selector (HTTP/BLE), mule URL, all persisted via config API
- **IO2RingClient interface** — abstract base for HTTP and BLE clients, swappable at runtime
- **28 unit tests** — state machine (9), VLD parser integration (4), LiveReading (4), Viatom BLE protocol (11): CRC-8, command building, frame reassembly, MTU chunking

### Changed
- **README** — removed misleading micro SD card references, updated ezShare description

## [3.2.5] - 2026-04-11

### Fixed
- **Session gap truncation** — `duration_cast<hours>` silently truncated sub-hour
  gaps to zero, preventing session splits when `SESSION_GAP_MINUTES` < 60. Now
  casts to minutes correctly (both ezShare and local code paths).
- **CSL/EVE file cross-assignment** — Matched CSL/EVE entries were never erased
  from the lookup map, causing the last session in a multi-session night to steal
  the first session's CSL file. Iterator-based erase after match (both code paths).
- **Docker timezone mismatch** — `is_today` folder check used `localtime()` which
  defaults to UTC in Docker containers, breaking live session polling for US/EU
  timezones. Now checks both local and UTC dates.
- **Frontend polling race conditions** — `trainNow()` and `pollBackfillStatus()`
  used `setInterval` + raw `subscribe`, allowing overlapping HTTP requests that
  could resolve out-of-order under Docker load. Replaced with RxJS
  `timer` + `switchMap` + `takeWhile`.
- **ezShare `<DIR>` regex** — Parser only matched HTML-entity `&lt;DIR&gt;`,
  failing silently on older/cheaper ezShare clones that send literal `<DIR>`.
  Now accepts both formats.
- **Deploy scripts hardcoded Pi IP** — `deploy_to_pi.sh` and
  `deploy_to_pi_native.sh` had hardcoded IP and password. Now read from
  `PI_HOST`/`PI_PASSWORD` env vars or `.env` file, with clear error if unset.

### Added
- **`build_and_deploy.sh`** — Single script to build frontend + backend, run
  tests, and optionally deploy. Supports `--deploy` and `--skip-fe` flags.
- **15 new unit tests** (309 total) — Session gap splitting (3), CSL/EVE map
  assignment (3), ezShare HTML parser firmware compatibility (9).

## [3.2.4] - 2026-04-08

### Fixed
- **Config loader ignores source/db type** — `BurstCollectorService` always
  defaulted to ezShare + PostgreSQL regardless of `config.json`. Root causes:
  `std::to_string().c_str()` dangling pointer UB in env var bridge, `DB_TYPE`
  env var never set, and `BurstCollectorService` hardcoded `DatabaseService`
  (PostgreSQL) instead of using `IDatabase` interface.
- **MQTT connects even when disabled** — `mqtt.enabled: false` in config now
  fully skips MqttClient creation. All `DataPublisherService` methods are
  null-safe (8 unit tests).
- **STR.edf not found in local mode** — `local_dir` points to DATALOG but
  STR.edf lives one level up at the SD root. Now checks parent directory first
  with case-insensitive fallback.
- **Windows MSVC** — `setenv` -> `_putenv_s`, `localtime_r` -> `localtime_s`.

### Added
- **Hot-reload config from web UI** — Settings page changes take effect on the
  next burst cycle without restart. Dirty-flag mechanism compares config
  snapshots and reinitializes only changed clients (DB, MQTT, LLM, source).
- **MySQL/MariaDB backend** — `database.type: "mysql"` now wired into
  `BurstCollectorService` via `IDatabase`. Docker image includes `libmariadb3`.
  CMake falls back to `mariadb` pkg-config when `mysqlclient` is unavailable.
- **STR.edf archival** — HTTP mode archives downloaded STR.edf to
  `CPAP_ARCHIVE_DIR` for persistence across container restarts.

### Changed
- **Renamed sleeplink -> cpapdash** — `SleeplinkBridge.h` -> `CpapdashBridge.h`,
  include paths `sleeplink/parser/` -> `cpapdash/parser/`, namespace
  `sleeplink::parser` -> `cpapdash::parser`, CMake target `sleeplink_parser`
  -> `cpapdash_parser`.
- **DatabaseService inherits IDatabase** — all methods marked `override`,
  `rawConnection()` returns `void*` (typed accessor via `pgConnection()`).
- **CI** — enabled `-DBUILD_WITH_MYSQL=ON` in Linux build + tests.

## [3.2.1] - 2026-04-02

### Fixed
- **Local mode crash** — `processSTRFile()` dereferenced null `ezshare_client_`
  when running with `CPAP_SOURCE=local`, causing a segfault after session
  completion. Now reads `STR.EDF` directly from the local directory.
- **Windows MSVC linker error** — added `PAHO_MQTTPP_IMPORTS` compile definition
  to `hms_mqtt` target so `mqtt::message::EMPTY_STR` resolves correctly from
  the Paho DLL.
- **GHCR package visibility** — CI workflow now sets the container package to
  public after Docker push (fixes unauthorized/404 on `docker pull`).

### Changed
- **Database schema files** — updated `scripts/schema.sql` (PostgreSQL) to v3.2.0
  with all 9 tables. Added `scripts/schema_sqlite.sql` and
  `scripts/schema_mysql.sql` reference files.

## [3.2.0] - 2026-04-02

### Added
- **Full OSCAR/SleepHQ-grade charting** — 12 overnight signal charts with
  per-minute resolution from `cpap_breathing_summary` and `cpap_calculated_metrics`:
  Flow Rate (with min/max band), Pressure (with min/max band), Mask Pressure,
  Leak Rate, Flow Limitation, Snore Index, Respiratory Rate, Tidal Volume,
  Minute Ventilation, I:E Ratio, EPR Pressure, Target Ventilation.
- **SpO2 and Heart Rate charts** — downsampled from per-second `cpap_vitals`
  (auto-hidden when no pulse oximeter data).
- **OSCAR-style overview + detail layout** — clickable thumbnail grid expands
  each signal into a large zoomable detail panel.
- **Time-range controls** — 30m / 1h / 2h / All range buttons with a slider
  to pan through the night. Mouse wheel zoom and drag pan via
  `chartjs-plugin-zoom`.
- **Event markers** — respiratory events (OA, CA, H, RERA) overlaid as colored
  dashed vertical lines on waveform charts.
- **Event distribution doughnut chart** on session detail page.
- **Date navigation** — prev/next day buttons and date picker on session detail.
- **Dashboard: 7 new charts** (was 2, now 9):
  - Pressure Trend (P50/P95, 30 days)
  - Leak Trend (L50/L95, 30 days)
  - Event Breakdown (stacked bar: OA/CA/H/RERA, 30 days)
  - Respiratory Trends (dual-axis: Resp Rate, Min Ventilation, Tidal Volume)
  - Cheyne-Stokes Respiration (bar chart, minutes per night)
  - EPR Level (stepped line, 30 days)
  - Therapy Mode card (CPAP/APAP/ASV/ASVAuto)
- **Mode-based chart visibility** — CSR and EPR charts hidden in CPAP mode;
  Target Ventilation only shown in ASV/ASVAuto mode (7/8). Mode sourced from
  `cpap_daily_summary.mode`.
- **3 new REST API endpoints**:
  - `GET /api/sessions/{date}/signals` — column-oriented per-minute signal data
  - `GET /api/sessions/{date}/vitals?interval=N` — server-side downsampled SpO2/HR
  - `GET /api/sessions/{date}/events` — event markers for chart annotations
- **2 new trend metrics**: `events` (oai/cai/hi/rin) and `respiratory`
  (resp_rate_50/tid_vol_50/min_vent_50) for `/api/trends/{metric}`.
- **4 database indexes** on timestamp columns for `cpap_calculated_metrics`,
  `cpap_vitals`, `cpap_breathing_summary`, and `cpap_events`.
- **Reusable SignalChartComponent** — standalone Angular component for
  Chart.js line charts with dark theme, annotation support, and configurable
  height/scales.
- **chart-helpers.ts** — utility functions for timestamp formatting, event-to-
  annotation conversion, dataset factory, and fill band generation.

### Added (Live Session Support)
- **Live session indicator** on sessions list — pulsing green "LIVE" badge with
  running duration for in-progress sessions. List auto-refreshes every 30s.
- **Live session detail view** — green banner showing start time, running
  duration (updates every 10s), and "Refreshing every 65s" indicator.
  Signal charts auto-refresh via 65s polling to match the burst collection
  interval. Polling stops automatically when session completes.
- **Sessions query** now includes in-progress sessions (removed
  `session_end IS NOT NULL` filter from sessions, signals, vitals, events).

### Dependencies
- Added `chartjs-plugin-annotation` (event markers on charts)
- Added `chartjs-plugin-zoom` (wheel zoom, pinch zoom, drag pan)

## [3.1.1] - 2026-03-29

### Fixed
- **Settings page**: Add all 4 source modes to dropdown (ezShare WiFi SD, Local Directory, Fysetc Poll HTTP, Fysetc MQTT) — previously only ezShare and Local were listed.

## [3.1.0] - 2026-03-29

### Added
- **Fysetc HTTP poll client**: New data source mode `fysetc_poll` replaces the
  MQTT-based FysetcReceiverService with a diff/ack HTTP protocol. The Fysetc
  device announces itself via `POST /fysetc/announce`, then hms-cpap polls it
  periodically for new/changed EDF files. Avoids SD bus corruption caused by
  WiFi radio interference during MQTT transmit.
- **FysetcHttpClient**: HTTP client for the 5 poll server endpoints
  (`/init`, `/poll`, `/file`, `/ack`, `/api/status`) with SD-busy retry logic.
- **FysetcPollService**: Worker thread with state machine
  (wait for announce -> init -> poll -> fetch -> ack -> session processing).
  Session completion detected via 2 consecutive stable polls.
- **`POST /fysetc/announce` endpoint**: Drogon route on port 8893 that receives
  the Fysetc device's IP and triggers the poll loop.
- **`fysetc_file_offsets` DB table**: Byte-exact offset persistence for
  incremental EDF transfers. Auto-migrated on connect.

## [3.0.1] - 2026-03-29

### Fixed
- **Cross-compile SEGV in getMetricsForDateRange**: Weekly/monthly AI summaries
  crashed with SIGSEGV (signal 11) when the binary was cross-compiled for ARM
  (Pi Zero 2 W). Root cause: pqxx `field::as<T>()` template instantiations in
  `libpqxx.so` (compiled natively on Pi) had ABI incompatibilities with code
  generated by the x86 cross-compiler — both GCC 14.2.0 but targeting different
  architectures. The cross-compiled code jumped to NULL (address 0x00000000) when
  calling into libpqxx's template code. Fix: bypass pqxx for result parsing in
  `getMetricsForDateRange`, using `libpq` C API directly (`PQexec`, `PQgetvalue`,
  `PQgetisnull`) with `stoi`/`stod` for type conversion. Debugged by reproducing
  the SEGV locally under QEMU user-mode emulation (`qemu-arm-static -L sysroot`).
- **AppConfig burst_interval default**: Changed from 300s to 65s. The v3.0.0
  `config.json` auto-generation overwrote the systemd `BURST_INTERVAL=65` env var,
  silently extending session detection from ~2 min to ~10 min.
- **Config.json as single source of truth**: Added `AppConfig::applyEnvFallbacks()`
  so env vars fill empty config fields on first run. Auto-detects PostgreSQL from
  `DB_HOST`/`DB_NAME` env vars, auto-enables MQTT from `MQTT_BROKER`.
- **Misleading log messages**: "Re-downloading" changed to "Checking" for session
  discovery (no download occurs during checkpoint comparison).

### Added
- **15 unit tests** for AppConfig precedence, env var fallback, and auto-detection
  logic (251 total).
- **QEMU-based local testing**: Cross-compiled ARM binaries can be tested on x86
  with `qemu-arm-static -L sysroot ./build-arm/hms_cpap`. Sysroot refreshed via
  rsync from Pi.

## [3.0.0] - 2026-03-28

### Added
- **Drogon Web Server**: hms-cpap now serves a REST API + Angular SPA on
  port 8893 alongside data collection. 12 endpoints: /health, /api/dashboard,
  /api/sessions, /api/sessions/:date, /api/daily-summary, /api/trends/:metric,
  /api/statistics, /api/summaries, GET/PUT /api/config, POST /api/setup,
  GET /api/config/test-ezshare.
- **Multi-database support**: IDatabase interface with 3 backends:
  - SQLite (default, embedded, zero dependencies)
  - MySQL/MariaDB (optional, compile-time flag BUILD_WITH_MYSQL)
  - PostgreSQL (optional, compile-time flag BUILD_WITH_POSTGRESQL)
  Runtime DB selection via config.json `database.type` field.
- **SqlDialect helpers**: Header-only SQL dialect functions for portable
  queries across all 3 databases (round, sleepDay, daysAgo, castDate, etc.)
- **AppConfig**: JSON-based config system at ~/.hms-cpap/config.json,
  replaces environment variables. Load/save/toJson with password redaction.
  Env var bridge for backward compatibility with existing services.
- **QueryService**: Multi-DB query service using IDatabase::executeQuery()
  + SqlDialect. All 7 data queries work across SQLite/MySQL/PostgreSQL.
- **cpap_summaries table**: Persists all AI-generated daily/weekly/monthly
  summaries with metrics for future UI display.
- **Angular SPA** (separate repo hms-cpap-ui): Dashboard with Chart.js
  (AHI trend + usage bars), sessions table, session detail with events,
  settings page (5 collapsible sections), setup wizard (3-step first-run).
- **deploy_to_pi_native.sh**: Native Pi build script (workaround for
  cross-compiler codegen bug).

### Changed
- BUILD_WITH_WEB replaces BUILD_WITH_HEALTHCHECK (default ON).
- Structured markdown LLM prompts for daily/weekly/monthly summaries.
- Weekly/monthly auto-trigger days configurable via WEEKLY_SUMMARY_DAY
  and MONTHLY_SUMMARY_DAY env vars.
- Dockerfile: added libsqlite3, BUILD_WITH_WEB=OFF until Drogon in image.

### Fixed
- **Session resume SEGV**: reopenSession() clears session_end when
  checkpoint files resume growing after mask re-wear.
- **Cross-compiler SEGV**: arm-linux-gnueabihf-g++ 14.2.0 produces
  NULL zview pointers for new DatabaseService methods. Native Pi build
  works perfectly. Root cause: cross-compiler codegen bug (not ABI mismatch).

## [2.1.0] - 2026-03-28

### Added
- **Weekly and monthly AI summaries**: LLM-generated trend analysis over
  7 or 30 days. Includes per-night breakdown, period averages, compliance
  rate, best/worst nights, and actionable recommendations.
- `DatabaseService::getMetricsForDateRange(days_back)` — queries per-night
  metrics across a date range (one row per sleep-night, oldest first).
- `DataPublisherService::publishRangeSummary(period, summary)` — publishes
  to `cpap/{device}/weekly/summary` or `cpap/{device}/monthly/summary`.
- HA discovery for weekly and monthly summary sensors (calendar-week/month icons).
- MQTT commands for on-demand generation:
  - `cpap/{device}/command/generate_weekly_summary` (default 7 days)
  - `cpap/{device}/command/generate_monthly_summary` (default 30 days)
  - Both accept optional JSON payload `{"days": N}` to override the range.
- Auto-trigger: weekly summary configurable via `WEEKLY_SUMMARY_DAY` env
  (0=Sun..6=Sat, default 0), monthly via `MONTHLY_SUMMARY_DAY` (default 1).
- `SummaryPeriod` enum (DAILY, WEEKLY, MONTHLY) in CPAPModels.
- `sleep_day` field on `SessionMetrics` for date-labeled range queries.
- `deploy_to_pi_native.sh` — builds natively on Pi instead of cross-compiling.
- Worker-thread queueing for MQTT-triggered range summaries (pqxx thread safety).
- n8n workflows: CPAP Weekly Summary - Discord (`detNx9TfVvp5lxnN`),
  CPAP Monthly Summary - Discord (`IDne0iIIay0dNakU`). Both active.
- 9 unit tests for range summary logic (236 total).

### Fixed
- **Cross-compiler SEGV**: Any new `DatabaseService` method SEGVs when
  cross-compiled with `arm-linux-gnueabihf-g++` 14.2.0 on x86, but works
  fine when built natively on Pi with the same GCC version. Same headers,
  same libs, same ABI — suspected cross-compiler codegen bug. Workaround:
  use `deploy_to_pi_native.sh` for Pi deployments.

## [2.0.4] - 2026-03-28

### Fixed
- **Second summary not sent after mask re-wear**: When user removes mask
  (session completes, summary sent), puts mask back on (checkpoint files
  grow), then removes mask again, the second completion never fired.
  Root cause: `session_end` was set on first completion but never cleared
  when files resumed growing. Added `reopenSession()` to clear `session_end`
  back to NULL when checkpoint files change on an already-completed session.

### Added
- `DatabaseService::reopenSession()` — clears `session_end` when a completed
  session resumes (checkpoint files grow again after mask re-wear).
- 5 unit tests for the completed → resumed → completed cycle (227 total).

## [2.0.3] - 2026-03-26

### Fixed
- **LLM summary never firing on session completion**: `saveSession()` UPSERT
  was writing `session_end` from EDF-parsed data on every re-download cycle.
  When files finally stabilized, `markSessionCompleted()` found `session_end`
  already set (IS NULL guard failed), returned false, and `generateAndPublishSummary()`
  was never called. Fixed by removing `session_end` from the UPSERT entirely —
  `markSessionCompleted()` is now the sole writer of `session_end`.

## [2.0.2] - 2026-03-25

### Fixed
- **Session completion summary not firing**: After parser refactoring (v2.0.1),
  the post-parse COMPLETED branch was dead code (parser always returns
  IN_PROGRESS). Summary generation now fires correctly from the checkpoint
  path when file sizes stop changing.
- **Local mode missing summary**: Local source mode checkpoint never called
  `generateAndPublishSummary()`. Added full completion sequence matching
  ezShare mode, plus `newly_completed` guard to prevent repeated marking.
- **STR data missing from LLM summary**: `processSTRFile()` was called before
  summary generation but STR record was not passed. Now passes latest STR
  record to enrich the summary with ResMed official daily metrics.

### Changed
- `publishMqttState` / `publishRealtimeState` simplified: always publishes
  `in_progress` + `session_active=ON` during parsing. Completion status
  is handled by `publishSessionCompleted()` from the checkpoint path.
- Removed dead `if (status == COMPLETED)` branch from post-parse flow.

## [2.0.1] - 2026-03-25

### Fixed
- **Multi-BRP session_start override bug**: Flow detection in `parseBRPFile`
  unconditionally overwrote `session_start` on each checkpoint. For multi-BRP
  sessions parsed after-the-fact (both files present), the second BRP's EDF
  start time replaced the first, causing a timestamp mismatch with discovery
  (which uses the first BRP's filename). This made `sessionExists()` fail on
  every cycle, triggering an infinite re-download/re-parse/re-publish loop.
  Fix: removed flow-based session boundary overrides entirely. Session
  timestamps now come from the filename (session_start) and EDF data
  (session_end = last BRP's start + data duration). Session completion is
  determined by the burst cycle's checkpoint size comparison, not the parser.
- **session_end NULL for archived sessions**: The parser's IN_PROGRESS/staleness
  logic (`records=-1` in EDF header) was nullifying `session_end` after it was
  correctly calculated from EDF data. Removed staleness check from parser —
  session completion is the burst cycle's responsibility.

### Changed
- Parser no longer sets session status (always defaults to IN_PROGRESS).
  Session completion is determined by the burst cycle (checkpoint file sizes
  stop changing), not the parser. EVE file is not reliable for this since
  it gets updated during the session.
- `session_end` is always calculated as last BRP's EDF header start time +
  (actual_records * record_duration), giving correct results for both single
  and multi-BRP sessions.

## [2.0.0] - 2026-03-24

### Added
- **PLD file parsing**: Machine's own 2-second summaries now parsed for all
  users. Extracts mask pressure, EPR/EPAP pressure, leak rate, respiratory
  rate, tidal volume, minute ventilation, snore index, and flow limitation.
  PLD values overwrite BRP-derived estimates (machine calculations are
  authoritative).
- **SA2 file support**: ResMed ASV and newer devices use `_SA2.edf` instead
  of `_SAD.edf` for oximetry data. Both recognized via `isOximetryFile()`.
- **ASV device support**: Full support for ResMed AirCurve ASV (Mode=7/8).
  Extracts ASV settings from STR.edf (S.AV.EPAP, pressure support min/max,
  S.AA.* for ASVAuto) and target percentiles (TgtIPAP, TgtEPAP, TgtVent).
- **ASV target ventilation**: PLD `TgtVent.2s` channel parsed on ASV devices,
  published via MQTT. NULL on CPAP/APAP devices.
- **New MQTT sensors** (57 total, up from 48): `current_mask_pressure`,
  `current_snore`, `current_target_ventilation` (realtime); `avg_mask_pressure`,
  `avg_epr_pressure`, `avg_snore`, `leak_p50`, `avg_target_ventilation`,
  `therapy_mode` (historical).
- **LLM summary enriched**: AI summary now includes mask pressure, EPR, snore,
  leak percentiles, and ASV-specific data (target ventilation, pressure support).
- **DB schema v2.0.0**: Auto-migration adds PLD/ASV columns to session_metrics
  and calculated_metrics tables.
- **37 new tests** (210 total): PLD parser, SA2 parser, ASV STR parser,
  ASV integration E2E, session discovery SA2.
- **Test fixtures**: Real ASV data from community contributor.

## [1.9.0] - 2026-03-18

### Added
- **Agentic AI module**: Conversational CPAP therapy assistant via MQTT
  - Natural language queries: `cpap/{device}/agent/query` -> `agent/response`
  - LLM tool-use loop with 7 read-only SQL tools (sessions, daily summary,
    trends, comparisons, vitals, statistics)
  - Multi-provider support: OpenAI, Ollama, Anthropic, Gemini for chat;
    separate Ollama endpoint for embeddings (nomic-embed-text 768-dim)
  - pgvector conversation memory: multi-turn context, summary embeddings,
    similar conversation retrieval, long-term fact storage
  - Parallel sub-agents: memory search runs concurrently with tool-use loop
  - Retained status topic: `cpap/{device}/agent/status` (idle/processing/error)
  - 26 unit tests (AgentTools, AgentMemory, AgentService with mock LLM)
  - 4 DB integration tests (schema, conversations, messages, expiry cleanup)
- **hms-shared v1.6.5**: Pinned tag with `generateWithTools()`, `embed()`,
  `toVectorLiteral()`, `DbPool` connection pool
- **Agent schema**: `scripts/agent_schema.sql` (3 tables: agent_conversations,
  agent_messages, agent_memory with pgvector indexes)
- **AGENT_ENABLED env var**: Opt-in activation (default false)

## [1.8.2] - 2026-03-18

### Added
- **force_complete MQTT command**: `cpap/{device}/command/force_complete` to
  manually mark a stuck session as completed, publish status, process STR,
  and generate LLM summary.
- **force_completed DB flag**: Sessions flagged `force_completed=TRUE` are
  skipped entirely in burst cycles (no download, no parse, no MQTT overwrite).
  Prevents parser's growing flag from undoing the force_complete.
- **Auto-migration**: `force_completed` column added to `cpap_sessions` on
  connect via `ALTER TABLE ADD COLUMN IF NOT EXISTS`.

## [1.8.0] - 2026-03-15

### Added
- **InsightsEngine**: Automated therapy trend analysis published to MQTT
  - AHI trend (30-day vs prior period comparison)
  - Leak-AHI correlation (median split, actionable if >0.5 AHI diff)
  - Pressure trend (auto-adjusting direction detection)
  - Therapy compliance (4-hour threshold, usage frequency)
  - Best vs worst night comparison with all metrics
  - 7-day rolling summary
  - MQTT topic: `cpap/{device_id}/insights/state` (retained JSON array)
  - HA discovery sensor: `therapy_insights` with JSON attributes
  - On-demand: `cpap/{device_id}/command/regenerate_insights`
- **MQTT discovery on startup**: Discovery messages now published on service
  start, not just on HA restart. Prevents missing entities after deploy.

### Fixed
- **Session summary**: Published as JSON `{"summary": "..."}` instead of plain
  text to work within HA's 255-char state limit. Full text accessible via
  `summary` attribute. Dashboard uses `state_attr()`.
- **regenerate_insights**: Auto-downloads STR.edf if cache is empty (e.g. after
  service restart with no new sessions).

## [1.7.0] - 2026-03-15

### Added
- **FysetcReceiverService**: New `CPAP_SOURCE=fysetc` mode that receives EDF data
  from FYSETC SD WiFi Pro via MQTT push instead of polling ezShare over HTTP.
  Manifest-driven protocol where hms-cpap controls file management:
  - `sync/request` + `sync/response` for realtime delta sync during therapy
  - `manifest` from FYSETC lists all files after therapy, hms-cpap diffs and
    sends `cmd/fetch` for missing/incomplete files (CSL, EVE, etc.)
  - `cmd/rescan` forces FYSETC to publish manifest + STR (sent on startup)
  - Base64-encoded chunks written to disk at correct byte offsets
  - STR.edf fetched from SD root (not in date folders) for daily therapy summary
  - Full session processing pipeline: EDFParser, DataPublisher, DB, nightly metrics
  - LLM session summary generation (same as ezShare mode, requires `LLM_ENABLED=true`)
  - Upload retry after bus yield interruption (`s_upload_pending` flag)
- **BRP-validated therapy detection** (FYSETC firmware): Bus activity alone no longer
  triggers session ON. Prescan must confirm BRP file exists -- prevents false
  positives from CPAP boot, settings changes, or STR updates.
- **8 new unit tests** for FysetcReceiverService: sync response, chunk write (new file
  + append at offset), manifest diff logic, root file handling, base64 round-trip.
- **Documentation**: `docs/FYSETC_RECEIVER.md` -- comprehensive protocol docs, MQTT
  topics, ResMed write timing, FSM states, configuration, ezShare vs FYSETC comparison.

### Changed
- `POST_THERAPY_IDLE_SEC` reduced from 120s to 65s -- empirically verified that
  STR.edf is written ~50s after mask-off, and BRP write interval is exactly 60s.
- FYSETC `session_active=OFF` deferred until post-therapy upload + manifest complete,
  ensuring hms-cpap has all data before triggering session processing.
- FYSETC FSM: new `FETCHING` state for `cmd/fetch` fulfillment with yield safety.
  New `fsm_on_rescan_request` + `fsm_on_fetch_request` callbacks from MQTT.

## [1.6.1] - 2026-03-14

### Fixed
- **LLM prompt missing pressure data**: `avg_pressure` was never queried from DB —
  data lives in `cpap_breathing_summary` but `getNightlyMetrics()` only joined
  `cpap_calculated_metrics`. Added LEFT JOIN on `cpap_breathing_summary` to pull
  avg/min/max pressure. Also: omit pressure/leak lines entirely when data is
  unavailable instead of showing `0.00 cmH2O`.

### Changed
- LLM prompt now includes min/max pressure and max leak rate for richer context.

## [1.6.0] - 2026-03-14

### Added
- **On-demand summary regeneration via MQTT**: Publish any message to
  `cpap/{device_id}/command/regenerate_summary` to refire the LLM session summary
  for the latest completed session. Queries DB for most recent session metrics and
  calls the configured LLM provider. Requires `LLM_ENABLED=true`.
- **10 new tests** (125 total): 7 unit tests for regeneration decision logic
  (happy path, no-sessions, no-metrics, LLM-disabled, MQTT-down, topic format,
  payload-ignored) and 3 E2E integration tests (MQTT command -> DB query -> summary
  publish round-trip, empty-DB graceful abort).
- **OpenAI GPT-5.2 support**: Updated hms-shared to v1.5.1 which uses
  `max_completion_tokens` instead of deprecated `max_tokens` for OpenAI chat
  completions (required by GPT-5.2+).

### Fixed
- **DST bug in `getLastSessionStart()`**: `std::tm` initialized with `tm_isdst=0`
  caused `mktime` to interpret timestamps as standard time during DST, shifting
  session lookups by 1 hour. Fixed with `tm_isdst=-1` (auto-detect), matching
  every other `mktime` call in the codebase.

## [1.5.2] - 2026-03-14

### Fixed
- **Session completion not firing**: v1.5.1 fix was too aggressive — when the latest
  session's files stopped growing, it was skipped entirely (no download = no parse = no
  completion path). Now completion fires in the unchanged-files path, gated on two guards:
  `newly_completed` (DB dedup, fires once) AND `is_most_recent` (by timestamp, not scan order).
  Old sessions never trigger completion actions.

### Added
- **9 completion decision unit tests**: covers latest-first-time, latest-already-done,
  old-session, single-session, scan-order-independence, multi-session dedup, second-cycle
  no-fire scenarios.

## [1.5.1] - 2026-03-14

### Fixed
- **Session active bug**: Old completed sessions were calling `publishSessionCompleted()`
  every burst cycle, setting `session_active=OFF` even when a newer session was actively
  running. Now unchanged sessions only update the DB without touching MQTT state, and
  session completion is gated on `status == COMPLETED` after parsing.

## [1.5.0] - 2026-03-14

### Added
- **LLM session summaries**: After each CPAP session completes, generates a
  natural language summary via configurable LLM provider (Ollama, OpenAI,
  Gemini, or Anthropic Claude). Summary published to MQTT as retained message
  at `cpap/{device_id}/daily/session_summary` with HA MQTT discovery.
- **Multi-provider LLM client** from `hms-shared` library (`hms::LLMClient`):
  supports Ollama `/api/generate`, OpenAI `/v1/chat/completions`, Google Gemini
  `/v1beta/models/:generateContent`, and Anthropic `/v1/messages`.
- **Prompt template file** (`LLM_PROMPT_FILE`): customizable prompt with
  `{metrics}` placeholder substitution. Ships with default `llm_prompt.txt`.
- **Model eviction** (`keep_alive: 0`): Ollama unloads model from VRAM
  immediately after generating, freeing GPU memory between nightly sessions.
- **n8n workflow** (`C7VJL3y93XXNv8Cw`): MQTT trigger on session summary
  pushes notification to iPhone via Home Assistant.

### Configuration (env vars)
- `LLM_ENABLED` — `true`/`false` (default: `false`)
- `LLM_PROVIDER` — `ollama`, `openai`, `gemini`, `anthropic` (default: `ollama`)
- `LLM_ENDPOINT` — API base URL (default: `http://127.0.0.1:11434`)
- `LLM_MODEL` — model name (default: `llama3.1:8b-instruct-q4_K_M`)
- `LLM_API_KEY` — API key (not needed for Ollama)
- `LLM_PROMPT_FILE` — path to prompt template file
- `LLM_KEEP_ALIVE` — seconds to keep model loaded, 0 = evict (default: `0`)

### Changed
- CMakeLists.txt: added `hms_llm` static library from `hms-shared` (nlohmann_json + curl)
- DataPublisherService: 47 sensors (was 46), added `session_summary` to STR discovery

## [1.4.1] - 2026-03-07

### Changed
- **Session gap threshold**: Default changed from 2 hours to 1 hour (60 minutes),
  matching confirmed ResMed behavior (session ends 1 hour after last file close).
- **Configurable session gap**: New `SESSION_GAP_MINUTES` env var to override the
  default. Shown in startup configuration output.

## [1.4.0] - 2026-03-06

### Added
- **Local source mode** (`CPAP_SOURCE=local`): Run HMS-CPAP against a local directory
  instead of an ezShare WiFi SD card. Set `CPAP_LOCAL_DIR=/path/to/DATALOG` and the
  burst cycle reads EDF files from the filesystem (SMB mount, USB, manual SD copy).
  Same session grouping, change detection, parsing, DB, and MQTT pipeline — no ezShare
  or WiFi dongle needed. Docker example:
  ```
  docker run -v /mnt/sd-card:/data/DATALOG \
    -e CPAP_SOURCE=local -e CPAP_LOCAL_DIR=/data/DATALOG \
    -e DB_HOST=postgres -e MQTT_BROKER=mqtt \
    ghcr.io/hms-homelab/hms-cpap:latest
  ```
- `SessionDiscoveryService::discoverLocalSessions()` — static method for local filesystem
  session discovery with same date filtering and 48-hour recent session logic.
- Change detection in local mode uses file sizes from `std::filesystem::file_size()`
  stored in KB (same format as ezShare mode) for consistent comparison.

## [1.3.0] - 2026-03-06

### Added
- **`--reparse` CLI mode**: Re-parse sessions from local archive for a date range.
  Usage: `hms_cpap --reparse /mnt/public/cpap_data/DATALOG 2025-08-18 2025-09-09`.
  Scans date folders, groups files into sessions (same 2-hour gap logic), deletes
  old DB records (cascade), and saves freshly parsed data. Works with archived files
  that are no longer on the ezShare SD card.
- `SessionDiscoveryService::groupLocalFolder()` — static method for local filesystem
  session grouping (no EzShareClient dependency). Replicates the 2-hour gap splitting
  and CSL/EVE matching logic using `std::filesystem`.
- `DatabaseService::deleteSessionsByDateFolder()` — deletes sessions by `brp_file_path`
  date folder match with FK cascade to all child tables.

## [1.2.0] - 2026-03-06

### Added
- **STR.edf daily therapy summaries**: Parse ResMed STR.edf (81 signals, 1 record/day) for
  official AHI, mask timing, pressure/leak/SpO2 percentiles, device settings, and cumulative
  patient hours. Data unavailable from BRP/EVE/CSL parsing alone.
- **13 new MQTT sensors** under `cpap/{device_id}/daily/`: str_ahi, str_oai, str_cai, str_hi,
  str_rin, str_csr, str_usage_hours, str_mask_events, str_leak_95, str_press_95, str_spo2_50,
  str_patient_hours, ahi_delta (cross-validation of STR vs calculated AHI)
- **`cpap_daily_summary` PostgreSQL table**: 30-column schema with UPSERT for idempotent writes,
  JSONB mask_pairs with on/off timestamps
- **`--backfill` CLI mode**: `hms_cpap --backfill /path/to/STR.edf` parses and saves all records
- **Python backfill script** (`scripts/str_backfill.py`): Custom lenient EDF parser (pyedflib
  rejects ResMed's non-standard Physical Dimension), bulk upsert with deduplication
- **STR download in burst cycle**: Downloads STR.EDF from ezShare root via `downloadRootFile()`,
  saves last 7 days to DB, publishes latest to MQTT (non-fatal on failure)
- `EDFFile::findSignalExact()` for unambiguous signal lookup (MaskOn vs MaskOff)
- `EzShareClient::downloadRootFile()` for DATALOG root files
- 21 new tests: 11 STR parser tests, 10 integration tests (DB upsert, MQTT publish, end-to-end)
- HA discovery count updated from 33 to 46 sensors

### Fixed
- **`HAStatusOffline_DoesNotRepublish` test flaky failure**: Retained MQTT messages from prior
  runs were counted as new. Fix: drain retained messages before asserting.
- **DST-safe date computation in STR parser**: Uses calendar arithmetic (mktime with tm_mday
  overflow) instead of epoch + seconds, preventing 1-hour offset when crossing DST boundaries.

### Changed
- Project cleanup: removed stale files from root (async_client.cpp.o, mosquitto.conf,
  REPO_STRUCTURE.txt, CMakeLists.txt.backup), moved docs to docs/, ARM toolchain to cmake/,
  schema to scripts/

## [1.1.8] - 2026-03-06

### Fixed
- **Stale nightly metrics from previous nights overwriting current**: Old completed sessions
  (re-discovered every cycle via 48-hour window) published their nightly metrics to MQTT,
  overwriting current night's values. Fix: sleep-day comparison skips publish for sessions
  outside the current noon-to-noon window.
- **Nightly metrics only published on session completion**: Historical MQTT topics (usage hours,
  AHI, events) were never updated during an active session — only when "stopped" was detected.
  Fix: publish nightly metrics after every active session parse, so HA stays current in real time.

### Changed
- ARM cross-compilation uses toolchain file instead of CMakeLists.txt swap hack
- Removed hardcoded `/usr/local/include` from native CMakeLists.txt

## [1.1.7] - 2026-03-05

### Fixed
- **MQTT subscriptions lost after reconnect**: `set_connected_handler` restored `connected_` flag
  but did not re-subscribe topics (wiped by `clean_session=true`). The `homeassistant/status`
  subscription was silently lost, so HA restarts no longer triggered discovery republish. Fix:
  re-subscribe all stored callbacks in the connected handler.

## [1.1.6] - 2026-03-05

### Fixed
- **Historical metrics based on single BRP session instead of full night**: `publishHistoricalState()`
  was called with metrics from one BRP session only. Nights with multiple therapy periods (CPAP
  turned off and back on) showed partial duration and wrong AHI, because each BRP file creates its
  own session row. Fix: new `getNightlyMetrics()` aggregates all sessions in the same sleep day
  (noon-to-noon window), sums `duration_seconds` for total usage hours, uses `MAX` for event counts
  (all sessions share the same EVE file so events are identical), and recomputes AHI as
  `total_events / total_hours`. All historical MQTT metrics now reflect the full night's therapy.

## [1.1.5] - 2026-03-04

### Fixed
- **Historical MQTT never published on session completion**: When BRP files stopped changing
  (session complete), code called `publishSessionCompleted()` which only published
  `session_status=completed` and `session_active=OFF` — AHI, event counts, and all other
  historical metrics were never sent to Home Assistant. HA showed stale zeros for every session.
- Fix: on session completion, load `SessionMetrics` from DB via new `getSessionMetrics()` and
  call `publishHistoricalState(const SessionMetrics&)` before the status publish.

### Added
- `DatabaseService::getSessionMetrics()` — loads aggregated session metrics from
  `cpap_session_metrics` + `cpap_calculated_metrics` for MQTT republishing
- `DataPublisherService::publishHistoricalState(const SessionMetrics&)` — public overload
  that publishes historical MQTT topics directly from a metrics struct (no full CPAPSession needed)
- Unit tests: `SessionMetrics_DefaultValues_AreZero`, `SessionMetrics_PopulatedFromSession`,
  `PublishHistoricalState_PublishesAHIAndEvents`, `PublishHistoricalState_ZeroEvents_PublishesZeros`

## [1.1.4] - 2026-02-25

### Fixed
- **Docker build**: Added missing build dependencies (libjsoncpp-dev, libpqxx-dev, Paho MQTT C/C++ from source)
- **libpqxx 6.x compatibility**: Replaced `conn_->close()` with `conn_.reset()` in DatabaseService (close() is protected in libpqxx 6.x)

### Added
- `.dockerignore` to exclude sysroot (220 MB), build dirs, and unnecessary files from Docker context
- `curl` in runtime image for health check endpoint

### Changed
- Restored native CMakeLists.txt (was accidentally overwritten with ARM cross-compilation config)
- Docker image reduced to 99 MB with proper dependency management

## [1.1.3] - 2026-02-21

### Fixed
- **Event Metrics Publishing**: Fixed obstructive apneas, central apneas, and hypopneas not being published to Home Assistant
- **Database Schema**: Added missing event count columns to `cpap_session_metrics` table (obstructive_apneas, central_apneas, hypopneas, reras, clear_airway_apneas)
- **Data Integrity**: Event counts now correctly stored and retrieved from PostgreSQL database

### Added
- Unit tests for event counting in calculateMetrics() function
- Database migration script for backfilling event counts from cpap_events table
- Tests for mixed event types, high AHI scenarios, and zero-event sessions

### Changed
- Updated DatabaseService.insertSessionMetrics() to include all event type columns
- Improved SessionMetrics structure documentation

## [1.1.2] - 2026-02-15

### Fixed
- **MQTT auto-reconnection**: Fixed `connected_` flag not updating after successful auto-reconnect, causing publish failures even when connection was restored
- **Home Assistant integration**: Added subscription to `homeassistant/status` topic to automatically republish discovery messages when Home Assistant restarts
- **Connection reliability**: Improved resilience to network interruptions and broker restarts

### Added
- Unit tests for MQTT reconnection behavior (`tests/mqtt/test_MqttClient.cpp`)
- Unit tests for Home Assistant status subscription (`tests/services/test_DataPublisherService.cpp`)

## [1.1.1] - 2026-02-13

### Added
- Pi Zero 2 W support and optimization
- Improved session grouping logic
- Health check HTTP endpoint
- 52 unit tests with 95% coverage

### Changed
- Migrated from hardcoded config to environment variables
- Optimized memory footprint (6.5 MB)
- Improved EDF parsing performance

### Fixed
- Session boundary detection accuracy
- File locking race conditions
- MQTT reconnection stability

## [1.1.0] - 2026-02-10

### Added
- Session discovery service
- PostgreSQL archival with file path tracking
- 34 comprehensive metrics (real-time + summary)
- Active file writing detection
- Burst collection mode

### Changed
- Refactored service architecture
- Improved error handling
- Enhanced logging

## [1.0.0] - 2026-02-03

### Added
- Initial C++ implementation
- ez Share WiFi SD client
- EDF file parser (OSCAR algorithms)
- MQTT publishing
- Home Assistant auto-discovery
- PostgreSQL storage
- Basic session grouping

### Features
- ResMed AirSense 10/11 support
- Bridge and direct WiFi modes
- Configurable collection intervals
- Systemd service integration

---

## Version History Summary

- **1.6.0** - On-demand LLM summary via MQTT, OpenAI GPT-5.2 support, DST bug fix
- **1.5.2** - Session completion fix (v1.5.1 too aggressive), 9 completion tests
- **1.5.1** - Session active bug fix (old sessions overwriting active state)
- **1.5.0** - LLM session summaries (Ollama/OpenAI/Gemini/Anthropic), n8n notifications
- **1.4.1** - Configurable session gap threshold (SESSION_GAP_MINUTES)
- **1.4.0** - Local source mode (CPAP_SOURCE=local), no ezShare needed
- **1.3.0** - --reparse CLI for local archive re-parsing
- **1.2.0** - STR.edf daily therapy summaries, 13 new MQTT sensors, backfill CLI, project cleanup
- **1.1.8** - Stale nightly metrics fix, ARM toolchain cleanup
- **1.1.4** - Docker build fixes, libpqxx 6.x compatibility
- **1.1.3** - Event metrics publishing fixes
- **1.1.2** - MQTT reconnection fixes, Home Assistant status subscription
- **1.1.1** - Pi Zero 2 W optimization, comprehensive testing
- **1.1.0** - Session discovery, archival, 34 metrics
- **1.0.0** - Initial release with core functionality

[Unreleased]: https://github.com/hms-homelab/hms-cpap/compare/v1.6.0...HEAD
[1.6.0]: https://github.com/hms-homelab/hms-cpap/compare/v1.5.2...v1.6.0
[1.5.2]: https://github.com/hms-homelab/hms-cpap/compare/v1.5.1...v1.5.2
[1.5.1]: https://github.com/hms-homelab/hms-cpap/compare/v1.5.0...v1.5.1
[1.5.0]: https://github.com/hms-homelab/hms-cpap/compare/v1.4.1...v1.5.0
[1.4.1]: https://github.com/hms-homelab/hms-cpap/compare/v1.4.0...v1.4.1
[1.4.0]: https://github.com/hms-homelab/hms-cpap/compare/v1.3.0...v1.4.0
[1.3.0]: https://github.com/hms-homelab/hms-cpap/compare/v1.2.0...v1.3.0
[1.2.0]: https://github.com/hms-homelab/hms-cpap/compare/v1.1.8...v1.2.0
[1.1.8]: https://github.com/hms-homelab/hms-cpap/compare/v1.1.4...v1.1.8
[1.1.4]: https://github.com/hms-homelab/hms-cpap/compare/v1.1.3...v1.1.4
[1.1.3]: https://github.com/hms-homelab/hms-cpap/compare/v1.1.2...v1.1.3
[1.1.2]: https://github.com/hms-homelab/hms-cpap/compare/v1.1.1...v1.1.2
[1.1.1]: https://github.com/aamat09/hms-cpap/compare/v1.1.0...v1.1.1
[1.1.0]: https://github.com/aamat09/hms-cpap/compare/v1.0.0...v1.1.0
[1.0.0]: https://github.com/aamat09/hms-cpap/releases/tag/v1.0.0
