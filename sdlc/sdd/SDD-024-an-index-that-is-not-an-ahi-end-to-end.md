# SDD-024: an index that is not an AHI, end to end

**Status:** Proposed
**Date:** 2026-09-06
**Repo:** `hms-cpap` — schema, three database backends, query layer, every UI
**Version:** target TBD (Albin's call)
**Supersedes:** SDD-023, which fixed one leaf of this and does not work without
the rest. Its commit is deliberately unpushed.
**Related:** SDD-022 (`format`), SDD-081 in `hms-cpapdash-api` (the same problem,
already solved there and worth copying from)

## What we shipped, plainly

`v5.2.0` ships working Sefam S.Box support: cards parse, sessions ingest and
save, the machine is selectable in Settings, the wizard and the installer, and
the README says it is supported.

It also ships **the apnea-only index labelled AHI in every interface**. An S.Box
scores apneas and does not mark hypopneas in any readable form, so its
events-per-hour is roughly two thirds of the same night's AHI on a ResMed
(hypopneas are 35% of the numerator on a real sample: 534 of 1536 across 175
nights). Steve sees a number he will compare against ResMed thresholds, because
we call it AHI and put a history graph next to it.

The parser knows better. `SessionMetrics::index_kind` has been correct since
parser `v2026.8.1`. It is honoured in exactly two places and both are leaves.

## The root cause: index_kind dies at the database

```
parser sets index_kind
  → BurstCollectorService parses               index_kind CORRECT
  → db_service_->saveSession(*parsed)          index_kind DROPPED, no column
  → db_service_->getNightlyMetrics(...)        returns the DEFAULT, AHI
  → publishHistoricalState / UI / reports      all believe it is an AHI
```

Verified: `index_kind` appears **nowhere** in `scripts/schema*.sql`,
`src/database/`, `src/web/`, `src/controllers/` or `frontend/src`.

This is why SDD-023 does not work. It branches on `m.index_kind` at
`DataPublisherService.cpp`, but the Sefam path calls it with metrics read back
from the database:

```cpp
// BurstCollectorService.cpp:1434, the Sefam branch
auto metrics = db_service_->getNightlyMetrics(device_id_, ss.session_start);
data_publisher_->publishHistoricalState(*metrics);
```

so the value is always `AHI` in practice. Its tests pass because they exercise
the pure rule and never cross the database. **A leaf fix on a broken trunk, and
green tests that proved nothing about the real path.**

## Decision

Carry `index_kind` the whole way, then teach every surface to render it.

### The quirk to respect

The number is still COMPUTED and STORED. It is not withheld, not nulled, not
zeroed. Only its *presentation* changes: it must never be called AHI, never be
graded against AHI thresholds, and never be averaged into something labelled
AHI. When there is eventually something to compare it against, the history is
already there. This is Albin's ruling from SDD-081 and it holds here:

> "yes we do compute we just dont create alerts or ai summaries based on it,
> maybe one day when we really compare agains AHI"

## Slice 1 — persistence

**Schema, all three files, kept in sync as `CLAUDE.md` requires.**
`scripts/schema.sql`, `scripts/schema_mysql.sql`, `scripts/schema_sqlite.sql`.

```sql
ALTER TABLE cpap_session_metrics ADD COLUMN index_kind TEXT DEFAULT 'ahi';
ALTER TABLE cpap_daily_summary   ADD COLUMN index_kind TEXT DEFAULT 'ahi';
```

Two tables, because both hold an `ahi` column and both feed UIs:
`cpap_session_metrics` (per session) and `cpap_daily_summary` (per night, which
is what the dashboard and the 30-day trend read).

`DEFAULT 'ahi'` matters: every existing row is ResMed and must keep reading as
one without a backfill. The cloud made the same choice —
`COALESCE(index_kind, 'ahi')` in `QueryController` — and copying it keeps the
two products answering the same way.

Migration follows the existing pattern in `SQLiteDatabase.cpp:308`: unconditional
`ALTER TABLE ... ADD COLUMN`, error ignored when it already exists. Three
backends, three call sites.

**Read and write.** `saveSession` writes it; `getNightlyMetrics` and the session
readers populate `SessionMetrics::index_kind` instead of leaving the default.
This is the line that makes SDD-023 start working, with no change to SDD-023.

## Slice 2 — the query layer

`src/web/QueryService.cpp` selects `ahi` in at least four places (lines 66, 94,
109, 116) feeding the dashboard, the trend and the index. Each gains
`index_kind` alongside, and the dashboard JSON grows a field per night.

`getDashboard()` returns `latest_night`; it gains `latest_night.index_kind`.
The 30-day AHI trend must NOT mix kinds silently — a user who changed machines
mid-window would otherwise get one line averaging two different measurements.

## Slice 3 — every UI surface

One shared Angular helper, not a condition repeated per component. The label and
the decision to grade live in one place; each surface asks it.

| surface | file | today | wanted |
|---|---|---|---|
| Dashboard headline | `pages/dashboard/dashboard.component.ts` | "AHI" + band colour | "Apnea index", no band |
| Dashboard key metrics | `components/dashboard/key-metrics.component.ts` | AHI tile | relabelled |
| CpapDash Index | `dashboard.component.ts` | composite using AHI | see below |
| Sessions list | `pages/sessions/sessions.component.{ts,html}` | AHI column | relabelled per row |
| Session detail | `pages/session-detail/session-detail.component.ts` | AHI | relabelled |
| Events | `pages/events/` | counts only | unaffected, verify |
| STR metrics | `components/dashboard/str-metrics.component.ts` | ResMed-only | unaffected, verify |
| myAir compare | `components/dashboard/myair-compare.component.ts` | ResMed-only | unaffected, verify |

**Per row, not per install.** The sessions list can hold both kinds at once if a
user changed machines, so the label is a property of the row.

**The CpapDash Index needs a decision.** SDD-019 defines it as a 0-100 composite
of usage, AHI and leak. Fed an apnea-only index it produces a score that looks
comparable to a ResMed user's and is not. Options: suppress it, compute it from
usage and leak only and say so, or keep it and label it. **Albin's call, and it
should not be guessed at.**

## Slice 4 — reports and summaries

**PDF reports** (`ReportGeneratorService`, `PdfRenderer`, `GnuplotService`) are
the surface most likely to be handed to a doctor. They read through
`QueryService`, so slice 2 carries the value; the label and any axis title need
the same treatment as the UI.

**The LLM prompt has a second bug SDD-081 missed.** The suppression at
`BurstCollectorService.cpp:2539` withholds the AHI line, but the prompt TEMPLATE
at line 224 still instructs:

```
* AHI assessment (good/moderate/elevated) with value
```

So the model is told to grade an AHI while being told there is not one. It will
comply with the instruction it can act on. The template needs the same
conditional as the value.

## Slice 5 — upload

`CardImport` and the upload controller have **no Sefam awareness** — grep finds
none. The manual upload page is how a user without a bridge gets data in, and
Steve has an SD card and no bridge. Whether a Sefam zip survives that path is
UNKNOWN and must be tested before it is claimed. If it does not, it is a bug in
what `v5.2.0` already advertises.

## Tests

The lesson from SDD-023 is specific: **a test that never crosses the database
proves nothing about a path that does.**

- Round trip per backend: save a session with `index_kind = Ungraded`, read it
  back, assert it survives. SQLite, Postgres, MySQL.
- An existing row with no `index_kind` reads as `ahi` (the default).
- `getNightlyMetrics` returns `Ungraded` for a Sefam night — the exact call
  `BurstCollectorService.cpp:1434` makes.
- **End to end:** ingest the Sefam fixture, then assert the MQTT sensor chosen
  is `apnea_index`. This is the test whose absence let SDD-023 look finished.
- A dashboard response for a Sefam night carries `index_kind`.
- The 30-day trend does not silently mix kinds.

Coverage stands at 80.8% against an 80% gate. This adds branchy code across
three backends, so its tests are not optional.

## Sequencing

1 → 2 → 3, 4, 5 in any order. Slice 1 alone makes SDD-023 correct, so the MQTT
commit ships with it rather than before it.

## Out of scope

- Grading the index. Nothing to grade it against.
- `hms-cpapdash-api`, which already did all of this (`63e7071`). Its remaining
  gap is its own Slice C: a dead severe-AHI toggle on the settings screen.
