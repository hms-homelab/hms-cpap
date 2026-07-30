# SDD-007: Cleaning schedules

**Status:** Proposed
**Date:** 2026-07-30
**Repo:** `hms-cpap` (public, MIT), plus one endpoint in `hms-cpapdash-api`
**Version target:** 4.7.0
**Depends on:** SDD-004 (equipment profiles + supplies)
**Mirrors:** `hms-cpapdash-api` SDD-043, shipped there as v2026.11.12

## Trigger

The local stack tracks when an accessory must be **replaced** and says nothing
about when it must be **washed**. Replacement is the quarterly half of CPAP care;
cleaning is the daily and weekly half, and the part people actually skip.

The cloud answered this in SDD-043. A self-hosted install currently cannot,
which makes the local product worse than the hosted one at the exact thing the
local product exists for.

## Relationship to SDD-043

SDD-043 is the authority on **what a cleaning schedule means**. This SDD is the
authority on **how it behaves in a single-household, three-backend, no-phone
install**. Where the two could drift, SDD-043 wins on semantics and this one
wins on plumbing.

| | SDD-043 (cloud) | SDD-007 (this) |
|---|---|---|
| Tenancy | `user_id` on every row | none; single household, per SDD-004 |
| Storage | PostgreSQL | SQLite, PostgreSQL and MySQL |
| Reminder delivery | Flutter local notification planner | MQTT sensors to Home Assistant |
| Source of truth | itself | **local**, cloud is an opt-in mirror |
| Replacement vs cleaning | deliberately separate concepts | unchanged, same reason |

**Deliberately identical:** the due computation. See §3.

## Principle

**Cleaning is not supplies.** A mask is replaced every 90 days and wiped every
day. SDD-043 kept those as separate concepts and so does this. Any design step
that starts folding `interval_days` into the supply wear model has scoped wrong.

## What already exists locally (do not rebuild)

Verified against 4.6.3 on 2026-07-30:

| Capability | Where | State |
|---|---|---|
| Equipment profiles, types, items | SDD-004, all three backends | shipping |
| `SupplyStatus`, pure and testable | `include/services/SupplyStatus.h` | shipping |
| Supply wear as retained MQTT sensors | `SupplyPublisher` | shipping |
| Opt-in cloud mirror with a full reconcile contract | `CpapDashSyncService` | shipping |
| Equipment page with `.item` / `.wear` styling | `frontend/.../equipment` | shipping |
| MySQL column migrations | `MySQLDatabase::migrateSchema()` | shipping (4.6.3) |

Every one of those is a thing this SDD reuses rather than duplicates.

## 1. Data model

Two tables, created in each backend's schema routine, following SDD-004's local
conventions: no `user_id`, `client_uuid` for sync identity, soft delete.

### 1.1 `cleaning_task_types` (preset catalog)

```
id                          INTEGER PRIMARY KEY
task_key                    TEXT NOT NULL UNIQUE
label                       TEXT NOT NULL
applies_to_type_key         TEXT              -- 'mask' | 'tubing' | ... | NULL = setup-wide
default_interval_days       INTEGER NOT NULL
is_system                   BOOLEAN DEFAULT 0
active                      BOOLEAN DEFAULT 1
created_at                  TIMESTAMP
```

Seeded with the same seven system rows SDD-043 defines, verbatim, so a user who
runs both stacks sees one vocabulary:

| `task_key` | Label | Applies to | Default |
|---|---|---|---|
| `mask_wipe` | Wipe the mask cushion | `mask` | 1 |
| `mask_wash` | Wash the mask and cushion | `mask` | 7 |
| `headgear_wash` | Wash the headgear | `headgear` | 7 |
| `tubing_wash` | Wash the tubing | `tubing` | 7 |
| `humidifier_empty` | Empty and rinse the water tub | `humidifier` | 1 |
| `humidifier_wash` | Wash the water tub | `humidifier` | 7 |
| `filter_check` | Check the filter | `filter` | 30 |

Seeded idempotently on `task_key`, the way SDD-004 seeds equipment types.

### 1.2 `cleaning_tasks`

