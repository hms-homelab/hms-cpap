# SDD-002: Full-Card OSCAR Residue Capture — non-EDF/non-junk files for a complete card image

**Status:** Draft
**Date:** 2026-06-28
**Companion:** the CpapDash cloud's full-card backup + OSCAR export capability (general residual sweep + DATALOG `.crc` capture, shipped cloud v2026.6.1)

## Parity mandate (the "must")

**Mirroring the CpapDash cloud is a requirement, not an analogy.** The 4-repo CpapDash
architecture (shared `hms-cpapdash-parser` + parity behavior) means a capability that
lands in the cloud must reach feature parity in this local app (`hms-cpap`). SleepHQ export already reached parity here (`SleepHqExportService`, shipped
v4.4.0). This SDD closes the **OSCAR full-card backup** side: pulling the non-EDF /
non-junk card residue so the local archive is a complete, OSCAR-importable card image —
exactly what the cloud now does in SDD-016 Phase 2.2/2.3. Where a predicate or denylist
exists on both sides (`isCpapEdf`, the junk/20 MB `residualSkip`), keep them in lockstep.

## Problem

`hms-cpap` already stores its archive in the **OSCAR-faithful SD layout** — files land at
`CPAP_ARCHIVE_DIR/DATALOG/<date>/<file>` plus root `STR.edf` / `Identification.*`
(`BurstCollectorService.cpp:475-530`, `AppConfig.h:244-254`). So OSCAR can in principle
read `~/.hms-cpap/` directly. But the archive is **incomplete**, because the download path
only fetches the 6 analytical EDF suffixes:

- `EzShareClient::downloadSession()` (`src/clients/EzShareClient.cpp:387-406`) hard-filters
  to `_BRP/_EVE/_SAD/_SA2/_PLD/_CSL.edf`. Everything else in a `DATALOG/<date>` folder —
  the per-night `.crc` next to each EDF — is never downloaded.
- `SessionDiscoveryService` (`src/services/SessionDiscoveryService.cpp:105-138`) only tracks
  BRP/PLD/SAD/SA2/CSL/EVE; it never enumerates `Identification.*`, `SETTINGS/`, `JOURNAL`,
  `.crc`, or `.tgt`.
- The Fysetc/sector path is narrower still: `FysetcSectorCollectorService::isCheckpointFile`
  (`src/services/FysetcSectorCollectorService.cpp:98-103`) grabs only BRP/PLD/SAD/SA2.

What IS on disk today (verified):
- **EDFs** — the 6 analytical suffixes per `DATALOG/<date>` (`downloadSession`).
- **STR.edf** — downloaded via `BurstCollectorService.cpp:589-590`
  (`downloadRootFile("STR.edf")`, with an `STR.EDF` fallback). Present.

What is MISSING (no download path exists):
- **`Identification.*`** (`.tgt`/`.json`/`.crc`) — there is **zero** download code for it
  (grep of `src/` finds none). This is the *highest-value* gap: `Identification.tgt` is how
  OSCAR identifies the machine model/serial, so without it an OSCAR import is degraded.
- **Per-night `.crc`** — confirmed live on a real ResMed card (2026-06-28): every
  `DATALOG/<date>` folder holds **5 EDF + 5 `.crc`**, and only the EDFs are pulled.
- **`SETTINGS/`** and **`JOURNAL`** — never enumerated or downloaded.

Because `SleepHqExportService` only uploads files that **already exist on disk** (it iterates
`DATALOG/<date>` with no filter, plus a fixed root list guarded by `if (fs::exists) continue`,
`SleepHqExportService.cpp:59-78`), it uploads STR.edf but **silently skips `Identification.*`
and the `.crc`** — they're simply not there. Completing the on-disk residue fixes **both** the
OSCAR-in-place archive and the SleepHQ upload in one move.

## Design

Mirror the cloud's split: EDFs are analytical (parsed, grouped into sessions); everything
else is **backup-only** (written to the OSCAR layout on disk, never parsed, never grouped).

### 1. Residue download (the only real new work)

Add, alongside the EDF download, a residue capture that pulls the non-EDF / non-junk files:

