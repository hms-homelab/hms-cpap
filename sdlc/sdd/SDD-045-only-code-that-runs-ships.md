# SDD-045: only code that runs ships

**Status:** Proposed 2026-09-19. Albin: "i want the real thing, that's worth
another release free of dead code."
**Date:** 2026-09-19
**Repo:** `hms-cpap`. Its own release, no behaviour change intended.
**Related:** SDD-040 (one burst cycle, which orphaned a discovery copy),
CLAUDE.md (which described a deleted file as merely excluded)

## Trigger

A cosmetic log fix on 2026-09-19 turned over one stone and found 3,000 lines of
code that nothing calls: a second copy of the session discovery rules, a local
EDF parser and models island superseded by the shared library a year ago, four
methods declared and never defined, an orphan frontend component, and a
handful of unused helpers and accessors.

That was one technique (does this function's name appear anywhere else) over
one language. It answers the shallowest question available, and it still found
that much. This SDD does the rest properly.

## 1. What dead means here, and the trap

Dead: **nothing reaches it in any shipped configuration.** Three things make
that harder than it sounds, and each has already bitten this repo:

1. **Build configurations.** Code can be live on one platform and absent on
   another: Fysetc and the PDF/report stack are excluded on MSVC, the BLE
   client is Linux-only, MySQL and PostgreSQL are build options. A sweep run on
   one machine would delete another platform's code. **A symbol is dead only
   when it is dead in the UNION of every shipped configuration**: macOS native,
   Linux (the Docker image and the Pi), Windows, with and without each database
   and each optional feature.
2. **Tests as the only caller.** Production code whose only callers are its own
   tests is dead, and the tests go with it. The exception is when the rule it
   pins still lives on another path: that happened this morning with the
   discovery copy, where seven tests moved onto the live path instead of being
   deleted. **Look at every such case before deleting either side.**
3. **Reached by name rather than by call.** D-Bus handlers, MQTT topic
   dispatch, Drogon's `ADD_METHOD_TO`, Qt slots, `loadComponent` routes, SQL
   built from strings. A grep for the identifier can miss all of these, and a
   linker sees some of them as live because the address is taken. **Every
   candidate is checked by hand before it is removed.**

## 2. What gets audited

| Area | How |
|---|---|
| C++ functions never called | `cppcheck --enable=unusedFunction` over the whole program, per build configuration |
| C++ functions never executed | `scripts/coverage.sh` already produces lcov data; `FNDA:0` lists every function the whole suite never ran. Cross-referenced with the call graph, not deleted on its own |
| Code the linker discards | Link with `-ffunction-sections -fdata-sections -Wl,--gc-sections -Wl,--print-gc-sections` on Linux. What the linker throws away is dead by construction |
| Unused private members, locals, parameters | `-Wunused-private-field`, `-Wunused-variable`, `-Wunused-parameter` (clang already has these; they are not currently errors) |
| Unused includes | include-what-you-use, reported not auto-applied: it is noisy and its "add this" half is not wanted |
| Files in no build target | Compare `git ls-files 'src/**.cpp'` against what each CMake target actually compiles |
| Frontend exports, components, services | `knip` (or `ts-prune`) over `frontend/`, plus a check that every component's selector appears in some template and every route target resolves |
| Unused translation keys | Compare the keys in `i18n/pages/*.en.json` with what the templates and TypeScript reference; five languages must then drop the same keys, so the build's parity gate keeps agreeing |
| Unused API surface | Every Drogon route matched against the frontend, the skill, the docs and the tests. A route nothing calls is either dead or an undocumented feature, and the difference is a decision, not a deletion |
| Database columns and tables | Every column in `scripts/schema*.sql` and the runtime creators matched against reads and writes. **Report only this release** (D5) |
| Dead scripts and packaging files | `scripts/`, `packaging/`: referenced by CI, a skill, a doc, or nothing |

## 3. Decisions (Albin's)

- **D1, scope. ACCEPTED 2026-09-19**: `hms-cpap` (service, frontend, scripts,
  packaging) **and `hms-cpapdash-parser`**. The parser is a shared library, so
  its consumers are the gate: every candidate there is checked against every
  repo that links it, not just this one, and the parser's own release and
  version bump come first so hms-cpap builds against a published version rather
  than a moving one.
- **D2, test-only production code.** Proposed: delete both sides, unless the
  rule is shared with a live path, in which case the tests move first (as the
  discovery tests did).
- **D3, platform code.** Proposed: nothing is deleted unless it is dead in
  every shipped configuration, proven by running the analysis in each rather
  than by reading `#ifdef`s.
- **D4, unused API surface and unused endpoints. ACCEPTED**: reported with a
  recommendation each, not deleted in bulk. Some are features nobody wired up
  yet, and that is Albin's call, not the tool's.
- **D5, database columns. ACCEPTED, they go too.** That makes this release one
  that touches user data, so it carries its own rules:
  - a column is dropped only when nothing in ANY repo reads or writes it, the
    runtime creators and all three `scripts/schema*.sql` agree it is unused,
    and no released version still writes it (an older binary may still be
    running against the same database, which is the case that turns a cleanup
    into data loss);
  - the migration runs on SQLite, MySQL and PostgreSQL, is idempotent, and is
    gated on the version the way SDD-032 and SDD-034's data migrations are;
  - **the database is backed up before it runs**, reusing
    `update::backupSqlite` for SQLite and naming the server database in the log
    for the other two (SDD-041 D6 set that shape);
  - SQLite cannot drop a column before 3.35 and never drops one from an older
    file cheaply, so where DROP COLUMN is not available the column is left in
    place and recorded as abandoned rather than rewritten table-by-table;
  - each dropped column gets a test on all three engines proving the migration
    runs twice with the same result.
- **D6, keeping it out. ACCEPTED**: one gate in CI that fails on NEW dead code
  only (a baseline file of known exceptions), so this does not have to be done
  again by hand. Without it the same 3,000 lines grow back.

## 4. How it is verified

- **The suite, all three engines, both time zones, before and after.** Same
  counts, zero failures. A cleanup that changes a test result is not a cleanup.
- **Every platform still builds**: macOS native, the Linux Docker image, the Pi
  armhf zip, Windows through CI, and the Qt supervisor.
- **The binaries still behave**: the e2e on macOS and on Windows (.72), and the
  add-on image boots and collects a night.
- **Per deletion, a reason recorded** in the commit: which tool flagged it,
  what was checked by hand, and what the callers were.
- **Nothing user-visible changes.** No release note beyond "removed unused
  code", because a user should not be able to tell.

## 5. Release

Its own release, Albin's number. A cleanup release that also carries a feature
makes a bisect ambiguous the first time something breaks, and the whole point
of this one is that nothing should change at all.
