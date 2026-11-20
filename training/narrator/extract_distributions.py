#!/usr/bin/env python3
"""SDD-013: the input ranges of the narrator corpus, from one install's own nights.

Reads an hms-cpap PostgreSQL database READ-ONLY and writes aggregate
distributions only: per-field quantiles, histograms, how often each figure is
present, and the ratios that tie figures together (event mix, max over avg,
mask over therapy pressure, ODI over AHI). No night, no date and no row is
written: the per-night values live in memory for the length of this run.

The per-night figures are the ones the renderer is given at runtime: the same
sleep-day aggregation as DatabaseService::getMetricsForDateRange (sessions
grouped by DATE(session_start - 12 h), events summed, calculated metrics
averaged over their samples), the night's STR row (cpap_daily_summary) and its
longest oximetry session over 60 s (getOximetrySummary).

Connection: the standard libpq environment (PGHOST, PGPORT, PGUSER,
PGPASSWORD, PGDATABASE) or --dsn. The session is opened read-only.

Usage:
    python extract_distributions.py --device-id <device> --output data/distributions.json
    python extract_distributions.py --device-id <a> --device-id <b> --output ...

The output is private (it describes a person's own therapy): it goes under
data/, which is not committed.
"""

import argparse
import json
import math
import sys
from collections import Counter

import numpy as np
import psycopg

QUANTILES = [0.01, 0.05, 0.10, 0.25, 0.50, 0.75, 0.90, 0.95, 0.99]
HIST_BINS = 20
MIN_NIGHTS_FOR_HISTOGRAM = 10

# The same aggregation as DatabaseService::getMetricsForDateRange (PostgreSQL).
NIGHTS_SQL = """
SELECT
    DATE(s.session_start - INTERVAL '12 hours')            AS sleep_day,
    SUM(s.duration_seconds)                                AS total_seconds,
    SUM(sm.total_events)                                   AS total_events,
    SUM(sm.obstructive_apneas)                             AS obstructive,
    SUM(sm.central_apneas)                                 AS central,
    SUM(sm.hypopneas)                                      AS hypopnea,
    SUM(sm.reras)                                          AS reras,
    SUM(sm.clear_airway_apneas)                            AS clear_airway,
    SUM(sm.unclassified_apneas)                            AS unclassified,
    SUM(sm.avg_event_duration * sm.total_events)
        / NULLIF(SUM(CASE WHEN sm.avg_event_duration IS NOT NULL
                          THEN sm.total_events END), 0)    AS avg_event_duration,
    MAX(sm.max_event_duration)                             AS max_event_duration,
    SUM(c.s_leak) / NULLIF(SUM(c.n_leak), 0)               AS leak_avg,
    MAX(c.max_leak)                                        AS leak_max,
    SUM(c.s_rr)   / NULLIF(SUM(c.n_rr), 0)                 AS respiratory_rate,
    SUM(c.s_tv)   / NULLIF(SUM(c.n_tv), 0)                 AS tidal_volume,
    SUM(c.s_mv)   / NULLIF(SUM(c.n_mv), 0)                 AS minute_ventilation,
    SUM(c.s_fl)   / NULLIF(SUM(c.n_fl), 0)                 AS flow_limitation,
    SUM(c.s_pp95) / NULLIF(SUM(c.n_pp95), 0)               AS pressure_p95,
    SUM(c.s_mask) / NULLIF(SUM(c.n_mask), 0)               AS mask_pressure,
    SUM(c.s_epr)  / NULLIF(SUM(c.n_epr), 0)                AS epr_pressure,
    SUM(c.s_snore) / NULLIF(SUM(c.n_snore), 0)             AS snore,
    SUM(c.s_tgt)  / NULLIF(SUM(c.n_tgt), 0)                AS target_ventilation,
    SUM(c.s_therapy) / NULLIF(SUM(c.n_therapy), 0)         AS therapy_pressure,
    SUM(b.s_press) / NULLIF(SUM(b.n_press), 0)             AS pressure_avg,
    MAX(b.max_press)                                       AS pressure_max,
    MIN(b.min_press)                                       AS pressure_min,
    CASE WHEN COUNT(*) > 1 AND MAX(d.leak_50_str) IS NOT NULL
         THEN MAX(d.leak_50_str) ELSE MAX(sm.leak_p50) END AS leak_p50,
    CASE WHEN COUNT(*) > 1 AND MAX(d.leak_95_str) IS NOT NULL
         THEN MAX(d.leak_95_str) ELSE MAX(sm.leak_p95) END AS leak_p95,
    MAX(COALESCE(sm.index_kind, 'ahi'))                    AS index_kind,
    COUNT(*)                                               AS sessions
FROM cpap_sessions s
JOIN cpap_session_metrics sm ON sm.session_id = s.id
LEFT JOIN (
    SELECT session_id,
           SUM(leak_rate) AS s_leak, COUNT(leak_rate) AS n_leak, MAX(leak_rate) AS max_leak,
           SUM(respiratory_rate) AS s_rr, COUNT(respiratory_rate) AS n_rr,
           SUM(tidal_volume) AS s_tv, COUNT(tidal_volume) AS n_tv,
           SUM(minute_ventilation) AS s_mv, COUNT(minute_ventilation) AS n_mv,
           SUM(flow_limitation) AS s_fl, COUNT(flow_limitation) AS n_fl,
           SUM(pressure_p95) AS s_pp95, COUNT(pressure_p95) AS n_pp95,
           SUM(mask_pressure) AS s_mask, COUNT(mask_pressure) AS n_mask,
           SUM(epr_pressure) AS s_epr, COUNT(epr_pressure) AS n_epr,
           SUM(snore_index) AS s_snore, COUNT(snore_index) AS n_snore,
           SUM(target_ventilation) AS s_tgt, COUNT(target_ventilation) AS n_tgt,
           SUM(therapy_pressure) AS s_therapy, COUNT(therapy_pressure) AS n_therapy
    FROM cpap_calculated_metrics GROUP BY session_id
) c ON c.session_id = sm.session_id
LEFT JOIN (
    SELECT session_id,
           SUM(avg_pressure) AS s_press, COUNT(avg_pressure) AS n_press,
           MAX(max_pressure) AS max_press, MIN(min_pressure) AS min_press
    FROM cpap_breathing_summary GROUP BY session_id
) b ON b.session_id = sm.session_id
LEFT JOIN cpap_daily_summary d
       ON d.device_id = s.device_id
      AND d.record_date = DATE(s.session_start - INTERVAL '12 hours')
WHERE s.device_id = %(device)s
  AND s.session_end IS NOT NULL
GROUP BY DATE(s.session_start - INTERVAL '12 hours')
"""

