# SDD-028: the ring's files next to the card

**Status:** Accepted 2026-09-13. Scope: both ways in; D1 root plus every
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
