# SDD-038: every night on the card gets imported

**Status:** Accepted 2026-09-18, in build. D1 a catch-up step at the END of the
cycle, not a planner rewrite (Albin: "yes b now"); D2 all missing folders at
once on a local source, 3 per cycle on an ez Share; D3 both transports; D4 a
folder that yields nothing is remembered and skipped.

**Why not the full planner.** Albin asked for the cloud's shape (Tier 1 the live
night, then history, then residual, `FirmwarePushController::nextCommand`
SDD-016/SDD-028) and then chose the smaller change first: `executeBurstCycle()`
is 906 lines carrying SDD-008, SDD-026, SDD-029 and SDD-037, and splitting it
into preemptible tiers is a different, riskier release. Today's cycle already
runs live-then-new-then-residual (new sessions are sorted newest-first at
`:1860` and stored file by file); this inserts history between new and residual
by running last. A night interrupting a history import mid-walk needs the real
planner and stays out of scope.
**Date:** 2026-09-18
**Repo:** `hms-cpap`. The burst's discovery anchor, both source branches.
**Reports:** hms-homelab/hms-cpap#34 (todd3835), CpapDash support 129 reply 747
(Michael), and the same shape earlier in support 128 (the night of 9/12).
**Related:** SDD-008 (the folder ledger), SDD-010 (the local dir is a card root),
SDD-029 (a removed night stays removed), SDD-037 (a local folder is a finished
night)

## Trigger

Two users, two transports, one sentence in the log:

```
CPAP: Found 16 date folders
CPAP: Scanning folders from: 20260909
CPAP: 3 folders with potentially new data
```

todd3835 rebuilt a container against a card holding 16 nights and kept his
database, whose newest session was 2026-09-09. Thirteen folders were never
looked at, on every cycle, forever: "It added exactly 1 day, 9/9. The rest of
the days are just missing."

Michael hit the same thing after a restart: "a fresh local rescan only covered
20 of the 61 date folders … 9/13 fell entirely outside the scan window."

## 1. Why

The cycle asks discovery for sessions from an anchor, not for what is on the
card. `BurstCollectorService.cpp:1336` takes `getLastSessionStart()`, `:1354`
takes `getNthLatestSessionStart(device, 2)`, and discovery keeps only folders
`>= last_date` plus the previous day
(`SessionDiscoveryService.cpp:571`, and the same rule in the ezShare path), then
per session `is_new || is_today || is_recent (48 h) || is_retained`
(`:643`).

That is right for a card that only grows forward, which is what an ez Share was
when the rule was written. It is wrong for every case where the FILES are older
than the DATABASE:

