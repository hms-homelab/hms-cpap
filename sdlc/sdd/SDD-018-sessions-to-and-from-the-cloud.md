# SDD-018: Sessions to and from the cloud

**Status:** Proposed
**Date:** 2026-08-23
**Repo:** `hms-cpap` (plus a small addition to `hms-cpapdash-api`)
**Version:** target TBD (Albin's call)
**Depends on:** hms-cpapdash-api SDD-065 (personal access token)
**Unblocks:** SDD-017's mode switch (this spec is what creates `CPAP_SOURCE=cloud`)

## Trigger

Two separate needs that share one credential and one set of decisions, which is
why they are specced together.

**Push — what Ken actually asked for.** "Stay connected in local mode all the time
and push to the API through hms-cpap." He does not want to switch his device to
cloud mode. He wants to keep it local, where hms-cpap collects from it, and still
get the cloud features. Today that is impossible: `CpapDashSyncService` syncs
equipment only, and there is no path that sends a night up.

**Pull — what SDD-017 needs.** When a device *is* in cloud mode it stops serving
files, its nights go to the API, and hms-cpap goes blind. hms-cpap has no
`CPAP_SOURCE=cloud`, so it cannot follow. Until it can, the local→cloud mode
switch is a button that silently breaks the dashboard the user is looking at.

If push ships, Ken may never need the mode switch at all.

## Push

**Transport: `POST /v1/upload` with a zip** (Albin, 2026-08-23). The endpoint
exists, is user-authenticated, is the same path the website uses, and already
knows how to unpack a card layout — it looks for `STR.edf` and `DATALOG/<date>/`,
which is the shape hms-cpap already keeps. No new ingest endpoint.

**Content: raw EDF, and the cloud parses it.** Both sides run the *same* shared
parser (`hms-cpapdash-parser`), so the numbers agree by construction rather than by
luck, and the cloud can reparse a night later when the parser improves — which it
does, repeatedly, and did twice this month. Sending hms-cpap's computed metrics
instead would create a second source of truth that could never be reparsed and
would silently drift from cloud-parsed nights at every parser fix.

**Backfill: everything, once, with a visible progress view.** The user's local
archive becoming their cloud history is the point of switching this on, and
hms-cpap frequently holds nights the cloud never saw — any period the M&M was
offline, which is the common case for the people who run hms-cpap at all. This is
a long-running job and must look like one; a silent multi-hundred-megabyte upload
is indistinguishable from a hang.

### Where the nights land, and the split we are accepting

`/v1/upload` gets-or-creates a per-user **virtual device** (`UPLOAD-<n>`, via
`UserService::getOrCreateVirtualDevice`). So hms-cpap's nights land there, not on
the real `FE-C3-…` row the M&M would have used.

For a user whose M&M has also pushed, the same night then exists under two device
rows. **This is accepted deliberately** (Albin, 2026-08-23), and it is a smaller
problem than it looks, because every read path already collapses across devices:

```sql
WITH ls AS (
  SELECT DISTINCT ON (date) * FROM sessions
  WHERE device_id IN <every device on the account> AND date ~ '^[0-9]{8}$'
  ORDER BY date, COALESCE(parsed_at, uploaded_at) DESC NULLS LAST, id DESC)
```

That is SDD-020, account-level, one session per date, **latest-wins**;
`dedupedDailySummary` does the same for the daily rows. A user never sees a night
twice, and a re-pushed night wins because its parse is newer — which is the
behaviour we want, since it is the same card data parsed by a newer parser.

What the split genuinely costs, stated plainly rather than hidden:

- the same raw files stored twice on the VPS, under two serials;
- the same night parsed twice;
- a device-scoped view (anything that filters to one serial rather than the
  account) sees a partial history either side of a mode change.

The alternative was letting `/v1/upload` accept a target serial so nights land on
the real device row. Not chosen; recorded here because if device-scoped views ever
matter, this is the decision to revisit.

## Pull

**A new incremental raw-file endpoint in hms-cpapdash-api.** `backup.zip` already
exports a device's stored files, but it is whole-device and all-or-nothing —
re-downloading an entire history to discover one new night is not a sync. What is
needed is list-by-date plus fetch, so hms-cpap can take only what it lacks.

**Raw files, not parsed data** (Albin, 2026-08-23). hms-cpap is raw-files-first
all the way down: its parsers, `MetricsCalculator`, the ML services, sleep staging
and the PDF report stack all expect EDF on disk. Pulling parsed summaries would
turn hms-cpap into a viewer of someone else's parse and quietly disable the
features people run it for. The query API (`/api/sessions`, `/api/daily-summary`,
signals, vitals, events) exists and is incremental, but it feeds a dashboard, not
a pipeline.

**This is what creates `CPAP_SOURCE=cloud`** — a fifth source alongside `ezshare`,
`local`, `lowenstein` and `fysetc`, pulling raw files into the same archive
directory the other sources fill, after which everything downstream is unchanged.
That is the whole reason to pull raw: the rest of hms-cpap does not need to know
where the files came from.

## Idempotency

**Server-side, keyed on (device, date, file)** (Albin, 2026-08-23). Re-delivering
a night overwrites rather than duplicates, whoever sent it.

This is not only about duplicates. A push path needs retries — a backfill of
months of nights over a home connection *will* be interrupted — and retries are
only safe if delivery is idempotent. Doing it at the point where identity is
actually knowable, rather than asking the client to check first, also closes the
race where a night still in progress is partially present on both sides.

Note the scope this does **not** cover, following from the section above: it
deduplicates within a device row, not across the two device rows a mode change
creates. The read-path collapse handles that case.

## Credential

Everything here is behind `UserAuthFilter` and therefore behind
hms-cpapdash-api **SDD-065**. With today's 24-hour JWT a backfill would die
mid-run and a recurring sync would stop after a day, so this spec cannot ship
before that one. Both directions must be added to SDD-065's PAT allowlist.

## Open questions

1. **Does hms-cpap's archive layout match what `/v1/upload` expects?** It should
   be card-shaped (`STR.edf` + `DATALOG/<date>/`), but that needs confirming
   against `CPAP_ARCHIVE_DIR` before building rather than assuming.
2. **Push cadence.** After the initial backfill: on each burst sweep, on session
   close, or on a timer? `CpapDashSyncService`'s existing `markDirty()`/`sweep()`
   shape is the precedent and probably the answer.
3. **What happens when both are on.** A device in local mode with push enabled is
   coherent. A device in cloud mode with pull enabled is coherent. Is push+pull
   simultaneously ever legitimate, or should enabling one disable the other?
4. **Failure visibility.** Equipment sync currently fails silently, which is how
   the 24-hour token bug went unnoticed for months. Neither of these paths should
   repeat that, but where the user sees it is not decided.

## Out of scope

- SDD-017's device controls and mode switch (this spec unblocks the switch, it
  does not contain it).
- Letting `/v1/upload` target a specific serial (see the split, above).
- Any change to how the M&M itself pushes.
