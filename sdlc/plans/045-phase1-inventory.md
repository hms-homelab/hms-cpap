# SDD-045 Phase 1: the inventory, with its evidence

> **Outcome, 2026-09-19.** Albin reviewed this and decided; the sweep is done.
> What was removed, what was kept and why, and the three findings that changed
> his earlier answers are recorded in §11 at the end.


Nothing here has been deleted. This is the report the plan says must be reviewed
before any deletion, one section per category, each candidate with what was
checked and what reaches it.

Two of the Phase 0 inventories were wrong and are corrected here (§7, §8); both
are marked with what was wrong with them.

## Baseline, 2026-09-19

| Area | Lines | Files |
|---|---|---|
| `src/` | 42,506 | 70 |
| `include/` | 11,448 | 90 |
| `tests/` | 42,278 | 100 |
| `desktop/` | 4,938 | 28 |
| `frontend/src/` | 18,662 | 114 |
| `scripts/` | 698 | 5 |
| `packaging/` | 441 | 3 |

## 1. C++ functions nothing calls

`cppcheck 2.21.0 --enable=unusedFunction` over `build/compile_commands.json`,
single-threaded (the check is silently disabled under `-j`), with pqxx and gtest
suppressions. 111 hits: 64 in third-party headers (ignored), 7 in the parser
library (§9), 40 here.

**The raw list is not usable on its own.** cppcheck could not parse the gtest
files, so every function whose only callers are `TEST_F` bodies came back as
"never used" -- which is most of the list. And the compile database covers the
main build only, so the `desktop/qt` Qt layer is invisible to it, making the pure
supervisor modules look dead as well. Every hit was therefore re-checked against
`src/`, `include/`, `tests/` and `desktop/` by reference.

### 1a. Dead: nothing anywhere calls these (7)

| Symbol | Where | Checked |
|---|---|---|
| `EzShareClient::downloadSession` | `src/clients/EzShareClient.cpp:383` | No caller in src, include, tests or desktop. The live download path is `BurstCollectorService::downloadSessionFiles`, which is its own implementation. `tests/clients/test_EzShareClient.cpp:691` already records it as uncovered. |
| `MyAirClient::fetchDevice` | `src/services/MyAirClient.cpp:676` | Declaration + definition only. |
| `SessionDiscoveryService::findLargestFile` | `src/services/SessionDiscoveryService.cpp:92` | Declaration + definition only. |
| `BurstCollectorService::getCurrentDateString` | `src/services/BurstCollectorService.cpp:1267` | Declaration + definition only. |
| `ConfigManager::getRequired` | `include/utils/ConfigManager.h:35` | Header-only, no caller. |
| `safeStd`, `linearSlope` | `src/ml/SleepStageFeatureExtractor.cpp:18,29` | File-local helpers, no caller in the file or anywhere else. |
| `PreflightReport::blocking()` | `desktop/qt/core/pure/PreflightReport.h:52` | No `.blocking()` call anywhere, including the Qt layer. Not to be confused with `blockingIssues()` in `FieldSpec.h`, which the settings dialog does call. |

### 1b. Live, flagged only because cppcheck could not see the caller (25)

Test seams called from `tests/` (`runBurstCycleForTest` 37 references,
`reloadConfigForTest`, `deviceIdForTest`, `deviceNameForTest`, `setSourceForTest`,
`setAppConfig`, `closeSettledOpenLocalNightsForTest`, `parseDirForNightForTest`,
`folderHasOneGroupForTest`, `buildRangeMetricsStringForTest`,
`injectDependenciesForTest`, `isDirtyForTest`, `setExportHookForTest`,
`resetForTest`), and the pure supervisor API called from the Qt layer
(`ConfigModel::loadFromApi`, `toPatch`, `toDiskJson`, `keepSecret`, `secretMode`,
`current`, `FieldSpec::restartReasons`, `settingsGroups`) or from inside its own
translation unit (`ConfigModel::changedPaths`, called by `toPatch`).

