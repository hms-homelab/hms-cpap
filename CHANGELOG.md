# Changelog

All notable changes to HMS-CPAP will be documented in this file.

The format is based on [Keep a Changelog](https://keepachangelog.com/en/1.0.0/),
and this project adheres to [Semantic Versioning](https://semver.org/spec/v2.0.0.html).

## [Unreleased]

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
- `LLM_ENDPOINT` — API base URL (default: `http://192.168.2.5:11434`)
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
