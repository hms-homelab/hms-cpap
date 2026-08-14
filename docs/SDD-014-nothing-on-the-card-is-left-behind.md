# SDD-014: Nothing on the card is left behind

**Status:** Shipped in 4.9.10. E2E green on macOS, Linux, Windows and Docker (SQLite + MySQL + PostgreSQL).
**Date:** 2026-08-13
**Repo:** `hms-cpap`, with a companion change in `hms-cpapdash-parser`
**Version:** 4.9.10 (parser v2026.1.11)
**Depends on:** SDD-008 (partial sessions), SDD-010 (local card root)
**Mirrors:** `hms-cpapdash-api` SDD-016 (full-card raw backup) and SDD-049 (container exemption)
**Closes:** issues #22, #23

## Trigger

One user, one card, two defects, one symptom. The dashboard reports **AHI 0.0**
for a night that OSCAR reads as **AHI 2.84** (AI 1.08, HI 0.54, CAI 1.22) off the
same bytes. Both defects were reported by nklmilojevic against 4.9.8 and both
were reproduced locally against 4.9.9 before this spec was written.

They are independent bugs that happen to compound. Either one alone produces a
wrong compliance number, which by SDD-008's own "wrong rather than uncertain"
standard is the worst failure this project can ship.

## Part 1: a merged session keeps one EVE and throws the rest away

### What happens

`SessionDiscoveryService::groupLocalFolder` merges mask-on blocks separated by
less than `SESSION_GAP_MINUTES` into one session. That part is correct and is the
canonical grouping. But `SessionFileSet` carries a **single** `csl_file` and
`eve_file` (`include/models/CPAPModels.h:202-203`,
`include/parsers/CpapdashBridge.h:71-72`) while `brp_files`, `pld_files` and
`sad_files` are all **vectors**. That asymmetry is the whole bug.

The matcher at `SessionDiscoveryService.cpp:809-820` walks `eve_files`, a
`std::map` keyed by filename prefix, takes the first entry within 12 hours of the
session start, and breaks:

```cpp
for (auto it = eve_files.begin(); it != eve_files.end(); ++it) {
    ...
    if (is_last_session || time_match) {
        session.eve_file = it->second.first;
        eve_files.erase(it);
        break;
    }
}
```

Map order is prefix order, which is chronological. The first EVE of a night
belongs to the earliest block, and the earliest block is routinely a
seconds-long mask-fit check whose EVE is the empty 832-byte stub. Every later
EVE, meaning every EVE that actually carries the night's annotations, is never
staged and never parsed.

**The same bug exists a second time at `SessionDiscoveryService.cpp:274-285`, in
the ezShare path.** The issue only names `groupLocalFolder`. Fixing one site
leaves every ezShare user broken, which is most of the fleet.

### Reproduced

A harness linked against the real objects, with the reporter's block layout (four
blocks in one DATALOG folder, each with its own EVE):

```
Split 4 checkpoint files into 1 session(s)
Session 20250812_233427: BRP=4 PLD=0 SAD=0 CSL=no EVE=yes

EVE files on the card : 4
EVE files kept        : 1     <- 20250812_233427_EVE.edf, the 832-byte stub
EVE files DROPPED     : 3
```

Four BRP files survive the merge because they are a vector. One EVE survives
because it is not.

### The part the issue did not find

Fixing `hms-cpap` alone does **not** fix this bug.

`EDFParser::parseSession` is directory-based: `hms-cpap` stages a session's files
into a temp directory and the parser scans that directory. The parser lives in
the shared `cpapdash-parser` library, and it has the identical defect at
`src/EDFParser.cpp:48`:

```cpp
std::vector<std::string> brp_files, pld_files, sad_files;
std::string eve_file, csl_file;          // <- not vectors
...
} else if (lower.find("_eve.edf") != std::string::npos) {
    eve_file = entry.path().string();     // last one wins
}
```

BRP, PLD and SAD are collected into vectors, sorted, and parsed in loops. EVE and
CSL are overwritten. Worse, the winner is decided by `directory_iterator` order,
which the standard does not specify, so with all four EVEs staged the surviving
one is not even deterministic.

Two consequences worth stating plainly:

1. This must be fixed in **both repos** or neither. `hms-cpap` decides which
   files belong to a session; the parser decides which of them it reads.
2. **`hms-cpapdash-api` links the same parser**, so the cloud has this defect too
   for any multi-block ResMed night. That is out of scope here, but it should not
   be discovered again from scratch later.