**No deletion here.** The finding is about the tooling, not the code: the CI gate
of D6 must run cppcheck over a compile database that includes `build-qt`, or it
will report this same list every run.

### 1c. Production code whose only caller is a test (9) -- D2 decisions

These are real: nothing in a shipped path reaches them. The question the SDD
poses is whether the rule they pin still lives elsewhere.

| Symbol | Tests | What the test pins, and where the rule lives now |
|---|---|---|
| `SetupService::canManageAutostart` | 2 | Pure predicate on `supervised`. The live path branches on the same flag inline. |
| `computeSupplyStatus(slot, ...)` | 13 | The slot-name overload. Production calls only `computeSupplyStatusForInterval`; the slot -> default-interval mapping the tests cover is inside this overload, so deleting it deletes that rule. |
| `BurstCollectorService::getLastBurstTime` | 4 | Worker-loop timestamp; the tests assert the loop updates it and `executeBurstCycle` does not. Nothing reads it in production. |
| `CpapDashSyncService::cursor` | 6 | Accessor over `loadState().cursor`, which the sync path does use. |
| `CpapDashSyncService::settings`, `setTransport`, `setStatePath` | 11 | Injection seams for the sync tests. `setTransport` and `setStatePath` are how the suite runs without a network or the real state file. |
| `SqlDialect::daysAgo` | 0 | The 17 `tests/` hits are unrelated local helpers (`daysAgoIso`, a lambda in `test_SupplyStatus.cpp`). This one is dead outright and belongs in 1a. |

Recommendation: `daysAgo` goes. `setTransport`, `setStatePath`, `settings` and
`isDirtyForTest` stay (they are the test harness, not dead production code).
`canManageAutostart`, `computeSupplyStatus`, `getLastBurstTime` and `cursor` are
Albin's call: each deletes a tested rule with no live caller.

### 1d. Unused helpers inside test files (3)

`verifyFileContent` (`tests/clients/test_EzShareClient.cpp:37`), `uniformObs`
(`tests/ml/test_hmm_smoother.cpp:19`), `partitionLbaOffset`
(`tests/parsers/test_Fat32Parser.cpp:194`). Test-local, no callers.

## 2. Routes nothing calls

70 routes declared. Four appear in no frontend file, no test, no doc and no
skill:

- `GET /api/equipment/cloud-sync`
- `GET /api/sessions/{date}/sleep_stages`
- `GET /api/sleep-stages/status`
- `GET /api/statistics`

Per D4 these are reported, not deleted: each is either an unwired feature or
dead, and that is a decision.

## 3. Translation keys

702 English keys across 13 fragments. 28 never appear literally in a template or
a `.ts` file. Only two families are built dynamically, and both were confirmed by
reading the construction site:

- `updateBanner.step.*` -- `update-banner.component.ts:30` and
  `settings.component.ts:838` concatenate `'updateBanner.step.' + flow.step`
- `reports.status.*` -- `reports.component.ts:88` builds
  `` `reports.status.${s}` `` from a fixed `KNOWN_STATUSES` list

That leaves **21 candidates**, none of them reachable by concatenation:

- `dashboard.myair.*` (8: `dataSource`, `difference`, `leak`, `maskOnOff`,
  `night`, `noCpapDash`, `noResMed`, `usage`) -- a superseded namespace. The live
  component `myair-compare.component.ts` is mounted from the dashboard and uses
  `myairCompare.*` throughout.
- `common.back`, `common.close`, `common.next`, `common.noData`, `common.retry`
- `dashboard.aiSummary.empty`, `dashboard.events.respiratoryEffort`,
  `dashboard.ml.stable`, `dashboard.pressure.average`,
  `dashboard.respiratory.title`
- `equipment.item.cancel`, `equipment.item.save`
- `settings.source.type`

