# Web Upload — CPAP zip + O2 Ring CSV (4.4.5)

hms-cpap serves a web upload page so data can be brought in by hand, without the
SD-card sync loop. It mirrors the cpapdash-api upload UX (two drop zones) but
stays on the trusted LAN (no auth).

Page: `frontend/.../pages/upload/` → nav "Upload" → route `/upload`.

## Endpoints (no auth — trusted LAN)

| Endpoint | Method | Body | Behaviour |
|----------|--------|------|-----------|
| `/api/upload/cpap` | POST | multipart `file` (.zip) | Extract → reparse (async) |
| `/api/upload/oximetry` | POST | multipart `file` (.csv) | Parse → store (sync) |

Both raise nothing on their own; they only run when a file is posted. The Drogon
client max body size is set to 512 MB (`main.cpp`) so multi-MB uploads aren't
rejected with `413`.

### CPAP zip — `/api/upload/cpap`

Wired only when an archive (`config.local_dir`) is configured. The handler:

1. Writes the upload to a temp file, then `PrismaIngestion::extractZip` into a
   staging dir.
2. **Mirrors the card** into the archive (`mirrorCardInto`, `utils/CardImport.h`):
   eight-digit date folders land under `DATALOG/`, and everything else keeps its
   path relative to the card root, so uploads are permanent and the archive looks
   like the card did. The card root is found inside the extraction, so a zip that
   wraps its contents in a folder works the same as one that does not.
3. Triggers `BackfillService::trigger(minDate, maxDate, "")` to reparse those
   nights from the archive — **async**. The page polls `/api/backfill/status`.
4. Returns `{status: "queued", sessions_found, dates[], files_copied,
   files_skipped, str_found}`.

**Upload the card ROOT**, the folder holding both `STR.edf` and `DATALOG`. That
is the layout SDD-010 pins and the one this handler expects.

`STR.edf` is no longer out of scope: if it is in the zip it is archived at the
card root and backfill picks it up, so the daily summary comes from the machine's
own record instead of being derived from sessions. It used to be dropped silently
while the endpoint still answered `queued`, which is issue #23 — a night then
reported AHI 0.0 against an STR that said otherwise. `hms_cpap --backfill
<STR.edf>` still works and is no longer the only way, which matters because a
web-upload user has no shell.

The only filter is `residualSkip` (`utils/CardResidue.h`): OS junk, media, office
documents, anything over 20 MB, and `ezshare.cfg`, which can hold WiFi
credentials. Whole-card containers (`.pdat`, `.pcfg`) are exempt from the size
cap — a Löwenstein card is one compressed blob, and capping it would drop the
only file there is. Real card files nothing would think to allowlist
(`Identification.tgt`, `*.crc`, `Journal.dat`) are kept.

### O2 Ring CSV — `/api/upload/oximetry`

`O2RingCsvParser::parse(content, filename)` → `saveOximetrySession("o2ring", …)`,
synchronous. Returns `{samples, valid_samples, avg_spo2, min_spo2,
sample_interval, duration_seconds}`.

`O2RingCsvParser` (`src/services/O2RingCsvParser.cpp`) yields the shared
`cpapdash::parser::OximetrySession` and reuses `VLDParser::calculateMetrics`, so
the upload path produces the same metrics/storage as the BLE/VLD path. Notes:

- Timestamp dialects: 24-hour `06:53:07 Apr 12 2026` **and** the O2 Ring S
  12-hour, quoted, comma-after-day form `"11:20:29PM Jun 19, 2026"`.
- Sample interval is **auto-detected** (smallest positive gap) — per-second
  exports aren't duration-inflated.
- Sentinels (SpO₂ 255 / HR 65535) → `0xFF` + invalid (also keeps the 16-bit HR
  sentinel from overflowing the `uint8_t` field).

## Wiring

Controller methods `uploadCpapZip` / `uploadOximetryCsv` (`CpapController`) read
the multipart file and delegate to static callbacks set in `main.cpp`
(`cpap_zip_import_` / `oxi_csv_import_`) where the DB + archive + backfill service
are in scope — the same static-callback pattern used by `backfill_trigger_`.

## Validated

Against real data in `/mnt/public/cpap_data` on a throwaway instance: a 2-night
zip → 3 sessions parsed (0 errors), and a Wellue CSV → 5462 samples (interval
auto-detected). Unit: `tests/services/test_O2RingCsvParser.cpp` (5 tests).
