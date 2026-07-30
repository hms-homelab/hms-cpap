# SDD-008: Partial sessions

**Status:** Proposed
**Date:** 2026-07-30
**Repo:** `hms-cpap`
**Version target:** 4.7.0
**Depends on:** SDD-004 (session lifecycle), SDD-005 phase 1 (Mule and Miner)

## Trigger

A transfer from the Mule and Miner that starts and does not finish leaves the
night **stuck showing as live, forever**. The dashboard reports a session in
progress at four in the afternoon, the sessions table shows a green dot on a
night that ended twelve hours ago, and nothing ever clears it. The user's only
recourse today is `POST /api/sessions/{date}/force-complete`, which requires
knowing the endpoint exists.

The state is wrong, it is visible on the first screen of the product, and it
resolves itself only by accident.

## The mechanism, precisely

A session is "live" purely by absence of an end stamp:

```sql
-- QueryService.cpp:102
SUM(CASE WHEN s.session_end IS NULL THEN 1 ELSE 0 END) as has_live
```

`BurstCollectorService` closes a session exactly one way
(`BurstCollectorService.cpp:1153`): when a burst pass finds **all checkpoint
files unchanged and no new files**, it calls `markSessionCompleted()`, which
sets `session_end`. If files did change, it calls `reopenSession()` instead,
clearing `session_end` so a resumed night (mask back on) can complete later.

That logic is correct for the case it was written for. It has no branch for the
case where the *transfer* fails rather than the *therapy* continuing:

- `downloadSessionFiles()` returns false and the pass logs
  `Failed to download session` and moves on. `session_end` is never set.
- Or the download half-succeeds, so sizes differ from the last checkpoint on the
  next pass, `all_unchanged` is false, and the session is treated as still
  growing when in fact it is stalled.

Either way the night stays open. There is no timeout, no retry ceiling, and no
state that says "this was interrupted".

## Mirror the cloud, because the cloud already paid for this

**Decision (Albin, 2026-07-30): mirror what `hms-cpapdash-api` does today.**

That repo hit this exact class of problem in production and its answer is more
disciplined than the timer-based heuristic this SDD first proposed. The design
lives on `device_sync_folders` and was arrived at through four separate
incidents, each of which is worth restating because each one is a trap this
repo can still walk into:

| Column | Incident | Lesson |
|---|---|---|
| `stable` | ticket #25, unit 11, 2026-07-07 | A night's progress showed permanently stuck at a fraction even once every file was downloaded. A folder must be able to report done the moment its **signature** is stable, and flip back to in-progress on real growth. |
| `str_due` | ticket 39 | STR debt is **explicit state**, set on a session-close transition and cleared once a non-empty STR parses. Retried **on recovery, never on a timer**, so a dead card is not hammered. |
| `sidecars_due` | ticket 41 | ezShare listings are **KB-rounded**, so a sidecar (EVE/CSL) that grows inside the same KB bucket is invisible to a size comparison, and its events are never fetched. |
| `resync_size` + `resync_count` | 2026-07-22 | Debt re-arming must be **bounded**. Unbounded, one unfinished night produced **339 re-pulls of one folder in three days**. |

The shape that follows from those:

1. **Transfer state is tracked per date folder, not on the session.** The
   session record keeps meaning what it means. Whether a night's *files* are all
   here is a different question from whether the *therapy* ended, and conflating
   them is what makes a stuck session unrepresentable.
2. **A folder is done when its signature is stable**, that is when file count
   and total size are unchanged from the previous burst and every listed file is
   stored. Not when a clock says so.
3. **Missing STR is recorded as debt**, not inferred from age. Set the marker at
   close, clear it when a non-empty STR parses.
4. **Retry on recovery, bounded.** The next successful stable re-list drives the
   retry. A cap on re-arms at the same signature stops the loop.

### What this changes about this SDD

The original draft proposed marking a night partial once STR had advanced past
it, with an age-based fallback. That is a **timer**, and the cloud's ticket-39
lesson is specifically that timers are the wrong instrument here: they hammer a
card that is already unreachable and they cannot tell "not here yet" from
"never coming".

So the age-based rule is dropped. `str_due` replaces it.

**Decision (Albin, 2026-07-30): no grace windows anywhere in this design.**
There is no age-based trigger, primary or secondary. A night's state is only
ever derived from observed facts (signature stability, files stored, STR
parsed), never from elapsed time. If a card is unreachable the state simply does
not advance, which is the honest answer, and the next successful listing moves
it.

### The KB-rounding sidecar gap, in scope

