# Changelog

All notable changes to HMS-CPAP will be documented in this file.

The format is based on [Keep a Changelog](https://keepachangelog.com/en/1.0.0/),
and this project adheres to [Semantic Versioning](https://semver.org/spec/v2.0.0.html).

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
