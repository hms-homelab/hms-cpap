# SDD-042: a Device ID change waits for a restart

**Status:** Accepted 2026-09-18. Albin: "yes to the restart now" (option B).
**Date:** 2026-09-18
**Repo:** `hms-cpap`. The collector's config reload and the Settings page.
**Related:** SDD-012 (settings and restart), SDD-039 (an empty list is not an
answer), ticket 129

## Trigger

Ticket 129. On 5.2.18 and again on 5.2.19, `/api/sessions` returned `[]` while
the database held 63 sessions, and no error appeared anywhere.

## 1. Cause, reproduced

A Settings save of `device_id` reaches two holders of it at different times:

- `BurstCollectorService::reloadConfig()` applies the new id **at once**, so every
  night collected afterwards is stored under it.
- `QueryService` was built in `main.cpp` with the id read at startup and keeps
  it, so every page asks for nights under the **old** id.

The query succeeds and finds nothing, so SDD-039's error surfacing has nothing
to report. MQTT discovery, backfill, ML and the agent also hold the startup id.

Reproduced on 5.3.0: start with the default id, `PUT /api/config
{"device_id":"23203544870", "local_dir": ...}`, let it ingest. The database holds
the nights under `23203544870` and `/api/sessions` answers `[]`. A restart shows
them. Michael typed his real serial over the built-in default in a fresh
install, which is exactly that sequence.

## 2. Decision

`device_id` becomes a **restart-required** setting, like the web port:

- the collector no longer applies a new `device_id` while running. It logs that
  the change takes effect after a restart, and keeps collecting under the id
  every other component also holds;
- Settings lists Device ID among the restart-required keys, so saving it
  raises the existing "Restart now" banner (SDD-012).

After the restart every component reads the same id from config at once, so
there is no moment when nights are stored under one id and read under another.
`device_name` stays hot: it is a label, not a key.

Not in this SDD: whether a user should set the id at all. The card carries the
machine's serial, and the built-in default is one specific machine's serial.
Deriving the id from the card is a separate SDD, because it has to leave every
existing install's id alone.

## 3. Tests

- Unit: a reload with a changed `device_id` leaves the collector's id
  unchanged; a changed `device_name` still applies.
- E2E: the section 1 reproduction on the fixed build. After the save, nights
  land under the id the pages read, and after "Restart now" under the new one,
  with `/api/sessions` answering both times.