```
id             INTEGER PRIMARY KEY
profile_id     INTEGER NOT NULL   -> cpap_equipment_profiles(id) ON DELETE CASCADE
item_id        INTEGER            -> cpap_equipment_items(id) ON DELETE SET NULL
client_uuid    TEXT                -- "" locally, NULL in SQL; sync identity
task_key       TEXT NOT NULL
label          TEXT NOT NULL       -- snapshot, see below
interval_days  INTEGER NOT NULL    -- CHECK > 0
time_minutes   INTEGER NOT NULL DEFAULT 510   -- 0..1439, local wall clock
start_date     TEXT NOT NULL       -- YYYY-MM-DD
enabled        BOOLEAN DEFAULT 0
last_done_at   TIMESTAMP
deleted        BOOLEAN DEFAULT 0
created_at     TIMESTAMP
updated_at     TIMESTAMP
```

A task belongs to a **profile**, matching SDD-004's no-unassigned rule: a travel
setup has its own schedule. `item_id` is optional because some tasks are
setup-wide.

`label` is snapshotted at creation, per SDD-043: editing the catalog later must
not silently rewrite what the user scheduled.

**Three schema files stay in sync** (`scripts/schema.sql`,
`schema_mysql.sql`, `schema_sqlite.sql`) per CLAUDE.md, and MySQL additionally
gets every new column listed in `migrateSchema()`, or an existing MySQL install
never gains them. That failure mode is why 4.6.3 added that pass.

## 2. Timezone, and the one thing 4.6.3 taught us

`time_minutes` is **local wall-clock minutes since midnight**, stored as an
integer and never localised by the service. `start_date` is a bare date string.

This is deliberate and it is the same call SDD-043 made, but the reasoning is
sharper here after 4.6.3: this repo's timestamp columns hold bare local wall
clock reached by a *matched* parse/render pair, and the oximetry bug was a
mismatched pair. Storing a minute-of-day integer and a date string sidesteps the
whole class: there is no instant to render, so there is nothing to get wrong.

`computeCleaningStatus` takes epochs and is pure, so any conversion happens at
one boundary, in the caller, and is testable there.

## 3. The due computation

`include/services/CleaningStatus.h` + `src/services/CleaningStatus.cpp`, sitting
beside `SupplyStatus`, **ported from `hms-cpapdash-api` unchanged apart from the
namespace**. Pure: no DB, no I/O, no clock.

```cpp
enum class CleaningState { Due, Upcoming, Disabled };

struct CleaningStatus {
    CleaningState state{CleaningState::Disabled};
    long long     next_due_epoch{0};
    int           days_until{0};   // negative once overdue
};

CleaningStatus computeCleaningStatus(long long start_date_epoch,
                                     int interval_days,
                                     int time_minutes,
                                     long long last_done_epoch,  // 0 = never
                                     bool enabled,
                                     long long now_epoch);
```

Anchoring, quoting SDD-043 because it must not be re-derived here:

- not enabled, or `interval_days <= 0`, gives `Disabled` and zeros
- never done anchors on `start_date` at `time_minutes`
- done before anchors one interval past the **day** it was done
- `Due` when `due <= now`, and it **stays** `Due` until marked done

It deliberately does not roll a missed slot forward. SDD-043 corrected itself on
exactly this point during implementation: rolling forward means a task the user
has never once done can never read as `Due`, and that is the user who most needs
telling.

**Drift guarantee.** There are now three implementations of this function: C++
in the cloud, C++ here, and the Dart port in the app. The GTest vectors in this
repo must be the *same vectors* as the cloud's, so the suites fail rather than
the behaviours silently diverging. This mirrors what SDD-035 wanted between
`SupplyStatus.cc` and `supply_status.dart`.

## 4. Delivery: MQTT, not notifications

hms-cpap has no phone and no notification planner. Its reminder channel is Home
Assistant, and the precedent already exists: `SupplyPublisher` turns supply wear
into retained MQTT sensors with discovery.

A new `CleaningPublisher` follows it exactly:

- one sensor per **enabled** task, keyed by task id
- state is the canonical string `due` / `upcoming` / `disabled`
- attributes carry `days_until`, `next_due`, `label`, `profile_name`,
  `task_key`, `interval_days`
