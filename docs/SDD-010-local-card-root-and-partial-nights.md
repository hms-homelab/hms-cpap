# SDD-010: The local card root, and partial nights in local mode

**Status:** Implemented
**Date:** 2026-08-04
**Repo:** `hms-cpap`
**Version:** 4.9.2
**Depends on:** SDD-008 (partial sessions), SDD-005 phase 1 (PreflightService)

## Trigger

Two defects that look unrelated and are not.

The first is a path contract that is written backwards. `CPAP_LOCAL_DIR` is
documented and used as the **DATALOG** directory, so `processSTRFile()` reaches
*upward* with `parent_path()` to find `STR.edf`, and then, when that misses,
searches *inside* DATALOG as a fallback. STR never lives inside DATALOG. The
fallback cannot succeed, and its only effect is to make a misconfiguration look
like a missing file.

The codebase already knows this hurts. `SleepHqExportService.h:67` documents the
exact symptom, that a directory "pointing at DATALOG instead of the card root
silently drops STR.edf", and works around it locally without fixing the contract
that caused it.

The second is that **local mode never participates in the SDD-008 partial
machinery at all.** `updateFolderLedgers` has exactly one call site,
`BurstCollectorService.cpp:1343`, inside the ezShare branch. The local branch
runs its own loop and never calls it, so no `sync_folders` row is ever written
for a local night: no ledger, no `str_due`, no `NightState`.

That is not an oversight. `isNightPartial()` opts local mode out deliberately:

> No row means no transfer was ever tracked for this night: the local directory
> and Prisma sources read from a filesystem, where there is no transfer that can
> stall. Reporting those partial would be a false alarm.

**That premise is wrong**, and the way it is wrong is the reason this SDD exists.

## Why the premise is wrong

Files reach the local or SMB folder two ways, and they behave nothing alike.

| Path | Shape | Does it stall? |
|---|---|---|
| **Manual copy**, usually off an SD card | One fast copy of everything at once | Briefly. Seconds. |
| **amanuense** (the Fysetc project) | Waits for end of therapy, then transmits from the CPAP into SMB | **Yes.** Slow enough that hms-cpap will reliably observe partial files |

A filesystem source is not automatically a settled source. With amanuense the
folder is a *transfer in progress* that happens to be reachable through a mount
point, which is exactly the condition `str_due` was built to describe.

The consequence today is concrete. Local completion at
`BurstCollectorService.cpp:1214-1237` calls `publishNightOutcome()`, which asks
`isNightPartial()`, which always answers false, so it always takes the full
branch and publishes metrics plus the LLM summary. **A local night that settles
without its STR is published as Complete with short metrics.** That is precisely
the outcome SDD-008's suppression exists to prevent; it simply never reaches
local mode.

## Decision: burst processing, not wait-for-copy

**Decision (Albin, 2026-08-04): keep partial detection and processing on every
burst, matching ezShare mode. Do not add a wait-for-copy-to-finish stage.**

The alternative considered was to hold off all processing until a burst observes
no change, on the grounds that an unchanged signature means the copy finished.
It was rejected, and the reason it is safe to reject matters:

**"Wait for the copy" is not a separate architecture. It already exists inside
the ledger.** `advanceFolder` settling *is* "two identical bursts", and the
comment at `BurstCollectorService.cpp:1339-1342` says so: "settling already
requires two observations of the same signature". Adopting `updateFolderLedgers`
in local mode yields that signal as a byproduct. There is no second process to
introduce and nothing to wait on.

So the only real question was whether to parse on every burst or only after
settling, and burst wins because **neither format corrupts under a partial
read**:

