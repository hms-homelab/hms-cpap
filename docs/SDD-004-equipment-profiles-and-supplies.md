# SDD-004: Equipment Profiles and Supply Reminders (local-first, optional cloud sync)

**Status:** Draft — awaiting approval
**Date:** 2026-07-19
**Repo:** `hms-cpap` (C++ / Drogon / Angular, self-hosted)
**Mirrors:** `hms-cpapdash-api` SDD-035 (cloud). The cloud model is the superset;
this brings the same feature to the self-hosted service, working **fully offline**,
with cloud sync as an explicit opt-in.

## Principle

**Local-first.** Everything works with no account, no network, no cloud. The user
manages equipment and gets supply reminders entirely on their own hardware. Cloud
sync is a button they may never press.

## The model (identical to SDD-035)

Everything owned is an **equipment item** with a **category** (`machine` |
`accessory`) that comes from its **type**. A machine never wears out; an accessory
carries a wear clock. Items are grouped into a named **profile** ("setup"): one
profile owns **exactly one machine** plus its accessories. Multiple machines =
multiple profiles. **Supply** is not a separate thing — it is the computed wear
state of an accessory (`fresh / due_soon / overdue / untracked`), derived on read.
There is no "unassigned" bucket.

## What differs from the cloud, and why

| Cloud (SDD-035) | Here | Reason |
|---|---|---|
| Multi-tenant, every table keyed by `user_id` | **No `user_id`** | hms-cpap is single-household and LAN-trusted; auth is intentionally absent |
| One PostgreSQL schema | **3 backends** (Postgres / SQLite / MySQL) **+ 3 checked-in `scripts/schema*.sql`** | hms-cpap ships all three; see §Schema discipline |
| Reminders via FCM push | **MQTT + Home Assistant discovery** | Self-hosted has no push infra; HA is where a homelab user already gets alerts |
| Server is the source of truth | **Local is the source of truth**, cloud is an optional mirror | Local-first principle |
| JWT `UserAuthFilter` on every route | No auth | LAN-trusted (existing project stance) |

## Schema discipline (the trap in this repo)

Adding a table means touching **six** places:

- `src/database/PostgresDatabase.cpp`, `SQLiteDatabase.cpp`, `MySQLDatabase.cpp`
- `scripts/schema.sql`, `scripts/schema_sqlite.sql`, `scripts/schema_mysql.sql`

v4.4.10 shipped precisely because the checked-in scripts had drifted behind the
in-code migrations and databases created from the scripts failed at runtime. This
SDD therefore requires a test asserting the three script files declare the same
equipment columns the code expects, so drift fails CI instead of a user's install.

## Data model

Tables use the existing `cpap_` prefix. Types are written per-dialect
(`SERIAL`/`AUTOINCREMENT`/`AUTO_INCREMENT`, `TIMESTAMPTZ`/`TEXT`/`DATETIME`).

```
cpap_equipment_types
  id, type_key, label, category('machine'|'accessory'),
  default_replace_after_days (NULL for machine), is_system, active,
  created_at, updated_at
  UNIQUE(type_key)

cpap_equipment_profiles
  id, client_uuid, name, active, deleted, created_at, updated_at
  UNIQUE(client_uuid) WHERE client_uuid IS NOT NULL

cpap_equipment_items
  id, profile_id -> cpap_equipment_profiles(id) ON DELETE CASCADE,
  client_uuid, type_key, category, brand, model, variant,
  started_using_at, replace_after_days (NULL = type default), notes,
  active, deleted, created_at, updated_at
  UNIQUE partial: one live machine per profile
```

**Seeded system types** (verbatim from the app's `supply_defaults.dart`, so local,
cloud and app all agree): machine (untracked), mask 90, tubing 90, filter 30,
humidifier 180, headgear 180. Due-soon window 14 days.

`client_uuid` exists **only** to make cloud sync idempotent; it is unused offline.

**One machine per profile** is enforced by a partial unique index where the dialect
supports it (Postgres, SQLite) and by an application check on MySQL, which does not
support partial indexes — the controller guard is the portable enforcement.

## Supply status

