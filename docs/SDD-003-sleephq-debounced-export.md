# SDD-003: Debounced, coalesced SleepHQ night export

**Status:** Implemented
**Date:** 2026-07-14
**Trigger:** Field report (support ticket #43): SleepHQ received a 1-minute
fragment as an entire night, and other nights never arrived.

## Problem

The live collector exports to SleepHQ the moment a session is marked
completed. "Completed" means one burst cycle saw checkpoint sizes unchanged,
which happens 1-2 minutes after ANY mask-off, including mid-night breaks.
Consequences observed in the field:

1. A 1-minute mask-on fragment completes early in the night; the export
   uploads the archive date folder while it only contains that fragment.
   SleepHQ shows a 1-minute night.
2. Exports are fire-and-forget detached threads. Any single file upload
   error aborts the whole import (`return false`) and nothing ever retries,
   so a failed full-night export leaves the fragment import as the only data.
3. The export can run before the late EVE/CSL catch-up and the ~50 s delayed
   STR flush, so even a final-session export can miss the events file.
4. One trigger site fired for every completed session, not just the most
   recent, multiplying partial imports.

## Design

`SleepHqExportService` gains a dirty-folder model; completion handlers no
longer export directly.

- **markDirty(date_folder)** — called by the burst collector wherever it
  previously called `exportDateAsync` (both sites, `auto_on_session` guard
  unchanged). Cheap, lock-protected, idempotent.
- **sweep(now)** — called once per burst cycle from `runLoop()` after
  `executeBurstCycle()`. For each dirty folder it snapshots the archive's
  `DATALOG/<folder>` (filename → bytes) and exports only when ALL hold:
  - the snapshot is unchanged since the previous sweep (growing sessions and
    late EVE/CSL/STR arrivals keep resetting the quiet timer);
  - the folder has been quiet for `sleephq.quiet_minutes` (default 15;
    covers the STR flush, late-EVE bursts, and typical short mask-off breaks);
  - no other export is in flight (serialized to respect SleepHQ token rate
    limits);
  - the failure backoff window has elapsed.
- **Retry with backoff** — a failed export keeps the folder dirty;
  `next_attempt = now + min(5 min * 2^(failures-1), 60 min)`, capped at 8
  attempts before giving up with an error log.
- **Re-dirty semantics** — a later completion in the same folder (mask back
  on after a long break) marks it dirty again; since every import re-uploads
  the whole folder, the final import of a night is always complete. If files
  change while an upload is running, the folder stays dirty and re-exports
  after the next quiet window.

State is in-memory only. A restart drops pending dirty marks; the next
session completion in the folder re-arms the export. Accepted trade-off:
a night with a multi-hour mid-night gap may produce two imports; the last
one is complete.

The backfill/local-import path (`exportFolderAsync`, `auto_on_backfill`)
is untouched: it is a one-shot bulk ingest with explicit directories and
does not suffer the mid-night fragmentation problem.

## Config

`sleephq.quiet_minutes` (config.json) / `SLEEPHQ_QUIET_MINUTES` (env),
default 15, minimum 1. Surfaced through the existing GET/PUT settings API.

## Tests

`tests/services/test_SleepHqExportService.cpp` drives `sweep()` with an
injected clock and export hook against a temp archive: quiet-window gating,
growth resetting the timer, retry backoff, disabled config, re-dirty after
success, and mid-upload changes keeping the folder dirty.