Any removal drops the same keys from all five languages in one commit, so the
build's parity gate keeps passing.

## 4. Database schema (D5)

**This corrects the Phase 0 scan, which read `scripts/schema_sqlite.sql` -- a
mirror, not what runs -- and matched names naively.**

Extracted from the runtime creators instead (`SQLiteDatabase.cpp`,
`MySQLDatabase.cpp`, `DatabaseService.cpp`, `AgentMemory.cpp`, including every
`ALTER TABLE ... ADD COLUMN`): **26 tables, 287 columns.** Each matched against
every read and write in `src/`, `include/`, `tests/`, `frontend/src/` and
`scripts/`, excluding the creators themselves and the three schema mirrors.

**Two columns are referenced nowhere:**

| Column | Definition |
|---|---|
| `cpap_myair_records.fetched_at` | `TIMESTAMP DEFAULT NOW()` / `TEXT DEFAULT (datetime('now'))` / `DATETIME DEFAULT NOW()` |
| `cpap_removed_nights.removed_at` | `TIMESTAMP DEFAULT CURRENT_TIMESTAMP` / `TEXT DEFAULT (datetime('now','localtime'))` / `DATETIME DEFAULT NOW()` |

Both are audit timestamps the **database** fills, not the code. No code reads
them; every row still has a correct value. Dropping them buys a few bytes a row
and loses the ability to answer "when was this night removed" by hand.

**Recommendation: keep both, recorded as deliberate.** That leaves nothing to
drop, which means **this release does not have to touch user data at all** -- no
migration, no backup step, no three-engine idempotency tests. Phase 4 closes
here unless Albin wants them gone.

The other repo that could share a database is the cloud monolith, and it does
not: hms-cpap owns its SQLite/MySQL/PostgreSQL instance outright.

## 5. Scripts and packaging

Every file in `packaging/` is referenced by an installer, a unit file, CI or a
test. In `scripts/`, referenced by nothing outside themselves:

| File | Status |
|---|---|
| `scripts/build_frontend.sh` | no reference anywhere (`build_and_deploy.sh` builds the frontend inline) |
| `scripts/ezshare-monitor.sh` + `.service` + `.conf.example` | a self-contained trio; nothing else mentions them |
| `scripts/retrofit_bytes_to_kb.sql` | one-off migration, no reference |
| `scripts/add_reports_table.sql` | superseded by the runtime creators |
| `scripts/fix_session_ends.py`, `scripts/reparse_sessions.py` | one-off repair tools, referenced only by themselves |
| `scripts/str_backfill.py`, `scripts/agent_schema.sql` | referenced only from CHANGELOG.md |

One-off repair scripts are not dead code in the same sense as an uncalled
function: they are tools Albin may want on a bad day. Reported, not proposed for
deletion, except `build_frontend.sh` and `add_reports_table.sql`, which are
superseded by something that runs.

## 6. Frontend exports, components and services

`knip` (default and extended runs), `ts-prune` as a cross-check, and the Angular
compiler's own diagnostics; then every candidate checked by hand against
selectors, route targets, `templateUrl`/`styleUrls`, injection sites and config
files, because Angular reaches code by name.

**No orphan files at all**: no unused component, template, stylesheet, service,
guard, interceptor or production dependency. All 27 selectors resolve (12 through
routes, 1 through `index.html`, 14 through a parent template), all 11
`loadComponent` targets resolve to a real exported class, and all 14 template and
stylesheet files are bound.

knip flagged 9 items and 6 of them are live code that is merely `export`ed
without an outside importer (`eventColor`, `INDEX_DECIMALS`, `CleaningState`,
`CleaningStatus`, `EquipmentCategory`, `SupplyInfo`) -- each is used inside its
own module or reached as the type of a field on an interface that is imported.
Dropping the `export` keyword is the only safe reading; the symbols stay.
ts-prune's six extra hits are the same pattern and all live.

