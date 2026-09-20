# Plan: SDD-045, the dead-code sweep

Two repos, two releases. `hms-cpapdash-parser` is linked by hms-cpap, the cloud
monolith (`hms-cpapdash-api`), `cpapdash-ingest` and
`hms-cpapdash-parser-philips`, so its sweep is judged against all four and ships
on its own tag first. hms-cpap then bumps to that version and ships its own.

Nothing is deleted before its phase's report is reviewed.

## Phase 0: tooling and a baseline (no code changes)

- `brew install cppcheck`; `npx knip` for the frontend (no install into the repo).
- Record the baseline: full suite on all three engines in both zones, the
  coverage ratchet number, and the line counts per area.
- Confirm every shipped configuration still builds today, so a later failure is
  attributable: macOS native, Linux image, Pi armhf, Windows (CI),
  Qt supervisor.

## Phase 1: inventory, per configuration (no code changes)

Run and keep the raw output of each, per configuration
(macOS / Linux / Windows-MSVC defines, with and without MySQL, PostgreSQL, BLE,
reports):

1. `cppcheck --enable=unusedFunction --project=build/compile_commands.json`
2. lcov `FNDA:0` from `scripts/coverage.sh`: functions the suite never runs
3. Linux link with `-ffunction-sections -Wl,--gc-sections -Wl,--print-gc-sections`
4. clang `-Wunused-private-field -Wunused-variable -Wunused-parameter`
5. files compiled by no target: `git ls-files` against each target's sources
6. frontend: `knip` for unused exports/files, selector and route reachability
7. i18n: keys in `i18n/pages/*.en.json` never referenced in a template or .ts
8. routes: every `ADD_METHOD_TO` against frontend, tests, skills, docs
9. schema: every column in the runtime creators and `scripts/schema*.sql`
   against reads and writes, in EVERY repo that touches the same database
10. `scripts/`, `packaging/`: referenced by CI, a skill, a doc, or nothing

**Output:** one report per category, each candidate with the evidence, the
configurations it is dead in, and what reaches it if anything. The union rule
(SDD-045 §1) is applied here: a symbol live in any configuration leaves the list.

## Phase 2: the C++ sweep (hms-cpap)

Delete in small, reviewable batches, each with its own build and suite run:
internals (statics, private helpers) → unused files → unused members and
parameters → includes (reported, applied only where obvious). Each commit says
which tool flagged it and what was checked by hand.

## Phase 3: frontend

Unused exports, components and services; then unused i18n keys, dropped from
all five languages together so the build's parity gate keeps passing. `npm run
build` after each batch, plus a click-through of the pages that lost anything.

## Phase 4: schema (D5, the one that touches user data)

For each column or table nothing reads or writes anywhere:

- prove no released binary still writes it (an older version may be running
  against the same database);
- add an idempotent, version-gated migration for SQLite, MySQL and PostgreSQL,
  after a backup (`update::backupSqlite` for SQLite, the server database named
  in the log otherwise);
- where SQLite cannot drop the column, leave it and record it as abandoned;
- a test per engine proving the migration is idempotent and loses nothing else;
- update all three `scripts/schema*.sql` in the same commit.

## Phase 5: the API report (D4)

Every uncalled route and public method, each with what it does and a
recommendation. Albin picks; only approved ones are removed.

## Phase 6: the gate (D6)

A CI step that fails on NEW dead code only, against a checked-in baseline of
known exceptions, with one line saying how to update the baseline deliberately.

## Phase 7: the parser library

The same phases 1 and 2 inside `hms-cpapdash-parser`, judged against all four
consumers (build each against the swept parser before its tag). Its own release
first; then hms-cpap bumps to it.

## Verification, before either tag

- Full suite, all three engines, both time zones: same test count as the
  baseline minus only the tests deliberately removed, zero failures.
- Every configuration builds: macOS, Linux image, Pi zip, Windows, supervisor.
- E2E: macOS and Windows (.72) as in the hms-cpap-e2e skill, plus the add-on
  image booting and collecting a night.
- The four parser consumers build and their suites pass against the new parser.
- Coverage at or above the ratchet.
- A user cannot tell: no behaviour change, no note beyond "removed unused code".
