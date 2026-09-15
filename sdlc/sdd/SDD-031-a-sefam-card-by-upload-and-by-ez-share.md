# SDD-031: a Sefam card by upload and by ez Share

**Status:** Accepted 2026-09-15. D1 Löwenstein upload included; D2 the
`uploads/` store; D3 an upload walks the whole history and fills what is
missing; D4 ez Share copies the whole card on its first burst and, once there
is history, looks only at the last two nights. Built and verified end to end
2026-09-15 (§5). Released in 5.2.9 (tag v5.2.9, 2026-09-15), installed on the
Pi.
**Date:** 2026-09-15
**Repo:** `hms-cpap` (the upload importer, the collector's ez Share path, the
Settings page). No parser change.
**Issue:** hms-homelab/hms-cpap#28 (kings8615 / sktxts87-prog, Sefam S.Box)
**Related:** SDD-010 (the card root), SDD-014 (the card mirror), SDD-002 (the
full-card listing on ez Share), SDD-028/029 (the same card-folder rules).

## Trigger

#28, 2026-09-15: "whenever I've tried it doesn't recognise the format". Albin:
the local-folder path is covered; two paths are missing: uploading a zip from
the UI, and an ez Share card reading the S.Box's SD slot directly.

## 1. What exists today (verified 2026-09-15)

- **Local folder** works for Sefam: transport `local`, format `sefam`,
  `local_dir` at the card; `SefamIngestion` walks for session manifests
  (`<model>/<serial>/DATA_<n>/DATA_<n>.INI` on the reporter's 1263R, or
  `<YYMMDD>/<HHMMSS>.ini` on a SleepBox) and the burst parses sessions newer
  than the last stored one.
- **The zip upload is ResMed-only.** `cpap_zip_import_` (main.cpp) extracts
  the zip, mirrors `DATALOG/YYYYMMDD` folders into the ResMed archive and runs
  the ResMed backfill. A Sefam card has no DATALOG, so it answers "No DATALOG
  date folders (YYYYMMDD) found in zip" — the reporter's error. A Löwenstein
  card zip gets the same answer.
- **ez Share is ResMed-only.** With transport `ezshare` the format is ignored:
  the collector builds the ez Share client and the ResMed DATALOG discovery.
  Settings shows the format choice only for the local folder.
