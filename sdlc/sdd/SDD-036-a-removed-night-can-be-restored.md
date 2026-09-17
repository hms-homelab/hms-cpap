# SDD-036: a removed night can be restored

**Status:** Implemented, not released (see §7). Accepted 2026-09-17: D1 the
Removed nights line under the sessions table; D2 an uploaded card restores the
removed nights it holds.
**Date:** 2026-09-17
**Repo:** `hms-cpap`. One route, the sessions page, the zip upload summary.
**Ticket:** CpapDash support 128 (Michael, ezShare, Docker on Linux)
**Related:** SDD-029 (remove a night), SDD-032 (the archive carries the real
record count)

## Trigger

Ticket 128. Michael removed 2026-09-12 and 2026-09-13 to get them downloaded
again. They never came back: not after several bursts, not after a restart, and
not after he uploaded known-good card zips, whose files did land in the archive.
The log said why:

```
BackfillService: 20260913 is a removed night, skipped
BackfillService: complete — parsed=0, saved=0, deleted=0, errors=0
```

His agent's conclusion: "Right now there's no way back for a night once it's
been removed, through any path in the app." That is correct for the UI.

## 1. What SDD-029 promised and what the UI can do

SDD-029 D3: "Reparse on that date restores it (clears the record, re-parses
the folder). No separate 'restore' button." The confirm dialog repeats it:
"Reparse this date to bring it back."

The Reparse action lives in the `⋮` menu of a session row
(`sessions.component.html:115`). Removing a night deletes every session of that
night, and `removeNight()` drops the row from the list
(`sessions.component.ts:305`). So the only button that restores a night is on a
row that no longer exists. The route works (`POST /api/sessions/{date}/reparse`
clears the record first, `CpapController.cpp:1629`); nothing in the app calls
it for a removed night.

Nothing lists what was removed either. `IDatabase::removedNights(device_id)`
exists on all three backends (`include/database/IDatabase.h:120`), but no route
exposes it.

## 2. The second way in that stays shut

A card zip upload of a removed night imports nothing:

- **ResMed zip:** `mirrorCardInto` copies the files into the archive, then the
  backfill skips the folder (`BackfillService.cpp:210`). Nothing in the upload
  response says so; the only trace is the log line above.
- **Sefam and Löwenstein zips:** `CardUpload.cpp:258,273` count it as
  `removed` and say so in the log.

SDD-029 §7 already made the opposite call for the ring: "The `.vld` upload is
not filtered: an operator uploading a night is asking for it." The card zip
got the stricter rule without that being decided.

## 3. Design

### 3.1 List the removed nights

`GET /api/removed-nights` → `{"nights": ["2026-09-12", "2026-09-13"]}`, from
`removedNights(device_id)`, keys turned from `YYYYMMDD` into `YYYY-MM-DD`, newest
first. Not under `/api/sessions/`, where `GET /api/sessions/{date}` would claim
the path.

### 3.2 Restore from the sessions page (D1)

When the list is not empty, the sessions page shows a line below the table:
"2 removed nights" that expands to one row per night: the date and a
**Restore** button. Restore calls the existing reparse route, which clears the
record and re-parses the archive's folder, then reloads the list the way
Reparse already does (`sessions.component.ts:280`).

No new restore route: D3 of SDD-029 stays the mechanism, it only gets a place
to be pressed.

The confirm text changes from "Reparse this date to bring it back" to "Restore
it from Removed nights below the list." i18n in all five languages
(`sessions.actions.removedNights`, `sessions.actions.restore`,
`sessions.actions.removeConfirm`).

### 3.3 What Restore brings back

Restore re-parses what the archive holds. It does not download from the card.
Since SDD-029 D2 keeps the archive's files, that is the whole night in the
normal case. When the archive folder is missing or empty, the reparse restores
the record and finds nothing to parse, and the burst will not fetch it either:
ezShare discovery scans from the newest stored night forward
(`SessionDiscoveryService.cpp:378`), so only the newest nights come back from
the card on their own. An older night with no files needs a card zip upload
(3.4). Out of scope here: a "download this night from the card again" action.

### 3.4 The zip upload (D2)

Proposed: an uploaded card restores the removed nights it contains, the same
rule SDD-029 set for the `.vld` upload. For a ResMed zip, `cpap_zip_import_`
clears the record for each date folder it mirrored before triggering the
backfill; `CardUpload` does the same for Sefam and Löwenstein sessions before
its skip check, and the `removed` counter goes away.

The alternative (keep skipping) needs the upload result to name the skipped
nights, so the user is not left reading logs as Michael was.

## 4. Decisions (Albin's)