# The STR row of a sleep day, as NightFacts::from reads it.
STR_SQL = """
SELECT record_date AS sleep_day, ahi, mask_events, leak_95, mask_press_95, mode,
       duration_minutes
FROM cpap_daily_summary
WHERE device_id = %(device)s AND duration_minutes > 0
"""

# getOximetrySummary: the longest session over 60 s of the day or the next.
OXI_SQL = """
SELECT avg_spo2, min_spo2, spo2_baseline, odi_3pct, time_below_90, time_below_88,
       avg_hr, min_hr, max_hr, duration_seconds
FROM oximetry_sessions
WHERE duration_seconds > 60
"""

NIGHT_FIELDS = [
    "usage_hours", "ahi", "total_events", "in_index", "reras", "other",
    "avg_event_duration", "max_event_duration",
    "therapy_pressure", "pressure_avg", "pressure_min", "pressure_max", "pressure_p95",
    "leak_avg", "leak_max", "leak_p95", "leak_p50",
    "mask_pressure", "epr_pressure", "snore", "target_ventilation",
    "respiratory_rate", "tidal_volume", "minute_ventilation", "flow_limitation",
    "sessions",
]
EVENT_TYPES = ["obstructive", "central", "clear_airway", "unclassified", "hypopnea"]
STR_FIELDS = ["ahi", "mask_on_off_pairs", "leak_95", "mask_press_95"]
OXI_FIELDS = ["avg_spo2", "min_spo2", "spo2_baseline", "odi_3pct", "time_below_90",
              "time_below_88", "avg_hr", "min_hr", "max_hr", "hours"]


def describe(values):
    """Quantiles, mean, spread and a histogram of [values] (no values kept)."""
    v = np.asarray([x for x in values if x is not None and not math.isnan(x)], dtype=float)
    out = {"n": int(v.size)}
    if v.size == 0:
        return out
    out["mean"] = round(float(v.mean()), 4)
    out["std"] = round(float(v.std()), 4)
    out["min"] = round(float(v.min()), 4)
    out["max"] = round(float(v.max()), 4)
    out["quantiles"] = {f"p{int(q * 100):02d}": round(float(np.quantile(v, q)), 4)
                        for q in QUANTILES}
    if v.size >= MIN_NIGHTS_FOR_HISTOGRAM and v.max() > v.min():
        counts, edges = np.histogram(v, bins=HIST_BINS)
        out["histogram"] = {"edges": [round(float(e), 4) for e in edges],
                            "counts": [int(c) for c in counts]}
    return out


def ratio(nights, num, den):
    vals = [n[num] / n[den] for n in nights
            if n.get(num) is not None and n.get(den) not in (None, 0)]
    return describe(vals)


def f(x):
    return None if x is None else float(x)


def night_from_row(r):
    hours = f(r["total_seconds"]) / 3600.0 if r["total_seconds"] else None
    parts = {k: int(r[k] or 0) for k in EVENT_TYPES}
    in_index = sum(parts.values())
    total = int(r["total_events"] or 0)
    reras = int(r["reras"] or 0)
    n = {
        "usage_hours": hours,
        "ahi": (in_index / hours) if hours else None,
        "total_events": total,
        "in_index": in_index,
        "reras": reras,
        "other": max(0, total - in_index - reras),
        "index_kind": r["index_kind"],
        "sessions": int(r["sessions"]),
    }
    n.update(parts)
    for k in NIGHT_FIELDS:
        if k not in n:
            n[k] = f(r.get(k))
    return n


