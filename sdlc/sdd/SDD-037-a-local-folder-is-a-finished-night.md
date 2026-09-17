# SDD-037: a local folder is a finished night

**Status:** Accepted 2026-09-17, in build. D1 close on save older than 5 days;
D2 `session_end` from the data; D3 fix the wording, the local folder is already
the copy; D4 the picker takes transport and format.
**Date:** 2026-09-17
**Repo:** `hms-cpap`. The burst's local branch, `markSessionCompleted` on three
backends, `PUT /api/config`, the Settings page.
**Ticket:** CpapDash support 129 (Michael, 5.2.17, Local Directory source)
**Related:** SDD-008 (the folder ledger and night_state), SDD-010 (the local dir
is a card ROOT), SDD-012 (Archive Directory in Settings), SDD-022 (transport and
format are two questions), SDD-026 (our numbers win)

## Trigger

Ticket 129, a first run of the Local Directory source against a verified card
copy: 63 sessions parsed with correct AHI, duration and events, but only the two
newest nights ever closed, both stamped with the wall clock of the ingest, and
the other 18 nights sit at `night_state: live`, `session_end: null` forever. In
the same run: the Archive Directory stayed empty though Settings says nights are
written there, and the Data Source picker would not move the source off
`ezshare` no matter how many times it was saved.

## 1. Why a local night never closes

