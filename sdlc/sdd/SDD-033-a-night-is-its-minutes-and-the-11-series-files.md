# SDD-033: a night is its minutes, and the 11 series' other signal files

**Status:** Accepted 2026-09-15 (§4): D1 a multi-session night's percentiles
are the STR's; D2 events are summed; D3 a patch for both repos. Built and
verified 2026-09-15 (§6), not released.
**Date:** 2026-09-15
**Repo:** `hms-cpap` (the night queries on three engines, the sessions list,
discovery, the archive), with `hms-cpapdash-parser` (the record-count rule).
**Issue:** hms-homelab/hms-cpap#33 (TLaren), found on the card he shared
(`aircurve11_vauto_sample.zip`, 3 nights, AirCurve 11 VAuto).
**Related:** SDD-030 (the bi-level sensors, confirmed on his card), SDD-032
(the archive's record count), SDD-014 (session grouping), SDD-026 (ours wins,
STR fills the gaps), SDD-002 (the DATALOG residue).

## Trigger

Albin, 2026-09-15: "check out the data from issue 33". TLaren's own card, run
through the 5.2.11 code with MQTT on, confirms every point of #33 (SDD-030):
`ipap`/`epap`/`pressure_support` published, `therapy_mode` 8, no oximeter reads
unknown, the STR's bi-level settings and targets published, the in-progress STR
day (`-1` sentinels) neither stored nor published. It also shows two defects
that are not his, and not the AirCurve's alone. Albin: "yes please both to the
sdd".

## 1. A night's averages count a session, not a minute

### What the card says

Night 2026-09-12 is two sessions: 161 minutes and 103 minutes. Read straight
from the PLD files (every 2 s sample):

| | session 1 | session 2 | night, by sample | published |
|---|---|---|---|---|
| IPAP (`Press.2s`) | 9.023 | 9.768 | **9.313** | 9.395 |
| EPAP (`EprPress.2s`) | 5.023 | 5.768 | **5.313** | 5.395 |

9.395 is (9.023 + 9.768) / 2: the two sessions weighed the same. (#33 quotes
5.31 for `hist_avg_epr_pressure` on his 5.2.7 install, the weighted figure;
which night that was is not known here.)

### Why

`getNightlyMetrics` and `getMetricsForDateRange` (SQLite, MySQL, PostgreSQL)
average each session's minutes first, then `AVG()` those averages across the
night's sessions (`SQLiteDatabase.cpp:2357-2396`, `:2513-2547`; the same shape
in `MySQLDatabase.cpp` and `DatabaseService.cpp`). Every minute-averaged
figure of a multi-session night carries it: leak, respiratory rate, tidal
volume, minute ventilation, inspiratory and expiratory time, I:E, flow
limitation, the pressure p95, mask, EPR/EPAP and therapy/IPAP pressure,
snore, target ventilation, and `avg_pressure` from the breathing summary. A
3-minute mask-fit session weighs as much as a 7-hour night.

The same holds one layer up:
- `aggregateDailySummaryFromSessions` (three engines) takes a plain mean of the
  sessions' `avg_mask_pressure`, `leak_p50`, `leak_p95`, `avg_spo2` and
  `avg_epr_pressure` (`SQLiteDatabase.cpp:2250-2254`). The AHI beside them is
  already duration-weighted.
- The sessions list (`QueryService.cpp:308-309`) does the same for SpO2 and
  heart rate.

### And the night's events are its biggest session's

The same two queries take each event type as `MAX` across the night's
sessions. On 2026-09-12 the first session's EVE holds 1 central apnea (23:45)
and the second's 5 other events (05:14 to 06:04), so the night has 6. The
`MAX` of each type gives 5, and the published `ahi` reads **1.14**, where the
daily summary (which sums) reads **1.36** and the card's STR says 1.30. Two
AHIs for one night, on every multi-session night. `MAX` has been there since
the first SQLite implementation (7c99f63) with no reason given; discovery
gives each session its own EVE files (SDD-014), so a sum counts nothing twice.

### Design

- **Minute figures**: per session, the subqueries return a sum and a count
  instead of an average, and the night divides the sums by the counts:
  `SUM(c.sum_leak) / NULLIF(SUM(c.n_leak), 0)`. That is the mean over the
  night's minutes, exactly what one long session gives today. `MAX`/`MIN`
  stay as they are.
