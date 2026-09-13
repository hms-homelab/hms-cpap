# The layers

How hms-cpap is put together: the layers, what lives in each, and how a web
request and a collection burst travel through them. For the tables themselves
see [db.md](db.md).

```
  Angular UI (frontend/)              Home Assistant (MQTT)       SleepHQ, myAir, Ollama
        │ HTTP /api/*                          ▲                           ▲
        ▼                                      │                           │
  Controllers (src/controllers/)        Publishers (src/mqtt/,       Clients (src/services/*Client,
        │            │                  src/services/*Publisher*)   src/clients/)
        ▼            ▼                         ▲                           ▲
  QueryService   Services (src/services/) ─────┴───────────────────────────┘
  (src/web/)          │        ▲
        │             │        │ parsed entities (cpapdash-parser)
        ▼             ▼        │
  Database layer: IDatabase (include/database/IDatabase.h)
        │  SQLiteDatabase │ MySQLDatabase │ PostgresDatabase → DatabaseService
        ▼
  SQLite / MySQL / PostgreSQL
```

`src/main.cpp` builds every piece and wires them together; nothing else owns
the process.

## 1. Entities (the data model in memory)

The therapy entities are **not defined in this repo**. They come from the
shared `cpapdash-parser` library and are aliased into the `hms_cpap` namespace
by `include/parsers/CpapdashBridge.h`:

| Alias here | Parser type | What it is |
|---|---|---|
| `CPAPSession` | `ParsedSession` | One mask-on session with everything parsed from its EDFs: events, vitals, breathing summaries, breaths, per-minute metrics, `SessionMetrics`, settings. |
| `SessionMetrics` | `SessionMetrics` | The session's computed numbers (AHI and its kind, event counts, leak, pressures, SpO2/HR). |
| `CPAPEvent`, `CPAPVitals`, `BreathingSummary`, `Breath`, `DesatEvent` | the parser's sample and event types | The rows under a session. |
| `STRDailyRecord` | `STRDailyRecord` | One day of the machine's STR.edf, stamped at local noon. |
| `OximetrySession`, `OximetrySample`, `OximetryMetrics` | the parser's oximetry types | One O2 ring recording, from a `.vld` (`VLDParser`) or a Wellue CSV. |
| `SessionFileSet`, `SessionFileRef` | local (`CpapdashBridge.h`) | A session as discovered on the card: its date folder, prefix, files and sizes, before parsing. |

`include/models/CPAPModels.h` and `src/models/CPAPModels.cpp` are the
pre-library copies. The build excludes the `.cpp` (`CMakeLists.txt`,
`tests/CMakeLists.txt`); do not add to them.

hms-cpap's own entities are small structs next to the code that owns them:

- `include/database/IDatabase.h`: the equipment and cleaning records
  (`EquipmentType`, `EquipmentProfile`, `EquipmentItem`, `CleaningTaskType`,
  `CleaningTask`) and the read results (`OxiSummary`, `OxiRangeSummary`,
  `OxiNightlyPoint`, `RemoveNightResult`).
- `include/services/SyncFolderState.h`: `FolderLedger` (a DATALOG folder's
  transfer state), `FolderObservation`, `FolderTransition`, `NightState`.
- `include/utils/AppConfig.h`: `AppConfig`, the whole configuration.

The web layer does not map rows onto entities: it returns `Json::Value`
straight from SQL (section 4).

## 2. Database layer (the "db models")

**`IDatabase`** (`include/database/IDatabase.h`) is the one interface every
other layer talks to. It is organised by concern: sessions (`saveSession`,
`sessionExists`, `getLastSessionStart`, completion and force-complete,
`deleteSessionsByDateFolder`, `removeNight` / `removedNights` /
`restoreNight`), session files, checkpoint sizes, the STR and daily summary
(`saveSTRDailyRecords`, `aggregateDailySummaryFromSessions`), metrics reads,
LLM summaries, oximetry, equipment, cleaning, the folder ledger, and two
escape hatches: `executeQuery` (parameterised SQL returning JSON rows) and
`insertReturningId`.

Three implementations:

| Class | File | Notes |
|---|---|---|
| `SQLiteDatabase` | `src/database/SQLiteDatabase.cpp` | The default. One connection guarded by a recursive mutex. Timestamps as text. |
| `MySQLDatabase` | `src/database/MySQLDatabase.cpp` | Compiled with `-DBUILD_WITH_MYSQL=ON`. |
| `PostgresDatabase` | `src/database/PostgresDatabase.cpp` | A thin adapter: every call forwards to **`DatabaseService`** (`src/database/DatabaseService.cpp`), the original libpqxx implementation. |

