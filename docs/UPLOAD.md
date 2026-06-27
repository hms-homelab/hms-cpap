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
2. Finds every `YYYYMMDD` folder in the extraction and **merges** its files into
   the DATALOG archive (`config.local_dir/<date>/`), so uploads are permanent.
3. Triggers `BackfillService::trigger(minDate, maxDate, "")` to reparse those
   nights from the archive — **async**. The page polls `/api/backfill/status`.
4. Returns `{status: "queued", sessions_found, dates[]}`.

STR.edf-level daily summaries are out of scope here (sessions only); use
`hms_cpap --backfill <STR.edf>` for those.

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