- **Session means** (SpO2 and heart rate in the sessions list and the night,
  the daily summary's EPR/EPAP pressure): weighted by the session's duration,
  `SUM(x * s.duration_seconds) / SUM(s.duration_seconds)` over the sessions
  that have the value (a NULL or a zero where the query already treats zero as
  absent does not dilute it).
- **Percentiles (D1)**: see §1.1.
- **Events**: `SUM` per type, like the daily summary and the sessions list.
  The AHI is then the same one the daily summary computes.

Nights already stored are corrected on read, and the daily summary on its next
re-derive (every burst re-aggregates the device). No reparse.

### 1.1 A multi-session night's percentiles are the STR's (D1)

A percentile does not combine. A night's p95 is the value 5% of the night's
samples exceed, and a session's p95 keeps one point of its samples, not where
the rest sit. On 2026-09-12, from the card's 2 s leak samples:

| | leak p95, L/min |
|---|---|
| session 1 (161 min) | 8.4 |
| session 2 (103 min) | 16.8 |
| **the night, from all samples** | **15.6** |
| **the STR's `Leak.95` for the day** | **15.6** |
| duration-weighted mean of the two | 11.7 |
| plain mean (stored today) | 12.6 |

The top 5% of the night is almost all the leakier second session; no
weighting of the two session values finds that. The machine computes its STR
day over every sample, which is why it matches exactly. A single-session night
is exact already: the parser takes its percentiles from the samples
(`hms-cpapdash-parser` `Models.cpp`), and a night whose mask-on blocks merged
into one session (his 2026-09-10) is a single session.

So, the standing rule's one exception (Albin, 2026-09-15: "for multisession
nights str wins"):

- The daily summary's percentile columns that our sessions fill, `leak_50`,
  `leak_95`, `mask_press_50` and `spo2_50`, take the STR's value on a night
  of more than one session, and ours on a single-session night (as today).
  Where the card has no STR day (Löwenstein, Sefam, a ResMed night the STR
  has not reached yet) ours stays, an estimate. The columns only ours never
  touch (`mask_press_95`, `mask_press_max`, `leak_max`, `spo2_95`) are the
  STR's already.
- **Storage**: today the STR's value is lost once ours is written. The STR
  save keeps ours (`COALESCE(leak_95, excluded.leak_95)` once
  `index_source = 'computed'`). So these four get the STR copy the index family
  got in SDD-026: `leak_50_str`, `leak_95_str`, `mask_press_50_str`,
  `spo2_50_str` on the three engines, their migrations and the three schema
  mirrors, written by every STR save. The re-derive picks:
  `CASE WHEN COUNT(sessions) > 1 THEN COALESCE(<_str>, ours) ELSE
  COALESCE(ours, <_str>) END`.
- The night's `leak_p50` and `leak_p95` (the `historical/leak_p50` and
  `historical/leak_p95` sensors and the AI night summary, today the `MAX`
  of the sessions') follow the same rule: the STR's copy on a multi-session
  night, the session's own on a single-session night.
- The published Home Assistant daily sensors (`str_leak_95` and the rest)
  are the STR's already and do not change.
- Not a percentile with an STR twin: the historical `pressure_p95` sensor is
  the mean of per-minute p95s of the 25 Hz pressure. It is not a night
  percentile on any night and has no STR counterpart. It is weighted by
  minutes like the other minute figures, and otherwise stays what it is.

## 2. The 11 series' other signal files

TLaren's card carries two signal files the code knows only by accident:

| file | signals | how hms-cpap fetches it | record-count repair (SDD-032) |
|---|---|---|---|
| `*_SA2.edf` | `Pulse.1s`, `SpO2.1s` | as a checkpoint, Range-resumed, like SAD | **no**: the rule names BRP/PLD/SAD |
| `*_TCV.edf` | `TrigCycEvt.40ms` (25 Hz), 300 to 900 KB a session | as DATALOG **residue** | **no** |

- **SA2** is the 11 series' oximetry file (SAD on the 10). It is resumed with
  Range like the others, so its archived header is the mid-recording one, and
  the repair skips it. With a ResMed oximeter attached, OSCAR would read that
  SpO2 short the way ticket 127 read the flow.
- **TCV** is not in `isCpapEdf`, so `downloadDatalogResidue` treats it as a
  card file that rides along: it downloads **every** such file in tonight's
  folder **in full on every burst**, for as long as the folder is scanned (the
  comment there assumes a tiny `.crc`). On this card that is up to 1.05 MB a
  night (three sessions' TCV on 2026-09-10), every 65 s, over the ez Share:
  the back-to-back full-download shape
  that wedged the ez Share on the bench. And the archive copies it only when
  its size changes, so its finalized header never lands either.
- The file header facts are read from his card; that ResMed leaves SA2's and
  TCV's count at `-1` while recording is inferred (his files came off the card
  finalized), from the same writer doing it to BRP/PLD/SAD. Whether OSCAR reads
  TCV at all is not known here; the repair costs nothing either way.
- The cloud's copy of the rule (`cpapdash-ingest-lib`) has the same gap for
  its OSCAR and SleepHQ archives.

### Design

- **The rule (parser)**: `isResmedSignalEdf` also names `_SA2.edf` and
  `_TCV.edf`. Same repair, same guard (only when the data divides evenly).
- **TCV is fetched like a checkpoint**: discovery collects it beside
  BRP/PLD/SAD and hands each session the TCV files that share a checkpoint
  prefix with its own (`session.tcv_files`, with their sizes and card stamps).
  It is not part of the grouping, so the sessions a night splits into do not
  change. `downloadSessionFiles` resumes it with Range like BRP, and the
  archive step repairs it. `isCpapEdf` names it, so the residue pass skips it.
- Local mode reads the card in place and is unaffected.

## 3. What does not change

The parser's reading of the night, the grouping of sessions, the STR path, the
sensors published (the same names, the right values), and every single-session
night's figures (a sum over one session's minutes is its average).

