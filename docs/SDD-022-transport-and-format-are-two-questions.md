# SDD-022: transport and format are two questions

**Status:** Proposed
**Date:** 2026-09-06
**Repo:** `hms-cpap` — backend, Angular frontend, and the Qt supervisor
**Version:** target TBD (Albin's call)
**Depends on:** nothing
**Related:** SDD-016 (the supervisor owns `FieldSpec`), SDD-010 (local card root),
SDD-080 (the five-language parity gate this must not break)

## Trigger

A Sefam S.Box owner cannot select his machine. `source: "sefam"` works, is tested,
and ingests his card end to end — but it can only be reached by hand-editing
`config.json` or setting an environment variable. Neither the web settings page,
the first-run wizard, nor the desktop configurator offers it.

Nor do they offer `lowenstein`, which has shipped since long before Sefam. Every
Prisma owner has been hand-editing a JSON file too.

So the immediate ask is "add Sefam to the UI". Looking at where to add it is what
found the real problem.

## The actual problem: one field, two questions

`config.source` is a single string with five legal values, and they answer two
different questions that have been welded together:

| value        | where files come from        | what format they are |
|--------------|------------------------------|----------------------|
| `ezshare`    | HTTP from the WiFi SD card   | ResMed               |
| `fysetc`     | raw sectors over TCP         | ResMed               |
| `local`      | a folder on this computer    | ResMed               |
| `lowenstein` | a folder on this computer    | Löwenstein Prisma    |
| `sefam`      | a folder on this computer    | Sefam S.Box          |

The bottom three are not three sources. They are **one** source — a folder on
disk — read by three different parsers. All three read the same config key:

```
src/services/BurstCollectorService.cpp:92    local        CPAP_LOCAL_DIR
src/services/BurstCollectorService.cpp:109   lowenstein   CPAP_LOCAL_DIR
src/services/BurstCollectorService.cpp:117   sefam        CPAP_LOCAL_DIR
```

### The conflation is already leaking

Two places in the code answer a transport question by enumerating vendor values,
and both have to be edited every time a vendor is added:

**`PreflightService::sourceNeedsArchive` (`src/services/PreflightService.cpp:251`)**

```cpp
// local and lowenstein read files that are already on disk. ezShare and
// Fysetc receive them over the network and must write them somewhere first.
return source == "ezshare" || source == "fysetc";
```

"Did this arrive over a network?" is purely about transport. Today it is answered
by listing the two transports that are not folders, and it is correct for `sefam`
only by accident — nobody updated it, and the omission happened to be right.

**`desktop/qt/core/pure/FieldSpec.cpp:25`** is a hand-copied duplicate of the same
function, with a comment saying `/// Mirrors PreflightService::sourceNeedsArchive.`
Two copies of one rule, in two languages, kept in step by hand.

**`CpapController.cpp:1121`** gates the local-directory layout classifier on
`source == "local"`, with a comment explaining that Löwenstein would hard-fail it
because a Prisma tree has no `DATALOG`. That comment is the design stating out
loud that these are the same transport with different formats — and that the
classifier is format-specific, not transport-specific.

## Decision

**Split the axes.** Albin's call, 2026-09-06, taken with the migration cost
stated: *"split it with a migration work for current installs"*.

Two fields:

```jsonc
{
  "transport": "local",     // ezshare | local | fysetc   — WHERE files come from
  "format":    "sefam"      // resmed | lowenstein | sefam | philips — WHAT they are
}
```

**Philips is in the enum from day one.** Albin's call, 2026-09-06: the
experimental DS2 support in `hms-cpapdash-parser-philips` gets the same
treatment as Sefam. It is also the clearest argument for the split. Under the
old scheme Philips would have been a FIFTH value welded onto `source`, and a
fifth entry in every place that enumerates a vendor list. Under this one it is
a value in an axis that already exists, and `sourceNeedsArchive` does not
change at all, because the TRANSPORT did not change.

`format` is meaningful for every transport, not only `local`. It is constrained
rather than free: `ezshare` and `fysetc` are ResMed-only today, so the UI offers
`format` as a choice only when `transport` is `local`, and pins it to `resmed`
otherwise. That is a UI affordance, not a data rule — the field still exists and
still says `resmed`, so nothing has to special-case its absence.

### Why not keep one flat list

Adding two more values to the existing enum is a smaller change and was the
alternative offered. It is rejected because it does not remove the two hand-kept
copies of `sourceNeedsArchive`, and every future vendor keeps costing an edit in
both of them plus an audit of which vendor list each of the five call sites meant.
The next vendor is not hypothetical; the S.Box arrived from a GitHub issue.

## Migration

Every existing install has `source` and no `transport`/`format`. There is no
account system and no server — the config is a file on the user's disk — so the
migration has to be silent, automatic, and safe to run repeatedly.

### The mapping

```
source=ezshare     -> transport=ezshare  format=resmed
source=fysetc      -> transport=fysetc   format=resmed
source=local       -> transport=local    format=resmed
source=lowenstein  -> transport=local    format=lowenstein
source=sefam       -> transport=local    format=sefam
(no legacy value)  -> transport=local    format=philips   (new, never had a `source`)
absent/unknown     -> transport=ezshare  format=resmed   (today's default)
```

### Rules

1. **Migrate on load, in `AppConfig`.** Not in the collector, not in a controller:
   the file is read in one place and every consumer must see the migrated pair.
2. **`source` is read when `transport` is absent, and ignored when it is present.**
   A config carrying both — written by an older build after a newer one has run —
   resolves to the new fields rather than silently reverting.
3. **Write the new fields, and KEEP WRITING `source`, for one release.** A user
   who upgrades, dislikes it and rolls back must not find a config the old build
   cannot read. The compatibility write is removed in the release after, and that
   removal is a separate change with its own note.
4. **`CPAP_SOURCE` keeps working.** `docs/REFERENCE.md:233,269`, `quickstart.sh:63-65`
   and every Docker invocation in the changelog use it, and the HA add-on passes
   configuration as environment. It maps through the same table. New equivalents
   `CPAP_TRANSPORT` and `CPAP_FORMAT` are added and take precedence when set.
5. **Migration is not a preflight failure.** A config that only has `source` is
   valid and always was.

## The work

### Backend

- `include/utils/AppConfig.h` — the two fields, the migration on load, the
  compatibility write, and the env precedence. This is where the mapping table
  lives; nothing else may reimplement it.
- `src/services/PreflightService.cpp:251` — `sourceNeedsArchive` becomes a
  question about `transport` alone, and stops enumerating vendors.
- `src/controllers/CpapController.cpp:1121` — the layout classifier gate becomes
  `transport == local && format == resmed`, which is what the existing comment
  already says in prose.
- `src/services/BurstCollectorService.cpp:91-124` — the ingester is chosen by
  `format` once `transport` is `local`.
- `src/main.cpp:153-170` — `printConfiguration` prints both.

### The supervisor / installer

- `desktop/qt/core/pure/FieldSpec.cpp:65` — `source` becomes two `Choice` fields.
  `local_dir` is revealed for `transport == local` rather than `source == local`,
  which is why Prisma and Sefam users cannot currently reach the folder picker
  even by typing the value in by hand.
- `desktop/qt/core/pure/FieldSpec.cpp:25` — **keep the duplicate, add a drift
  test.** Albin's call, 2026-09-06: *"drift dont remove it"*. The copy exists so
  the supervisor's pure core stays free of the service's headers, which is what
  makes it testable on every platform with no Qt and no display (SDD-016). The
  cost of that independence is a second copy of the rule; the fix is a test that
  fails the moment the two answers disagree, not a dependency that removes the
  independence. The test enumerates all three transports and asserts both
  implementations agree on each.

  This is deliberately the conservative option **for now**, not forever: *"until
  we know is good"*. Removing a duplicate at the same time as changing the rule
  it duplicates means a failure could be either change, and this SDD is already
  moving the rule from vendors to transports. Once the split has shipped and
  proven itself, collapsing the two copies is a candidate for its own change —
  where a failure can only mean one thing.

### Frontend

- `pages/settings/settings.component.ts` — the `<option>` list becomes two selects.
- `pages/setup/setup.component.ts` — the first-run radio group gains the choice.
- `models/config.model.ts` — the shape.
- **i18n: 10 files.** `i18n/pages/settings.{en,es,fr,pt,hu}.json` and
  `setup.{...}.json`. The prebuild parity gate fails the build if any language
  drifts, so all five land together or none do. Vendor names (`ResMed`,
  `Löwenstein Prisma`, `Sefam S.Box`) are proper nouns and are NOT translated;
  the field labels are.

### Tests

- The mapping table, both directions, including the absent and unknown cases.
- A config with both `source` and `transport` resolves to `transport`.
- Migration is idempotent: running it on already-migrated config changes nothing.
- `sourceNeedsArchive` for all three transports.
- `CPAP_SOURCE=lowenstein` still selects the Prisma ingester.
- `CPAP_TRANSPORT`/`CPAP_FORMAT` override `CPAP_SOURCE`.
- Coverage: the gate is 80.0% and currently passes at exactly 80.0%. This adds
  branchy config code, so its tests are not optional — see the note below.

## Risks

**The coverage gate has no headroom.** The last CI run passed at exactly 80.0%
(10362 of 12958 lines). Any untested branch added here fails the build. This is a
statement about sequencing, not a reason to lower the gate.

**Rollback.** Covered by rule 3 for one release. After that a downgrade loses the
source setting and falls back to the `ezshare` default, which is a documented
one-line fix for the user, not silent data loss.

**Nothing about the reading paths changes.** No parser, no ingester, no schema.
If a night parses today it parses identically after this. The blast radius is
configuration and the three UIs that edit it.

## Out of scope

- Offering `format` for `ezshare`/`fysetc`. Both are ResMed-only in fact; when a
  non-ResMed WiFi card appears, the field is already there to carry it.
- Auto-detecting the format from the folder. `classifyLocalDir` and
  `SefamIngestion::initialize` could between them make a good guess, and a wrong
  guess against a user's explicit choice is worse than asking. Worth its own SDD.
- The cloud (`cpapdash-api`). It has its own upload path and its own unowned S.Box
  work; nothing here touches it.