A local session is stored open and closed on a LATER cycle, never the one that
parsed it (`BurstCollectorService.cpp:2142`: "Session is always IN_PROGRESS
during parsing. Completion ... fires from the checkpoint path when file sizes
stop changing between cycles"). The only close in the local branch is
`:1713`, reachable only through `exists_in_db && all_unchanged`.

That second look never comes for an old night. The cycle asks discovery for
sessions from an anchor: `last_session_start` and `retain_from`, the second
latest stored start (`:1336`, `:1354`). `discoverLocalSessions` keeps folders
`>= last_date` plus the previous day (`SessionDiscoveryService.cpp:571`), and
per session keeps only `is_new || is_today || is_recent (48 h) || is_retained`
(`:643`). After the first run stores everything, the 18 older nights match none
of those, are never returned again, and cannot be closed.

**Every other bulk path already closes what it saves**: BackfillService
(`:293`, "Backfilled sessions are complete"), the CLI reparse
(`main.cpp:385`), the card upload (`CardUpload.cpp:238`), and the burst's own
Löwenstein branch (`:1437`). The local ResMed branch is the one that inherited
the ezShare "wait until the files stop growing" contract without inheriting a
way to look at an old night twice.

## 2. Why the timestamp is meaningless

`markSessionCompleted` writes the clock, on all three backends:
`SET session_end = datetime('now')` (`SQLiteDatabase.cpp:1469`), `NOW()`
(`MySQLDatabase.cpp:1935`), `CURRENT_TIMESTAMP` (`DatabaseService.cpp:1688`),
each guarded by `session_end IS NULL`. The insert never writes it
(`DatabaseService.cpp:802`: "markSessionCompleted() owns it"), although the row
already carries `duration_seconds` from the parse.

For a live ezShare night that is right: the moment we saw the files stop is the
best estimate of mask-off. For an import of a finished night it is the moment
the import ran, which is why Michael's two closed nights share one timestamp.
Nothing reads `session_end` as a clinical value today (the dashboard and the
session list derive hours from `duration_seconds`, SDD-026), so this is about
the field not lying, not about a number on screen.

## 3. What "live" means without a ledger

A local source writes no `cpap_sync_folders` row, so `night_state` falls back to
"any session of this night has `session_end IS NULL`"
(`QueryService.cpp:389` for the list, `:475` for the detail). There is no
recency or mtime in it. So closing the session IS what clears the LIVE badge;
nothing else has to change.

## 4. The archive, and what Settings claims

`archiveSessionFiles` is called only in the ezShare block
(`BurstCollectorService.cpp:2067`). The local branch parses the user's folder in
place and copies nothing, which is right: the folder already IS a card layout
(SDD-010), OSCAR can import it directly, and duplicating someone's card into a
second folder is not a service we should perform behind their back.

The Settings hint does not know that. `settings.source.archiveHint` ("Where
collected nights are written, as a card layout ... Point OSCAR's card import
here") is rendered for every transport
(`settings.component.ts:102`). The required-tag and the missing-warning are
already transport-aware (`PreflightService::sourceNeedsArchive`, ezShare and
Fysetc only); only the sentence is not.

## 5. The source picker has never worked

The Settings page edits `config.transport` and `config.format`
(`settings.component.ts:35`, `:47`) and PUTs the whole config.
`CpapController::updateConfig` reads neither: its only source field is
`if (j.isMember("source")) config_->source = ...` (`:365`), and the `source` the
page sends back is the one it was given. So `source` and `transport` never move,
whatever is picked, on any install since SDD-022 split the two fields. The
wizard's `setupApply` (`:696`) has the same shape, which is why a fresh install
is fine and only a later change is not.

`AppConfig::applyLegacySource()` derives transport+format FROM source, and
`AppConfig::collectorSource(transport, format)` derives the collector's source
FROM the pair (`AppConfig.h:40`, `:90`). The second is what the save needs.

## 6. Design

### 6.1 A local night closes when it is stored (D1)

In the local branch, after a session is parsed and saved, close it the way
BackfillService does, EXCEPT while it may still be growing. "May still be
growing" is **5 days** (Albin, 2026-09-17): a session that started less than 5
days ago keeps today's behaviour, closed by the checkpoint path when its files
stop changing; anything older is closed on save. Wider than discovery's 48 h
`is_recent` on purpose, so a folder that is a live card, or one an external sync
writes into on a lag, is never called Done while it is still filling.

A later change to an old night's files still re-parses (the `files changed`
path), and `markSessionCompleted`'s `IS NULL` guard means the row keeps its
first close. `reopenSession` stays ezShare-only.

### 6.2 `session_end` comes from the data when the data knows it (D2)

`markSessionCompleted(device_id, session_start, end)` gains an optional end.
Where a session was just parsed from files that are not growing (the local close
above, BackfillService, the CLI reparse, the card upload), pass
`session_start + duration_seconds`. The live ezShare close passes nothing and
keeps the clock, which is the right answer there.

### 6.3 The archive hint tells the truth per transport (D3)

`settings.source.archiveHint` stays for ezShare and Fysetc. For a local
transport the hint becomes: the nights are read where they are, this folder is
used for SleepHQ export and reports, and OSCAR should be pointed at the local
folder itself. Five languages.

### 6.4 `PUT /api/config` accepts the pair the page edits (D4)

`updateConfig` reads `transport` and `format` when present, sets
`config_->source = AppConfig::collectorSource(transport, format)`, and re-exports
`CPAP_SOURCE` beside the `CPAP_ARCHIVE_DIR` export that is already there, so the
next burst picks it up through `markConfigDirty()` without a restart. A body
that carries only the legacy `source` still works (the wizard, and any script).
`setupApply` gets the same treatment.

## 7. Decisions (Albin's)

- **D1, when a local night closes. ACCEPTED 2026-09-17, with a 5-day window**
  (Albin: "yes to d1 but 5 days window"): on save for any session older than 5
  days, the checkpoint behaviour for anything newer (6.1).
- **D2, what `session_end` should say. ACCEPTED 2026-09-17** ("yes from the
  data"): `session_start + duration_seconds` wherever a finished night is
  imported, the clock only for the live ezShare close (6.2).
- **D3, the archive in local mode. ACCEPTED 2026-09-17** ("fix the settings
  wording dont need to copy into the archive local is already a copy"): the
  words change (6.3), the behaviour does not.
- **D4, the picker.** Proposed as 6.4. No alternative worth naming: it is a bug.

## 8. Tests

- `markSessionCompleted` with an explicit end, each backend: writes that end,
  still refuses a row that already has one, still matches within the 5 s window.
- Local burst, first cycle: a folder of old nights is stored AND closed, one
  `session_end` per night derived from its own data, no two alike; a session
  inside 48 h stays open until its files stop changing.
- Local burst, second cycle: nothing re-closes, nothing re-parses, and an old
  night whose files changed re-parses without losing its `session_end`.
- `night_state` for a local night is "complete" once closed (QueryService, no
  ledger row).
- `PUT /api/config` with `{transport, format}`: config.json gets the derived
  `source`, `CPAP_SOURCE` is re-exported, a body with only `source` still works.
- The Settings page shows the local hint for a local transport and the archive
  hint otherwise.

## 9. Release and verification

Albin's number. Before the tag, the full e2e, since this touches the session
lifecycle and all three backends write `session_end`. Every environment is a
NATIVE install; nothing here is verified in a VM:

| Environment | Where | Engines |
|---|---|---|
| macOS | this Mac (192.168.2.55) | SQLite, PostgreSQL 16 native here, MySQL on the NAS |
| Linux | the hub, 192.168.2.15 | SQLite, and the same two over the LAN |
| Windows | CpapDash-Win, 192.168.2.72 | SQLite, plus MySQL and PostgreSQL over the LAN |

MySQL is the NAS at 192.168.2.2:3306 (`NAS_MYSQL_TEST_*` in `~/cool_shit/.secrets`),
PostgreSQL is the native `postgresql@16` on this Mac, both reachable as of
2026-09-17. On .72, RDP answers and port 22 did not, so SSH may need starting
before a remote run.

Each environment runs a local-source import of a multi-night card copy, and the
three checks this SDD is about: the older nights close, each with its own
`session_end`, and the Settings picker moves the source and survives a restart.
The reply on ticket 129 is Albin's.

## 10. As built (2026-09-17, 5.2.18)

Where the build differs from section 6, and what the runs showed.

- **`markSessionCompletedAt()` is a NEW virtual, not a defaulted argument** on
  `markSessionCompleted()`. The suite's gmock doubles override the two-argument
  form; adding a parameter would have broken every one of them. The base
  implementation falls back to the old call, so a backend that has not
  overridden it behaves exactly as before.
- **One helper header, `include/utils/SessionEnd.h`**: `dataEndOf()` (the
  parser's end, else start + duration, else nothing), `closeWithDataEnd()` and
  `localNightHasSettled()`. It includes `parsers/CpapdashBridge.h`, NOT
  `models/CPAPModels.h`, whose definitions collide with the shared parser's
  aliases.
- **The end the data knows is the parser's `session_end` when it has one**, and
  that is usually LATER than `session_start + duration_seconds`, because
  duration counts therapy time and the span includes the gaps within a night.
  The precedence is deliberate: the span is the night, the duration is the
  therapy.
- **The local close also marks the night dirty for SleepHQ**, as the checkpoint
  close does, so a settled import still queues its export.

**Tests:** `tests/database/test_SessionEndBackends.cpp`, 13 cases (4 per engine
plus the helper's own), green on SQLite, MySQL (the NAS) and PostgreSQL (native
on the Mac). Full suite 1959 tests, 1851 passed, 108 skipped, 0 failed, under
the local zone AND `TZ=UTC`, with all three engines live. CI run 35272399183 on
the branch: build-and-test, macos-build, windows-build, windows-desktop-test,
coverage (Linux against a real PostgreSQL) and linux-armhf all green.

**E2E, four environments, same card copy** (five DATALOG folders from the
2026-08-27 backup, four of them holding sessions). In every one: the instance
started as ezShare, `PUT /api/config {transport: local, format: resmed}` moved
the source and persisted it, the import stored every night and closed all five
sessions on the first pass, and the five `session_end` values were identical
across environments and all distinct, e.g. `2026-03-28 21:55:29 ->
2026-03-29 02:06:56`.

| Environment | Result |
|---|---|
| macOS native (this Mac) | as above, plus the Settings hint and picker checked in a browser |
| Linux native (hub, 192.168.2.15) | identical |
| Docker (hub, card read-only at `/data/cpap_source`, SQLite in `/config`) | identical; this is the shape ticket 129 runs |
| Windows native (CpapDash-Win, 192.168.2.72, the CI binary) | identical |

Two things the runs taught, neither a defect in this work:

- **A night reads `partial` until its STR day record lands.** Windows showed
  `partial` for two nights where the Mac showed `complete`, purely because it
  was sampled earlier: `partial` is the ledger's `str_due` flag (SDD-008), and
  the next burst logged "STR arrived for 20260330 (therapy day 20260330), night
  is no longer partial" and cleared it. Sample after the STR pass, or the state
  is read mid-flight.
- **In Docker the card must be somewhere the container user can read.** A copy
  under `/home/<user>` gave "Skipping unreadable folder … (Permission denied)"
  for every folder and an empty import; `/tmp` with `chmod a+rX` works. The
  message is clear and per folder, which is what made it a one-minute
  diagnosis.
