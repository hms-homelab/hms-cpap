# SDD-040: one burst cycle for every transport

**Status:** Proposed, 2026-09-18. Albin has already settled three of it (§4):
the local folder gets an `IDataSource`, staging and archiving are no-ops when
the source IS the archive, and this ships as its own release.
**Date:** 2026-09-18
**Repo:** `hms-cpap`. `BurstCollectorService::executeBurstCycle()`, a new
`LocalDataSource`, `SessionDiscoveryService`.
**Related:** SDD-008 (the folder ledger), SDD-010 (the local dir is a card root),
SDD-016 and SDD-028 in `hms-cpapdash-api` (the cloud's tiered planner),
SDD-037 (a local folder is a finished night), SDD-038 (the history catch-up)

## Trigger

Every local-source defect this month was the same shape: the local branch
inherited part of the ez Share contract and not the rest.

- SDD-037: it inherited "close a night when its files stop growing" without
  inheriting a way to look at an old night twice, so nights stayed LIVE forever
  (support 129).
- SDD-038: it inherited the forward-only scan anchor, which is right for a card
  that grows and wrong for a folder of history (#34, support 129).
- `night_state` falls back to the no-ledger path for local sources, because the
  local branch does not write the folder ledger the ez Share branch does.

Albin, 2026-09-18: "im inclined to do the same burst cycle in both mode."

## 1. Why the two branches exist at all

`IDataSource` (`include/clients/IDataSource.h`) is already the seam: list date
folders, list files in one, fetch a file, fetch a range, fetch a root file.
`EzShareClient` and `FysetcDataSource` implement it. **Nothing implements it for
a local folder**, so `executeBurstCycle()` grew a second branch
(`:1583` onward) that reads the filesystem directly, stages symlinks and copies
into a temp dir, and parses from there.

The result is one 906-line function with two halves that drift apart every time
either is touched. The drift IS the bug class.

## 2. Design

### 2.1 `LocalDataSource`

A new implementation of `IDataSource` over a card root: `listDateFolders()` is a
directory read of `DATALOG`, `listFiles()` reads one date folder with sizes,
`downloadFile()`/`downloadFileRange()`/`downloadRootFile()` read bytes from
disk. `supportsRange()` answers for a file, where a range is a seek, not a
request.

The cycle then has one path. The local branch is deleted, not ported.

**Every behaviour is pinned against what `EzShareClient` ACTUALLY does**, read
from its source, not inferred from the interface. Albin, 2026-09-18: "i want to
make sure we are not buildin onn top of user evironmental quircks like we did
with oximetry and deep folders check". The first draft of the range read assumed
"always append"; the real client truncates at offset 0 and appends otherwise,
and always appending would have doubled a file on the first full read.

| Behaviour | `EzShareClient` | `LocalDataSource` |
|---|---|---|
| `listFiles` | files only, directories dropped | same, and sorted by name (a superset guarantee: a directory read has no inherent order) |
| `listDir` | files AND directories, empty on failure | same |
| sizes | the integer KB in the card's HTML | `bytes / 1024`, truncating |
| `downloadFileRange` at offset 0 | truncates the local copy | same |
| `downloadFileRange` resuming | appends | same |
| parent directories | created | same |

Two divergences, deliberate and commented where they live:

- **A range at or past EOF answers true with nothing appended.** An ez Share
  cannot: the response falls outside 200/206, so the client returns false and
  the caller re-reads the whole file, and per the collector's sidecar rule
  asking at all can wedge the card. A file on disk has a knowable end, and
  "nothing new" is exactly the signal a night settles on.
- **A 0-byte file is a file, not a failure.** `EzShareClient` deletes the
  destination and returns false for an empty result, because over HTTP an empty
  body means the fetch failed. On disk it means the card holds an empty file,
  and refusing it would turn a real card file into an error that repeats every
  cycle.

### 2.2 When the source IS the archive, staging, archiving and the residual walk
are all no-ops (D1, Albin: "yes to the rule that staging and archiving need no
ops", "in the case of the local mode would not need to walk on residual since is
there already")

The shared path downloads into a staging directory and then mirrors it into the
archive's card layout. For a local source that would copy the user's own folder
into a second folder, which SDD-037 D3 already rejected: "local is already a
copy". So the cycle asks the source one question, "are your files already where
the archive would put them?", and when the answer is yes it parses them in
place, skips the mirror, and leaves the archive alone.

The residual walk goes the same way, for the same reason. It exists to pull a
card's non-EDF files (`Identification.tgt`, `SETTINGS/`, `JOURNAL`, the per-night
`.crc`) ACROSS a transport into the archive. With a local folder they are
already there: walking it would read the user's own files to copy them onto
themselves. So the residual tier is empty by definition for a local source, and
the tiers reduce to the live night, new folders, then history.

That keeps today's local behaviour exactly, while the code path is shared.

### 2.3 One parse per night (D7)

The two branches also parse differently, and only one of them can survive.

- The ez Share path hands the parser the whole date folder, once per group:
  `session_dir = local_base_dir + "/" + session.date_folder` (`:1926`, `:2054`).
- The local path copies one group's files into a temp dir first (`:1771`),
  because "parseSession reads ALL files in a dir".

Measured rather than reasoned about (a harness over the real parser and two real
nights, 2026-09-18):

| | block A alone | block B alone | both in one directory |
|---|---|---|---|
| duration | 27,300 s | 2,460 s | 29,760 s, the sum |
| events | 7 | 0 | 7, the union |
| breaths | 7,245 | 704 | 7,949, the sum |

`parseSession()` MERGES a directory into one session, with therapy time summed
rather than the envelope. That is the night, which is what Albin says parsing is
always about: "Parsing should be ALWAYS about the entire night, not the a
checkpoint file."

It also exposes a latent defect on the EZ SHARE side. If a folder ever resolves
to two groups, that path parses the whole folder twice and stores the same
merged night under two `session_start` values, double-counting its events and
breaths. It stays hidden because discovery merges a night's checkpoints into one
group in practice ("Split 9 checkpoint files into 1 session(s)" in both users'
logs), but the unified cycle must not inherit it.

**The rule: one parse per night.** A date folder that resolves to a single group
is parsed IN PLACE, with no copy at all, which is the common case and is what
makes the no-op rule above real for a local source. A folder that genuinely
splits has each group's files isolated first, because that is the only way the
parser can be told which block it is looking at.

### 2.4 What local gains by being on the shared path

- the folder ledger (SDD-008), so `night_state` stops falling back to the
  open-session test and a local night can read `partial` for the right reason;
- one catch-up (SDD-038), one close rule (SDD-037), one place for the next one;
- the tier order, once D5 lands, instead of a second implementation of it.

## 3. Decisions

- **D1, staging and archiving for a local source. SETTLED 2026-09-18**: no-ops,
  as in 2.2.
- **D2, the abstraction. SETTLED 2026-09-18**: `LocalDataSource` behind
  `IDataSource`, one cycle for every transport.
- **D3, the release. SETTLED 2026-09-18**: its own SDD and its own release, after
  SDD-038 and SDD-039.
- **D4, does the local path write the folder ledger?** Proposed: yes (2.3). It
  is the upside of unification and it makes `night_state` mean one thing. The
  cost is a ledger row per night per install that never had one, and a
  first-run migration that leaves old nights without rows until they are
  re-read.
- **D5, does the tier planner come with this or after?** Proposed: with it, and
  only then. Once there is one cycle, ordering the work as the cloud's
  `nextCommand` does (live night, new folders, history, residual last) is a
  small change on one path instead of a duplicated one. Alternative: unify
  first, plan later, which is two releases through the same risky function.
- **D7, how a night is parsed. SETTLED 2026-09-18** (Albin: "Parsing should be
  ALWAYS about the entire night, not the a checkpoint file", then "yes go"): one
  parse per night, in place when the folder resolves to one group, with the
  groups isolated only when a folder genuinely splits (2.3). This also closes
  the ez Share double-store that the measurement turned up.
- **D6, Fysetc.** Proposed: leave it where it is. It already implements
  `IDataSource`, so it joins the shared path for free, but its own staging rules
  (raw sectors) deserve their own reading before anything is deleted.

## 4. Tests

- `LocalDataSource` against a real card copy: folders, files with sizes, a byte
  range, a root file, and a missing folder.
- The shared cycle with each source: the same card, imported through
  `LocalDataSource` and through a fake ez Share, produces the same sessions,
  the same session ends and the same ledger rows.
- The no-op rule: after a local import the archive directory is untouched (no
  DATALOG written, no STR copied), the user's folder is unchanged, and no
  residual walk ran (the counters stay at zero and nothing is re-read).
- Every SDD-037 and SDD-038 test still passes unchanged, since the behaviour
  they pin is the behaviour that must survive the refactor.
- The full e2e in all four environments, as SDD-037 ran: macOS, Linux, Windows,
  Docker.

## 5. Release

Albin's number, after SDD-038 and SDD-039. This is a refactor of the most
load-bearing function in the app; it ships on its own so a regression has one
obvious suspect.