`makeDatabaseFromConfig()` (`include/database/DatabaseFactory.h`) picks the
engine from `DB_TYPE` / `config.json`.
Each backend creates and migrates its own schema on `connect()`.

Dialect differences outside the backends go through `include/database/SqlDialect.h`
(`sql::param(i, dbType)` for placeholders, and friends), which is how
`QueryService` and the few raw-SQL services write one query for three engines.

**Rule:** a new table or column is added to all three backends and all three
`scripts/schema*.sql` files, with a test in `tests/database/` parameterised over
the engines (SQLite always runs; MySQL and PostgreSQL run when `MYSQL_TEST_*` /
`PGHOST` are set). A change that lands on one engine and not the others is this
codebase's most expensive recurring bug.

## 3. Services (src/services/)

The behaviour lives here. Grouped by what they do:

**Collecting from the machine**
- `BurstCollectorService`: the collection loop. Every burst it lists the source
  (ezShare over WiFi, a local card root, Fysetc sectors, Lowenstein, Sefam),
  downloads what changed, parses, stores, updates the folder ledger, writes the
  STR history, derives the daily summary, publishes to MQTT, and imports the
  ring's `.vld` files beside the card. Owns `OximetryService`.
- `SessionDiscoveryService`: groups card files into sessions. The one place
  session splitting is done; do not reimplement it.
- `SyncFolderState`: the pure SDD-008 rules for whether a night's files are all
  here (`advanceFolder`) and the night key (`strDayForSessionStart`).
- `RemovedNights.h`: the SDD-029 checks every re-ingest path uses.
- `PrismaIngestion`, `SefamIngestion`: Lowenstein and Sefam card formats.
- `FysetcSectorCollectorService`: rebuilds files from raw SD sectors streamed
  by the Fysetc bridge.
- `BackfillService`: re-reads a date range from the archive (the per-session
  Reparse and the Backfill page use it).
- `OximetryService` (the ring's live pull through the mule), `OximetryImport`
  (a `.vld` from the card folder or the upload).

**Publishing and exporting**
- `DataPublisherService`: sessions and nights to MQTT for Home Assistant,
  with discovery via `src/mqtt/DiscoveryPublisher`.
- `SupplyPublisher` / `SupplyStatus`, `CleaningPublisher` / `CleaningStatus`:
  equipment wear and cleaning due-times to MQTT. The `*Status` classes are pure
  logic, ported from the phone app so both compute the same dates.
- `SleepHqExportService` / `SleepHqClient`: raw card files to SleepHQ.
- `CpapDashSyncService`: optional mirror of equipment and cleaning to the
  CpapDash cloud.
- `ReportGeneratorService`, `GnuplotService`, `PdfRenderer`,
  `services/reports/`: PDF reports (POSIX builds only).

**Reading from elsewhere**
- `MyAirService` / `MyAirClient`: the patient's nights from ResMed myAir, for
  the comparison page.

**Intelligence**
- `InsightsEngine`: rule-based insights (AHI, leak, pressure, hours) over
  recent STR days.
- `SleepStageClassifier`, `LiveSleepStageRunner`, `MLTrainingService`, and the
  models in `src/ml/` (decision tree, random forest, HMM smoother, feature
  extraction): sleep stage inference.
- `src/agent/` (`AgentService`, `AgentTools`, `AgentMemory`): the LLM agent and
  its read-only data tools.

**Setup and environment**
- `SetupService` (what the first-run wizard needs), `PreflightService` (checks
  the configuration before anything starts), `DeviceDiscoveryService` (mDNS
  browse for Mule and Miner units).

## 4. Web query layer (src/web/)

`QueryService` answers every read the UI makes: dashboard, sessions list and
detail, daily summary, trends, statistics, signals, vitals, events, oximetry,
summaries, insights, the myAir comparison. It holds its own `IDatabase`
connection (a second one on MySQL/PostgreSQL, since the client libraries are
not thread-safe; the same one on SQLite) and runs dialect-aware SQL through
`executeQuery`, returning `Json::Value` rows as the API sends them. It also
folds the folder ledger into each night's `night_state` (live, complete,
partial). It keeps no cache.

`IngressBase` serves the UI under a Home Assistant Ingress prefix (SDD-021).

