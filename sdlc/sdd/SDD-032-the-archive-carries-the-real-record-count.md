# SDD-032: the archive carries the real record count

**Status:** Released 2026-09-15: parser v2026.8.3, hms-cpap v5.2.11. On the
Pi the first start's sweep checked 1471 signal files and repaired 302, which
left none of its archive's files with data carrying a stale count. Accepted
2026-09-15: D1 the repair moves into the public parser; D2 a sweep of the
existing archive at startup.
**Date:** 2026-09-15
**Repo:** `hms-cpap` (the archive writers, the startup sweep), with
`hms-cpapdash-parser` (the repair itself).
**Ticket:** CpapDash support #127 (Michael Kearney, M&M bridge, OSCAR)
**Related:** the monolith's OSCAR empty-card fix (2026-07-16, same customer),
`cpapdash-ingest-lib` `CardArchive.h` (`repairEdfDataRecords`), SDD-002 (the
OSCAR layout), SDD-011 (the archive is the files' only consumer-facing copy).

## Trigger

Ticket 127, 2026-09-15: OSCAR imports a truncated flow graph from
hms-cpap's archive (`HMS_Data`), a minute or two at each end of the night,
while the same night from an independent ez Share client imports whole.
Compared file for file, every `BRP`/`PLD`/`SAD.edf` has the same size and a
different hash; `CSL`/`EVE.edf` are identical. A file that matched in the
morning no longer matched after the night's next checkpoint appeared.

## 1. What is wrong

- **ResMed writes the EDF record count late.** `num-data-records` (header
  offset 236, 8 ASCII bytes) reads `-1`, or a count lower than the data,
  while a signal file is recording; the true count is written when the file
  is finalized. OSCAR trusts the field. The parser and SleepHQ compute the
  count from the file size, which is why nothing else noticed.
- **hms-cpap never fetches the finalized header.** `downloadSessionFiles`
  resumes BRP/PLD/SAD with a Range request from the size already on disk
  (`BurstCollectorService.cpp:447-481`). The header is fetched once, while the
  file records, and the finalize rewrite, inside bytes already held, is never
  read again. CSL/EVE are fetched whole on a card-stamp change, which is why
  they match.
- **A size-only archive copy.** `archiveSessionFiles` skips a file whose size
  equals the archived copy's (`:621-631`), so even a correct refetch would
  never reach the archive: finalizing changes bytes, not size.
- **The Fysetc transport takes the same path.** It is a data source
  (`FysetcDataSource`) behind the same discovery, `downloadSessionFiles` and
  archive step, so the fix below covers it. (`FysetcSectorCollectorService`,
  which also resumes by the archived size, is not constructed anywhere.)
- The ticket's hypothesis, a regroup that rewrites and corrupts the files, is
  not what happens: the other client re-pulled the finalized file, hms-cpap
  did not.
- **On disk.** The Pi's archive (Albin's own card) holds 302 of 759 signal
  files with data whose count is stale (`-1`, or `187` for 245 records).
- The monolith fixed this on 2026-07-16 for its OSCAR zip, and ingest does the
  same for the SleepHQ archive (`cpapdash-ingest-lib`, private).

## 2. Design

### 2.1 The repair (D1: in the parser)

`cpapdash::parser` gains `EdfRecordCount.h`, the ingest library's two
functions, the same rules:

- `isResmedSignalEdf(name)`: `*_BRP/_PLD/_SAD.edf`, any case. EVE/CSL
  (annotations) and STR are not touched.
- `repairEdfDataRecords(buf, n)`: recompute the count as
  `(size - header_bytes) / record_bytes`, `record_bytes` the sum of the
  samples per record times 2, and overwrite offset 236 only when the data
  divides evenly and the stored value differs. Any doubt in the header leaves
  the bytes alone.
- One addition for a caller holding only the header:
  `repairEdfDataRecords(header, header_len, file_size)`. It reads nothing past
  `header_len`, so hms-cpap repairs a file by reading its header, not the
  whole night's waveform.

ResMed writes the finalized count left-justified and space-padded (`47      `
on a real card), which is what the repair writes: a repaired file is the
card's finalized file, byte for byte, when only the count changed.

The ingest library keeps its copy until it moves onto this one; that is a VPS
change, not part of this SDD.