The good news is that the parser fix is small and safe: `parseEVEFile` only ever
does `session.events.push_back(event)` (`src/EDFParser_EVE.cpp:50`) and never
clears, so parsing several EVE files in sequence accumulates correctly with no
other change.

### Design

**`hms-cpap`:**

- `SessionFileSet::csl_file` / `eve_file` become `csl_files` / `eve_files`
  (`std::vector<std::string>`), matching the three checkpoint vectors beside them.
- Both matcher sites (`:274-285` ezShare, `:809-820` local) collect **every**
  time-matching entry instead of breaking on the first. The `is_last_session`
  catch-all keeps its current meaning: it sweeps whatever is left over so no file
  is orphaned.
- Staging (`main.cpp:327-331`) loops the new vectors like it already loops BRP,
  PLD and SAD.
- The summary line prints counts (`EVE=3`) instead of `yes`/`no`.

**`hms-cpapdash-parser`:**

- `eve_file` / `csl_file` become vectors, sorted with the same
  `sort_by_filename` comparator already used for BRP/PLD/SAD, then parsed in a
  loop. `has_events` becomes "any EVE parsed".

### Decision: a `cpap_session_files` child table

`cpap_sessions.eve_file_path` and `csl_file_path` are single `TEXT`/`VARCHAR(512)`
columns in all three engines, written at `main.cpp:346-349` and read back for
support attachments. Multiple EVEs per session no longer fit.

(SleepHQ export does **not** read them. `collectExportFiles` walks the
`DATALOG/<date>` directory and takes every regular file, so it already ships all
the EVEs. See "SleepHQ" below for what changes there.)

**Decided (Albin, 2026-08-13): a child table.** A delimited column was the cheap
option and the one that rots, because it is invisible to whoever reads it next.

```
cpap_session_files(session_id, kind, rel_path)
  kind IN ('brp','pld','sad','eve','csl')
  UNIQUE(session_id, rel_path)
```

This is the one place a session's real file set lives, so it takes all five kinds
rather than only the two that forced the change. Consequences:

- Three schema files stay in sync: `scripts/schema.sql`,
  `scripts/schema_mysql.sql`, `scripts/schema_sqlite.sql`.
- A version-gated auto-migration creates the table and backfills it from the
  existing `*_file_path` columns, so old rows keep their provenance.
- The `cpap_sessions.*_file_path` columns **stay**, holding the first file of each
  kind. They are read by SleepHQ export and support attachments today, and
  breaking those readers is not part of fixing an AHI number. They become a
  denormalised convenience; the child table is the truth.
- Reparse must clear a session's rows before reinserting, or a re-run doubles
  them. `deleteSessionsByDateFolder` already runs first in the backfill path, so
  the delete cascades if the FK is declared to.

## Part 2: the upload must mirror the card, not a slice of it

### What happens

`/api/upload/cpap` keeps only `YYYYMMDD` directories. The lambda at
`main.cpp:991-1009` runs a recursive iterator, matches directory names against
`^[0-9]{8}$`, and copies the regular files inside them. Everything at the card
root, and everything in any non-date directory, is silently dropped. The endpoint
still answers `{"status":"queued"}`.

A user who zips their card root, which is the natural thing to do and is exactly
the layout SDD-010 pins, loses `STR.edf` on the way in. Backfill then falls back
to session-derived daily summaries, and with Part 1 also in play the summary it
derives reads 0.0.

### Reproduced

Uploaded a realistic card-root zip to the real endpoint on an isolated instance:

| In the zip | On the card afterwards |
|---|---|
| `DATALOG/20250812/*.edf` | kept |
| `STR.edf` (90 KB) | **lost** |
| `Identification.tgt` | **lost** |
| `SETTINGS/SET1.tgt` | **lost** |

The service logged the reporter's line verbatim:

```
[warning] BackfillService: no STR.edf at <card>; deriving the daily summary from sessions instead.
```

### Design: this is tier 4, and tier 4 already exists

The cloud solved this. `hms-cpapdash-api` SDD-016 defines the full-card raw
backup, and its Tier 4 residual sweep collects everything the analytical DATALOG
walk does not claim, so a card can be reconstructed faithfully for OSCAR. The
upload path here should be the same functionality, and should not invent a second
policy.

**Layout.** Reproduce the zip's card root under `local_dir` exactly: eight-digit
date directories land under `DATALOG/`, every other file keeps its relative path
from the card root, root files stay at the root. This is the same
OSCAR-faithful layout SDD-016 reconstructs on export.