## 4. Decisions (Albin's, 2026-09-15)

- **D1 percentiles**: "for multisession nights str wins" (§1.1). The first
  proposal, a duration-weighted mean of the session percentiles, was
  withdrawn: on his card it lands further from the truth (11.7) than today's
  plain mean (12.6). The rejected alternatives were per-session histograms
  merged per night (exact on every machine, but a parser output and a new table
  on three engines) and leaving percentiles approximate.
- **D2 events**: "yes to sum". The published AHI is the daily summary's.
- **D3 release**: "yes to both patches bumps". §1 and §2 in one patch for
  hms-cpap and a parser patch for the rule, pinned. The cloud's copy of the
  rule is a VPS change, not in this SDD.

## 5. Tests

- The three engines, parameterized like `test_BilevelPressureBackends.cpp`: a
  night of two sessions of unequal length (161 and 103 minutes of per-minute
  rows): each minute figure is the minute-weighted mean, each session mean
  the duration-weighted one, the events their sum and the AHI the daily
  summary's; a single-session night reads as before.
- The same three engines for D1: an STR day saved before and after the
  re-derive, on a two-session night and on a one-session night: the four
  percentile columns are the STR's on the first and ours on the second,
  whichever write lands last; with no STR day they are ours; the `_str`
  columns exist after the migration on a database created by the last
  release.
- Discovery: a TCV file joins its session's `tcv_files` and not the grouping
  (the split is the same with and without it); the residue pass skips it; a
  second burst resumes it with Range.
- Parser: SA2 and TCV are named; EVE/CSL/STR still are not.
- Archive: an SA2 and a TCV with a stale count are repaired by the archive
  step and the sweep.
- End to end on TLaren's card: the local path publishes IPAP 9.313 and EPAP
  5.313 for 2026-09-12, and an AHI equal to the daily summary's; the daily
  summary's `leak_95` for 2026-09-12 is 15.6 (the STR's); an ez Share
  stand-in serving his card with Range fetches each TCV once and then resumes
  it, and the archived night is byte-identical to the card.

## 6. As built (2026-09-15)

### Code

- The night queries on the three engines (`getNightlyMetrics`,
  `getMetricsForDateRange`): each session's minute subquery returns a SUM and
  a COUNT per figure, and the night divides the summed sums by the summed
  counts. Events are summed; the mean event duration is weighted by events;
  the AHI follows from the summed counts.
- The daily summary re-derive: session means weighted by duration, and D1
  applied by a SECOND statement, not by joining the row being written. MySQL
  leaves an `INSERT ... SELECT` that reads its own target undefined: joined
  there, the re-derive silently stopped updating the night (its DailyHours
  cases caught it). The follow-up `UPDATE` counts the night's sessions in
  `cpap_sessions` and takes `COALESCE(<x>_str, <x>)` when there is more than
  one.
- `leak_50_str`, `leak_95_str`, `mask_press_50_str`, `spo2_50_str` on the
  three engines, their migrations and the three schema mirrors, written by
  every STR save; `include/database/StrPercentile.h` holds the one rule for
  what counts as "the STR has none" (a leak of 0 is a reading, a pressure or
  SpO2 of 0 is not, a negative never is).
- `QueryService`'s sessions list: SpO2 and heart rate weighted by duration.
- TCV: `SessionFileSet::tcv_files`, filled by both discovery paths from the
  checkpoint prefix (outside the grouping), fetched by
  `downloadSessionFiles` like a checkpoint, and named by `isCpapEdf` so the
  residue sweep skips it. Parser v2026.8.4 names `_SA2.edf` and `_TCV.edf` in
  `isResmedSignalEdf`, so SDD-032's repair covers them.

### Tests