- retained, with Home Assistant discovery config, same as supplies
- disabled and soft-deleted tasks publish an empty retained payload so their
  entity disappears rather than lingering with a stale value

**No scheduling in hms-cpap.** The service publishes *state*; when to be
nudged is an automation the user writes, which is how the rest of a Home
Assistant house already works and is strictly more flexible than a fixed alert.
This is the one place this SDD deliberately does less than SDD-043, and it is
because the local product's audience already owns the notification layer.

Published on the burst sweep, alongside `SupplyPublisher`, so there is no new
scheduler and no detached thread.

## 5. Cloud mirroring

Local is the source of truth. The cloud is an opt-in mirror, exactly as SDD-004
framed equipment, and everything works with `cpapdash.enabled = false`.

### 5.1 What the cloud already offers, and the one gap

`hms-cpapdash-api` **already serves a complete cleaning API**, shipped in
v2026.11.12 and in use by the app: `GET /v1/cleaning/types`, `GET /v1/cleaning`,
`POST /v1/cleaning`, `PUT /v1/cleaning/{id}`, `DELETE /v1/cleaning/{id}`,
`POST /v1/cleaning/{id}/done` and `POST /v1/cleaning/suggest`.

The gap is narrow and specific: those are **per-row, UI-shaped** calls. Equipment
mirrors through `POST /v1/equipment/sync`, a **bulk reconcile** that takes a
batch plus a cursor and returns the reconciled set, and there is no cleaning
equivalent. So there are two ways to mirror, and this is a real decision rather
than a missing feature:

**(a) Reuse the existing per-row endpoints.** No cloud change at all, which
means no cross-repo release dependency and this SDD ships entirely from one
repo. The costs are real though: it is N+1 requests per sweep, there is no
atomic batch so a mid-sequence failure leaves the two sides disagreeing, and
there is no `server_time` cursor to resume from, so `CpapDashSyncService`'s
existing cursor-and-tombstone contract would have to be partly reimplemented
against a different shape.

**(b) Add `POST /v1/cleaning/sync`.** One new endpoint mirroring the equipment
one, after which the local side is a near-copy of the equipment sync path and
inherits its reconcile, its cursor, its tombstones and its tests unchanged. The
cost is a cross-repo change and a release in a repo already at v2026.11.12.

**Decision (Albin, 2026-07-30): (b).** Add `POST /v1/cleaning/sync` to
`hms-cpapdash-api`, mirroring the equipment endpoint. The reconcile contract in
`CpapDashSyncService` is the part most likely to be got subtly wrong, and this
is the option where none of it has to be rewritten: the local side becomes a
near-copy of the equipment sync path and inherits its cursor, its tombstones and
its tests unchanged. The cost is one cross-repo release, and it falls entirely
in phase 4.

### 5.2 Reconcile rules

Unchanged from `CpapDashSyncService`'s documented contract, which is already
correct and already tested with an injected transport:

- every row carries a `client_uuid`, backfilled before the first push, which is
  what makes a retried sync idempotent instead of duplicating
- last-write-wins per row by `updated_at`, compared as **epochs** (local writes
  `...Z`, the cloud writes a Postgres offset stamp; a string compare is wrong)
- a tie, or a stamp either side cannot parse, resolves in favour of **local**
- `deleted:true` rows are tombstones and apply as tombstones
- server ids bind to local rows by uuid so later syncs update rather than insert

### 5.3 Profile identity across the boundary

A cleaning task references a profile, and local profile ids are not cloud
profile ids. The join is `client_uuid`, which equipment profiles already carry
and already sync. A task whose profile has no `client_uuid` yet is **held back**
rather than pushed with a dangling reference: the profile syncs first, then the
task follows on the next sweep. Same for `item_id`.

## 6. Local API surface

New `CleaningController`, mirroring `EquipmentController` route-for-route so the
frontend service is a near copy of the existing one.

| Method | Route | Purpose |
|---|---|---|
| GET | `/api/cleaning/types` | preset catalog |
| GET | `/api/cleaning` | tasks, each with computed `status` |
| POST | `/api/cleaning` | create |
| PUT | `/api/cleaning/{id}` | update interval, time, start, enabled, label |
| DELETE | `/api/cleaning/{id}` | soft delete |
| POST | `/api/cleaning/{id}/done` | stamp `last_done_at`, return recomputed status |
| POST | `/api/cleaning/suggest` | create the suggested set for a profile, all disabled |