| Format | Behavior on a partial file | Verdict |
|---|---|---|
| **ResMed EDF** | `EDFParser.h:42` handles "incomplete/growing files (partial last data record)". `EDFParser.cpp:195-214` covers `num_records_header == -1` (written while recording), `actual > header` (appending), `== header` (complete), `< header` (truncated) | Degrades gracefully, improves each burst. Designed for it |
| **Lowenstein ZIP** | `PrismaIngestion.cpp:51` cannot open a truncated `.pdat` at all, since a ZIP's central directory is at the end. Logs and returns false | Fails loudly. Never produces wrong numbers |

Prisma additionally self-heals: extraction is gated on `last_write_time`
(`PrismaIngestion.cpp:92-95`), so it re-extracts once the copy completes and the
mtime advances.

Waiting would buy reduced log noise and CPU. It would not buy correctness.

## Part 1: `local_dir` is the card ROOT

`STR.edf` and `DATALOG/` are siblings at the root of a ResMed card. This is not a
heuristic to be probed, it is the layout ResMed writes, so it is stated once and
depended on everywhere.

**The contract:**

- `local_dir` names the **card root**.
- `STR.edf` is resolved **only** at `<local_dir>/STR.edf`. The search inside
  DATALOG is deleted.
- Sessions live at `<local_dir>/DATALOG/<YYYYMMDD>/`.
- A missing STR is **not an error**. The summary falls back to per-session
  CSL/EVE aggregation, the same path ezShare mode already uses.

