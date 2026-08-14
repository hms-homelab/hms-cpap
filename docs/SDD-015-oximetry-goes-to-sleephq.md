# SDD-015: The ring's oximetry goes to SleepHQ too

**Status:** Implemented; e2e pending
**Date:** 2026-08-14
**Repo:** `hms-cpap`
**Version:** target TBD (Albin's call)
**Depends on:** SDD-003 (debounced SleepHQ export), SDD-014 (the export reads the card)
**Related:** 4.4.5 web upload (`O2RingCsvParser`)

## Trigger

A Mule and Miner user with a Wellue or Viatom ring has their SpO2 and heart
rate collected, parsed, stored and charted, and then loses all of it the moment
the night goes to SleepHQ. The export ships what is on the card. Oximetry never
was on the card: it arrives over the wire from the ring and lands straight in
`oximetry_sessions` / `oximetry_samples`.

So the one place a user goes to view CPAP and oximetry side by side gets only
half of what this machine already has. For a ring owner that is the half they
bought the ring for.

The reverse direction already works. `/api/upload/oximetry` takes the CSV the
phone app exports and `O2RingCsvParser::parse` turns it into an
`OximetrySession`. This spec is that function's inverse.

## What SleepHQ accepts

SleepHQ imports Wellue/Viatom oximetry as **CSV**, the file the ring's own app
exports, uploaded alongside the CPAP data rather than through a separate
oximetry flow. SleepHQ's own export is a different format of its own making;
that direction is not this spec's business.

Three real customer exports settle the shape, from support ticket 16 on the
VPS. They are not one format. There are **two dialects**, and a device picks
both its header text and its timestamp style together:

```
Checkme O2 Max
Time,Oxygen Level,Pulse Rate,Motion,O2 Reminder,PR Reminder
00:16:55 Jun 26 2026,95,86,29,0,0

O2Ring S
Time,SpO2(%),Pulse Rate(bpm),Motion,SpO2 Reminder,PR Reminder,
"11:20:29PM Jun 19, 2026",89,60,0,0,0,
```

Four things here that a from-memory implementation gets wrong:

- **Six columns, not three.** `Motion` is real data, and the two `Reminder`
  columns are always present.
- **The header text is per-device.** `Oxygen Level` versus `SpO2(%)`,
  `O2 Reminder` versus `SpO2 Reminder`. Our parser survives this by reading
  columns **positionally** and ignoring the header, which is worth keeping.
- **The O2Ring S dialect has a trailing comma on every line**, header included,
  so each row is seven fields with the last one empty.
- **Motion is not a flag.** A real row carries `29`.

Filenames follow the device, not the night:

```
Checkme O2 Max _20260626001655.csv     <- note the space before the underscore
O2Ring S_20260619232029.csv
```

so the convention is `<device name>_<YYYYMMDDHHMMSS>.csv`, the timestamp being
the session start.

Both dialects are files real users upload to SleepHQ, so both are accepted.
**This spec emits the Checkme dialect**: 24-hour, unquoted, six columns, no
trailing comma. It is the one with no quoting rules to get wrong, and it is the
dialect our parser treats as primary.

## What we already have

- `oximetry_sessions` + `oximetry_samples`, written by both the ring intake and
  the CSV upload path.
- A per-night sample query already exists, in `QueryService.cpp:649-657`,
  selecting `timestamp, spo2, heart_rate, motion` for a sleep day, engine-aware
  through the `sql::` helpers. **The exporter should not grow a second way to
  ask this question.**
- `O2RingCsvParser`, which is the authority on the format because it is what
  reads it today.

## Design

### One writer, and it is the parser's inverse

`O2RingCsvWriter::write(const OximetrySession&) -> std::string`, beside the
parser and named for it, so the pair is obvious to whoever finds one of them.

The parser accepts two dialects seen in the wild:

```
06:53:07 Apr 12 2026        24-hour, unquoted
"11:20:29PM Jun 19, 2026"   12-hour AM/PM, quoted, comma after the day
```

**The writer emits the first and only the first.** A writer that can produce
two dialects is a writer with a bug waiting in the branch nobody exercises. The
parser keeps accepting both, because files in the wild have both; that
asymmetry is deliberate and is the point.

Output, byte for byte the shape of a real Checkme export:

```
Time,Oxygen Level,Pulse Rate,Motion,O2 Reminder,PR Reminder
06:53:07 Apr 12 2026,97,58,0,0,0
```

`Motion` is carried through from the stored sample. The two `Reminder` columns
are written as `0`: we do not collect them, and a real file always has them.

### Invalid samples are written as the sentinels, not dropped

A ring reports "no reading" as SpO2 255 / HR 65535, and the parser maps those to
`0xFF` so `OximetrySample::valid()` excludes them. The writer emits the original
sentinels rather than skipping the row.

Dropping them would silently compress the timeline: a gap where the ring was off
the finger would become a shorter night rather than a night with a gap, and the
interval auto-detection on re-import would then measure the wrong cadence. The
existing `QueryService` query filters on `valid`, which is right for a chart and
wrong here, so **the exporter needs its own query that does not filter**.

### It goes on its own, as its own import

**Decided (Albin, 2026-08-14): the CSV is uploaded ALONE. Not zipped, and not
mixed in with the respiratory upload.**

So this is not another entry in `collectExportFiles`. The night's card files go
up as they do today, and the oximetry CSV goes up as a **separate SleepHQ
import** containing exactly one file. `SleepHqClient` already supports this
without changes: `createImport` / `uploadFile` / `processFiles` are per-import,
so a second import is a second call of the same three.

That also disposes of the placement question this spec previously left open.
There is no card layout to fit into when the import contains one file: it goes
at that import's root, `import_path ""`.

Why separate is the right call and not just a preference: the two uploads have
different failure modes and different lifetimes. A night's EDFs settle when the
machine stops writing; a ring session settles when the ring finishes syncing,
which is a different clock. Bundling them would mean one retry policy, one
backoff, and a failed CSV holding a good night's therapy data hostage.

### Which night, and what it is called

One CSV per ring session, named the way the ring names its own exports:

```
<device name>_<YYYYMMDDHHMMSS>.csv     from the session's start time
```

The device name comes from the stored session where we have it, falling back to
`O2Ring`. Matching the real convention matters more than being tidy: if SleepHQ
keys on the filename at all, a name it has never seen is the thing that breaks.

The file is generated to a temp path at export time rather than stored. It is
derived data, and writing it into the card archive would put a file on the card
that was never on the card.

The trigger stays the one that already exists: when a night is exported, its
ring session, if there is one, is exported too. Same moment, two imports.

## What changes

| File | Change |
|---|---|
| `include/services/O2RingCsvWriter.h`, `src/services/O2RingCsvWriter.cpp` | **NEW.** The parser's inverse |
| `src/services/SleepHqExportService.cpp` | `oximetrySessionFor`, the night's samples unfiltered by `valid` |
| `src/services/SleepHqExportService.cpp` | `exportOximetry`, a second oximetry-only import beside the night's |
| `docs/UPLOAD.md` | the round trip is now both ways |

### Two things the implementation changed

**No new method on four backends.** The spec proposed adding one to `IDatabase`
and implementing it four times. `executeQuery` plus the `sql::` dialect helpers
already exist and are what `QueryService` uses for the same question, so the
query lives in the export service and costs no interface churn. The read is
still split from the upload (`oximetrySessionFor`) for the same reason
`collectExportFiles` was: it is the half that can be tested without a network.

**Numbers come back as strings.** `executeQuery` returns whatever the engine
gave it, and the engines disagree: reading `spo2` with `asInt()` throws "Value
is not convertible to Int" on SQLite. Every numeric read goes through a
tolerant conversion. This was a runtime surprise rather than a compile error,
and the multi-engine test is what caught it.

## Tests

- **Round trip.** `parse(write(session))` returns the same samples, the same
  timestamps and the same interval. This is the test that matters, and it is
  free because both halves are ours.
- A session with sentinel samples round trips **with the gaps intact**, and the
  re-parsed session has the same duration as the original.
- The 12-hour dialect still parses, so keeping the reader permissive is pinned.
- A night with no oximetry adds no file, rather than an empty one.
- Sub-minute and per-second sessions keep their detected interval.
- Backends: the new query returns the same rows on SQLite, MySQL and
  PostgreSQL, including the invalid samples the chart query hides.
- The oximetry import contains exactly one file, and the night's card import
  is **unchanged** by the presence of a ring session. That second half is the
  regression guard: this must not perturb what already works.
- A failed oximetry upload does not fail the night's therapy upload.

Fixtures are **written by hand in the shape of** the real exports, not copied
from them. The three files that settled the format are a customer's therapy
data sitting in a support ticket, and they do not belong in a public repo.

## Deliberately out of scope

- **Bundling the CSV with the card files.** Decided against: see above. If
  SleepHQ ever grows a combined import that handles both, revisit.
- **The cloud (`hms-cpapdash-api`).** It has its own SleepHQ path and its own
  oximetry store.
- **Other vendors.** This is the Wellue/Viatom CSV because that is the format we
  parse and the one SleepHQ takes.

## Sources

- SleepHQ O2 Ring manual page — https://sleephq.com/o2
- Apnea Board wiki, Wellue/Viatom file import —
  https://www.apneaboard.com/wiki/index.php/Wellue_Viatom_File_Import
- `ezshare_cpap`, which uploads `SD_Card` CSVs to SleepHQ —
  https://github.com/iitggithub/ezshare_cpap
