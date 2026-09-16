# SDD-034: the missing session key on an old database

**Status:** Accepted 2026-09-15 (§4). It runs on the update path, as Albin
asked ("yes to the mysql migration sdd needs to go when update"), and the
FIRST release only looks: it reports what it would collapse and changes
nothing. The repair itself follows in the next patch, once the reports from
real installs have been read.
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

### 2.2 The first release only looks (D3)

`IDatabase` gains `inspectSessionKey()`, answering three things: is the unique
key there, how many `(device_id, session_start)` groups have more than one
row, and how many rows those groups hold. It reads; it never writes. The
default implementation says "key present", so a backend that does not
implement it reports nothing.

The startup migration calls it and logs one line, only when something is
wrong:

```
cpap_sessions has no unique key on (device_id, session_start): 12 night(s)
stored twice or more, 27 row(s) in them. This build only reports it
(SDD-034); the next one collapses them.
```

Nothing else happens in this release. That line is what says whether any real
install has the gap at all, and how big it is where it does.

### 2.3 Repair, then add the key (the next release)

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

## 4. Decisions (Albin's, 2026-09-15)

- **D1 which row survives**: the longest `duration_seconds`, ties broken by
  the largest `id`. A session row only ever grows, so that is the most
  complete copy; the newest id alone would shrink a night to the few minutes a
  restart wrote after the full block.
- **D2 the rows hanging off the losers**: deleted with them. They are a
  shorter copy of what the keeper holds and every one is derived from card
  files still on disk, so a Reparse rebuilds any night. Re-pointing them would
  need a merge rule per table, since the loser's minutes collide with the
  keeper's on `(session_id, timestamp)`.
- **D3 how careful the first release is**: it only looks. The release carrying
  this reports what it WOULD collapse and changes nothing, so the logs from
  real installs are read before a single row is deleted. The repair lands in
  the next patch.
- **D4 telling the user**: the log line, for now.

## 5. Tests

**This release (the report):** each engine, given a `cpap_sessions` created
the old way (no unique key) holding two rows for one start:
`inspectSessionKey()` says the key is missing and counts one group of two
rows; the rows are still there afterwards, untouched; on a database with the
key it reports present and zero, and reads nothing else.

**The next release (the repair):** same starting point, and after the
migration one row survives (the longer one), its metrics and minutes are
intact, the loser's rows are gone, the index is there, a second run changes
nothing, and saving the same session twice then leaves one row with the
updated duration.

## 6. Release

The report goes in the next patch (Albin's number). The repair follows in the
one after, once the reports say who has the gap.