Pure function, ported from `hms-cpapdash-api/src/services/SupplyStatus.cc` (itself a
port of the app's `supply_status.dart`). No DB, no I/O:

```
interval = item.replace_after_days ?? type.default_replace_after_days
untracked if interval <= 0 or started_using_at is unset
else replace_by = started + interval; state = overdue | due_soon(<14d) | fresh
```

Keeping all three implementations byte-equivalent in behaviour is the point; a
shared fixture set is used in tests.

## REST API (Drogon, new `EquipmentController`)

No auth (project stance). Mirrors the cloud paths so the Angular page can be
adapted rather than rewritten.

| Method | Route |
|---|---|
| GET/POST | `/api/equipment/types` |
| PUT/DELETE | `/api/equipment/types/{id}` |
| GET | `/api/equipment/profiles` (nested items + computed `supply`) |
| POST | `/api/equipment/profiles` (optionally seed items) |
| PUT/DELETE | `/api/equipment/profiles/{id}` |
| POST | `/api/equipment` (400 on a second machine in a profile) |
| PUT/DELETE | `/api/equipment/{id}` |
| GET | `/api/supplies` (urgency-sorted wear view) |
| POST | `/api/equipment/cloud-sync` (**opt-in**, see below) |

## Reminders — MQTT / Home Assistant

No push. Supplies are published as HA entities via the existing
`DiscoveryPublisher`, so they land where the user already gets homelab alerts and
can drive any automation they like:

- `sensor.cpap_<type>_days_left` per tracked accessory
- `sensor.cpap_<type>_wear_percent`
- `binary_sensor.cpap_supplies_due` (ON when anything is due_soon/overdue)
- attributes carry profile name, brand/model, replace-by date

Republished each burst cycle (the loop that already runs), so no new scheduler.

## Cloud sync — opt-in, modelled on the SleepHQ integration

`sleephq` is the precedent: a config block with credentials plus `enabled` and
auto-trigger flags, driven by a service. Same shape:

```json
"cpapdash": {
  "enabled": false,
  "api_url": "https://api.cpapdash.com",
  "email": "",
  "password": "",
  "auto_sync": false
}
```

**Local always wins as the working copy.** Sync is a reconcile, not a replace.

Flow (`POST /api/equipment/cloud-sync`, and optionally after local edits when
`auto_sync`):

1. Obtain a user JWT from `POST /v1/auth/login` (24h; cached, re-fetched on 401).
2. `POST /v1/equipment/sync` with local profiles + items changed since the stored
   cursor, each carrying its `client_uuid`.
3. Apply the response: last-write-wins per row by `updated_at`, tombstones drop
   local rows, server ids bound to local rows for future syncs, `server_time`
   stored as the next cursor.

The cloud endpoint already accepts a `profiles` array and returns
`profiles` + `default_profile_id`, so **no cloud-side change is required.**

### Open question for approval

Storing the account **password** in `config.json` mirrors SleepHQ's
`client_secret`, but a plaintext cloud password is a bigger liability than an API
secret. Options: (a) password in config like SleepHQ, (b) paste a long-lived token
instead, (c) add a device-token endpoint to the cloud API. **Recommend (b)** — no
cloud change, no password at rest, revocable.

## Non-goals

- No resupply commerce.
- No change to session/therapy ingestion.
- No auth added to hms-cpap.
- No medical claims; intervals encode common resupply cadence.

## Phases

1. **Schema + DB layer** — 3 backends, 3 scripts, `IDatabase` methods, drift test.
2. **SupplyStatus port** + pure tests against the shared fixture set.
3. **EquipmentController** — types/profiles/items CRUD, one-machine guard.
4. **MQTT/HA supply entities** via `DiscoveryPublisher`.
5. **Angular Equipment page** (adapted from the cloud one already built).
6. **Cloud sync** — `CpapDashSyncService` + the opt-in button.

Each phase: tests first, full suite green before deploy (repo rule).

## Follow-up, queued BEHIND this feature

**Port the SleepHQ upload fixes from `hms-cpapdash-api`.** The two services have
diverged (cloud `SleepHqExportService.cc` 9079 B / `SleepHqClient.cc` 7565 B vs
local 8710 B / 7513 B). Cloud-side commits that look worth bringing over:

- `6bc1a2a` — export unparsed nights via a storage disk-walk fallback (nights that
  never parsed are currently just skipped locally).
- `4d1fb05` — empty-folder ledger delete, discovery pacing, and SleepHQ **root
  files** handling.

Not started until Phases 1–6 below are done. Needs its own diff review first: the
two codebases have different storage layouts (cloud object storage vs the local
`local_dir` card mirror), so these are ports, not copies.

## Test plan

- Supply status parity with the cloud/app fixtures (boundaries, override beats
  default, machine untracked).
- One-machine-per-profile: 400 on second machine; index rejects a racing insert on
  Postgres/SQLite; app guard covers MySQL.
- Schema-script drift: the three `scripts/schema*.sql` declare what the code reads.
- Backend parity: the same CRUD suite runs against SQLite (always) and
  Postgres/MySQL when reachable, else skipped.
- Sync: idempotent re-sync is a no-op; last-write-wins both directions; tombstone
  propagation; a failed login degrades to local-only without data loss.
- Offline: every local operation works with `cpapdash.enabled = false`.
