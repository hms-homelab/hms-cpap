# SDD-028: the ring's files next to the card

**Status:** Released in 5.2.6 (2026-09-13); amended in 5.2.7 (§6, 2026-09-14). Accepted 2026-09-13. Scope: both ways in; D1 root plus every
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