`sidecars_due` exists upstream because ezShare listings round to KB. **hms-cpap
talks the same ezShare protocol through the mule**, and `BurstCollectorService`
decides "unchanged" by comparing exactly those KB-rounded sizes. The same bug is
therefore latent here: an EVE or CSL file that grows inside one KB bucket after
the early-night copy is invisible to the size comparison, and those event
annotations are never downloaded. That is silent data loss on event counts.

**Decision (Albin, 2026-07-30): fix it here rather than in a separate SDD.** It
is the same failure and the same code path, and the debt mechanism this SDD is
already building is exactly the mechanism it needs.

The upstream fix carries one operational constraint that must come with it:
while `sidecars_due` is set, EVE and CSL are re-downloaded **in full from offset
0**. Not from the last known offset. An offset at or past the card's real EOF
**hangs the ezShare**, and because the listing is KB-rounded it cannot prove
where the real end is, so 0 is the only provably safe fetch. This is the kind of
detail that looks like an inefficiency and is actually the whole point.

## Decisions

| # | Question | Decision |
|---|---|---|
| 1 | State model | **Mirror the cloud**: per-folder transfer state plus explicit STR debt. Session state is derived, not flagged. |
| 2 | MQTT and LLM for an incomplete night | **Suppress both**, publish only the partial fact. A truncated night's AHI and usage hours are wrong rather than uncertain, retained MQTT values persist in Home Assistant history, and a stored LLM summary narrates a night that did not happen that way. Both are hard to retract. |
| 3 | Grace window | **None.** No age-based trigger, primary or secondary. State derives from observed facts only. |
| 4 | Sidecar KB-rounding gap | **Fix it in this SDD**, not a separate one: same failure, same code path, and the debt mechanism here is what it needs. |
| 5 | Compliance and trends | **Exclude from aggregates, show with a badge.** A stalled transfer undercounts usage hours, so including a partial night can report someone below the 4-hour threshold on a night they actually met it. For a product used to demonstrate compliance that is the worst error available. The night stays visible in the sessions list, marked, so the gap is honest rather than hidden. |

## Design

### Storage

A new `cpap_sync_folders` table, the local counterpart of the cloud's
`device_sync_folders`, minus the device fan-out this repo does not have:

```
date_folder     TEXT PRIMARY KEY     -- YYYYMMDD
files_listed    BOOLEAN DEFAULT 0
complete        BOOLEAN DEFAULT 0    -- every listed file is stored
stable          BOOLEAN DEFAULT 0    -- signature unchanged since last burst
last_total_size BIGINT  DEFAULT -1   -- -1 = never observed
last_file_count INTEGER DEFAULT -1
str_due         BOOLEAN DEFAULT 0    -- STR debt, set at close, cleared on parse
sidecars_due    BOOLEAN DEFAULT 0    -- EVE/CSL refetch debt, same close transition
resync_size     BIGINT  DEFAULT -1   -- signature at which debt was last armed
resync_count    INTEGER DEFAULT 0    -- re-arms at that signature; cap 3
updated_at      TIMESTAMP
```

`stable` is separate from `complete` on purpose, per ticket #25: a folder can be
stable (stopped growing) while a file is still being stored, and reporting done
only on the conjunction is what stopped progress showing stuck at a fraction.

Three schema files stay in sync, and every column joins
`MySQLDatabase::migrateSchema()`. Skipping that last part is how an existing
MySQL install silently never gains them, which is the failure 4.6.3 fixed.

### Detection, on the burst sweep

No new scheduler; the collector already runs and already has the listing.

```
signature = (file_count, total_size) from the listing

if signature != (last_file_count, last_total_size):
    stable = false            # real growth, night is alive
    record the new signature
else:
    stable = true             # stopped growing

complete = stable AND every listed file is stored locally

on the close transition (not stable -> stable AND complete):
    arm str_due and sidecars_due     # both bounded by resync_size/resync_count
    -> str_due clears when a non-empty STR parses for this night
    -> sidecars_due clears once EVE/CSL have been refetched from offset 0
```

A night is reported **partial** when its folder is `complete` but `str_due` is
still set: the transfer settled, and the machine's own daily record never
arrived. That is a fact about state, not an inference from a clock.

### Sidecar refetch

While `sidecars_due` is set, the folder's EVE and CSL files are re-downloaded in
full **from offset 0**, then the debt clears. Offset 0 is not a simplification:
a fetch at or past the card's real EOF hangs the ezShare, and a KB-rounded
listing cannot prove where the real end is, so it is the only provably safe
request. The cost is re-pulling a small file; the alternative is a hung bridge.