- a container rebuilt against a card with history (#34);
- a local folder pointed at an archive (support 129);
- a card restored, swapped, or borrowed from another machine;
- any night that was skipped once, for any reason, and fell behind the anchor.

`BackfillService` already scans every folder with no anchor at all
(`:224` groups each folder directly), which is why "run a backfill over the
range" is the workaround we keep giving people. The machinery exists; nothing
starts it on its own.

## 2. What "never seen" means

A date folder whose night has no session row for this device. The parts are all
present already:

- the folder list is what discovery just enumerated;
- a folder maps to a night with `strDayForSessionStart()` (SDD-029 §7: the
  folder name IS the night key, checked against a real card);
- `markUnparsedNightsForExport()` (`BurstCollectorService.cpp:3383`) already
  walks the archive asking exactly this question for SleepHQ, so the query
  shape is not new.

A removed night (SDD-029) is not "never seen": it was removed on purpose and
stays out until a Reparse or an upload restores it.

## 3. Design

### 3.1 The catch-up pass

At the end of a cycle, when nothing else needed doing, take the folders
discovery listed, drop the ones with a session for that night, drop removed
nights, and hand the oldest **N** of what is left to the same path a backfill
uses. Log one line naming what it found and what it will do, so an install with
nothing to catch up says nothing and an install with 13 missing nights says so
in the log the user is already reading.

Bounded by N per cycle (D2) so a 61-folder card cannot turn one burst into an
hour, and so the live night is never behind a history import.

### 3.2 Where it runs

In the collector, after the store loops, for both branches. The ezShare path
pays a listing per folder, which is why N matters more there; the local path is
a directory read.

### 3.3 What it does not change

The anchor stays as it is for the live path: the newest nights are still what a
cycle looks at first, and the 48-hour window and the retained second-newest
night are untouched. This adds a slow lane, it does not widen the fast one.

## 4. Decisions (Albin's)

- **D1, is the catch-up automatic? ACCEPTED**: yes, as the last step of the
  cycle (3.1), not a planner rewrite. See the status note.
- **D2, how much per cycle? ACCEPTED**: every missing folder in one pass on a
  local source, where the cost is a directory read; **the oldest 3 per cycle on
  an ez Share**, where each folder costs a listing and its downloads on the
  card's WiFi. A 41-folder backlog is complete in about 14 cycles, and no cycle
  grows by more than three folders of work.
- **D3, does it apply to an ez Share too? ACCEPTED**: both. Michael's 9/12 and
  9/13 were an ez Share.
- **D4, a folder that yields no session? ACCEPTED**: remembered and skipped,
  with one log line naming it. Held in memory, as the SDD-028 `.vld` scan state
  is: it costs no migration, and a restart retrying each bad folder once is the
  behaviour we want anyway when a mount was the reason.

## 5. Tests

- Discovery: a card with 16 folders and a database whose newest session is the
  15th returns the 13 older folders as missing, in oldest-first order, and none
  of them once they are stored.
- A removed night is never in the missing set; restoring it puts it back.
- The bound holds: N folders per cycle, the oldest first, and the newest nights
  are still handled before it.
- A folder that yields no session is not retried on the next cycle (D4).
- E2E, the reported shape: import a card, delete every session but the newest,
  restart, and the history comes back by itself within a few cycles. This is
  todd3835's exact case and it is reproducible in one command.

## 6. Release

Albin's number, with SDD-039 if that lands together. The replies on #34 and
support 129 are Albin's.

## 7. As built (2026-09-18)

- **`BurstCollectorService::historyCatchUpFolders(card_folders, cap)`** answers
  "which folders has this database no night for", oldest first, capped. The
  night key and the folder name are the same string (SDD-029 §7), so it is a set
  difference: `SELECT DISTINCT <sleepDay(session_start)>` for this device, minus
  removed nights, minus `history_tried_`.
- **Discovery takes the answer**: `discoverNewSessions()` and
  `discoverLocalSessions()` gained a `catch_up_folders` set. It is added to the
  folder list AFTER the anchor's cut, and a session in one of those folders
  bypasses the new/today/recent/retained tests, which by construction all refuse
  it. That is the whole mechanism; everything else is unchanged.
- **The caps are where D2 put them**: the local branch passes 0 (all of them),
  the ez Share branch passes `kEzShareCatchUpPerCycle` (3) and pays one
  `listDateFolders()` for the question.
- **D4 is a two-set rule**: what the last cycle asked for is `history_requested_`,
  and anything still missing when the next cycle looks moves to
  `history_tried_`. The log says so once: "20260701 yielded no night, not asking
  for it again this run".
- **Not done, on purpose**: the tier planner. Today's order (live, new, then
  catch-up folders inside the same discovery pass) is the shape SDD-040 makes
  explicit once there is one cycle to put it in.

**Tests:** three cases in `test_SessionDiscoveryService.cpp` (a folder behind the
anchor is scanned when named, naming one does not drag in its neighbours, a
named folder that is not on the card changes nothing). Full suite 1965 tests,
1857 passed, 108 skipped, 0 failed, local zone and `TZ=UTC`, all three engines.

**E2E, the reported shape, on the 15-night card from #34**: import all 15
nights, delete every session but the newest (a rebuilt container keeping its
database), restart.

```
CPAP: 14 night(s) on the card are not in the database; importing 14 this cycle (20260826 first)
CPAP: Scanning folders from: 20260909
CPAP: catching up on 14 folder(s) the database has no night for
```

All 15 nights were back after that one cycle. An empty folder added afterwards
was asked for once and then remembered, and the next cycles logged no catch-up
at all.