**Finding the card root.** A zip may wrap its contents in a folder
(`card/DATALOG/...`) or not (`DATALOG/...`). The card root is the deepest
directory containing `DATALOG/`, or, when no `DATALOG` exists, the single common
prefix directory. Everything is stored relative to that.

**Denylist.** Copy the live policy from `FirmwarePushController.cc:277-320`, not
the copy in `test_ResidualSweep.cc`, which has drifted and is missing the
exemption below. CpapDash is not a storage cloud, so skip: files over 20 MB,
names beginning `._`, `.DS_Store`, `Thumbs.db`, `desktop.ini`, **`ezshare.cfg`**
(it can hold WiFi credentials and must never be archived), and the image, video,
audio, office, and archive extensions listed there.

It is a denylist, never an allowlist. Real card files such as
`Identification.tgt`, `*.crc`, `*.log` and `Journal.dat` match no sensible
allowlist and must survive.

**The Lowenstein exemption.** `.pdat` is exempt from the 20 MB cap, at any depth,
identical to the API. A Lowenstein Prisma card is not a browsable tree: it
carries one compressed container, `therapy.pdat`, holding the entire therapy
history. A real 2026-07 client upload was 8.8 MB and it only grows. The cap
exists to stop a holiday video riding along; applied to a container it would drop
the only file on the card and report a clean, complete, empty pass.

`.pcfg` is exempt on the same reasoning. It is the other half of the same pair:
`PrismaIngestion::detectAndExtractZips` looks for `therapy.pdat` and
`config.pcfg` together (`PrismaIngestion.cpp:78-79`). The API's `residualSkip`
lists only `.pdat` today, so this is one line of drift between the two
codebases, in the safe direction. Worth a one-line follow-up there rather than
dropping it here.

Note `.zip` and `.bin` are deliberately **not** denied upstream, for the same
reason. The extension is a bad proxy; size is the honest gate.

**Reporting.** The response gains the counts it should always have had: files
copied, files skipped, and whether an `STR.edf` was seen. A silent success that
dropped the payload is how this shipped in the first place.

### Open decision: how wide is the exemption

The API exempts `.pdat` at **any depth**. Albin's instruction here was
"compressed files found in the **root** of the SD card". Root-scoped is tighter
and is what this spec proposes, since `PrismaIngestion` only ever looks for these
at the card root anyway. Say the word and it becomes depth-independent to match
the API exactly.

## Part 3: SleepHQ exports what the card holds

Once the card is mirrored faithfully and the file set is recorded properly, the
export should use both. `collectExportFiles`
(`SleepHqExportService.cpp:218-247`) does neither today: it walks
`DATALOG/<date>` and takes whatever it finds, then adds four **hardcoded** root
names, `STR.edf`, `Identification.tgt`, `Identification.json` and
`Identification.crc`.

The directory walk is not wrong so much as uninformed. It cannot tell one
session's files from another's, and it re-derives on every export something the
database now knows exactly.

**Decided (Albin, 2026-08-13):**

- **Session files come from `cpap_session_files`.** It is the record of what
  belongs to the night. `rel_path` already carries the card-accurate
  `DATALOG/<date>/<name>` form written at `main.cpp:344`, so the SleepHQ
  `import_path` is just its parent directory and no path is rebuilt by hand.
- **Root files become a walk of the card root**, filtered by the same
  `residualSkip` denylist Part 2 introduces, instead of four hardcoded names.
  `Journal.dat`, `SETTINGS/`, `*.crc` and everything else a real card carries
  then ship too, which is the point of mirroring the card in the first place.

The denylist is doing real work on this path: `ezshare.cfg` can hold WiFi
credentials, and this is the one code path that uploads card contents to a third
party. It must never be exported.

The two changes are also why Part 2 matters beyond backfill. Before it, a
web-upload user's card root held nothing to walk.

## What changes