**Four things are provably dead:**

| Candidate | Evidence |
|---|---|
| `MetricCard` interface (`models/session.model.ts:1`) | The only occurrence in the repo is its own declaration. `MetricCardComponent` does not use it -- it declares five discrete `@Input()`s and has no `trend` field. |
| `IndexKind` type (`utils/index-kind.ts:19`) | Declaration only. Its sibling `HasIndexKind` deliberately types `index_kind?: string \| null`; the three importers take only the functions. It is load-bearing *documentation* of the SDD-024 contract, so removing it is a judgement, not an obvious win. |
| `RouterLink` in `SessionsComponent` | The compiler already says so: `NG8113: RouterLink is not used within the template of SessionsComponent`, the only NG8113 in the build. `routerLink` appears nowhere in that template. The `Router` service on the same import line is used. Both knip and ts-prune missed it. |
| `tsconfig.spec.json` + the `test`/`pretest` scripts | Nothing references `tsconfig.spec`; no `*.spec.ts` exists in `src`; `angular.json` has no `test` target; no karma, jasmine, vitest or jest in `package.json`. `npm test` would fail today. The README still advertises Vitest, which is stale scaffold text. |

Three are uncertain and stay: `prettier` (no automated caller, but `.prettierrc`
is tracked, so it is deliberate editor tooling), `@angular/compiler` (no source
reference, but it is the AOT peer the toolchain needs), and
`public/favicon.ico` (browsers request `/favicon.ico` whether or not anything
links it). `proxy-dev.json` and `proxy.conf.json` are referenced by no config and
are byte-equivalent duplicates of each other, usable only from a hand-typed
`ng serve --proxy-config`; one of the two could go.

## 7. Files compiled by no target -- the Phase 0 list was wrong

Phase 0 reported nine `desktop/qt` files as compiled by nothing. They are
compiled by `desktop/qt/CMakeLists.txt` in the separate `build-qt` tree, which is
absent from the main `compile_commands.json`. **No file is orphaned.** The
correction that matters is for D6: the gate needs both compile databases.

## 8. The union rule, and what the per-configuration runs would add

The Phase 1 analyses ran on macOS, so the obvious objection is that a symbol
called only from Windows or Linux code would look dead here. For the candidates
in 1a it does not apply, for two measured reasons:

1. **Every `.cpp` in `src/` is compiled in the macOS build.** Comparing
   `git ls-files` against `build/compile_commands.json`: 203 files compiled, and
   the only nine absent are the `desktop/qt` Qt layer, which builds in the
   separate `build-qt` tree and was grepped by hand. There is no platform-only
   source file that could hold a caller.
2. **The cross-check was a plain textual grep, which the preprocessor cannot
   hide anything from.** A call sitting inside `#ifdef _WIN32` is invisible to
   cppcheck on macOS but not to grep, and none of the candidates has one.
   Each was also checked for an enclosing open `#if`; none is inside one.

### Unused variables, fields and parameters (done)

clang `-fsyntax-only` with `-Wunused-*` over all 174 first-party translation
units in the compile database: **23 warnings**, and most are not defects.

Every unused *parameter* is a callback or mock signature the interface dictates
(`DataPublisherService.cpp:43` and its test doubles, `BaseReportGenerator.cpp:155`,
`FysetcTcpServer.cpp:163`). Those stay; naming them is what documents the
signature.

The genuine leftovers are seven unused variables and constants:

| Symbol | Where |
|---|---|
| `opt_dbl`, `opt_int` | `src/database/DatabaseService.cpp:1035,1036` |
| `ncols` | `src/database/DatabaseService.cpp:2343` |
| `kMdnsGroup` | `src/services/DeviceDiscoveryService.cpp:51` |
| `kDay` | `tests/services/test_CleaningPublisher.cpp:28` |
| `publish_success` | `tests/services/test_DataPublisherService.cpp:359` |
| `dir_lba` | `tests/services/test_FysetcSectorCollectorService.cpp:362` |