### 2.2 hms-cpap: every archived signal file is repaired

The archive (`archive_dir`) is what OSCAR reads and what the SleepHQ export
uploads, so the repair is applied to the archived copy, in place: 8 bytes
rewritten, the file's size unchanged. The download copy used for Range
resumes and parsing is left as it is.

- **The burst's archive step** (ez Share and Fysetc): after
  `archiveSessionFiles` copies (or skips) the night's files, the archived
  signal files of that night are repaired, same size or not.
- **The zip upload**: each signal file `mirrorCardInto` copies.
- **D2, the sweep**: once, before the first burst, every `DATALOG/<date>/`
  signal file under the archive the burst writes (`archive_dir`, or its
  default `<data_dir>/cpap_data`), so a history already on disk (ticket 127's
  `HMS_Data`, the Pi's 302) is repaired without downloading anything. One log
  line: files checked, repaired. Not in local mode, whose card is the user's.

The repair computes the count from the size, so it does not wait for the
machine to finalize: a file whose data ends on a whole record is right after
any write, and a later append is repaired again by the next pass.

Never touched: the user's own `local_dir` (a card or share the service does
not write into), non-ResMed archives (names do not match), and a file whose
data does not divide evenly.

The SleepHQ export compares file sizes (`scanFolder`), so repairing in place
does not queue a re-export. OSCAR keeps a night it already imported; ticket
127's user re-imports once (purge that night in OSCAR) to see the whole flow.

## 3. Decisions (Albin's, 2026-09-15)

- **D1** "move to parser": one public copy, the repair in
  `hms-cpapdash-parser`; hms-cpap pins the release.
- **D2** "yes to the sweep": the existing archive is repaired at startup.
- "bump the patch": parser 2026.8.3, hms-cpap 5.2.11.

## 4. Tests

- Parser: `-1` repaired to the record count; a too-low count repaired; a
  correct count left alone (returns false); data that does not divide evenly
  left alone; a header too short, a bad `ns` or a bad header size left alone;
  the header-only form reads nothing past its buffer and agrees with the
  whole-buffer form; the name rule.
- hms-cpap: the archive step repairs a same-size signal file and leaves
  EVE/CSL alone; the sweep repairs across date folders, reports its counts, and
  leaves STR and non-signal files untouched; the zip mirror repairs what it
  copied; a second sweep repairs nothing.
- End to end: a throwaway instance behind a local ez Share stand-in that
  serves Range: a signal file first pulled mid-recording (header `-1`), then
  grown and finalized on the card; the archived copy ends byte-identical to
  the card's. And a startup sweep over a copy of the Pi archive's stale files.

## 5. As built (2026-09-15)

- Parser (`hms-cpapdash-parser` c4f2bb5, released as v2026.8.3):
  `EdfRecordCount.{h,cpp}` as §2.1, 7 tests; its suite 235 passed, 0
  failed, local zone and TZ=UTC.
- hms-cpap: `utils/ArchiveRecordCount.{h,cpp}` (`repairSignalEdfFile` reads
  the header only and writes back 8 bytes; `repairSignalEdfsIn`;
  `sweepArchiveSignalEdfs`), called from `archiveSessionFiles`,
  `mirrorCardInto` and once at the top of the worker thread. Tests in
  `tests/utils/test_ArchiveRecordCount.cpp`. Full suite 1645 passed, 0 failed,
  local zone and TZ=UTC.
- End to end, a throwaway instance behind a local ez Share stand-in that
  answers Range. The card was a real night (Albin's card backup), moved to
  this afternoon so it reads as recording now. Its BRP first held 30 whole
  records with the count `-1`.
  - Startup: the sweep repaired the 6 stale signal files of two April nights
    copied from the Pi's archive. On the next start it found 6 and repaired
    none.
  - Burst 1 archived the BRP with the count repaired to 30.
  - The card's BRP was then finalized (39 records, the true count). Burst 2
    resumed it with a Range request (+54018 bytes from byte 181084). The
    download copy kept `-1`, as in ticket 127. The archived copy was repaired
    to 39, and all 8 files of the night are byte-identical to the card's
    finalized ones.

## 6. Release

Parser 2026.8.3 first, hms-cpap 5.2.11 pinned to it, tagged once validated.
The reply on ticket 127 is Albin's.
