# SDD-019: One definition of the index

**Status:** Proposed
**Date:** 2026-08-26
**Repo:** `hms-cpapdash-parser` (the definition) plus `hms-cpap` (the surface)
**Version:** target TBD (Albin's call)
**Depends on:** nothing
**Unblocks:** SDD-020 (there is nothing to compare myAir against until this exists)

## Trigger

hms-cpap computes no score of any kind. Grep for it: `src/` and `include/` contain
`zScore`, `f1Score` and a flow-limitation score inside `EDFParser.cpp`, and nothing
that answers "how was last night". The web dashboard shows AHI, usage and leak as
three separate numbers and leaves the reader to combine them.

The index does exist, once, in one place: `cpapdash-app/lib/services/sleep_score.dart`,
shipped as SDD-006 F3/F4. hms-cpapdash-api does not have it either. Its only hit for
`score` is a translation key, `dashboard.keyMetrics.ahiScore`.

So "we already do this in the app and the api" is half true. It is in the app. Moving
it to hms-cpap by writing it again would make three copies of a set of tunable
constants, in three languages, with nothing keeping them equal.

## What the definition currently is

Verbatim from `sleep_score.dart`, because these numbers are the spec:

| Component | Weight | Full credit at | Zero credit at |
|---|---|---|---|
| Usage | 40 | >= 7 h | 0 h |
| AHI | 40 | <= 5 | >= 30 |
| Leak (95th percentile) | 20 | <= 24 L/min | >= 40 L/min |

Each present component contributes its weight and the sum is renormalised over the
weights actually present, so a machine that reports no leak scores out of 80 rather
than being punished for silence. All three absent returns null, not zero.

Bands: >= 85 excellent, >= 70 good, >= 50 fair, below that needs attention.

Streaks live in the same file: a night counts as compliant at >= 4 h, the current
streak is consecutive calendar dates ending at the newest night, and milestones are
7, 30, 100 and 365.

## Where the definition lives

**`hms-cpapdash-parser`** (Albin, 2026-08-26).

hms-cpap and hms-cpapdash-api both already link that library, and it is already the
place where "the two sides agree by construction rather than by luck" is the stated
reason a thing lives there (SDD-018). The score is pure arithmetic over three doubles,
which is the easiest possible thing to share and the easiest possible thing to let
drift.

Dart stays a second implementation, because the app scores nights offline and is not
going to call into C++ for it. It stops being an independent definition and becomes a
copy held to the C++ one by shared test vectors, below.

### What gets added

New `include/cpapdash/parser/SleepIndex.h` + `src/SleepIndex.cpp`, namespace
`cpapdash::parser`, no I/O and no dependencies beyond `<optional>`:

- the weights and cutoffs as named constants, one place;
- `std::optional<int> nightlyIndex(std::optional<double> usage_hours,
  std::optional<double> ahi, std::optional<double> leak_95)`;
- `IndexBand bandFor(int)`;
- `std::optional<double> trailingAverage(span of nightly indices, int nights)`;
- `int currentStreak(...)` and `int bestStreak(...)` over (date, usage_hours) pairs,
  since those are equally pure and equally copyable.

### The contract that keeps Dart honest

`tests/fixtures/sleep_index/` in the parser repo, two plain text tables:

- `index_vectors.csv`, one line per night as
  `usage_hours,ahi,leak_95,expected_index,expected_band`, an empty field meaning the
  input is absent and an empty `expected_index` meaning the night does not score.
  Covers full credit, zero credit, every single-component-missing case, the all-missing
  null, a rejected negative leak, and both sides of each band boundary.
- `streak_vectors.csv`, one line per case as
  `name;date:hours,date:hours,...;expected_current;expected_best`, newest night first.

CSV rather than JSON on purpose: the parser links no JSON library and pulling one in to
read a five column test table would cost more than the table is worth. Both formats are
three lines of parsing in C++ and in Dart.

The parser's GTest reads them. A Dart test in `cpapdash-app` reads the same files,
vendored by a small sync script.

A weight change is then one edit plus one regenerated fixture, and the app fails its
own tests until it follows. That is the entire point of this SDD.

## What hms-cpap does with it

The inputs are already in the database. `cpap_daily_summary` holds `patient_hours`,
`ahi` and `leak_95` per `record_date`, on all three engines. One row in, one number out.

**Computed on read, not stored.** A stored index is wrong the moment a weight moves,
and then there are two answers in the same database, one of them silently stale.
`/api/daily-summary` and `/api/sessions` gain an `index` field filled in at query time.
No migration, no backfill, nothing to invalidate.

Surfaces:

- the dashboard, as the headline number the three existing metrics currently make the
  reader assemble by eye;
- MQTT discovery, as `daily_sleep_index`, following the exact shape
  `DataPublisherService::publishDaily*` already uses, so Home Assistant gets it for free
  and SDD-021 does not have to add anything;
- the PDF report, once it is on the dashboard.

## Naming

The app calls it "Sleep score". myAir's field is also called `sleepScore`, and SDD-020
puts both on the same screen. Two things called the same thing on one page is a support
ticket waiting to happen.

Recommendation, not a decision: ours is the **CpapDash index**, theirs stays the
**myAir score**. The app's existing user-facing string changes with it, which is a
translation pass across en/es/fr.

## Open questions

1. Does the app's string change now or when SDD-020 ships? Renaming twice is worse than
   renaming late.
2. Streaks in hms-cpap: worth surfacing, or app-only? The functions come along either
   way; the question is whether the web dashboard shows them.
3. The parser is pinned by `GIT_TAG` in hms-cpap's CMakeLists. This needs a parser
   release before hms-cpap can consume it, same dance as SDD-014.

## Out of scope

- Changing any weight or cutoff. This SDD moves the definition, it does not argue with
  it. If real data argues with it later, that is a one-line change in one file, which is
  precisely what this buys.
- Any claim that the index means something clinically. It is therapy quality, the
  wording in `sleep_score.dart` already says so, and it stays that way.
