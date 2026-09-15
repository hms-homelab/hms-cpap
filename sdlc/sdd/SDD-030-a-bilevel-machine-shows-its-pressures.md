# SDD-030: a bi-level machine shows its pressures

**Status:** Accepted 2026-09-15 ("go" on the triage). D1-D3 taken as proposed;
Albin can overrule any.
**Date:** 2026-09-15
**Repo:** `hms-cpap` (the database layer on three engines, the MQTT publisher,
the collector). No parser change.
**Issue:** hms-homelab/hms-cpap#33 (TLaren, AirCurve 11 VAuto, add-on 5.2.7)
**Related:** the parser's machine family (its SDD-064: `STRDailyRecord::family`
and the `bl_*` settings), SDD-019 (absent is not zero), SDD-026 (our numbers).

## Trigger

#33: an AirCurve 11 VAuto card parses cleanly (sessions, events, AHI and usage
match the card), but Home Assistant sees AirSense pressures. No IPAP, EPAP or
pressure-support sensor; EPAP published as `avg_epr_pressure`; `therapy_mode`
reading 0 where the card says 8; `str_spo2_50` at 0.00 % with no oximeter.

## 1. What the code does today (verified 2026-09-15)

- The parser reads all three PLD pressure channels per minute:
  `MaskPress.2s` → `mask_pressure`, `Press.2s` → `therapy_pressure`,
  `EprPress.2s` → `epr_pressure`. On an AirSense, Press is the delivered
  pressure and EprPress the expiratory set-point; on a bi-level they are IPAP
  and EPAP (the reporter's arithmetic: Press − EprPress equals the prescribed
  `S.VA.PS` of 4.00 on both sessions of a night).
- hms-cpap stores `mask_pressure` and `epr_pressure` per minute in
  `cpap_calculated_metrics` and averages them per night. **`therapy_pressure`
  is checked (a minute that has only it is kept) but never stored**: no
  engine has the column. So IPAP is thrown away.
- `avg_pressure` is the mean of the BRP breathing summaries, the measured
  waveform; on a bi-level it averages EPAP and IPAP breath by breath.
- The parser tags every STR day with the machine family from its signal set
  (`BiLevel` when `S.VA.*` or `S.S.*` exist) and reads the prescribed bi-level
  settings (`bl_max_ipap`, `bl_min_epap`, `bl_ps`, `bl_ipap`, `bl_epap`) and
  the daily targets (`tgt_ipap_*`, `tgt_epap_*`). hms-cpap uses none of them.
- The mode is kept as the card reports it (8 on a VAuto) and the daily row
  keeps it. The session's `therapy_mode` is set by the parser for Löwenstein
  only, so on a ResMed that sensor is never published; where the reporter's 0
  comes from is not found without his card. The AI night summary names mode
  8 "ASV (Variable EPAP)", which on an AirCurve is VAuto.
- `str_spo2_50` is published unconditionally, so no oximeter reads 0 %.

## 2. Design

### 2.1 Store IPAP

`cpap_calculated_metrics.therapy_pressure` (REAL/FLOAT/DOUBLE) on the three
engines, with each engine's add-column migration and the three schema
mirrors. The per-minute insert writes it (COALESCE-upsert, like its
neighbours). The nightly reads average it into `avg_therapy_pressure`, the
same way `avg_epr_pressure` is averaged, so IPAP and EPAP come from the same
minutes. Nights stored before the upgrade gain it on their next reparse.

### 2.2 Know the machine is bi-level

The collector takes the family from the STR days it parses (the newest day's
family) and hands it to the publisher. No new storage: every source reads the
STR early in a run (local mode every burst; ezShare on the first burst).
Until an STR has been read the publisher behaves as today.

### 2.3 Publish the bi-level pressures

On a `BiLevel` machine the publisher announces and publishes three more
historical sensors, per night:

| sensor | value |
|---|---|
| `ipap` | nightly mean of `therapy_pressure` (PLD Press) |
| `epap` | nightly mean of `epr_pressure` (PLD EprPress) |
| `pressure_support` | `ipap − epap` |

And three daily sensors from the STR day, when the card has them:
`str_max_ipap` (`S.VA.MaxIPAP`, or `S.S.IPAP` on a fixed bi-level),
`str_min_epap` (`S.VA.MinEPAP`, or `S.S.EPAP`), `str_pressure_support`
(`S.VA.PS`), plus `str_tgt_ipap_95` and `str_tgt_epap_95`.

The mapping is pure functions (`include/services/BilevelSensors.h`), tested
without a broker like `indexSensorsFor`.

A machine that stops being a bi-level (a user who changed machines) has these
entities removed: an empty retained payload on each state topic and on each
discovery config, SDD-023's rule that a switch must not leave an entity frozen
on its last value.

### 2.4 The mode: the session's first, the STR's when it is 0, named through the family

The standing rule (Albin, 2026-09-15: "prio is always the session data, with a
fallback to the STR if session values are 0") applied to the mode. No parser
fills a ResMed session's mode, and every engine stores "none" as 0 (SQLite and
MySQL bind a missing int as 0, PostgreSQL uses `value_or(0)`), so the per-night
`therapy_mode` sensor read 0 on every machine: the reporter's 0, reproduced on
two AirCurves. `therapyModeFor(session, str)` returns the session's mode unless
it is 0, else the STR's; the publisher applies it whichever of the historical
and STR publishes arrives last, and the AI night summary uses it too.

