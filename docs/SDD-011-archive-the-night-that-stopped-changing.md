# SDD-011: Archive the night that stopped changing

**Status:** Proposed
**Date:** 2026-08-05
**Repo:** `hms-cpap`
**Version target:** unassigned
**Depends on:** SDD-002 (card residue), SDD-003 (SleepHQ debounced export)
**Reported by:** Michael (mjk.usc@gmail.com), support ticket 67, 2026-08-04

## Trigger

Michael's night of 2026-08-03 is visible in the hms-cpap dashboard and absent
from disk. His archive at `~/Desktop/CPAPData/DATALOG` holds every folder from
`20260718` to `20260802` and stops. The card holds `20260803` and `20260804`.

He worked the problem himself and landed on a plausible wrong answer:

> "My guess is that it may get picked up at 12:00 PM (noon). ?? hms-cpap is on
> it's 65 sec burst cycle so will get my answer in about an hour 10 min"
>
> "Noon passed, resmed machine made new empty folder 20260804, hms-cpap is aware
> but it didnt trigger pushing 20260803 out."

There is no noon trigger. The folder was never going to appear, on any burst,
ever.

## The mechanism

`executeBurstCycle()` reaches the archive only through the download path.

```
:1489   if (all_unchanged && !has_new_files) {
            ... markSessionCompleted / publish / summary ...
:1547       "No changes, skipping download"
            continue;                      <-- nothing added to downloaded_sessions
        }
...
:1576   if (downloaded_sessions.empty()) {
:1577       "CPAP: No sessions downloaded successfully"
:1578       return false;                  <-- RETURNS HERE
        }
...
:1586   archiveSessionFiles(...)           <-- unreachable
:1602   captureCardResidue(...)            <-- unreachable
```

Once a session is complete and its checkpoint files stop changing, every
subsequent burst takes the `continue` at `:1548`, leaves `downloaded_sessions`
empty, and returns at `:1578`. The archive block below it never runs.

The state is **terminal**. A session that reaches "stable and complete" without
having been archived can never be archived, because the only code that writes
the archive is behind a branch that will never be taken again for that session.

Michael's log is the mechanism verbatim:

```
CPAP: Session 20260803_234806 stopped (all checkpoint files unchanged)
SQLite: Session already has session_end set
   No changes, skipping download
CPAP: No sessions downloaded successfully
```

Corroborating: the `STR.edf` in his archive has mtime `Aug 3 20:35`, while the
session started at `23:48`. The archive has received no write of any kind since
that session appeared, which is exactly what an unreachable archive block
predicts.

## What it costs

Four symptoms, one cause. Three of them are silent.

| Symptom | Why |
|---|---|
| Folder missing from the archive | the archive step never ran |
| OSCAR "gets sniffed and passed without error but no new data" | there is no new folder for OSCAR to read |
| A zip of the archive uploaded to CpapDash parses but creates no session | the zip was built from an archive that lacks the night |
| SleepHQ export "doesn't always update" | `SleepHqExportService.cpp:37` reads `archive_base/DATALOG/<folder>`, so there is nothing to send |

The dashboard looks correct throughout, because the session **is** in SQLite.
Only the on-disk copy is missing, so every consumer that reads files rather than
the database silently gets nothing. That is the worst shape a data bug can take:
the product reports success and the user finds out through a third-party tool.

`captureCardResidue()` is stranded by the same return, so `Identification.*`,
`SETTINGS/` and `Journal.dat` also stop being refreshed.

## Scope

**ezShare and Fysetc.** Both reach this branch through `data_source_`.

**Not local mode.** It reads from a filesystem that already holds the files and
does not archive at all. SDD-010 does not touch this and does not fix it.

## The fix: skip only what is already on disk

The skip condition is asking the wrong question. "Have the files stopped
changing" is not the same as "is there nothing left to do", and the gap between
those two is exactly this bug.

```
skip  <-  unchanged  AND  already archived
```

Concretely: before taking the `continue`, verify that every file in
`session.file_sizes_kb` exists and is non-empty under
`<CPAP_ARCHIVE_DIR>/DATALOG/<date_folder>`. When any is missing, fall through to
the normal download path, which already archives on the way out.

Three reasons to derive this from disk rather than track it as a debt column:

1. **The disk is the fact being asserted.** A debt flag can disagree with
   reality; a `stat` cannot. This is the same reasoning `updateFolderLedgers`
   already uses for `all_files_stored`, so the pattern is established here.
2. **No migration, no new state.** Nothing to backfill for existing installs,
   and it starts working on the first burst after upgrade.
3. **Self-healing.** A user who deletes or moves their archive gets it rebuilt
   rather than silently never repopulated.

`archiveSessionFiles()` already dedups by size and no-ops on an empty temp dir,
so re-entering the download path for an already-archived night is cheap and
idempotent.

### Bounded by construction

This cannot cause a mass re-download. Discovery only ever offers sessions that
are new, today's, within 48 hours, or among the two most recent stored
(SDD-010's retention anchor). An old night that was never archived stays
unarchived until something asks for it explicitly.

## Deliberately out of scope

- **Back-healing nights already missing.** Michael has one; a user who has been
  running this for months could have many. `--reparse` and the backfill service
  already exist for that, and folding a bulk repair into the burst cycle is how
  a 65-second loop turns into an hours-long download storm. If we want it, it
  belongs behind an explicit action.
- The other ticket-67 items: SpO2 CSV duplicating one file across two dates,
  SpO2 entry rejecting July, charts with missing sections. Unrelated causes.

## Tests

| Suite | Covers |
|---|---|
| `test_BurstCollectorService.cpp` | A complete, unchanged session whose archive folder is MISSING is downloaded and archived rather than skipped |
| | A complete, unchanged session that IS fully archived is still skipped, so steady state costs nothing |
| | A partially archived folder (one file missing, one zero-length) counts as not archived |
| | `captureCardResidue` runs on the pass that repairs an archive |
| | Local mode is unaffected: no archive check, no behaviour change |

## Verification

Reproduce Michael's exact state on a bench card: let a session complete and
archive normally, delete its folder from the archive, confirm today's code never
recreates it across several bursts, then confirm the fix recreates it on the
next burst and leaves it alone on the burst after.