- **D1, where Restore lives.** Proposed: the collapsible "Removed nights" line
  under the sessions table (3.2). Alternatives: a date field "Restore a night"
  in Settings; or keep the removed night as a greyed row with only Restore in
  its menu.
- **D2, does uploading a card restore its removed nights?** Proposed: yes (3.4),
  matching the `.vld` upload. Alternative: keep skipping, report it in the
  upload result.
- **D3, the reply to Michael.** The two curls in ticket 128 (reply 734) already
  restore his nights on 5.2.16. This SDD changes nothing for him beyond the
  button.

## 5. Tests

- Each backend: `removedNights` after two removals and one restore returns the
  one left, as `YYYY-MM-DD` through the route; an install with none returns an
  empty list.
- The route: 200 with the list; no removed nights is 200 with `[]`.
- Frontend: the line is hidden with no removed nights; Restore calls
  `/api/sessions/{date}/reparse` and the night leaves the removed list.
- D2 as decided: a ResMed zip for a removed night stores its sessions and
  clears the record (or: stores nothing and names the night in the result);
  the same for a Sefam zip.
- E2E on a throwaway instance (pre-written config, per the SDD-014 trap):
  remove a night, see it in the list, Restore, the sessions and daily row come
  back, a burst later it is still there.

## 6. Release

5.2.17 (Albin, 2026-09-17: "bump the patch"). The reply on ticket 128 is Albin's.

## 7. As built (2026-09-17)

Where the build differs from §3, and what the throwaway run showed.

- **Two helpers in `include/services/RemovedNights.h`**, next to the SDD-029
  ones: `removedNightDates()` (the list, `YYYY-MM-DD`, newest first) and
  `restoreUploadedNights()` (clears only the removed nights among the dates
  given, either form, and returns them).
- **Route:** `GET /api/removed-nights` → `{"nights": [...]}`, through a
  `removed_nights_` hook wired beside `night_remove_` in `main.cpp`. With no
  hook wired it answers an empty list, not 503: nothing removed is the truth.
- **D2 is done in the upload handler, not in `CardUpload`.** §3.4 proposed
  dropping the skip inside `importCardSessions`. That function imports the whole
  kept store (`uploads/<format>/`), not this upload, so dropping its skip would
  restore every removed night any earlier upload had put there. The handler
  instead clears the records for the nights THIS upload holds (the ResMed
  mirror's `dates`, the Sefam/Löwenstein `cardNights`) before it triggers the
  import; the skip and its `removed` counter stay for everything else. The
  reply carries `restored_nights`, and each is logged as
  `Upload: restored removed night <date>`.
- **Sessions page:** the line sits under Load more and shows whenever a night
  was removed, even when the table is empty. Restore calls
  `reparseSession(day)`, takes the night off the list at once, and reloads the
  sessions two seconds later as Reparse does. Removing a night reloads the list.
  The confirm text now points at Removed nights or a card upload, in all five
  languages (the page fragments; `npm run i18n:gen` rebuilt the bundles).

**Tests:** two cases added to `tests/database/test_RemoveNightBackends.cpp`
(`TheListNamesTheRemovedNightsNewestFirst`,
`AnUploadRestoresTheRemovedNightsItHoldsAndNoOther`); the file's 27 tests pass
on SQLite, PostgreSQL 16 (a throwaway local database) and MySQL (the NAS test
database). Full suite 1943 tests, 1661 passed, 282 skipped (engine-gated), 0
failed, under the local zone and `TZ=UTC`. The frontend has no spec files; it
builds with `ng build --configuration production`.

**E2E** (throwaway instance on port 18936, pre-written config, local source,
a copy of 20260330 and 20260406 from the 2026-08-27 card backup):
1. Both nights stored; `GET /api/removed-nights` → `[]`.
2. Remove 2026-03-30 → `{sessions:1, daily:1, ledger:1}`; listed; still gone
   after a minute of 20 s bursts.
3. In a browser, Sessions → Removed nights (1) → Restore: the backfill logged
   `parsed=1, saved=1`, the night came back at 0.68 h as before, the list
   emptied.
4. Removed again, then uploaded a zip holding `DATALOG/20260330` + `STR.edf`:
   the reply said `restored_nights: ["2026-03-30"]`, the backfill saved 1, the
   night came back.
5. Removed 2026-04-06 and uploaded the same zip, which does not hold it:
   `restored_nights: []`, 2026-04-06 stayed removed through the next bursts,
   2026-03-30 stayed stored.

Not exercised end to end: a Sefam or Löwenstein zip (same helper, covered by
the backend test) and an ezShare install (the route and page do not depend on
the source).