def summarize_device(conn, device):
    with conn.cursor(row_factory=psycopg.rows.dict_row) as cur:
        cur.execute(NIGHTS_SQL, {"device": device})
        nights = [night_from_row(r) for r in cur.fetchall()]
        cur.execute(STR_SQL, {"device": device})
        strs = [{"ahi": f(r["ahi"]), "mask_on_off_pairs": int(r["mask_events"] or 0) // 2,
                 "leak_95": f(r["leak_95"]), "mask_press_95": f(r["mask_press_95"]),
                 "mode": int(r["mode"] or 0)} for r in cur.fetchall()]
    nights = [n for n in nights if n["usage_hours"]]
    count = len(nights)
    if count == 0:
        return {"nights": 0}

    out = {"nights": count, "str_days": len(strs)}
    out["index_kind"] = dict(Counter(n["index_kind"] for n in nights))
    out["presence"] = {k: round(sum(1 for n in nights if n.get(k) is not None) / count, 4)
                       for k in NIGHT_FIELDS}
    out["fields"] = {k: describe([n.get(k) for n in nights]) for k in NIGHT_FIELDS}

    # The event mix: each type's share of the events counted in the index.
    with_events = [n for n in nights if n["in_index"] > 0]
    out["event_mix"] = {
        t: describe([n[t] / n["in_index"] for n in with_events]) for t in EVENT_TYPES}
    out["event_mix"]["nights_with_zero_index_events"] = sum(
        1 for n in nights if n["in_index"] == 0)
    out["ratios"] = {
        "leak_max_over_avg": ratio(nights, "leak_max", "leak_avg"),
        "leak_p95_over_avg": ratio(nights, "leak_p95", "leak_avg"),
        "leak_p50_over_avg": ratio(nights, "leak_p50", "leak_avg"),
        "pressure_p95_over_therapy": ratio(nights, "pressure_p95", "therapy_pressure"),
        "pressure_avg_over_therapy": ratio(nights, "pressure_avg", "therapy_pressure"),
        "mask_over_therapy": ratio(nights, "mask_pressure", "therapy_pressure"),
        "epr_over_therapy": ratio(nights, "epr_pressure", "therapy_pressure"),
        "max_over_avg_event_duration": ratio(nights, "max_event_duration", "avg_event_duration"),
        "reras_per_hour": describe([n["reras"] / n["usage_hours"] for n in nights]),
        "other_per_hour": describe([n["other"] / n["usage_hours"] for n in nights]),
        "minute_ventilation_over_rr_tv": describe(
            [n["minute_ventilation"] / (n["respiratory_rate"] * n["tidal_volume"] / 1000.0)
             for n in nights
             if n["minute_ventilation"] and n["respiratory_rate"] and n["tidal_volume"]]),
    }
    if strs:
        out["str"] = {k: describe([s[k] for s in strs]) for k in STR_FIELDS}
        out["str"]["mode"] = dict(Counter(str(s["mode"]) for s in strs))
    return out


def summarize_oximetry(conn):
    with conn.cursor(row_factory=psycopg.rows.dict_row) as cur:
        cur.execute(OXI_SQL)
        rows = cur.fetchall()
    oxi = [{k: f(r[k]) for k in OXI_FIELDS if k != "hours"} | {
        "hours": f(r["duration_seconds"]) / 3600.0} for r in rows]
    out = {"sessions": len(oxi)}
    out["fields"] = {k: describe([o[k] for o in oxi]) for k in OXI_FIELDS}
    out["ratios"] = {
        "baseline_minus_avg": describe([o["spo2_baseline"] - o["avg_spo2"] for o in oxi
                                        if o["spo2_baseline"] and o["avg_spo2"]]),
        "avg_minus_min": describe([o["avg_spo2"] - o["min_spo2"] for o in oxi
                                   if o["avg_spo2"] and o["min_spo2"]]),
    }
    return out


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    ap.add_argument("--device-id", action="append", required=True,
                    help="device_id to summarize (repeatable)")
    ap.add_argument("--dsn", default="", help="libpq DSN (default: PG* environment)")
    ap.add_argument("--output", required=True)
    args = ap.parse_args()

    with psycopg.connect(args.dsn, options="-c default_transaction_read_only=on") as conn:
        conn.read_only = True
        out = {
            "about": "SDD-013 narrator corpus input ranges: aggregates only, no nights.",
            "devices": {f"device_{i + 1}": summarize_device(conn, d)
                        for i, d in enumerate(args.device_id)},
            "oximetry": summarize_oximetry(conn),
        }
    with open(args.output, "w") as fh:
        json.dump(out, fh, indent=1)
    for name, d in out["devices"].items():
        print(f"{name}: {d.get('nights', 0)} nights summarized", file=sys.stderr)
    print(f"oximetry: {out['oximetry']['sessions']} sessions summarized", file=sys.stderr)


if __name__ == "__main__":
    main()
