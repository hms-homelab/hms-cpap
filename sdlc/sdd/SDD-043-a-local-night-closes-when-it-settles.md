# SDD-043: a local night closes when it settles, whenever it was stored

**Status:** Accepted 2026-09-18 ("yes on 041"), to ship with the SDD-041 release.
**Date:** 2026-09-18
**Repo:** `hms-cpap`. The burst collector's local close, one query helper.
**Related:** SDD-037 (a local folder is a finished night; D1, the 5-day window),
SDD-038 (history catch-up), SDD-040 (one burst cycle), ticket 129

## Trigger

Ticket 129, 2026-09-18. After a container restart, nights 9/13, 9/14 and 9/15
were imported again from a local folder, and all three stayed `live` with no
`session_end`. Their rows were never written again after the import, although
9/16 and 9/17 closed normally.

## 1. Cause, from the code

A local night is closed by one of two paths (SDD-037):

1. **When it is stored**, by `closeIfSettledLocalNight()`, but only if it is
   already older than `kLocalSettledAfter` (5 days, D1). Younger nights are left
   open on purpose: a local folder can be a mounted card or a lagging sync.
2. **Later**, by the "checkpoint files unchanged" check. That runs only for
   sessions discovery offers again: the newest nights, the last 48 hours, the
   retained session, and history catch-up.

A night first stored when it is **between about 2 and 5 days old** gets neither.
It is too young for (1) when it is stored, and too old for discovery to offer it
again for (2). Nothing looks at it again, so it stays `live` for good. A restart
that re-imports recent history makes exactly that set of nights, and so does a
first import of a folder whose newest nights are a few days old.

A smaller fault sits in path (2): when it closes a local night it stamps the
current time (`markSessionCompleted`), not the night's own end, which is what
SDD-037 D2 settled ("from the data").

## 2. Design

- **Each cycle, for a local source**, every session still open that started more
  than `kLocalSettledAfter` ago is closed at `session_start + duration_seconds`,
  the span already stored for it. This needs no re-parse and no rediscovery, so
  it covers every night whatever path stored it. It is a no-op once nothing is
  open. A session with no stored duration stays open, as SDD-037 already rules:
  nothing the data knows, no invented end.
- **Path (2) for a local source closes on the stored span too**, not the clock.
- **One helper**, `openSessionsStartedBefore(db, device, cutoff)`, built on
  `executeQuery` and the `sql::` dialect helpers, so it is one implementation for
  SQLite, MySQL and PostgreSQL rather than three.

**The end is the stored span, not the parser's last record.** For a session
with gaps inside it, `start + duration` lands before the parser's end, because
the duration counts recorded time only. Measured on the SDD-037 test card: 8
minutes early on a 4-hour session. Night hours and indices come from the
duration (SDD-026), not from `session_end`, so no report changes. Matching the
parser exactly would mean re-parsing every settled night, which is what this
design avoids.

ezShare and Fysetc are unchanged: their nights close when the transfer
settles, which is a fact about the transfer, not the calendar.

## 3. Tests

- The helper, on all three engines: open sessions before the cutoff are
  returned with their durations; closed ones and later ones are not.
- The sweep: an open local night older than 5 days is closed at start plus
  duration; a younger one and a non-local source are left alone; a night with
  no duration stays open.