A mode number is named with the family: on a bi-level VAuto is 8 on an
AirCurve 11 and 6 on an AirCurve 10 (two real cards, both with only VAuto
settings in their STR); any other bi-level number is "Bi-level (mode N)"
rather than a guessed name. On the other families the existing names stand.

### 2.5 Absent SpO2 is absent

`str_spo2_50` carries a value only when above 0, the rule SDD-019 applies to
leak. Otherwise it is published as `None`, retained: Home Assistant's MQTT
payload for unknown. Skipping the publish would not be enough, because an
earlier version's retained 0.00 stays on the broker and on the dashboard (the
broker-backed test caught exactly that). The entity stays, for the night an
oximeter is attached.

## 3. Decisions

- **D1 scope:** all of §2 in one release. (Alternative: §2.5 alone as a patch.)
- **D2 `avg_pressure`:** kept as it is on every machine, so no Home Assistant
  dashboard or automation that reads it breaks; the bi-level sensors are added
  beside it. (Alternatives: relabel it, or suppress it on a bi-level.)
- **D3 the reporter's card:** his offered zip is Albin's reply; until it
  arrives the tests are synthetic, and the source of the reported 0 mode stays
  open.

**Parser change (corrected 2026-09-15).** This section first said the parser
needed no change. Real data proved otherwise: the parser read the daily
targets (`TgtIPAP.*`, `TgtEPAP.*`, `TgtVent.*`) only inside its ASV branch
(mode 7 or 8). An AirCurve 11 VAuto is mode 8 and got them by accident; an
AirCurve 10 VAuto is mode 6 and lost them. `hms-cpapdash-parser`
`EDFParser_STR.cpp` now reads them whenever the card wrote them, like the
bi-level settings; `test_str_family.cpp` pins it (mode 6 keeps its targets, a
machine without the signals has none). hms-cpap's release then needs a parser
tag and its `GIT_TAG` pin moved to it.

## 4. Tests

- Database, parameterised over the engines: a minute's `therapy_pressure`
  round-trips, and the nightly read returns its mean as `avg_therapy_pressure`.
- Pure mapping: a bi-level night gives `ipap`, `epap` and their difference; an
  AirSense gives none; a missing channel gives none of the three; STR settings
  map from both `S.VA.*` and `S.S.*`; mode names through the family; SpO2 0
  and −1 are absent.
- Full suite under the local zone and `TZ=UTC`.

## 5. Release

A patch (Albin's number). The reply on #33 is Albin's.

## 6. As built (2026-09-15)

- `therapy_pressure` on `cpap_calculated_metrics` in all three engines and the
  three mirrors; the per-minute insert writes it (fill-in upsert); the nightly
  read returns `avg_therapy_pressure`.
- Tests, all green: `test_BilevelPressureBackends` (three cases per engine, run
  on SQLite, a throwaway PostgreSQL 16 and the NAS MySQL); the MySQL migration
  suite's must-exist column list now includes `therapy_pressure`; the add-column
  migration proven on an existing database on PostgreSQL (column dropped,
  reconnect, cases pass) and SQLite (a 5.2.7-era database gains it on start);
  `test_BilevelSensors` (pure rules); two broker-backed publisher tests against
  a local mosquitto: a bi-level STR day publishes its settings and `None` for
  SpO2, and a bi-level night publishes IPAP 9.02, EPAP 5.02, PS 4.00 after the
  sensors are announced, while leaving bi-level removes them. Full suite 1614
  passed, 0 failed, local zone and `TZ=UTC`, with the broker up.
- **On real cards (2026-09-15)**, with Albin's go and the owners' consent: one
  night each of an AirCurve 11 VAuto (Ken Narod, unit 12, product 39494) and
  an AirCurve 10 VAuto (device 77, product 37289), copied read-only from the
  cloud to a throwaway instance with a local broker, and deleted afterwards.
  Both label the PLD channels `Press.2s`/`EprPress.2s`; on both
  Press − EprPress equals the prescribed `S.VA.PS` (3.00). Checked against an
  independent reader of the EDF files:

  | | AirCurve 11 VAuto | AirCurve 10 VAuto |
  |---|---|---|
  | ipap / epap (night) | 8.786 / 5.786 (file 8.79 / 5.79) | 11.709 / 8.709 (file 11.71 / 8.71) |
  | pressure_support | 3.00 | 3.00 |
  | therapy_mode | 8 (was 0) | 6 (was 0) |
  | str_max_ipap / min_epap / PS | 14 / 5 / 3 | 15 / 8 / 3 |
  | str_tgt_ipap_95 / epap_95 | 9.00 / 6.00 | 12.84 / 9.84 = file (was absent) |
  | str_spo2_50 | None | None |

  Two findings from them: the stored-0 mode (§2.4) and the parser's ASV-gated
  targets (§3). An Air11 file copied mid-write keeps a stale record count in
  its header (3, 0 against 442 minutes on disk); the parser sizes from the
  data, as it should.
- Full suite after both: hms-cpap 1616 passed, 0 failed (local zone and
  `TZ=UTC`, broker up); parser 171 passed.