## 5. Controllers (src/controllers/)

Drogon HTTP controllers, thin by design: they validate the request, call a
service or `QueryService`, and shape the response. The logic they need lives in
services so it can be tested, because **the test binary excludes the
controllers** (and `src/web/`, `main.cpp`, the CLI and the PDF stack).

| Controller | Routes |
|---|---|
| `CpapController` | `/health`, `/api/dashboard`, `/api/sessions` (list, detail, signals, vitals, events, breaths, oximetry, sleep stages; `DELETE` removes a night; `force-complete`, `generate-summary`, `reparse`), `/api/daily-summary`, `/api/trends/{metric}`, `/api/statistics`, `/api/summaries`, `/api/events`, `/api/realtime`, `/api/config`, `/api/setup/*`, `/api/capabilities`, `/api/myair/*`, `/api/logs`, `/api/ml/*`, `/api/llm-prompt`, `/api/discover/devices`, `/api/sync/now`, `/api/backfill*`, `/api/insights`, `/api/oximetry/collect`, `/api/upload/*`, `/api/sleephq/export/{date}`, `/api/reports*` |
| `EquipmentController` | `/api/equipment*`, `/api/supplies` |
| `CleaningController` | `/api/cleaning*` |

Where a controller needs something only `main.cpp` can build (the backfill
service, a second database connection, the importer), `main.cpp` hands it a
static `std::function` hook: `backfill_trigger_`, `oxi_csv_import_`,
`cpap_zip_import_`, `night_remove_`, `night_restore_`, `ml_train_trigger_`.
The hook's body calls a tested function (for example `removeNightByDate` in
`RemovedNights.h`); the hook itself is wiring.

## 6. Parsers and clients

- `src/parsers/`: `Fat32Parser` (the Fysetc sector path). EDF, STR, VLD,
  Lowenstein and Sefam parsing is in `cpapdash-parser`; this repo's
  `EDFParser.cpp` is excluded from the build for that reason.
- `src/clients/`: the transports. `EzShareClient` (the ezShare card over WiFi,
  through the hms-mm bridge), `FysetcTcpServer` / `FysetcDataSource`, `O2RingClient` (HTTP to
  the ring mule) and `O2RingBleClient` (BlueZ). `IDataSource` and
  `IO2RingClient` are the seams the tests mock.

## 7. Frontend (frontend/)

Angular, standalone components, built with `npm run build` into
`frontend/dist`, which Drogon serves.

- `src/app/pages/`: one folder per page (dashboard, sessions, session-detail,
  compare, events, reports, equipment, settings, setup, upload, logs).
- `src/app/services/cpap-api.service.ts`: every HTTP call to `/api/*`.
- `src/app/models/`: the TypeScript shapes of the API responses.
- `src/app/i18n/`: translations, one JSON per page per language (en, es, fr,
  hu, pt). A key missing from any language fails the build.

## How a web request flows

`GET /api/sessions?limit=20`: Drogon routes it to `CpapController::sessions`,
which calls `QueryService::getSessions`, which runs one dialect-aware query
against the web connection and adds `night_state` from `listSyncFolders()`.
The JSON rows go back as they came out of the database.

A write, `DELETE /api/sessions/2026-08-23`: `CpapController::sessionRemove`
checks the date and calls the `night_remove_` hook, which runs
`removeNightByDate` → `IDatabase::removeNight` on the web connection, one
transaction, and returns the counts.

## How a burst flows

`BurstCollectorService::executeBurstCycle`, every `burst_interval` seconds:

1. Read the delta point (`getLastSessionStart`) and the removed nights.
2. List the source and let `SessionDiscoveryService` group the files into
   `SessionFileSet`s.
3. For each session: skip it if its night was removed or force-completed;
   download what grew (compared against `checkpoint_files`); parse it with
   `cpapdash-parser`; `saveSession`, `replaceSessionFiles`, then
   `aggregateDailySummaryFromSessions`.
4. `updateFolderLedgers`: advance each folder's SDD-008 ledger and arm the STR
   and sidecar debts when a folder settles.
5. `processSessionSummary`: parse the card's STR, write it minus removed nights
   (`saveSTRDailyRecords`), clear STR debts, re-derive the daily summary.
6. Import the ring's `.vld` files beside the card (local source). At the end of
   the cycle, mark archived nights that have files but no parsed session for
   SleepHQ (`markUnparsedNightsForExport`). MQTT publishing happens along the
   way, per stored session and per night.
