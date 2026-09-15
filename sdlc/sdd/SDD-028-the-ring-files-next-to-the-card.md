# SDD-028: the ring's files next to the card

**Status:** Released in 5.2.6 (2026-09-13); amended in 5.2.7 (§6, 2026-09-14);
D1 amended to one level deeper, and a ring file without the extension read
by its header (§7, released in 5.2.10, 2026-09-15, installed on the Pi). Accepted 2026-09-13. Scope: both ways in; D1 root plus every
top-level folder; D2 the ring's clock stored as-is.
**Date:** 2026-09-13
**Repo:** `hms-cpap`. One shared importer, the local-mode burst, the O2 upload
route and its page.
**Issue:** hms-homelab/hms-cpap#32 (todd3835)
**Related:** SDD-010 (the local folder is the card ROOT), the live ring path
(`OximetryService`), the Wellue CSV import (#17, v4.8.2)

## Trigger

#32, todd3835, 2026-09-13:

> I'm using another tool to pull my o2 readings in. I'm wondering if there's
> support to read the VLD files from the same directory as my CPAP readings.
> Currently the "Oxymetry" folder is in the same spot at my DATALOG folder.

Albin, 2026-09-13: "yes to the vld", both ways: scan the folder, and take a
`.vld` on the upload.

## 1. What exists today

- **The ring's own format is already parsed.** `OximetryService` pulls `.vld`
  files from the ring over the O2 Ring mule, parses them with the shared
  `cpapdash::parser::VLDParser` and stores them with
  `saveOximetrySession(kOximetryDeviceId, …)`
  (`src/services/OximetryService.cpp:53-70`).
- **A file is known by its name.** `oximetry_sessions.filename` is UNIQUE and
  the save is `ON CONFLICT (filename) DO UPDATE`; the live path skips a name
  already stored (`oximetrySessionExists`). The same night arriving twice is one
  row.
- **The upload takes only the Wellue CSV.** `POST /api/upload/oximetry` →
  `oxi_csv_import_` → `readO2RingCsv` (`src/main.cpp:1019-1040`); the page's
  picker is `accept=".csv"` (`upload.component.html:41`). A `.vld` fails with
  "No readable rows in CSV".
- **Local mode never looks outside `DATALOG/`.** The burst's local branch
  (`BurstCollectorService.cpp:1500`) reads `DATALOG/` and the root's `STR.edf`;
  a folder of ring files beside `DATALOG/` is invisible.

## 2. Design

### 2.1 One importer

`importVldFile(db, bytes, filename) → {ok, skipped, samples, avg_spo2, error}`
in `src/services/OximetryImport.{h,cpp}`: parse with `VLDParser::parse`, refuse
an unparseable file by name, save under `kOximetryDeviceId` exactly as the live
path does. The live path, the folder scan and the upload then store the same
row for the same file.

### 2.2 The folder beside `DATALOG`

In local mode, every burst, after the STR step and before the "no new sessions"
return (so it runs even on a night with no new CPAP file):

- look for `*.vld` (case-insensitive) in the card root and in each folder
  directly under it except `DATALOG` and `SETTINGS` (D1);
- skip a name already stored (`oximetrySessionExists`), so a steady-state burst
  costs one directory listing and one lookup per file;
- import the rest through 2.1, logging one line per file and a count.

A file that fails to parse is logged by name and skipped, never retried in a
loop: its name is remembered for the process's life, as the live path's
`processed_files_` does.

ezShare mode (the card over WiFi) is out of scope: the ring's files are not on
the CPAP's card.

### 2.3 The upload

`POST /api/upload/oximetry` dispatches on the extension: `.vld` →
2.1, anything else → the CSV importer as today. The page's picker takes
`.csv,.vld` and says so; the result line is the same (samples, average SpO2).
Re-uploading a file already stored updates it in place (the UNIQUE filename).

## 3. Decisions (Albin's)

- **D1, which folders.** Proposed: the root and every folder directly under it
  except `DATALOG` and `SETTINGS`, one level, so any tool's folder name works
  (todd3835 wrote "Oxymetry"; others use "Oximetry" or a tool's own name).
  `.vld` is specific to Viatom/Wellue, so a false match is unlikely. The
  alternative is only folders named `Oximetry`/`Oxymetry`, case-insensitive.
- **D2, the clock.** The ring keeps the phone's clock, which changes for DST,
  while a ResMed's does not. The live and CSV paths already store the ring's
  clock as-is; this keeps that, and the offset question stays where SDD-110
  (hms-cpapdash-api) is taking it. Proposed: unchanged here.

## 4. Tests

- Importer: a synthetic VLD v3 (as `test_OximetryService.cpp:170` builds one)
  saves one session under `kOximetryDeviceId` with its samples; garbage bytes
  are refused by name and nothing is saved; the same file twice is one row.
- Folder scan (a temp card root): a `.vld` in `Oxymetry/`, one in `Oximetry/`
  and one at the root are imported; one inside `DATALOG/` and one in `SETTINGS/`
  are not; a second burst imports nothing; an unparseable file is skipped once.
- Upload: a `.vld` multipart imports through the importer; a `.csv` still takes
  the CSV path.
- The three database backends through the existing save (no new SQL).

## 5. Release

A patch on 5.2.5 (Albin's number), the normal tag flow. The reply on #32 is
Albin's.

## 6. Amendment, 2026-09-14 (5.2.7): a file that changes, and a scan that says what it saw

**Trigger.** On 5.2.6 todd3835 (PostgreSQL, local card root `/CPAP`) reported
that the upload imports his `.vld` but the `Oxymetry/` folder does not. His
burst log shows the scan runs (it sits between the STR save and "Found 2
session(s) to process") and logs nothing: the 5.2.6 scan printed a line only
when it imported or refused something, so "no files where it looked" and
"files already stored" looked the same. The cause is not yet known; his folder
listing is asked for. Two defects of §2.2 surfaced either way.

**2.4 A file is known by its name AND its size and modified time.** 5.2.6
skipped any filename already stored. A pass that caught the file while the
other tool was still writing it stored a short night (reproduced: a 640-byte
file cut at 340 bytes stored 60 of its 120 samples) and never read it again.
The scan now remembers, per path, the size and modified time the file had
when it was stored, the card files' own rule. A stored file whose signature
changed is stored again (same row, samples replaced; checked on SQLite and
PostgreSQL). The memory is per process: after a restart a file already stored
is trusted as it is, so a file stored short and completed exactly across a
restart stays short. Persisting the signature would need a column on
`oximetry_sessions` on three engines; not done for that window.

**2.5 A file that will not parse is retried when it changes.** 5.2.6 kept it
refused until a restart. It is now refused at the signature it had, and read
again once that changes.

**2.6 The scan says what it saw.** One line: the card root, how many `.vld`
files, where it looked (the root and each folder it searched), that DATALOG
and SETTINGS are not searched, which searched folders hold sub-folders it does
not descend into, and how many files are unreadable. Logged on the first pass
and again only when it changes, so a steady card costs no log lines:

```
O2Ring: card folder /CPAP: no .vld files in the root, Oxymetry/ (DATALOG and
SETTINGS are not searched); Oxymetry/ holds 1 folder(s), which are not searched
```

Imports and updates keep their own lines (`O2Ring: imported …`,
`O2Ring: updated … (it changed since it was stored, …)`).

Not changed: the depth (one level below the root, D1). If todd's listing shows
his tool nests files per ring or per month, that is a separate decision.

**Tests.** `test_OximetryImport.cpp`: a stored file that changes is stored
again and then left alone; an unreadable file is read again when it changes;
a file stored before this run is trusted; nothing found names the folders it
looked in and the nested one it did not; the summary is logged once. A
PostgreSQL case (runs when `PGHOST` is set) checks the re-read replaces the
samples. End to end on a throwaway instance: a nested folder only, then a
half-written file (60 samples), then the whole file (updated, 120 samples),
then two quiet bursts with no log lines.

## 7. Amendment, 2026-09-15: one level deeper, always

**Trigger.** On 5.2.9 todd3835 still gets no automatic import. His card
listing (#32, 2026-09-15) shows `/CPAP/OXYMETRY/20260913/` and
`/CPAP/OXYMETRY/20260914/`: one folder per night inside `OXYMETRY`, the way
DATALOG is laid out. The files are one level below where D1 looks, which is
the case §2.6's line names ("OXYMETRY/ holds 2 folder(s), which are not
searched"). The layout may be his own, but a tool that files each night in its
own folder is a reasonable thing to support.

**D1 amended (Albin, 2026-09-15: "always on is fine by me").** The scan
reads the root, each folder directly under it except `DATALOG` and
`SETTINGS`, and each folder inside those. It does not go deeper. There is no
setting: the files there are either ring files the user wants, or there are
none, and then the extra depth costs only the listings. The rest of §2.2 and
§6 is unchanged: the size and modified time rule, the retry of an unreadable
file when it changes, and a name already stored is the same night.

**Cost.** One more directory listing per folder in a searched folder, per
burst. A tool that adds a folder every night adds one listing per night;
after a year that is about 365 small listings every burst, on todd's SMB share
perhaps a second or two. The stat per file was already paid for a flat folder
of the same files.

**The summary line (§2.6)** names a searched folder with its sub-folders as
`OXYMETRY/ and its 2 folder(s)`. Folders a further level down are counted per
top folder, not listed one by one, so a year of nights stays one short line:

```
O2Ring: card folder /CPAP: 2 .vld file(s) in the root, OXYMETRY/ and its 2
folder(s) (DATALOG and SETTINGS are not searched)
```

**A ring file without the `.vld` extension (Albin, 2026-09-15: "yes lets do
it").** Not every export keeps the extension: the one real Wellue export we
have had (Lee Hardin's ring, 2026-08-18, the file the parser's header layout
was recovered from) was a bare binary. So in the folders the scan searches, a
file with no extension at all is read for its first 13 bytes. It is a ring
file when:
- the `u16` at offset 0 is 3 (the version, which the parser already requires);
- offsets 2-8 are a real date and time (year 2000-2099, month 1-12, day 1-31,
  hour, minute and second in range);
- the `u32` at offset 9 equals the file's size on disk (the real layout's own
  size field).

A random file matching all three is not a practical risk. A match goes
through the same importer as a `.vld`: the same parser, the same row by
filename, and the same re-read on change. A file that does not match is
remembered by its size and modified time, as an unreadable file is, and its
header is read again only when it changes. A file with any other extension is
not looked at. The summary names the extensionless ones: `3 .vld file(s) (1
without the extension) in ...`.

Only a file shaped like that export is recognised. A different Viatom export
would not match; it is left alone, never imported as something it is not.

**Tests.** `test_OximetryImport.cpp`: todd's layout
(`OXYMETRY/20260913/*.vld`, `OXYMETRY/20260914/*.vld`) is imported, and the
second pass imports nothing; a file three levels down is not read, and the
summary counts its folder as not searched; `DATALOG` and `SETTINGS` stay out.
An extensionless ring file is imported under its bare name. An extensionless
file whose offset 9 is not its size, and a text file with no extension, are
not, and their headers are not read again until they change. The header check
on its own covers the version, each date field and the size. The test helper
writes the real layout: the size at offset 9, the duration at 13, the
interval at 22. End to end on a throwaway instance with a card root laid out
like todd's.

**As built, 2026-09-15.** `OximetryImport.{h,cpp}` as above. An extensionless
ring file still being written fails the size check (offset 9 holds its final
size), so it is imported once complete, never short. One already stored and
unchanged is not re-read. Full suite 1640 passed, 0 failed, local zone and
TZ=UTC.

End to end: a card root laid out like todd's. It held STR.edf and a night from
a real card, `System Volume Information/` (with an extensionless
`IndexerVolumeGuid`) and `LOST.DIR/`. The ring files were synthetic VLD v3 in
the real layout, since no real `.vld` is on hand.
- Depth, run 1. Burst 1 imported `OXYMETRY/20260913/…vld` and
  `OXYMETRY/20260914/…vld` with every sample (120 and 150). A folder
  `OXYMETRY/20260915/` added later was imported on the next burst (90), and
  the burst after that logged nothing.
- Extensionless, run 2. `OXYMETRY/20260914/20260914224500` started half
  written (400 of 790 bytes), and burst 1 left it alone. Once it was complete
  it was imported under its bare name with all 150 samples. The Windows
  `IndexerVolumeGuid` was never taken for one. The following burst logged
  nothing.

The line it logged:

```
O2Ring: card folder <card>: 2 .vld file(s) (1 without the extension) in the
root, LOST.DIR/, OXYMETRY/ and its 2 folder(s), System Volume Information/
(DATALOG and SETTINGS are not searched)
```