plus three unused test-fixture member functions (`read`, `isMySQL`, `sentTo`) and
the two file-local functions already listed in 1a and 1d.

`kMdnsGroup` is worth a second look rather than a deletion: an unused mDNS group
address in the discovery service suggests a path that was started and not
finished, which is a question for Albin, not a sweep.

What the per-configuration runs would still add is the other direction:
**discovering dead code that only exists inside a platform guard** (a
Windows-only helper nothing calls on Windows). That is a gap in coverage, not a
risk to the list above. Still owed, and worth having before D6's gate is written:

- lcov `FNDA:0` from `scripts/coverage.sh` (functions the suite never executes)
- Linux link with `-ffunction-sections -Wl,--gc-sections -Wl,--print-gc-sections`
- cppcheck with the Windows and Linux define sets, and with BLE, MySQL and the
  report stack toggled

## 9. The parser library

Seven candidates in `hms-cpapdash-parser`, listed here only so they are not
lost; they are judged in Phase 7 against all four consumers and ship on their own
tag first.

`parseSTRFromBuffer` (`EDFParser_STR.cpp:20`), `detectFlowApneas`
(`FlowEvents.cpp:47`), `percentile` (`PrismaParser_Signals.cpp:17`),
`currentStreak`, `bestStreak`, `milestoneFor` (`SleepIndex.cpp:117,141,164`),
`parseFile` (`VLDParser.cpp:141`).

## 10. Found along the way, not dead code

`tests/database/test_RemoveNightBackends.cpp` counted file rows with
`rel_path LIKE 'DATALOG/2099%'` and no device scope, so on the shared MySQL and
PostgreSQL test databases it counted rows another run had left behind. SQLite
never showed it because each run gets its own temp file. Fixed by joining to
`cpap_sessions` and scoping to the per-process device id.

## 11. What was actually done

### Three findings that changed an earlier decision

The inventory was right about what nothing calls and wrong three times about
what that *meant*. Each was put back to Albin before anything was deleted:

1. **The parser is one function, not seven.** Only `percentile` is dead
   (anonymous namespace, no caller in its own translation unit, unreachable
   from outside by construction). `parseSTRFromBuffer` is LIVE via the cloud
   monolith and cpapdash-ingest. The other four are uncalled but PUBLIC API in
   shipped headers, with tests and an SDD behind the streak trio. **And
   `hms-cpapdash-parser-philips` is not a consumer at all** -- it is a
   standalone fork with its own duplicate sources and its own
   `project(cpapdash_parser VERSION 1.0.0)`. There are three consumers, not the
   four SDD-045 D1 assumes, and all three sit on the current tag.
2. **`computeSupplyStatus`'s 13 tests were the supply-status coverage.** They
   reach the live `computeSupplyStatusForInterval` *through* the wrapper, so
   deleting them would have removed the thresholds, wear fraction, clamping and
   untracked rules from the suite. Ported onto the live function first, exactly
   the §1.2 exception; the wrapper then went.
3. **The `CpapDashSyncService` seams are harness on a LIVE service.**
   `main.cpp` creates it and three controllers hold it. Its whole 34-test suite
   runs on `setTransport` and `setStatePath`. Kept.

A fourth correction came later: the four "unused" routes were not four unwired
features. `/api/sessions/{date}/sleep_stages` and `/api/sleep-stages/status`
are the READ half of a shipping feature (Settings has a Sleep stage toggle,
`LiveSleepStageRunner` writes the rows), and `/api/equipment/cloud-sync` is a
working manual trigger for the live sync service. Only `/api/statistics` went.

### Removed