- `tests/database/test_NightAggregationBackends.cpp`, 6 cases on each engine:
  the minute-weighted means, the summed events and the AHI, D1 on a
  multi-session night, a single-session night keeping its own percentile, a
  card with no STR day, and the STR's sentinels not being copied.
- TCV in `test_SessionDiscoveryService.cpp` (three cases) and `_TCV.edf` in
  `test_CardResidue.cpp`; the new columns in the MySQL migration list.
- Full suite 1654 passed, 0 failed, local zone and TZ=UTC. Postgres: the
  engine suites pass locally. MySQL (the NAS box over the LAN): 23 pass,
  including the new suite on MySQL and the migration.
- **Three MySQL failures that predate this work** (they fail with the branch
  stashed too). Albin, 2026-09-15: "can we check those tests in mySQl before
  we tag this version". Chased to three separate causes, and MySQL is now
  116 of 116:
  - `IndexKindSurvivesTheRoundTrip`: a real bug, MySQL only.
    `insertSessionMetrics` never wrote `index_kind` and `getNightlyMetrics`
    never read it, so every night on MySQL came back as an AHI since the
    column landed (2026-09-06, SDD-024). A Sefam night's apnea-only index was
    published under the name AHI, which is the one thing SDD-024 exists to
    prevent. Fixed in its own commit.
  - `ALiveNightGrowsPastTheStrSnapshot`: the NAS test database was created
    before `UNIQUE KEY uq_device_session (device_id, session_start)` existed
    (in the table definition since 2026-03), so the session upsert never
    matched and a growing night inserted a duplicate row instead of updating.
    The key was added to that database. **An install whose tables predate
    that key has the same gap, and no migration adds it**: open for Albin,
    since de-duplicating a user's sessions is a data change.
  - `TheSessionsListSurvivesItsOwnSql`: another test's ring recording, left
    in the shared test database. The oximetry arm is keyed on the ring's own
    device, so it rides along in every device's list. The test now asserts on
    the CPAP arm's rows.

### End to end, on TLaren's card (#33)

Local mode, MQTT on. Night 2026-09-12, two sessions of 161 and 103 minutes:

| | before | now | the card |
|---|---|---|---|
| `ipap` | 9.395 | **9.3133** | 9.313 |
| `epap` | 5.395 | **5.3133** | 5.313 |
| `historical/ahi` | 1.14 | **1.3636** | STR 1.30, daily summary 1.36 |
| `total_events` | 5 | **6** | 1 + 5 in the two EVE files |
| `leak_p95` | 16.8 | **15.60** | 15.6 from every leak sample, and the STR's day |

`pressure_support` stays 4.00, and `avg_pressure` still reads 6.44, the
waveform mean SDD-030 D2 kept.

Over a local ez Share stand-in serving the same card: burst 1 attached each
`_TCV.edf` to its session and fetched all six once (`TCV: 20260910_220005_TCV.edf
(294 KB)`, …), and they are in the archive. Burst 2 asked for no TCV at all,
where the residue sweep used to re-download every one of them in full.

### Regression check: the same cards through both binaries

Albin, 2026-09-15: "we need to make sure we did not introduce any regression
using this new calculations". The 5.2.11 binary was built from the commit
before this one and each card was run through both, from an empty database,
and every stored night compared.

| card | nights | sessions | per-session metrics | nights changed |
|---|---|---|---|---|
| AirSense (Albin's own, 40 folders) | 181 | identical | identical | 26 |
| AirCurve 11 (TLaren's) | 9 | identical | identical | 1 |
| Löwenstein Prisma | 18 | identical | identical | 2 |
| Sefam S.Box | 173 (241 sessions) | identical | identical | 0 |

- Every changed night is a multi-session night, and every multi-session night
  changed: 26 of 181, 1 of 9, 2 of 18, 0 of 173. No single-session night moved
  anywhere.
- Only the intended columns moved: `mask_press_50` (26), `leak_95` (22),
  `leak_50` (6) on the ResMed cards, `epr_level` (2) on the Löwenstein one.
  The AHI, the event count, the duration, the mode and the index kind are
  untouched on every card.
- The API agrees: `/api/sessions` (400 nights), `/api/statistics`,
  `/api/insights` and `/api/dashboard` are byte-identical between the two
  binaries on the AirSense card.
- The summed events change only a night whose sessions each carry events. On
  the AirSense card one session of each pair holds them all, so its published
  AHI is unchanged on all 26; on TLaren's card the two sessions hold 1 and 5,
  which is the 1.14 that became 1.36.

## 7. Release

Parser 2026.8.4, hms-cpap 5.2.12 pinned to it (Albin's numbers), tagged once
validated. The reply on #33 is Albin's.