| File | Change |
|---|---|
| `include/models/CPAPModels.h`, `include/parsers/CpapdashBridge.h` | `eve_file`/`csl_file` become vectors |
| `src/services/SessionDiscoveryService.cpp` | both matchers collect all, not first; summary prints counts |
| `src/main.cpp` (staging) | loop the EVE/CSL vectors |
| `src/main.cpp` (`cpap_zip_import_`) | calls `mirrorCardInto`, reports counts |
| `include/utils/CardImport.h`, `src/utils/CardImport.cpp` | **NEW.** The card mirror, lifted out of the lambda so it is testable |
| `src/utils/CardResidue.cpp` | `residualSkip` brought up to SDD-049: `.pdat`/`.pcfg` exempt from the cap, `.zip`/`.bin` no longer denied |
| `scripts/schema.sql`, `schema_mysql.sql`, `schema_sqlite.sql` | `cpap_session_files` + version-gated migration and backfill |
| `src/database/{DatabaseService,SQLiteDatabase,MySQLDatabase,PostgresDatabase}.cpp` | write and read the child table |
| `src/services/SleepHqExportService.cpp` | session files from the table, root files from a denylist-filtered walk |
| `hms-cpapdash-parser/src/EDFParser.cpp` | EVE/CSL vectors, sorted, parsed in a loop |
| `docs/UPLOAD.md` | STR is no longer out of scope |

### Three things the implementation changed

**A duplicate denylist was avoided.** This spec said to copy the live
`residualSkip` from the API. hms-cpap already had one, in
`utils/CardResidue.h`, and it was stale against SDD-049: it denied `.zip` and
`.bin` outright and had no container exemption, so an **ezShare** sweep of a
Löwenstein card would drop `therapy.pdat` once it passed 20 MB. The fix landed
there instead of in a new file, which fixes the SD path as well as the upload.
One existing test pinned `backup.zip` as junk and was updated deliberately.

**The card mirror moved out of `main.cpp`.** It was a lambda, which is why #23
shipped wrong in the first place: nothing could test it. It is now
`mirrorCardInto` in `utils/CardImport.h`, with seven tests covering the wrapped
zip, deep date folders, the credential refusal, the container exemption and
re-upload idempotence.

**The SleepHQ export keeps the date folder's other files.** Making the table
authoritative dropped the `.crc` sidecars that live inside `DATALOG/<date>` --
`downloadDatalogResidue` puts them there on purpose and the table only records
the five analytical kinds. The table now decides the session files and anything
else in the folder that passes the denylist rides along. Without this a night
would have shipped to SleepHQ without its checksums.

`UPLOAD.md` currently tells users STR is "out of scope (sessions only); use
`hms_cpap --backfill <STR.edf>`". A web-upload user has no shell, and the data was
in the upload. That sentence goes.

## Tests

**Part 1**
- Four blocks merging into one session keep all four EVEs. This is the reporter's
  card and it is the regression test; the harness written for this spec becomes a
  gtest.
- The ezShare matcher gets the same test, because it is the same bug.
- A night whose only EVE is the 832-byte stub still reports zero events rather
  than failing.
- Two genuinely separate sessions still get their own EVE each, so the fix does
  not merge what SESSION_GAP_MINUTES separates.
- Parser: a session directory with three EVE files yields the union of their
  events, and the result does not depend on directory iteration order.

**Part 2**
- A card-root zip lands `STR.edf` at the root and date folders under `DATALOG/`,
  and backfill then finds the STR instead of deriving from sessions.
- A wrapped zip (`card/DATALOG/...`) resolves to the same layout.
- `SETTINGS/`, `Identification.tgt`, `*.crc` and `Journal.dat` all survive.
- `ezshare.cfg`, `.DS_Store`, `._foo`, a 30 MB `.mp4` and a `.docx` are all
  skipped, and the response counts them.
- A 25 MB `therapy.pdat` at the root survives the cap; a 25 MB `.mp4` does not.
- The end-to-end case from this spec: upload the card root, get the machine's own
  STR-derived AHI rather than 0.0.

**Part 3**
- A session with three EVEs exports all three, with `import_path`
  `DATALOG/<date>` for each.
- Reparsing a night does not double its rows, and therefore does not double the
  export.
- `Journal.dat`, `SETTINGS/*` and `*.crc` at the card root are exported, where
  the old hardcoded list dropped them.
- **`ezshare.cfg` is never exported**, on any path. This is the one place card
  contents leave the machine.
- A session whose files predate the migration still exports, from the rows the
  migration backfilled out of the old `*_file_path` columns.

## Deliberately out of scope

- **`hms-cpapdash-api`'s copy of the parser defect.** Same root cause, different
  release train. Worth its own issue so it is not rediscovered.
- **The per-minute flow truncation** on these merged nights
  (`hms-cpapdash-parser` issue #1). Separate bug, separate fix.
- **Option C's `cpap_session_files` table.** Only if support attachments come to
  need the full file set.
- **Making `local_dir` accept a DATALOG path again.** SDD-010 settled that.
