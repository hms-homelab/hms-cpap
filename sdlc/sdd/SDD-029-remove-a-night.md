# SDD-029: remove a night

**Status:** Released in 5.2.6 (2026-09-13, see §7). Accepted 2026-09-13:
D1 it stays removed (the record in 3.1); D2 the database only; D3 Reparse
restores it.
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

## 7. As built (2026-09-13)

Where the build differs from §3, and what the throwaway run showed.

- **The key is `night`, `YYYYMMDD`**, not a `sleep_day DATE`: the same string
  `strDayForSessionStart()` returns and the DATALOG folders are named with
  (checked on a real card: folder `20260329` holds `20260330_041405_BRP.edf`),
  so the folder-level skips compare strings with no conversion. One helper
  header, `include/services/RemovedNights.h`, holds every check.
- **The ring's night is on the ring's clock.** The oximetry parsers read the
  ring's display time as UTC (`timegm`), so `oximetryNightOf()` shifts back
  12 h on the UTC clock. The CPAP rule (local clock) would put a ring start
  between noon and noon-plus-the-UTC-offset on the night before.
- **Where the skips are:** the STR write at all three callers (burst, backfill,
  `--backfill` CLI); the four burst store loops (local, ezShare, Lowenstein,
  Sefam), beside `isForceCompleted`; the folder ledger (`updateFolderLedgers`,
  no row and no sidecar refetch for a removed folder, found in the E2E, where
  the ledger row came back every burst); the `.vld` card scan; the **live ring
  pull** (`OximetryService`, not in §3.1: it remembers the file→night pair so a
  removed night is not re-downloaded every poll, and fetches it again once
  restored); `markUnparsedNightsForExport`; the backfill folder loop.
- **Not touched:** `aggregateDailySummaryFromSessions` and session discovery.
  The first derives rows only from stored sessions, which a removed night no
  longer has; the second only proposes, and the store loops refuse.
- **The `.vld` upload is not filtered**: an operator uploading a night is
  asking for it. The card scan skips a removed night's file without adding it
  to the refused set, so a restore brings it back on the next pass.
- **Route:** `DELETE /api/sessions/{date}`; a date that is not `YYYY-MM-DD` is
  400; any valid date is 200 with the counts (zero when nothing was there).
- **Reparse** (`POST /api/sessions/{date}/reparse` and `--reparse`) clears the
  record first. The backfill derives the daily row from sessions only when the
  card has no STR, so a night the STR does not cover gets its daily row back
  on the next burst (seconds later), not from the reparse itself.

**Tests:** `tests/database/test_RemoveNightBackends.cpp`, six cases on each
engine, run on SQLite, PostgreSQL 16 (a throwaway local database) and MySQL
(the NAS test database), plus the key rules; three `.vld` cases in
`test_OximetryImport.cpp`. Full suite 1850 tests, 1572 passed, 278 skipped
(engine-gated), 0 failed, under the local zone and `TZ=UTC`.

**E2E** (throwaway instance, port 18993, pre-written config, card with
20260330 and 20260406 from a card backup plus todd3835's 20260823 and a
`.vld`): removing 2026-04-06 (STR-covered) and 2026-08-23 (newest, with the
ring night) returned `{sessions:1, daily:1, ledger:1, oximetry:0|1}`; two
bursts later neither night had sessions, a daily row or ring data, the STR
write logged 180 records instead of 181, and each store loop logged "on a
removed night, skipping". Reparse of 2026-08-23 restored it: the record
cleared, the backfill re-stored the session, the next burst re-derived the
daily row (477 min, as before) and re-imported the ring night.

**On the Pi** (picpapdash2, the live ezShare install, 2026-09-13, branch build
cross-compiled on the hub): Albin's history copied from the NAS card copy into
the archive (287 nights before 2026-08-27, read-only mount, nothing the card
delivered overwritten) and backfilled: 256 sessions, 0 errors. Night
2026-08-23 (two sessions, one starting 02:27 on the 24th; 60 file rows;
daily 234 min computed, STR 261) removed through the route: `{sessions:2,
daily:1, ledger:1}`, the nights either side untouched. A backfill of
08-22..08-24 logged "20260823 is a removed night, skipped", re-parsed the
neighbours and rewrote the archive's STR (which covers 08-23) without the
day's row coming back. Reparse restored it to the baseline, row for row. The
ezShare burst path could not be exercised there: the card's AP had been gone
since 12:59 (NetworkManager `ssid-not-found` on wlan1), hours before the test.

Two fixes the Pi run needed, both outside §3:
- **Reparse answered 503 on every ezShare install**: the backfill service was
  wired only when `local_dir` was set. It is now wired from `local_dir`, else
  `archive_dir` (a1c1695). Without it D3 cannot hold on the Pi.
- **The backfill left the STR's numbers on the nights it imported**: it derived
  the daily summary from sessions only when the card had no STR, where the
  burst always does (SDD-026). 196 of the 233 imported nights read
  `index_source='str'`; after the fix all 233 read `computed` (6da1b44).