**Scope (Albin's call): everything, including the CLI.** `CPAP_LOCAL_DIR`,
`BackfillService::Config::local_dir`, and the `--backfill` / `--reparse`
arguments all mean the card root. One rule, no exceptions, because two meanings
for "the local dir" inside one binary is how this defect was born.

### Detecting the misconfiguration

A pure classifier, so it can be unit tested without a service:

```cpp
enum class LocalDirLayout {
    Root,       // contains DATALOG/            -> usable
    IsDatalog,  // contains YYYYMMDD folders directly -> user pointed at DATALOG
    Unusable    // neither                      -> wrong path, or not mounted
};
LocalDirLayout classifyLocalDir(const std::string& path);
```

**Decision (Albin): hard fail. No auto-correcting up one level.** Silently
repairing the path would make the contract negotiable again, which defeats the
purpose of pinning it. This follows the SDD-005 rule already in force: never
discover a configuration error through a retry, validate up front and report the
cause *and* the remedy.

**Decision (Albin): `Unusable` fails too, with its own message.** A wrong path
and an unmounted share are as broken as a DATALOG path, and staying quiet is how
they go unnoticed.

### What failing means

The preflight check is **non-fatal**, which looks like a contradiction and is
not. A fatal check refuses to boot, and refusing to boot takes down the web
server, which is the only thing that can tell the user what to fix. The SDD-005
rule is that a knowable error is found up front and reported with cause and
remedy rather than discovered through a retry. That is satisfied. What is
stopped is **ingestion**, not the product.

Failing must not destroy what the user already has:

1. **Stop ingesting.** No sessions, no summaries, no ledger rows. Nothing is
   inserted while the configuration is wrong.
2. **Keep serving.** Sessions already in the database continue to render. A
   config mistake must not look like data loss.
3. **Say so, app-wide.** A red banner names the problem and the fix, and links to
   settings.

### Surfacing it

**Decision (Albin): app-wide, carried on `/api/capabilities`.**

The flag is computed on demand from the configured path rather than cached from
the collector. It is a couple of `stat` calls, it can never go stale, and a
remounted share clears the banner on the next page load without a restart.

No polling is needed and none is added. A misconfigured path cannot self-heal
without a settings change, and changing settings reloads the view. `/api/status`
was considered and rejected as unnecessary for a single flag.

## Part 2: local nights use the folder ledger

Local mode calls `updateFolderLedgers(new_sessions, <local_dir>/DATALOG)` before
its session loop, for the same ordering reason documented at
`BurstCollectorService.cpp:1330-1337`: the loop is where a settling session
publishes, and that publish asks the ledger whether the night is partial, so a
ledger updated afterwards is always one cycle stale.

Everything downstream already works and is untouched:

| Piece | State |
|---|---|
| `advanceFolder(prev, obs)` | Pure. No transport knowledge |
| `nightState(l)` | Pure. `!complete → Live`, `complete && str_due → Partial`, else `Complete` |
| Two-burst settling | Already implemented |
| `QueryService.cpp:197-201` | Already emits `row["partial"]` |
| `isNightPartial()` | **No logic change.** Its `if (!ledger) return false` is now correct for a new reason; only the comment is rewritten |

One property is worth stating because it looks like a bug and is not.
`obs.all_files_stored` verifies each listed file exists and is non-zero *on
disk*. In ezShare mode that confirms the download landed. In local mode the
source **is** the disk, so it reads true whenever the listing is readable.
That is correct, and it reduces settling cleanly to signature stability across
two bursts, which is the rule this SDD wants.

### Lowenstein stays out of the ledger

**Decision (Albin): Prisma nights do not participate at all.**

This is not a preference, it is forced. `processSessionSummary()` early-returns
for `cpap_source_ == "lowenstein"` and never reaches `processSTRFile()`, which
is the only caller of `clearStrDebtForParsedDays()` (`:736`). And Lowenstein has
no `STR.edf` in the first place (`IDatabase.h:102`). So an armed `str_due` on a
Prisma night could never be cleared, by any code path, ever.

**Arming the ledger uniformly would latch 100% of Lowenstein nights as Partial,
permanently.**

Keeping Prisma out means `getSyncFolder` finds no row for those nights, and
`isNightPartial` returns false, which is the correct answer. The cost, accepted
knowingly, is that Prisma has no settle signal, so the "cannot open ZIP" log
noise during a slow copy stays as-is. Correctness is unaffected.

## What changes

| File | Change |
|---|---|
| `include/utils/CardLayout.h`, `src/utils/CardLayout.cpp` | **New.** `LocalDirLayout` + `classifyLocalDir()` |
| `src/services/PreflightService.cpp` | `checkSource` classifies instead of only checking existence; distinct remedies per layout |
| `src/services/BurstCollectorService.cpp` | STR at root only, DATALOG fallback deleted; DATALOG derived from root; ledger call in the local branch, ResMed only; ingestion gated on layout; `isNightPartial` comment |
| `src/services/BackfillService.cpp` | Same root contract, same deletion of the fallback |
| `src/controllers/CpapController.cpp` | `/api/capabilities` reports `config_error` |
| `src/main.cpp` | `--reparse` takes the card root and validates it; usage text updated; zip import extracts into `<root>/DATALOG`. (`--backfill` takes a path to `str.edf`, a file, so it is unaffected) |
| `include/utils/AppConfig.h`, `include/services/BackfillService.h`, `include/services/SessionDiscoveryService.h` | Comments corrected to say root |
| `frontend/` | App-wide red banner |
| `README.md`, `CHANGELOG.md`, `config.json.example` | Documentation of the root contract |

## Tests

| Suite | Covers |
|---|---|
| `test_CardLayout.cpp` (new) | `Root` / `IsDatalog` / `Unusable`, empty path, missing path, DATALOG present but empty, non-date folders ignored |
| `test_PreflightService.cpp` | Each layout produces the right fatal check and a remedy that names the fix |
| `test_BurstCollectorService.cpp` | STR found at root; STR **not** found when only inside DATALOG; misconfigured layout inserts nothing; local burst writes a ledger row; a local night settling without STR reports Partial; a Lowenstein night never gets a ledger row |
| `test_BackfillService.cpp` | Root contract and the deleted fallback |

## Deliberately out of scope

- Auto-correcting a DATALOG path. Rejected above.
- Gating Prisma ZIP extraction on settling. Unavailable by construction once
  Lowenstein is out of the ledger, and it was only ever a log-noise fix.
- Migrating existing configs. The hard fail plus the banner *is* the migration.
- `/api/status`. Not needed for one flag.