- **Per DATALOG date folder:** after the EDFs, also download every other file the listing
  reports that passes the junk denylist (the `.crc`; brand-agnostic — any future non-EDF
  metadata too). EDFs keep their no-cap treatment; non-EDF files honor the 20 MB cap.
- **Card root + subdirs:** a one-shot residue pass for `Identification.*`, `SETTINGS/*`,
  `JOURNAL*`, and any other non-junk root/subdir file (machine-agnostic — other brands keep
  their metadata under different names, so do not hardcode `.crc`).
- Implement as a sibling to `downloadSession()` in `EzShareClient` (and the same for
  `FysetcDataSource` / the sector collector, which read FAT32 and can already fetch any
  file). Reuse the shared `IDataSource` `listFiles()` so both transports get it.

**Shared predicates (lockstep with cloud):**
- `isCpapEdf(name)` — the 6 EDF suffixes; the single gate that splits analytical vs residue.
- `residualSkip(name, size)` — port of the cloud's denylist verbatim: drop multimedia,
  office docs, archives, AppleDouble/`.DS_Store`/`ezshare.cfg`, and anything > 20 MB.

### 2. Storage layout — no change

The archive is already OSCAR-faithful, so residue just writes into the same tree:
`DATALOG/<date>/<file>.crc`, root `Identification.*`, `SETTINGS/<file>`. Nothing new on the
layout side — that is the whole reason this is cheap here.

### 3. Discovery / parse — residue is backup-only

`SessionDiscoveryService` and the parsers stay EDF-only. Residue files are written to disk
and **not** enumerated as session files, not parsed, not grouped — identical to the cloud's
"no `session_files` row / no parse" rule. They exist purely for the OSCAR image + SleepHQ.

### 4. SleepHQ export — free parity win

`SleepHqExportService` already uploads *all* files present in `DATALOG/<date>` plus the root
metadata list. Once §1 puts the residue on disk, SleepHQ export becomes complete with **no
code change** — it starts shipping the `.crc` automatically. (Optionally widen its fixed
root list / add `SETTINGS/` so newly-captured root+settings residue uploads too.)

### 5. (Optional) backup.zip parity

The cloud exposes `GET /api/devices/{serial}/backup.zip`. Here the archive is already a local
OSCAR folder, so the equivalent is "point OSCAR at `~/.hms-cpap/`". A convenience
`GET /api/backup.zip` (zip the archive in the same `DATALOG/<date>/` + root layout) can
mirror the cloud for the web UI / remote pulls — lower priority, since the in-place folder is
already importable.

## Tests

- **Residue download:** a `DATALOG/<date>` listing with EDF + `.crc` → both land on disk;
  EDFs keep no size cap; a > 20 MB non-EDF and any junk extension are dropped.
- **Discovery untouched:** residue files do NOT create extra sessions or alter grouping
  (`SessionDiscoveryService` ignores them).
- **OSCAR completeness:** after a sync, `DATALOG/<date>/` contains the `.crc` next to each
  EDF, and root holds `Identification.*` — an OSCAR import of `~/.hms-cpap/` is byte-complete.
- **SleepHQ:** export of a captured folder now includes the `.crc` (regression-guards the
  silent miss).
- Keep the `isCpapEdf` / `residualSkip` tests in lockstep with the cloud's
  (`test_ResidualSweep` / `test_BackupExport`).

## Open questions

1. **Both transports in v1, or phase the Fysetc/sector path?** ezShare HTTP is the common
   case; the FAT32 sector path needs the same residue read for true parity. Recommend both,
   sector path as a fast-follow if it adds risk.
2. **`backup.zip` endpoint** — needed, or is the in-place OSCAR folder enough? (Lean: defer.)
3. **Re-capture cadence:** residue rides each per-session/per-folder download, so new nights
   capture automatically (same as the cloud). Existing archives need a one-time residue
   backfill pass over already-synced `DATALOG/<date>` folders + root.

## Migration

Additive and backward-compatible. New syncs capture residue going forward; a one-shot
backfill (re-list each archived `DATALOG/<date>` + the card root, pull only the missing
non-EDF/non-junk files) completes historical folders without re-downloading EDFs.