This is what recovers events appended after the early-night copy, which the size
comparison alone can never see.

### Bounding the retry

`resync_count` caps re-arming at the same signature at **3**, which is enough
for the normal close, refetch, reopen, re-close, confirm cycle. Without it, a
night whose refetches keep coming back different loops forever, which is exactly
what produced 339 re-pulls upstream. A signature change resets the count.

### Clearing

Unchanged from the trigger's own wording: a later burst clears it. `str_due`
clears when a non-empty STR parses for that night, and `reopenSession()` already
handles the case where the files themselves resume growing. Partial is never
terminal, and no manual step is required.

### Clearing

Per the trigger's own wording, a later burst clears it. A partial session that
subsequently receives its missing files goes back through the existing path:
`reopenSession()` already clears `session_end`, and the partial marker clears
with it. Nothing about recovery is new; it is the existing resume path plus one
flag reset.

This matters because it means partial is **not terminal**. A user whose bridge
was simply off for an evening gets the night completed properly the next time
the files arrive, with no manual step.

### Storage

One column on `cpap_sessions`, defaulting to false, added to all three schema
files and to `MySQLDatabase::migrateSchema()`. That last part is not optional:
4.6.3 exists partly because MySQL had no migration path, and an install that
does not gain the column would read it as missing at runtime.

### Surfacing

The sessions table and the dashboard both currently derive "live" from
`session_end`. Whichever answer question 1 takes, both call sites change
together, plus `sessions.component.ts:169`, which reimplements the same test in
TypeScript.

The API needs to distinguish the three states in its session payloads so the
frontend is not re-deriving them a third time.

## Testing

Per CLAUDE.md the test binary excludes `main.cpp`, controllers and `web/`, so
the decision goes in a pure function and the collector calls it.

- **The signature/debt state machine**, table-driven, no DB: a changed
  signature clears `stable`; an unchanged one sets it; `complete` requires both
  stable and all files stored; the close transition arms `str_due` exactly once;
  a non-empty STR parse clears it.
- **The retry bound**: re-arming at the same signature stops at 3, and a
  signature change resets the count. This is the 339-re-pulls regression.
- **Stuck-at-a-fraction**: a folder whose files are all stored reports done even
  though an earlier burst saw it mid-transfer. This is ticket #25.
- **Sidecar refetch**: the close transition arms `sidecars_due`; while set, EVE
  and CSL are requested from offset 0 and never from a stored offset; the debt
  clears once both are refetched. The offset-0 assertion is the one that matters,
  because the failure it prevents is a hung bridge rather than a wrong number.
- **Sub-KB growth**: a sidecar that grows within one KB bucket is still refetched,
  which is the whole reason the debt exists. A size-comparison-only path fails
  this test, which is the point of writing it.
- **Recovery**: a partial session that receives files clears the flag and
  completes, which is the property the whole design rests on.
- **Cross-backend**, in the shape of `test_OximetryBackends.cpp`: the new column
  round-trips on SQLite always and on PostgreSQL/MySQL when reachable.
- **MySQL drift guard**: the new column joins `test_MySQLMigration.cpp`'s
  critical list.
- **Regression**: a genuinely live session, mid-therapy with files still growing,
  must never be marked partial. This is the one that matters most, because a
  false positive here breaks live monitoring, which is a feature people watch in
  real time.

## Non-goals

- Retrying or repairing the transfer beyond the bounded STR debt. This SDD
  describes the state honestly; it does not make the Mule and Miner more
  reliable.
- Changing when a session is considered complete in the healthy case.
- Backfilling historical stuck sessions. They clear on the next burst that sees
  them, or via the existing force-complete endpoint.
- Any change to parsing or to how metrics are computed from the files that did
  arrive.

## Open items

- Whether a partial night should be visible in reports at all, or only in the
  sessions table.
- `force-complete` currently means "close this now". If `partial` lands, the two
  overlap and the endpoint's meaning should probably be restated rather than
  left ambiguous.

## References

- `src/services/BurstCollectorService.cpp:1153-1240` (completion and resume)
- `src/web/QueryService.cpp:102` (`has_live`)
- `frontend/src/app/pages/sessions/sessions.component.ts:169` (the TS copy)
- `include/database/IDatabase.h:57,60,99` (`markSessionCompleted`,
  `reopenSession`, `getLastSTRDate`)
- `docs/RESMED_WRITE_TIMING.md` (STR timing)