| Area | What |
|---|---|
| C++ | The 8 functions of §1a, plus the cascade deleting `fetchDevice` exposed: `parseDevice`, the `MyAirDevice` struct and 3 tests, leaving myAir with only its live sleep-records path |
| C++ | `canManageAutostart` (its rule is `autostartOwner`, live and separately tested), `getLastBurstTime` with its write site and orphaned member, `computeSupplyStatus` after the port |
| Routes | `/api/statistics` and its handler. `QueryService::getStatistics` stays: the PDF reports call it |
| Frontend | `MetricCard`, `IndexKind`, the `RouterLink` import (NG8113 is now absent from the build), `tsconfig.spec.json` with its `test`/`pretest` scripts, and the `export` keyword off 6 symbols that are live inside their own module |
| i18n | 21 keys, dropped from all five languages in one go so the parity gate never saw a mismatch |
| Scripts | `build_frontend.sh`, `add_reports_table.sql`, `retrofit_bytes_to_kb.sql` |
| Leftovers | 7 unused variables and constants, 3 unused test-fixture members |
| Parser | `percentile` |

### Kept, each for a stated reason

The sync seams and their 34 tests; the sleep-stage read routes; the manual
cloud-sync trigger; both audit columns (so this release never touches user
data); `prettier` (`.prettierrc` is tracked); `@angular/compiler`;
`public/favicon.ico`; the one-off repair scripts (`fix_session_ends.py`,
`reparse_sessions.py`, `str_backfill.py`, the ezshare-monitor trio); every
unused *parameter*, which documents a callback or mock signature.

`kMdnsGroup` was checked before deletion rather than swept: the vendored mdns
library joins 224.0.0.251 itself in `mdns_socket_open_ipv4`, so the constant was
a leftover from before that library arrived, not an unfinished discovery path.
Removed, with a comment saying where the group now comes from.

### Verification

| Run | Result |
|---|---|
| Full suite, local zone, SQLite + MySQL + PostgreSQL | 1834 passed, 0 failed |
| Full suite, `TZ=UTC`, all three engines | 1834 passed, 0 failed |
| Agent suites | 109 passed, 0 failed |
| Parser suite, Lowenstein + Sefam on | 235 passed, 0 failed |
| Frontend `npm run build` | clean, i18n parity gate passed, no NG8113 |

Before the sweep the suite was 1842. The difference is exactly the 8 tests that
went with the code they covered: 3 myAir `parseDevice`, 1 `canManageAutostart`,
2 `getLastBurstTime`, and 2 SupplyStatus tests made redundant by the port
(`AgreesWithTheSlotVariant`, which compared two variants where one now remains,
and `ExplicitZeroIntervalIsUntracked`, already covered by
`NonPositiveIntervalIsUntracked`).

## 12. The gate (D6)

`scripts/dead-code-gate.sh`, run by a `dead-code` job in `docker-build.yml`
(configure only, no build: the compile database is all cppcheck needs).

**It does not decide what is dead, on purpose.** That is the lesson of this
whole exercise: of 111 cppcheck hits, 8 were real, and every wrong one came from
a caller cppcheck could not see. An automated verdict would either delete a test
seam or rubber-stamp everything. So the baseline
(`scripts/dead-code-baseline.txt`) lists **every** symbol the tools currently
raise, 34 of them, and the gate fails only when a symbol appears that nobody has
classified. The person who added it decides: delete it, or baseline it with a
note saying what reaches it.

Two details it encodes so they are not rediscovered:

- **Both build trees.** It runs cppcheck over `build/compile_commands.json` and
  again over `desktop/qt`, because the Qt layer builds elsewhere and is absent
  from the main database. Without the second run the gate reports the entire
  supervisor as dead on every run.
- **No `-j`.** cppcheck silently disables `unusedFunction` when threaded. The
  first run of this analysis during Phase 0 returned zero findings and looked
  like good news.

Proven by watching it fail: a throwaway `gateProbeNobodyCallsThis` added to
`SupplyStatus.cpp` was caught by name and file with exit 1, and the gate went
green again when it was removed.
