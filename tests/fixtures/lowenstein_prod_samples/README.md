# Lowenstein prod samples

Real (read-only) export from the CpapDash production DB (`vps-hms`, database
`cpapdash`), devices 31 (`UPLOAD-51`) and 43 (`UPLOAD-75`) — both
`manufacturer=lowenstein` file-upload devices. No PII: `owner_email` is empty
on both rows, serials are synthetic upload IDs, not real device serials.

Pulled 2026-07-23 while investigating hms-cpap issues #14 (cpap_daily_summary
never populated for Lowenstein) and #15 (pressure/SpO2/HR not aggregated into
session_metrics). Kept around for:

1. Seeding a local sqlite scratch DB to sanity-check
   `IDatabase::aggregateDailySummaryFromSessions` (issue #14 fix) against
   real numbers instead of only synthetic gtest fixtures.
2. The upcoming WMEDF signal-aggregation investigation (issue #15) --
   cpapdash-api's own `session_metrics.avg_mask_pressure` /
   `avg_epr_pressure` / `avg_spo2` are NULL for every row here even though
   `avg_pressure` (a different, already-wired-up field) is populated, so
   these rows double as a real-world repro for #15 too.

`sessions_export.csv`: one row per calendar day (cpapdash-api uploads are
batch, not live per mask-on/off like hms-cpap's own collector), columns:

```
device_id, date, session_start, session_end, therapy_hours, sessions_ahi,
pressure_avg, pressure_95, leak_95, obstructive_apneas, central_apneas,
hypopneas, reras, clear_airway_apneas, avg_pressure, pressure_p50,
pressure_p95, leak_p50, leak_p95, avg_spo2, avg_mask_pressure,
avg_epr_pressure, therapy_mode
```

`sessions_ahi` / `pressure_avg` / `pressure_95` / `leak_95` are cpapdash-api's
own already-computed per-day values (from its `sessions` table) -- useful as
ground truth to sanity-check any re-derived aggregation against.
