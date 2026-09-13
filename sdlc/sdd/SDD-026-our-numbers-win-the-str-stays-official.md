# SDD-026: our numbers win, the STR stays official

**Status:** Accepted 2026-09-08 ("dale"), amended the same day to the STR's hours,
amended again 2026-09-13 back to OUR hours: calculated metrics supersede the STR,
which only fills what we did not compute (see "The hours are ours")
**Date:** 2026-09-08, amended 2026-09-13
**Repo:** `hms-cpap`. Schema, three backends, the two daily-summary writers,
the dashboard, list and detail queries, two dashboard panels.
**Version:** 5.2.4
**Related:** hms-cpapdash-api v2026.30.0 (the cloud already does the index half
of this), `hms-cpapdash-parser/docs/RESMED_CALCULATION_RULES.md`, SDD-019 (the
week index), SDD-024 (index kind)

## Trigger

On picpapdash2 the dashboard headline for 2026-09-07 read the STR's AHI while
the sessions list read ours, and the two differ: 4.24 against 4.04, 3h04 against
3h13. Albin, 2026-09-08: "we cant let the STR win we need to do it like the
cpapdash.com because is more accurate the way we calculate than STR and our
duration as well (no time between masks) and in general calculation (even if it
shows partial, that's fine) should win. We still show the resmed oficial, is
just gonna be different."

## Why the STR wins today

`cpap_daily_summary` has one set of index and duration columns and two writers.
`aggregateDailySummaryFromSessions()` derives them from our sessions;
`processSTRFile()` upserts the STR's values over the same columns whenever an
STR is read. The dashboard, the week index, the 30-day trends and compliance all
read that table, so once an STR arrives the machine's figures replace ours
everywhere except the sessions list, which computes from `cpap_session_metrics`.
The key-metrics panel patches over it by re-reading the newest session, which is
why the headline and the trend can disagree on the same page.

## The rule

Two copies of every index and of the night's duration, each in its own column,
each with one writer:

| Column | Writer | Meaning |
|---|---|---|
| `ahi`, `ai`, `hi`, `oai`, `cai`, `uai`, `rin` | sessions | `count / hours`, full precision, our event counts from the EVE files over the hours in the next row |
| `duration_minutes`, `patient_hours` | sessions | the sum of our session spans; the STR's `Duration` only for a night we have no sessions for |
| `ahi_str`, `ai_str`, `hi_str`, `oai_str`, `cai_str`, `uai_str`, `rin_str` | STR | ResMed's own, floored to one decimal by the file format |
| `duration_minutes_str` | STR | ResMed's own mask-on time |
| `index_source` | both | `computed` when the session writer has filled the row, `str` when only the STR has, so a mismatch is explainable |

**Ours wins wherever the night has sessions**, partial or not. Everything that
reads `ahi` and `duration_minutes` today keeps reading those columns and gets
our numbers without knowing anything changed: the headline, SDD-019's week
index, the 30-day trends, compliance, the PDF report, the MQTT sensors.

**The STR still fills the same columns when we have nothing.** The STR carries
history the card no longer does: 207 therapy days on Albin's card against 12
nights of session files. A night with no sessions keeps the STR's index and
duration in `ahi` and `duration_minutes`, marked `index_source = 'str'`, so
the trends stay populated for the whole history. The session writer overwrites
that the moment the night gets sessions. The STR writer never overwrites a
`computed` row.

**Leak, pressure and SpO2 are ours too (2026-09-13).** `leak_50`, `leak_95`,
`mask_press_50` and `spo2_50` are measured by our sessions, so ours stand
wherever the sessions have them, and the STR fills one only where ours is NULL.
The STR writer no longer overwrites them on a night we have sessions for.

**The STR keeps what we do not compute.** Mode, EPR level, pressure setting,
mask pairs, and the figures we have no copy of (`mask_press_95`,
`mask_press_max`, `leak_max`, `spo2_95`, respiratory rate, tidal volume, minute
ventilation) are the machine's. Our session writer has only placeholders for
mode and mask pairs, so it fills those only until an STR has written the row.

**The official figure is always on the page.** The STR panel on the dashboard
switches from the shared columns to `ahi_str` and `duration_minutes_str`, with
its subtitle saying it is the machine's own report. It will read differently
from the headline, by design, and the sessions list already explains why.

### The hours are ours (amended 2026-09-13)

On 2026-09-08 this SDD was amended to divide by the STR's `Duration` whenever
the row had one, to match cpapdash.com's 4.24 over 3h04m for the night of
2026-09-07 (ours read 4.04 over 3h13m). That rule is withdrawn.

The STR counts a mask-on period only once it ENDS. During a live night it is a
snapshot of the last mask-off, so the STR's hours stop moving at the first
break. On picpapdash2 the night of 2026-09-12 read 47 minutes on the
dashboard, with 71 minutes recorded across three mask-ons and the mask on: the
STR had been read once, after the first mask-off (21:38 to 22:25). Our single
event was divided by those 47 minutes too, so the AHI read 1.28 instead of
0.85. The cloud hit the same freeze on unit 9 the same night (47 minutes of STR
against 62 recorded) and was fixed in `cpapdash-ingest` 2026.8.23.

Albin, 2026-09-13: "using the STR as showing the dash or the session is a
regression specially since the live session should be parsing and showing the
actual files in growing state", and "between a STR and a parsed session the
parsed sessions and calculated metrics wins, with the STR fallback". His
standing rule: calculated metrics supersede the STR.

So the night's hours are the sum of our session spans, every index divides by
them, and they grow with the files. The STR's `Duration` sits in
`duration_minutes_str` beside them and fills `duration_minutes` only for a
night we have no sessions for. The session writer no longer reads anything off
the STR's columns, so the order the two writers run in no longer matters.

**One night, one set of numbers, on every surface.** The sessions list groups
a night the same way and takes the same hours, so its row reads what the
dashboard reads. Each session card on the detail page shows its own recorded
span and its own index, and the cards add up to the night.

What stays ours is the numerator, and that is the whole point: the STR's own
index is floored to one decimal by its file format, ours is exact.
`RESMED_CALCULATION_RULES.md` section 4 says exactly this, and the cloud does
exactly this, minus one thing: the cloud falls back to the STR's index when
our event count is below the bound the floored STR implies. This SDD does not
fall back. A night with fewer events in our copy than the STR's floor implies
is a night with missing files, and SDD-008's Partial state is the place that
is reported, not a silent swap of the number.

## What changes

1. **Schema.** Eight new REAL columns and `index_source TEXT` on
   `cpap_daily_summary` in the three schema files, added by the existing
   version-gated migrations on SQLite, `addColumnIfMissing` on MySQL, and the
   PostgreSQL migration path.
2. **`processSTRFile` / `saveSTRDailyRecords`.** Writes the seven `_str`
   indexes and `duration_minutes_str` unconditionally. Writes `ahi` and friends
   and `duration_minutes` only where `index_source` is not `computed`, and sets
   `index_source = 'str'` there. On a `computed` row it fills `leak_50`,
   `leak_95`, `mask_press_50` and `spo2_50` only where they are NULL
   (2026-09-13); the fields we do not compute it writes as before.
3. **`aggregateDailySummaryFromSessions`.** Runs after every session save,
   not only when the STR is missing. Writes the index columns,
   `duration_minutes`, `patient_hours`, `mask_events`, `index_kind` and
   `index_source = 'computed'`, all over the sum of our session spans
   (2026-09-13; it no longer joins the STR's row). `leak_50`, `leak_95`,
   `mask_press_50` and `spo2_50` are ours where the sessions have them and keep
   what is there otherwise; mode, EPR level and mask pairs stay the STR's once
   it has written the row.
4. **`getDashboard()`.** The latest-night row also returns `ahi_str`,
   `duration_minutes_str` and `index_source`. Nothing else in the query moves.
5. **Frontend.** The STR panel binds `ahi_str` and `duration_minutes_str`. The
   key-metrics panel drops its re-read of the newest session, since the row is
   ours now; the race that SDD-024 documents goes with it.
6. **Reports and MQTT** read the same columns they read today and change
   value, not code.

## Tests

- SQLite, MySQL and PostgreSQL, the same cases each:
  STR then sessions: `ahi` and `duration_minutes` are ours, `ahi_str` and
  `duration_minutes_str` are the STR's, leak p95 is ours,
  `index_source = 'computed'`.
  Sessions then STR: identical result with no re-derive in between, so order
  does not matter.
  STR with no sessions: `ahi = ahi_str`, `index_source = 'str'`.
  A live night (2026-09-13): an STR snapshot of the first mask-off does not
  cap the hours; a later session save grows them.
  A leak gap: where our sessions have no leak, the STR's fills it.
- QueryService: the latest-night payload carries the three new fields; the
  sessions list, the session cards and the dashboard read the same hours and
  index for the same night.
- A migration test that a 5.2.3 database gains the columns and keeps its rows.

## Out of scope

- The cloud. It keeps its own code; since `cpapdash-ingest` 2026.8.23 it
  follows the same rule (our usage, leak and pressure; the STR fills gaps).
- Recomputing history on upgrade. Rows get their `_str` copy on the next STR
  read, which every burst that closes a night performs, and their computed
  copy on the next session save. A reparse from Settings forces both.
