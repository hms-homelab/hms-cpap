# SDD-029: remove a night

**Status:** Accepted 2026-09-13. D1 it stays removed (the record in 3.1);
D2 the database only; D3 Reparse restores it.
**Date:** 2026-09-13
**Repo:** `hms-cpap`. Three database backends, one route, the burst's re-ingest
paths, the sessions page menu.
**Issue:** hms-homelab/hms-cpap#31 (todd3835)
**Related:** SDD-008 (folder ledger), SDD-026 (the STR stays official, our
numbers win), SDD-028 (the ring's files next to the card)

## Trigger

#31, todd3835: "Is there a way to delete a specific day? … Alternatively I
could wipe it all and start from scratch and just delete that day's data."
Albin, 2026-09-13: "add a remove night to the options of sessions that
completely remove the night from the db".

## 1. What a night is today

A night is a sleep day, `date(session_start - 12h)` (SQLite:1324, MySQL:1761,
Postgres DatabaseService:1368), and it lives in thirteen places:

| Table | Keyed by | From cpap_sessions |
|---|---|---|
| cpap_sessions | id; UNIQUE(device_id, session_start) | parent |
| cpap_session_files | session_id | **no cascade, any backend** |
| cpap_session_metrics, cpap_breathing_summary, cpap_events, cpap_breaths, cpap_vitals, cpap_calculated_metrics, cpap_sleep_stages | session_id | ON DELETE CASCADE |
| cpap_daily_summary | (device_id, record_date) | none |
| cpap_summaries (AI) | device_id, period, range | none |
| cpap_sync_folders (ledger) | date_folder | none |
| oximetry_sessions (+ samples, cascaded) | filename; start_time | none |

No path removes a night. `deleteSessionsByDateFolder` (reparse's first step)
deletes sessions and their file rows on Postgres and SQLite, and **only the
sessions on MySQL** (`MySQLDatabase.cpp:1977`), which orphans
`cpap_session_files`. Nothing ever deletes a `cpap_daily_summary` row.

## 2. Why a plain delete does not stick

Five paths put a deleted night back:

1. **The local burst writes the whole STR history every cycle**
   (`processSessionSummary` → `saveSTRDailyRecords`, BurstCollector:850): the
   day's summary row returns on the next burst. The ezShare, force-complete,
   MQTT and backfill paths do the same.
2. **The SDD-028 folder scan** re-imports the night's `.vld`, which is known by
   filename only.
3. **Session discovery** re-imports a night if it was the newest
   (`session_start > getLastSessionStart()`, SessionDiscovery:443-459, 626-637).
4. **SleepHQ auto-export** queues any archived folder with no stored sessions
   (`markUnparsedNightsForExport`, BurstCollector:3199).
5. **Backfill and the reparse CLI** delete and re-parse whatever is in range.

## 3. Design

### 3.1 The record of what was removed

New table `cpap_removed_nights (device_id, sleep_day DATE, removed_at,
PRIMARY KEY (device_id, sleep_day))` on all three backends. Removing a night
writes it; every path in §2 consults it (D1):

- `saveSTRDailyRecords` and `aggregateDailySummaryFromSessions` skip a removed
  day;
- session discovery and the burst's store loop skip a session whose sleep day
  is removed;
- the `.vld` import skips a file whose start falls on a removed night (the
  oximetry night is `date(start_time - 12h)`, the same rule);
- `markUnparsedNightsForExport` skips a removed night;
- backfill and the reparse CLI skip it too, unless the night is restored (D3).

### 3.2 Removing it

`IDatabase::removeNight(device_id, sleep_day)`, one transaction per backend:

1. insert the `cpap_removed_nights` row;
2. delete `cpap_session_files`, then `cpap_sessions` for every session whose
   sleep day matches (the seven child tables cascade);
3. delete the `cpap_daily_summary` row for the day;
4. delete `oximetry_sessions` whose sleep day matches (samples cascade);
5. delete the day's `cpap_sync_folders` row, so the ledger stops calling it
   partial;
6. delete the night's `cpap_summaries` (AI) rows whose range is that one day.

Returns the counts per table for the log and the UI.

The files on disk (the archive, the card) are not touched (D2).

Also fixed: MySQL's `deleteSessionsByDateFolder` deletes `cpap_session_files`
first, as the other two do.

### 3.3 Route and menu

`DELETE /api/sessions/{date}` → `removeNight` → `{removed: {sessions, daily,
oximetry, …}}`. The sessions page `⋮` menu gains **Remove night**, behind the
page's existing `window.confirm` pattern with the date in the question; on
success the row leaves the list. i18n keys in all five languages
(`sessions.actions.removeNight`, `sessions.actions.removeConfirm`).

## 4. Decisions (Albin's)

- **D1, does it stay removed?** Proposed: yes, the record in 3.1, so a night
  removed today is still gone after the next burst. Without it the day's summary
  row is back within one burst in local mode (the STR), and so is the newest
  night's data.
- **D2, the files.** Proposed: the database only, as asked. The archive keeps
  the card's files (SleepHQ, OSCAR and the zip export read them; removing them
  is a different, destructive feature).
- **D3, getting it back.** Proposed: Reparse on that date restores it (clears
  the record, re-parses the folder). No separate "restore" button.

## 5. Tests

- Each backend: removing a night deletes every row in §3.2 and nothing of the
  night before or after; the counts come back; MySQL leaves no orphan file rows.
- Re-ingest: after removal, a local burst with the STR present does not bring
  back the daily row; the `.vld` scan does not re-import that night's file;
  discovery does not re-store it when it was the newest; SleepHQ does not queue
  it.
- Reparse of that date restores it.
- The route: 200 with counts; an unknown date is 200 with zero counts.

## 6. Release

A minor on top of SDD-028 (Albin's number). The reply on #31 is Albin's.
