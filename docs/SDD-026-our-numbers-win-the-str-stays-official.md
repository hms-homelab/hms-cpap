# SDD-026: our numbers win, the STR stays official

**Status:** Accepted 2026-09-08 ("dale"), amended the same day to the STR's hours
**Date:** 2026-09-08
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
| `duration_minutes`, `patient_hours` | sessions | the STR's `Duration` when the night has one, the sum of our session spans when it does not |
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

**The STR keeps the fields it is better at.** Leak percentiles, mask pressure
percentiles, mode, EPR level, pressure setting, mask pairs: the machine took
those over its own therapy window and we would be guessing. The session writer
stops overwriting them: it fills them only where the STR has left NULL. Today
it replaces the STR's leak p95 with a mean of per-session p95s, which is a
different quantity under the same name.

**The official figure is always on the page.** The STR panel on the dashboard
switches from the shared columns to `ahi_str` and `duration_minutes_str`, with
its subtitle saying it is the machine's own report. It will read differently
from the headline, by design, and the sessions list already explains why.

### The hours, and why they are the STR's

The first cut of this SDD divided by our summed session span. On the night of
2026-09-07 that read 4.04 over 3h13m while cpapdash.com read 4.24 over 3h04m
for the same 13 events. The cloud's hours are the STR's `Duration`
(`hms-cpapdash-api` `ParsingService.cc`, the STR upsert and
`computeIndexesForDevice`), which is the machine's own mask-on time; our span
is the BRP recording, which runs longer than the therapy it records. Albin,
2026-09-08: "lets match the cloud, and divide vs STR duration when it has
one". So the hours are the STR's `Duration` wherever the row has one and our
span only until it arrives, and every index divides by those hours. The
session writer reads `duration_minutes_str` off the row it is about to
update, which is why the collector re-derives after every STR read: the night
moves onto the STR's hours the moment they are known.

**One night, one set of numbers, on every surface.** The sessions list groups
a night the same way and takes the same hours, so its row reads what the
dashboard reads. The session cards on the detail page take the night's hours
in proportion to what each session recorded, so a single session gets all of
them and several still add up to the night, and each card's index is its own
events over its share. Before this the dashboard read 4.24 over 3h04m while
the list and the card read 4.04 over 3h13m for the same single-session night.

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
   `index_source = 'str'` there. Kind A fields as today.
3. **`aggregateDailySummaryFromSessions`.** Runs after every session save,
   not only when the STR is missing. Writes the index columns,
   `duration_minutes`, `patient_hours`, `mask_events`, `index_kind` and
   `index_source = 'computed'`. Kind A fields with `COALESCE(existing,
   excluded)` so an STR value is never replaced.
4. **`getDashboard()`.** The latest-night row also returns `ahi_str`,
   `duration_minutes_str` and `index_source`. Nothing else in the query moves.
5. **Frontend.** The STR panel binds `ahi_str` and `duration_minutes_str`. The
   key-metrics panel drops its re-read of the newest session, since the row is
   ours now; the race that SDD-024 documents goes with it.
6. **Reports and MQTT** read the same columns they read today and change
   value, not code.

## Tests

- SQLite, MySQL and PostgreSQL, the same three cases each:
  STR then sessions: `ahi` is ours, `ahi_str` is the STR's, leak p95 is the
  STR's, `index_source = 'computed'`.
  Sessions then STR: identical result, so order does not matter.
  STR with no sessions: `ahi = ahi_str`, `index_source = 'str'`.
- QueryService: the latest-night payload carries the three new fields.
- A migration test that a 5.2.3 database gains the columns and keeps its rows.

## Out of scope

- The cloud. It keeps its own rule; this SDD says where the two differ.
- The sessions list, which already computes and does not change.
- Recomputing history on upgrade. Rows get their `_str` copy on the next STR
  read, which every burst that closes a night performs, and their computed
  copy on the next session save. A reparse from Settings forces both.
