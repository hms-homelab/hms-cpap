# SDD-034: the missing session key on an old database

**Status:** Draft 2026-09-15, for Albin's decisions (§4). He asked for it on
the update path: "yes to the mysql migration sdd needs to go when update".
**Date:** 2026-09-15
**Repo:** `hms-cpap` (the three database backends' migrations).
**Found in:** SDD-033 §6, chasing three MySQL-only test failures before
tagging 5.2.12.

## Trigger

On the MySQL test box a night that grew from 47 to 71 minutes stayed at 47.
The cause is not the night code: `cpap_sessions` there has only
`PRIMARY KEY (id)`. The upsert every save relies on,

```sql
INSERT INTO cpap_sessions (...) VALUES (...)
ON DUPLICATE KEY UPDATE duration_seconds = VALUES(duration_seconds), ...
```

matches nothing without `UNIQUE (device_id, session_start)`, so each burst
INSERTS THE SAME SESSION AGAIN instead of updating it.

## 1. What is wrong, and for whom

- **All three backends declare the key today.** `UNIQUE (device_id,
  session_start)` is in the `CREATE TABLE` of SQLite, MySQL and PostgreSQL. A
  database created by a current build is fine. Nothing adds it to a database
  created before the declaration did (MySQL's landed 2026-03-28, 3d5d303).
- **What a missing key does, every burst, on a growing night**: a second row
  for the same start, with its own metrics, calculated minutes, breathing
  summary, events and file rows. The night's summary sums them, so the hours
  and the event count inflate; the session list shows the night twice; the
  ledger and `sessionExists` answer about whichever row they find first. The
  visible symptom on the test box was the opposite one, a night frozen at its
  first mask-off, because the duplicate carried no metrics row and the
  aggregate's JOIN dropped it.
- **Who has it**: an install whose `cpap_sessions` predates the declaration
  for its engine. Not knowable from here; the check below is cheap and says so
  per install.

## 2. Design

### 2.1 Ask the database, not the version

At migration time, each backend asks whether a unique index over
`(device_id, session_start)` exists on `cpap_sessions`:

- SQLite: `PRAGMA index_list(cpap_sessions)` and `PRAGMA index_info(<name>)`.
- MySQL: `SHOW INDEX FROM cpap_sessions` (`Non_unique = 0`).
- PostgreSQL: `pg_index` joined to `pg_class`/`pg_attribute`.

Nothing else runs when it is there, which is every current install and every
run after the first repaired one.

### 2.2 Repair, then add the key

When it is missing:

1. **Count the duplicates** (`GROUP BY device_id, session_start HAVING
   COUNT(*) > 1`) and log the number of groups and rows before touching
   anything.
2. **Keep one row per group** (D1): the one with the largest
   `duration_seconds`, ties broken by the largest `id`. That is the most
   complete copy of the night: a session row only ever grows.
3. **Delete the other rows and the rows hanging off them** (D2):
   `cpap_session_metrics`, `cpap_calculated_metrics`, `cpap_breathing_summary`,
   `cpap_events`, `cpap_breaths`, `cpap_vitals`, `cpap_session_files`,
   `cpap_sleep_stages`. They are a shorter copy of what the keeper already
   holds, and every one of them is derived from card files that are still on
   disk: a Reparse rebuilds any of it.
4. **Add the key**: `CREATE UNIQUE INDEX` on SQLite and PostgreSQL (an upsert's
   `ON CONFLICT (device_id, session_start)` is satisfied by a unique index),
   `ALTER TABLE ... ADD UNIQUE KEY uq_device_session` on MySQL.
5. **One transaction per engine**, and one log line whatever happens: the
   number of groups collapsed, rows deleted, and whether the key is now there.
   If the key still cannot be created, the run says so and leaves the data
   alone rather than half-repairing it.

### 2.3 What this does not touch

Nights on a database that already has the key (no rows are read). The daily
summary: it is re-derived from the sessions every burst, so it corrects itself
on the next one. The archive and the card: nothing on disk is written.

## 3. Risk

This is the first migration that DELETES a user's rows. The bound on the
damage is that it only ever deletes rows a unique key would have forbidden,
and only the shorter copies of them. A dry-run switch is cheap to keep (log
what it would delete, change nothing) and is worth having for the first
release that carries this.

## 4. Decisions (Albin's)

- **D1 which row survives.** Proposed: the longest `duration_seconds`, then
  the largest `id`. (Alternative: the newest `id` outright, which is wrong for
  a night whose last burst wrote a short row after a restart.)
- **D2 the rows hanging off the losers.** Proposed: delete them. (Alternative:
  re-point them at the keeper, which risks colliding with the keeper's own
  rows on `(session_id, timestamp)` and would need a merge rule per table.)
- **D3 when it runs.** Proposed: at startup with the other migrations, so an
  add-on user gets it by updating, with a dry-run option for one release.
- **D4 telling the user.** Proposed: the log line only. (Alternative: a line
  in the Settings page's health block.)

## 5. Tests

- Each engine, in its own suite: take a database with the key, drop it, write
  two rows for one start with metrics on each, run the migration, then assert
  one row survives (the longer one), its metrics and minutes are intact, the
  loser's rows are gone, the index is there, and a second run changes nothing.
- The upsert works again afterwards: saving the same session twice leaves one
  row and updates its duration.
- A database that already has the key: the migration reads and writes nothing
  (no log line, no deletions).

## 6. Release

A patch (Albin's number), with the dry-run default decided in D3.
