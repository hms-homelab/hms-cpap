# SDD-001: Advanced Signal Charts — Event Spans, Desaturation, Breath-by-Breath

**Status:** Draft
**Date:** 2026-06-14
**Companion:** `hms-cpapdash-parser/sdd/001-advanced-signal-analysis.md` (shared parser — data layer)

## Problem

hms-cpap collects bursts (`BurstCollectorService`), parses with the shared `hms-cpapdash-parser`, stores to its `cpap_*` tables, serves the Angular SPA, and publishes to MQTT/Home Assistant (`DataPublisherService`). Compared with established CPAP analysis tools, three signal-analysis affordances are missing:

1. **Event overlay on flow is vertical-lines-only.** `frontend` `signal-chart` + `session-detail` render events via `chart-helpers.ts` `eventAnnotations()` as dashed vertical lines; `duration_seconds` is fetched but unused, so an event's *span* over the flow trace isn't shown.
2. **No desaturation detection on the machine-SpO2 path.** SpO2 from SAD.edf is charted via `cpap_vitals`, but there's no desaturation/ODI on that path (ODI exists only for O2Ring oximetry today).
3. **No breath-by-breath.** Finest stored resolution is per-minute `cpap_breathing_summary` / `cpap_calculated_metrics`; the parser detects breaths but they are discarded.

The shared parser SDD fixes the data layer (emits `Desaturation` events into `session.events`, populates `odi`/`spo2_drops`, and adds a persisted `breaths` list). This SDD persists, serves, and renders that in hms-cpap.

## Design

### Database (`scripts/schema.sql`)

```sql
CREATE TABLE IF NOT EXISTS cpap_breaths (
    id                 SERIAL PRIMARY KEY,
    session_id         INTEGER REFERENCES cpap_sessions(id) ON DELETE CASCADE,
    onset              TIMESTAMPTZ NOT NULL,
    tidal_volume       REAL,   -- mL
    inspiratory_time   REAL,   -- s
    expiratory_time    REAL,   -- s
    flow_limitation    REAL,   -- 0..1
    UNIQUE(session_id, onset)
);
CREATE INDEX IF NOT EXISTS idx_cpap_breaths_session_onset ON cpap_breaths(session_id, onset);
ALTER TABLE cpap_session_metrics ADD COLUMN IF NOT EXISTS odi REAL;
ALTER TABLE cpap_session_metrics ADD COLUMN IF NOT EXISTS spo2_drops INTEGER;
```

Desaturations are stored in the existing `cpap_events` table (`event_type='Desaturation'`, `duration_seconds`, `details` carrying `nadir`/`depth` as JSON) — no new events table. SQLite parity: the project also runs a SQLite path (`tests/database/test_SQLiteDatabase.cpp`); add the same DDL there.

### Ingestion

The shared parser now returns `session.breaths`, `Desaturation` events in `session.events`, and `session.metrics.odi/spo2_drops`. Extend the session-persist path (the code that writes `cpap_breathing_summary`) to batch-insert `cpap_breaths` and persist the two metric columns. Desat events flow through the existing `cpap_events` insert unchanged. Re-parse on a burst is idempotent via `UNIQUE(session_id, onset)` / `UNIQUE(session_id, event_timestamp)`.

### Endpoints (`src/web/QueryService.cpp`)

- `GET /api/sessions/{date}/events` — add `duration_seconds` + `details` to the JSON; `Desaturation` rows are included automatically.
- `GET /api/sessions/{date}/breaths` (new) — column-oriented `{ onset, tidal_volume, inspiratory_time, expiratory_time, flow_limitation }`, scoped like the other session endpoints.
- `GET /api/sessions/{date}/detail` (or the metrics endpoint) — add `odi`, `spo2_drops`.

### MQTT / Home Assistant (`DataPublisherService`)

Publish `odi` and `spo2_drops` as additional session attributes/sensors so HA dashboards surface them alongside AHI. Breath-level data is **not** published to MQTT (too granular for HA) — it stays REST-only for the SPA. `LiveSleepStageRunner` (30 s epochs) is unaffected; desat/breath are orthogonal.

### Angular (`frontend/src/app/components/`)

`session` model: type `duration_seconds` numeric and use it; add `'Desaturation'` to the event-type union; new `BreathData` interface; metrics gain `odi`/`spo2_drops`.

1. **F1 — event spans.** `chart-helpers.ts` `eventAnnotations()`: emit a chartjs-plugin-annotation **`box`** annotation `{ xMin: onsetIdx, xMax: idx(onset+duration) }` with a translucent type color, instead of (or alongside) the line. Zero-duration types (RERA) keep the dashed line. Extend `session-detail`'s window re-indexing to shift `xMin`/`xMax` together. Add an event-type **legend** strip (extend the badges component to render a color key, not just counts).
2. **F2 — desaturation.** No new fetch: desat events arrive via `/events` and render as orange spans on the **SpO2** signal (`showEvents: true` on the SpO2 signal def, filtered to `Desaturation`). Add an **ODI** value to the SpO2 metric card + the events breakdown.
3. **F3 — breaths.** `session-detail.component.ts` adds a `getSessionBreaths(date)` call. New chart mode on the flow chart: when zoomed to a window ≤ ~60 s, overlay breath boundaries (faint line annotations at each onset) + alternating-breath shading, and enable the already-registered `chartjs-plugin-zoom` for wheel/pinch zoom. Add a per-breath **Flow Limitation** mini-chart to the overview strip.

## Tests

- C++ (GTest, `tests/`): `cpap_breaths` created (Postgres + SQLite); breaths persisted from a parsed fixture; `/breaths` returns arrays; `/events` includes `Desaturation` + `details`; `odi`/`spo2_drops` persisted; idempotent re-parse. Extend the existing `test_DataPublisher*` / `test_DatabaseService_pg` suites.
- Frontend: Jasmine spec for `chart-helpers` (box vs line for non-zero vs zero duration); breath overlay gated on zoom level.
- E2E: `ng serve` against the local backend, load a real session, verify flow spans / SpO2 desat shading + ODI / breath overlay on zoom (Playwright).

## Open questions

- HA: expose ODI as a numeric sensor only, or also a binary "significant desaturation" alert? Default: numeric sensor; alerting is a later `InsightsEngine` concern.
- Default desat threshold (3% AASM vs 4% CMS) is owned by the shared parser (`DesatParams`); hms-cpap uses the library default.

## Migration

1. `schema.sql` (+ SQLite) for `cpap_breaths`, `odi`, `spo2_drops`; ingestion persistence.
2. `/breaths` + `/events` `details` + `/detail` metrics; MQTT `odi`/`spo2_drops`.
3. Angular: event spans + legend (F1), SpO2 desat + ODI (F2), breath overlay + zoom (F3).
4. GTest green (`ctest`); `ng serve` + Playwright on real data.
