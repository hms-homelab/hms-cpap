# SDD-039: an empty list is not an answer, and an upload does not block the app

**Status:** Accepted 2026-09-18, in build, all three decisions as proposed
(Albin: "yes, all of SDD-039"). D1 a failed query is an error, not an empty
list; D2 the zip upload is queued on the backfill worker; D3 the thread count
follows the cores.
**Date:** 2026-09-18
**Repo:** `hms-cpap`. `QueryService` / the read path, the zip upload handler,
the Drogon thread count.
**Reports:** CpapDash support 129, replies 746 and 747 (Michael, 5.2.18)
**Related:** SDD-014 (the zip upload), SDD-037 (the local source)

## Trigger

Two symptoms on one install, neither reproducible here on the same build and
the same data:

- `/api/sessions` returned `[]` with 63 rows sitting in `cpap_sessions`, under
  a device id that matches;
- `/api/dashboard` "never returned at all … had to Ctrl-C after several
  minutes", while the container stayed healthy at 0.21% CPU, and a zip upload
  running at the same time ended in "Upload Failed".

His source and archive are on an SMB share; `/config` and the SQLite file are
on local disk.

## 1. Why a wrong query looks like no data

`executeQuery` returns an empty array when the statement fails to prepare. The
SQLite path logs one line to stderr and returns `arr`
(`SQLiteDatabase.cpp:2960`), and the Postgres and MySQL paths do the same shape.
`getSessions` hands that straight back as the response body, so a query that
FAILED and a night list that is genuinely EMPTY are the same 200 with `[]`.

The sessions query is the one most exposed to this: it is a `UNION ALL` of two
arms built for three dialects, and its own comment says so ("UNION ALL matches
arms by column ORDER … or the whole statement fails to prepare and the sessions
list goes empty"). Any schema drift, on any engine, presents to the user as "my
data is gone" and to us as a support ticket with no error in it.

This is why Michael's report cannot be diagnosed from what we have: the one
piece of evidence that would settle it is a stderr line nobody was watching.

## 2. Why an upload can stall the whole app

`uploadCpapZip` runs `cpap_zip_import_` INLINE on the request thread
(`CpapController.cpp`, the handler body): extract the zip, classify the card,
mirror every file into the archive, then reply. Drogon runs with
`setThreadNum(2)` (`main.cpp:1300`).

On a local disk that is a second or two. On a stalled SMB mount the copy blocks
in the kernel, holds one of the two threads for as long as the mount takes, and
a second slow or blocked request leaves nothing to serve `/api/dashboard`,
which then looks exactly like a deadlock: healthy container, idle CPU, no
answer. I could not reproduce it on local disk (upload of his 23 MB zip, 186
files, dashboard answering 200 in 0.0 s throughout), which fits: the stall is
the mount, the outage is the thread count.

## 3. Design

### 3.1 A failed query says so (D1)

`executeQuery` reports failure rather than returning an empty result that means
two different things. The read path turns that into a 500 with the engine's
message, so the page can say "the database refused this query" instead of
drawing an empty list, and the message reaches the support log without anyone
having attached a terminal.

### 3.2 An upload does not hold a request thread (D2)

The upload writes the zip to disk, hands the import to the backfill worker (the
thread that already owns long ingest work and already has a status endpoint the
upload page polls), and answers at once with `{"status": "queued"}`. The page
already knows how to poll `/api/backfill/status`; it is what a Sefam or
Löwenstein upload does today (SDD-031 D3).

### 3.3 More than two threads (D3)

`setThreadNum(2)` was never a decision, it is the number that was typed. Two
means any two slow requests are an outage. Proposed: the number of cores,
clamped to a small range, so a Pi stays modest and a laptop is not one blocked
read away from an unresponsive dashboard.

## 4. Decisions (Albin's)

- **D1, what a failed query returns.** Proposed: an error to the caller (3.1).
  Alternative: keep returning empty and only log louder, which keeps every
  future instance of this unreportable.
- **D2, the upload path.** Proposed: queue it on the backfill worker (3.2).
  Alternative: keep it inline and only raise the thread count, which leaves a
  10-minute SMB copy holding a connection and a user watching a spinner with no
  status.
- **D3, thread count.** Proposed: cores, clamped (3.3). Alternative: a fixed
  larger number, or a config key, which is one more thing to get wrong.

## 5. Tests

- A deliberately broken query (a column that does not exist) returns an error
  from `executeQuery` and a 500 from the endpoint, on each engine, and the
  message names the column.
- `getSessions` with a healthy database still returns rows, and genuinely empty
  stays a 200 with `[]`.
- The upload endpoint answers `queued` without waiting for the copy; the
  backfill status reports the import; a card that is not a card still fails the
  same way it does now.
- A slow mount does not take the app down: with the import queued, the dashboard
  answers while a copy is in flight (simulated with a deliberately slow
  destination).

## 6. Release

Albin's number, with SDD-038 if they land together. The reply on support 129 is
Albin's. Worth saying in it: this does not explain his empty list on its own, it
makes the next one diagnosable.

## 7. As built (2026-09-18)

- **D1 is a second door, not a changed one.** `executeQuery()` has 185 call
  sites and its "empty on failure" contract is load-bearing in places that only
  want rows (test teardowns delete from tables that may not exist). So
  `IDatabase::executeQueryChecked()` was added beside it, returning
  `{rows, ok, error}`. SQLite implements it directly and `executeQuery()` now
  calls it; MySQL and Postgres record `last_query_error_` in their failure
  branches and the checked call reads it. A backend that overrides neither
  reports success, exactly as before.
- **One door for the read path**: every read in `QueryService` goes through a
  private `query()` that throws `QueryFailed` when the statement did not run.
  The controllers already catch and answer 500 with the message, so nothing else
  changed there. 20 call sites, no behaviour change on a healthy database.
- **D2**: `BackfillService::triggerZipImport(path, job)` queues the existing
  importer on the worker that already owns long ingest, and the handler answers
  `{"status":"queued"}`. The page already polls `/api/backfill/status`, which is
  how a Sefam or Löwenstein upload has always reported (SDD-031 D3).
- **D3**: `setThreadNum(min(8, max(4, hardware_concurrency())))`.

**Tests:** a three-engine case in `test_SessionEndBackends.cpp`: a healthy query
is `ok` with rows, a column that does not exist is `!ok` with the engine's
message and no rows, the old `executeQuery()` door still returns empty for the
same SQL, and a healthy query after a failed one is `ok` (no latched error).
Full suite 1965 tests, 1857 passed, 0 failed, local zone and `TZ=UTC`.

**E2E**: uploading the 23 MB zip from #34 answered `queued` in **0.1 s** (it
used to hold the request for the whole mirror), `/api/dashboard` answered 200
throughout the import, and the backfill status reported
`parsed=16, saved=16, errors=0` when it finished.
