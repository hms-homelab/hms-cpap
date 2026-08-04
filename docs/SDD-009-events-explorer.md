# SDD-009: Events explorer

**Status:** Approved (Albin, 2026-08-04)
**Date:** 2026-08-04
**Repo:** `hms-cpap`
**Version target:** 4.9.1 (assigned by Albin, 2026-08-04)
**Depends on:** nothing new; reads `cpap_events` as populated since v4.0

## Trigger

Support ticket 67. The owner's "beloved Resp Events Table" keeps disappearing,
so he exported six years of OSCAR CSV and had an LLM build him a standalone
searchable events table. When a user rebuilds a feature outside the product,
the product is missing the feature.

## The mechanism, precisely

The dashboard's events section is not a table of events at all. It is a
breakdown card fed exclusively by `/api/daily-summary`, which reads
`cpap_daily_summary` for the latest night. Any gap in that table (STR missing
before the 4.8.1 fallback, the PostgreSQL read blackout of issue #18) hides
the section entirely, together with three sibling sections. The per-event
rows the user actually wants exist in `cpap_events` (typed, timestamped, with
durations and details) but are only reachable one night at a time via
`/api/sessions/{date}/events`.

## Design

One new read endpoint, one new page. No new tables, no new writers.

### Backend

`GET /api/events` on `CpapController`, backed by
`QueryService::getEvents(start, end, types, min_duration, limit, offset)`:

```sql
SELECT e.event_type, e.event_timestamp, e.duration_seconds, e.details,
       sleepDay(s.session_start) AS sleep_day
FROM cpap_events e
JOIN cpap_sessions s ON s.id = e.session_id
WHERE s.device_id = ?
  [AND sleepDay(s.session_start) >= ?]     -- start, optional
  [AND sleepDay(s.session_start) <= ?]     -- end, optional
  [AND e.event_type IN (?, ...)]           -- types, optional
  [AND e.duration_seconds >= ?]            -- min_duration, optional
ORDER BY e.event_timestamp DESC
LIMIT ? OFFSET ?
```

Built with the `sql::` dialect helpers like every other query in
`QueryService`; parameters appended in clause order so `?` dialects line up.
Query parameters: `start`, `end` (sleep days, `YYYY-MM-DD`), `types`
(comma-separated event type strings as stored: `Obstructive`, `Central`,
`Hypopnea`, `RERA`, `Clear Airway`, `Apnea`, `CSR`, `Desaturation`),
`min_duration` (seconds), `limit` (default 100, max 500), `offset`.

Pagination follows the sessions-list convention: the client walks
`offset` by rows loaded and infers "more" from a full page.

### Frontend

`/events` page (standalone component, lazy route, nav link between Sessions
and Reports):

- Filter bar: from/to date inputs, one checkbox per event type (all on by
  default), minimum duration in seconds. Changing a filter reloads from
  offset 0.
- Table: sleep day, clock time, type, duration, details. Newest first.
- Row click navigates to `/sessions/{sleep_day}`.
- "Load more" identical to the sessions list.

The existing dashboard breakdown card stays exactly as it is.

## Explicitly out of scope (open decisions left with Albin)

- CSV export of the filtered result.
- Cross-night aggregates (counts per type over the filtered range) beyond
  what the table itself shows.
- Server-side sort options; newest-first is the only order in v1.

## Test plan

`QueryService` and controllers are outside the unit-test binary, so
verification is a live smoke against a seeded SQLite instance: filters
individually and combined, pagination walking, empty result, and a
type string containing a space (`Clear Airway`) surviving the query.
Frontend: production build plus the same smoke instance.