`/suggest` reads the profile's items and creates one disabled task per matching
preset, so a setup with no humidifier gets no water-tub tasks. Idempotent on
`(profile_id, task_key)`.

`status` is computed on read and never stored, exactly like `supply`.

## 7. Frontend

A **Cleaning** section on the existing `/equipment` page, below Accessories,
inside the selected setup's panel, reusing the `.item` / `.wear` styling so it
reads as one page rather than a bolted-on feature. Per row: label, enable
switch, `every [N] days`, time, start date, "Mark done", and a next-due line
coloured like the wear bars.

Empty state offers one button, "Add suggested tasks".

## 8. Testing

Per CLAUDE.md the test binary excludes `main.cpp`, controllers and `web/`, so
the controller stays a passthrough and everything below is reachable.

- **`computeCleaningStatus`**, using **the cloud suite's vectors verbatim**:
  disabled short-circuit, never-done stays due rather than rolling forward, done
  anchors on completion day, the exact due boundary, interval of 1, a start date
  far in the past, and negative `days_until` while overdue.
- **Cross-backend parity**, in the shape of `test_OximetryBackends.cpp`: the
  same CRUD and `/suggest` idempotency asserted against SQLite always and
  PostgreSQL/MySQL when reachable, so "works on my engine" cannot survive. This
  is how 4.6.3 found that four read methods were stubs on two backends.
- **MySQL drift guard** extended: the new columns join
  `test_MySQLMigration.cpp`'s critical list, so an existing install that does not
  gain them fails a test rather than a user's dashboard.
- **`CleaningPublisher`** with an injected publish sink, asserting topic and
  payload shape, discovery config, and that a disabled or deleted task clears
  its retained entity.
- **Sync reconcile** with the injected transport, no network: last-write-wins
  both directions, tie favours local, tombstones apply, a task whose profile has
  no `client_uuid` is held back.

Full suite green before tagging. A filtered run is not a deploy gate.

## 9. Phases

Each is independently shippable.

1. **`CleaningStatus` + schema + local CRUD**, three backends, with the drift
   guard and parity tests. Useful on its own via the API.
2. **MQTT publishing.** `CleaningPublisher` on the burst sweep.
3. **Frontend.** The Cleaning section on `/equipment`.
4. **Cloud mirroring.** Per the §5.1 decision: either straight onto the existing
   per-row endpoints, or `POST /v1/cleaning/sync` in `hms-cpapdash-api` first and
   then the local sync extension. Last either way, because it is the only phase
   that can be blocked by the other repo.

## 10. Non-goals

- Scheduling or firing notifications from hms-cpap. It publishes state; Home
  Assistant decides what to do about it.
- Changing `SupplyStatus`, supply wear, or the replacement model in any way.
- Per-user anything. Single household, per SDD-004.
- Migrating existing cloud cleaning tasks down into a local install. A local
  install starts empty and `/suggest` fills it.
- A second notification path (email, push) from the local service.

## 11. Open items

- The seven preset labels are English-only here. The local frontend has no i18n
  bundle system yet, unlike the cloud's SDD-038, so Spanish is deferred rather
  than half-built.
- Whether a due cleaning task should also surface on the local dashboard, not
  just `/equipment`, is deferred and additive.
- Phase 4 needs a `hms-cpapdash-api` release carrying `POST /v1/cleaning/sync`
  before the local half can land. That repo is at v2026.11.12 today.

## References

- `hms-cpapdash-api` `sdd/043-cleaning-reminders-and-supply-push.md`
- `hms-cpapdash-api` `include/services/CleaningStatus.h`, `src/services/CleaningStatus.cc`
- `docs/SDD-004-equipment-profiles-and-supplies.md`
- `include/services/SupplyStatus.h`, `include/services/SupplyPublisher.h`
- `include/services/CpapDashSyncService.h` (reconcile contract)
- `include/database/MySQLDatabase.h` `migrateSchema()` (4.6.3)