- **The ez Share client can already read any folder**: `listDir(path)` returns
  files and folders with their size and card timestamp, `downloadByPath` fetches
  one file (both from SDD-002's full-card sweep).
- **Real cards to test on**: the reporter's S.Box dump
  (`~/cool_shit/data/sefam_samples`, 1263R, 276 manifests) and Löwenstein
  samples (`~/cool_shit/data/prisma-samples`).

## 2. Design

### 2.1 The upload reads the card it was given

After extraction the importer asks what the card is, by its files (the
parser's `detectManufacturer`, cross-checked by `SefamIngestion::initialize()`
finding manifests), not by the install's configured format, so a ResMed user
and an S.Box user use the same page.

- **ResMed**: unchanged.
- **Sefam**: the card tree is copied into the upload store (D2); every session
  in it not already stored is parsed and saved, through the same code the
  burst's Sefam loop uses (moved into one function the two call). Not only
  sessions newer than the last stored one (D3): an upload is how history
  arrives. The reply says what landed: sessions found, imported, already
  stored, refused, and the nights they cover.
- **Löwenstein** (D1): the same, through `PrismaIngestion`, if included.

### 2.2 ez Share reads a Sefam card

- Settings offers the format for ez Share too (ResMed or Sefam S.Box); the
  config pair is transport `ezshare`, format `sefam`.
- Each burst mirrors the card's Sefam tree from the ez Share into the archive
  (`archive_dir`, required, as for the Mule and Miner): list from the root down
  to the manifests' depth, fetch a file that is new or whose size or card
  timestamp changed (the card files' rule), then run the existing Sefam
  ingestion on the archive. The ResMed discovery is not used.
- Löwenstein over ez Share is not offered: a Prisma answers the ez Share with
  error 601 (known from the Mule and Miner compatibility list).

### 2.3 What does not change

The local-folder Sefam path; the ResMed paths; the parser. The S.Box night is
parsed once it is on disk, as the local path does today (no live growth).

## 3. Decisions (Albin's)

- **D1 Löwenstein upload.** Proposed: include it. The upload refuses a Prisma
  zip for the same reason and the switch in §2.1 is the same one. Löwenstein
  over ez Share stays out (error 601).
- **D2 where an uploaded Sefam/Löwenstein card is kept.** Proposed: a store
  under the data directory, `<data_dir>/uploads/<format>/`, merged across
  uploads, never the user's configured `local_dir` (their card or share, which
  the service should not write into). Kept so a Reparse can re-read it.
- **D3 older nights in an upload.** Proposed: every session not already stored,
  not only those newer than the last.
- **D4 ez Share Sefam on the first burst.** Proposed: mirror the whole card
  (history included), then the normal delta; the S.Box card is small (the
  reporter's 276 sessions are ~40 MB).

**Answered 2026-09-15 (Albin):** D1 yes, include Löwenstein. D2 yes, the
`uploads/` folder. D3 yes, "it should walk the historic and refill it", except
the ez Share path: there it "should only look for the last 2 nights IF
there's already a historic", and "on first burst yes copy the whole Sefam
card". So the ez Share mirror copies everything while the database holds no
session for the device, and afterwards lists only the session folders of the
card's two most recent nights.

## 4. Tests

- Upload: the reporter's card zipped is recognised as Sefam and imports its
  sessions; a second upload of the same zip imports nothing new; a ResMed zip
  behaves as today; a zip of neither is refused by name. (Löwenstein, if D1.)
- ez Share: a fake data source serving the reporter's tree mirrors it into the
  archive, the Sefam ingestion parses it, a second burst fetches nothing, a file
  that grows is fetched again.
- End to end: a throwaway instance importing the reporter's card by upload,
  and another reading it through a local stand-in for the ez Share HTTP API.

## 5. As built (2026-09-15)

### Code

- `CardUpload.{h,cpp}`: `classifyUploadedCard` reads the card by its files. It
  looks for a Sefam session folder (DATA_<n> or a YYMMDD day holding an .ini)
  that `SefamIngestion` then accepts, then a Löwenstein `.pdat`/`.wmedf`, then
  the parser's ResMed check. The Sefam and Löwenstein walks are its own and go
  four levels deep. The parser's `detectManufacturer` stops at three levels for
  Sefam, one short of the reporter's zip, whose card sits under a
  `cpap files/` wrapper. It checks `.wmedf` only at the top level.
- The upload store (D2): a Sefam card is merged into
  `<data_dir>/uploads/sefam/`. Each Löwenstein `.pdat` is opened into
  `<data_dir>/uploads/lowenstein/<file stem>/` and never kept as a `.pdat`,
  because `PrismaIngestion` would open a root `therapy.pdat` into the one temp
  cache it shares with a Löwenstein install's own card, and the browser's
  `therapy(1).pdat` name is not the one it looks for. A `.pcfg` beside it opens
  into the same folder.
- `importCardSessions` imports every session on the card the database does not
  hold (D3). Removed nights (SDD-029) stay removed. Then the daily summary is
  re-derived from the sessions. It runs on the backfill worker
  (`BackfillService::triggerCardImport`). The upload reply is `queued` with the
  format and the card's nights, and the counts come through
  `/api/backfill/status`, which the upload page already polls, and one log
  line. There is no MQTT or AI summary for uploaded history.
- Not as §2.1 proposed: the upload has its own parse-and-save loop and does
  not share one with the burst's Sefam loop. The burst loop also publishes and
  summarises, which the upload must not do.
- `SefamCardMirror.{h,cpp}` (D4): the whole card while the device has no
  stored session, then only the session folders of the card's two most recent
  nights, by the folders' card stamps shifted back 12 h. Without stamps it
  takes the four highest session numbers. A file is fetched when it is new, or
  when its size or card stamp differs from the `.ezshare_sefam_manifest` in
  the archive. The fetch goes to `.part` and is then renamed. An empty root
  listing is an error, never "nothing new".
- The collector: transport `ezshare` + format `sefam` is source
  `sefam_ezshare` (`AppConfig::collectorSource`). It needs `archive_dir`, and
  hot-reloads like the other sources. Settings shows the format choice for
  ez Share.
- A fix found on the way: `SefamIngestion` walked the folder only on the
  first burst, so a night added after startup was never seen by the local
  Sefam path either. It now re-walks every burst (`rescan()`).

### Tests

- `test_CardUpload.cpp` (12), `test_SefamCardMirror.cpp` (8, a fake card data
  source), a rescan test in `test_SefamIngestion.cpp`, and `CollectorSource`.
- Full suite: 1899 tests, 1637 passed, 262 skipped (card fixtures and the
  PG/MySQL connections), 0 failed, in local time and under TZ=UTC.

### End to end (throwaway instances, the samples in §1)

- **Sefam upload**, the reporter's zip as sent: recognised as `sefam`. 242
  sessions on 173 nights, 241 imported, 1 refused (`DATA_241`, every channel a
  stub, which the parser declines). 173 daily rows. A second upload of the
  same zip: nothing new.
- **Löwenstein upload** (`therapy(1).pdat` zipped), on a fresh database: 20
  sessions on 18 nights, 20 imported. Identical, session for session (start
  and duration), to the same card read through the local-folder Löwenstein
  path.
- **ez Share**, a local HTTP stand-in serving the reporter's card in the
  ez Share listing format, with the newest night (`DATA_240`, `DATA_241`)
  held back:
  - Burst 1: whole card, 243 folders listed, 3362 files fetched, the archive
    byte-identical to the card, 240 sessions imported.
  - Burst 2, after the held-back night was put on the card: last two nights.
    6 folders listed; 28 files fetched (the two new folders only); 16
    unchanged (`DATA_239` and the two loose serial files). 1 imported,
    `DATA_241` refused as in the upload.
  - Burst 3, card unchanged: 0 fetched, 44 unchanged.
  - 241 sessions on 173 nights, the same as the upload.
- Known, unchanged by this SDD: a refused session newer than the last stored
  one is re-parsed and refused again each burst (on the S.Box path, local
  folder or ez Share). It is cheap (~100 ms).
- Known, unchanged by this SDD: the upload route is wired only when the
  install has a `local_dir` or an `archive_dir`, and answers 503 otherwise,
  for every format. Every Sefam or Löwenstein setup has one of them: the local
  folder, or the archive that ez Share Sefam requires.

## 6. Release

A patch (Albin's number). The reply on #28 is Albin's.
