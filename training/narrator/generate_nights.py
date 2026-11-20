#!/usr/bin/env python3
"""SDD-013: synthetic nights for the narrator corpus.

Each night is drawn from the aggregate distributions extract_distributions.py
wrote (one install's own nights), widened with published clinical ranges so
the corpus covers what one person's nights rarely show: excellent nights,
non-compliant and short nights, partial nights, high leak, high AHI, a machine
whose index is not an AHI (SDD-024 Ungraded), missing SpO2, missing STR,
bi-level and ASV pressures, and machines other than a ResMed (Luna, Philips).
No real night is read or copied.

A night is physiologically consistent by construction:
  - the event types sum to the events in the index, and with the RERAs and the
    other types to total_events; the AHI is those events over the usage hours
    (as the database computes it), and usage_percent is hours over 8;
  - leak percentiles are ordered (median <= avg <= 95th <= max) and scale with
    the leak scenario; pressures sit inside the machine's range, with EPAP /
    EPR under the delivered pressure and IPAP over it on a bi-level;
  - SpO2 is ordered (min <= avg <= baseline), ODI and the time below 90/88 %
    follow the AHI, and min HR <= avg <= max;
  - a figure the machine does not record is absent from the JSON, never 0.

Output: JSONL in the shape model/NightJson.h reads, one night per line, plus a
"tags" object naming the scenario (stripped before rendering).

Usage:
    python generate_nights.py --distributions data/distributions.json \
        --count 500 --seed 13 --output out/nights.jsonl
"""

import argparse
import datetime as dt
import json
import sys

import numpy as np

# Scenario weights. Each axis is drawn independently, so rare combinations
# (a short, high-leak ASV night without SpO2) occur at their product.
MACHINES = {            # weight
    "resmed_apap": 0.40,
    "resmed_cpap": 0.10,
    "resmed_bilevel": 0.10,
    "resmed_asv": 0.07,
    "philips": 0.10,
    "luna": 0.10,
    "ungraded": 0.13,   # SDD-024: an apnea-only index, no hypopneas, no types
}
SEVERITY = {"excellent": 0.15, "typical": 0.40, "mild": 0.20, "moderate": 0.13, "severe": 0.12}
USAGE = {"typical": 0.62, "short": 0.18, "very_short": 0.07, "long": 0.08, "full": 0.05}
LEAK = {"typical": 0.62, "none": 0.13, "high": 0.18, "very_high": 0.07}
P_OXIMETRY = 0.40        # nights with an O2 Ring; the rest are "missing SpO2"
P_STR_RESMED = 0.85      # ResMed nights with an STR row; the rest "missing STR"
                         # (the card has no record for the night yet)
P_PARTIAL = 0.12         # a group of figures was not recorded that night
P_EVENT_DURATION = 0.30  # parsers that report event durations
P_OTHER_EVENTS = 0.12    # nights with recorded event types outside the index

# Published clinical ranges (AASM severity bands; ResMed's 24 L/min
# unintentional leak line; typical PAP pressure ranges; overnight oximetry).
AHI_BANDS = {"excellent": (0.0, 1.0), "mild": (5.0, 15.0), "moderate": (15.0, 30.0),
             "severe": (30.0, 80.0)}
USAGE_BANDS = {"short": (1.0, 4.0), "very_short": (0.05, 1.0), "long": (9.0, 12.5),
               "full": (7.8, 9.0)}
LEAK_P95_BANDS = {"none": (0.0, 2.0), "high": (24.0, 50.0), "very_high": (50.0, 110.0)}


def pick(rng, weights):
    keys = list(weights)
    p = np.array([weights[k] for k in keys], dtype=float)
    return keys[rng.choice(len(keys), p=p / p.sum())]


class Empirical:
    """Draws from one field's histogram, clipped to its p01..p99."""

    def __init__(self, desc, fallback):
        self.desc = desc or {}
        self.fallback = fallback

    def sample(self, rng):
        h = self.desc.get("histogram")
        q = self.desc.get("quantiles", {})
        if not h:
            lo, hi = self.fallback
            return float(rng.uniform(lo, hi))
        counts = np.asarray(h["counts"], dtype=float)
        edges = h["edges"]
        i = rng.choice(len(counts), p=counts / counts.sum())
        v = float(rng.uniform(edges[i], edges[i + 1]))
        return float(np.clip(v, q.get("p01", edges[0]), q.get("p99", edges[-1])))


def r2(x):
    return None if x is None else round(float(x), 2)


class Generator:
    def __init__(self, dist, rng):
        self.rng = rng
        dev = dist["devices"]["device_1"]
        self.f = dev["fields"]
        self.mix = dev["event_mix"]
        self.ratios = dev["ratios"]
        self.str = dev.get("str", {})
        self.oxi = dist.get("oximetry", {}).get("fields", {})
        emp = lambda k, fb: Empirical(self.f.get(k), fb)
        self.e_usage = emp("usage_hours", (4.0, 9.0))
        self.e_ahi = emp("ahi", (0.5, 4.0))
        self.e_leak_p95 = emp("leak_p95", (2.0, 22.0))
        self.e_p95 = emp("pressure_p95", (6.0, 12.0))
        self.e_rr = emp("respiratory_rate", (13.0, 18.0))
        self.e_tv = emp("tidal_volume", (300.0, 500.0))
        self.e_fl = emp("flow_limitation", (0.01, 0.12))
        self.e_snore = emp("snore", (0.0, 0.05))
        self.e_reras_h = Empirical(self.ratios.get("reras_per_hour"), (0.0, 1.0))
        self.e_baseline = Empirical(self.oxi.get("spo2_baseline"), (95.0, 99.0))
        self.e_hr = Empirical(self.oxi.get("avg_hr"), (58.0, 85.0))

    # -- the axes ---------------------------------------------------------------

    def usage(self, kind):
        if kind == "typical":
            return float(np.clip(self.e_usage.sample(self.rng), 4.0, 11.0))
        lo, hi = USAGE_BANDS[kind]
        return float(self.rng.uniform(lo, hi))

    def ahi(self, kind, machine):
        if kind == "typical":
            v = self.e_ahi.sample(self.rng)
        else:
            lo, hi = AHI_BANDS[kind]
            v = float(self.rng.uniform(lo, hi))
        if machine == "resmed_asv" and kind in ("moderate", "severe"):
            v *= 0.6   # ASV nights are treated central apnea: rarely extreme
        return v

    def event_mix(self, machine):
        """Shares of the index events by type: obstructive, central, clear
        airway, unclassified, hypopnea."""
        rng = self.rng
        base = {t: max(self.mix.get(t, {}).get("mean", 0.0), 0.0) for t in
                ("obstructive", "central", "clear_airway", "unclassified", "hypopnea")}
        if machine == "philips":       # PRS1 scores clear airway, not central
            base["clear_airway"], base["central"], base["unclassified"] = base["central"], 0.0, 0.0
        elif machine == "luna":
            base["unclassified"] = 0.0
        elif machine == "resmed_asv":
            base["central"] = 0.55
        alpha = np.array([base[t] for t in base]) * 12.0 + 0.05
        alpha[[i for i, t in enumerate(base) if base[t] == 0.0]] = 1e-9
        shares = rng.dirichlet(alpha)
        return dict(zip(base, shares))

    # -- one night ----------------------------------------------------------------

    def night(self, i, day):
        rng = self.rng
        machine = pick(rng, MACHINES)
        tags = {"machine": machine, "severity": pick(rng, SEVERITY),
                "usage": pick(rng, USAGE), "leak": pick(rng, LEAK)}
        resmed = machine.startswith("resmed")
        graded = machine != "ungraded"

        hours = round(self.usage(tags["usage"]), 4)
        target_ahi = self.ahi(tags["severity"], machine)
        in_index = int(rng.poisson(target_ahi * hours)) if target_ahi > 0 else 0
        if tags["severity"] == "excellent" and rng.random() < 0.35:
            in_index = 0

        m = {"sleep_day": day, "index_kind": "ahi" if graded else "ungraded",
             "usage_hours": hours, "usage_percent": round(hours * 100.0 / 8.0, 4)}
        if graded:
            shares = self.event_mix(machine)
            counts = rng.multinomial(in_index, list(shares.values()))
            keys = ["obstructive_apneas", "central_apneas", "clear_airway_apneas",
                    "unclassified_apneas", "hypopneas"]
            m.update({k: int(c) for k, c in zip(keys, counts)})
            reras = int(rng.poisson(self.e_reras_h.sample(rng) * hours)) \
                if machine in ("resmed_apap", "resmed_cpap", "resmed_bilevel", "philips") else 0
            other = int(rng.poisson(rng.uniform(0.2, 3.0) * hours)) \
                if rng.random() < P_OTHER_EVENTS else 0
            m["reras"] = reras
            m["other_events"] = other
            m["total_events"] = in_index + reras + other
        else:
            # SDD-024: the parser's index is apneas per hour; no types.
            m["total_events"] = in_index
        m["ahi"] = round(in_index / hours, 4) if hours > 0 else 0.0
        tags["ahi"] = round(m["ahi"], 2)

        if in_index > 0 and rng.random() < P_EVENT_DURATION:
            avg = rng.uniform(11.0, 32.0)
            m["avg_event_duration"] = round(avg, 4)
            m["max_event_duration"] = round(avg * rng.uniform(1.2, 3.5), 4)

        self.pressures(m, machine, tags)
        self.leak(m, tags, machine)
        if resmed or machine == "philips":
            self.breathing(m, machine)

        partial = rng.random() < P_PARTIAL
        if partial:
            tags["partial"] = self.drop_group(m)

        night = {"id": f"n{i:07d}", "metrics": m}
        if resmed and rng.random() < P_STR_RESMED:
            night["str"] = self.str_row(m, machine, tags["mode"])
        if rng.random() < P_OXIMETRY:
            night["oximetry"] = self.oximetry(m, hours)
        tags["str"] = "str" in night
        tags["oximetry"] = "oximetry" in night
        night["tags"] = tags
        return night

    def pressures(self, m, machine, tags):
        rng = self.rng
        if machine in ("resmed_apap", "philips", "luna", "ungraded"):
            p95 = float(np.clip(self.e_p95.sample(rng) * rng.uniform(0.8, 1.6), 4.5, 20.0))
            therapy = p95 * rng.uniform(0.72, 0.97)
            mode = 1
        elif machine == "resmed_cpap":
            therapy = float(rng.uniform(5.0, 14.0))
            p95 = therapy + rng.uniform(0.0, 0.3)
            mode = 0
        elif machine == "resmed_bilevel":
            epap = float(rng.uniform(5.0, 12.0))
            ps = float(rng.uniform(3.0, 8.0))
            therapy = epap + ps * rng.uniform(0.8, 1.0)   # delivered IPAP
            p95 = epap + ps + rng.uniform(0.0, 1.5)
            m["avg_epr_pressure"] = round(epap, 4)
            tags["ipap"], tags["epap"] = r2(therapy), r2(epap)
            mode = 8 if rng.random() < 0.6 else 6
        else:  # ASV
            epap = float(rng.uniform(5.0, 10.0))
            therapy = epap + rng.uniform(2.0, 8.0)
            p95 = therapy + rng.uniform(2.0, 6.0)
            m["avg_epr_pressure"] = round(epap, 4)
            m["avg_target_ventilation"] = round(rng.uniform(5.0, 9.5), 4)
            mode = 8 if rng.random() < 0.5 else 7
        tags["mode"] = mode

        if machine in ("luna", "ungraded"):
            # The Luna's own record: the pressure it delivered, no waveform figures.
            m["avg_therapy_pressure"] = round(therapy, 4)
            if machine == "luna":
                m["avg_epr_pressure"] = round(therapy, 4)
            return
        m["avg_therapy_pressure"] = round(therapy, 4)
        m["pressure_p95"] = round(p95, 4)
        if machine == "philips":
            return
        # ResMed: the mask waveform figures (BRP) and the EPR set-point.
        m["avg_pressure"] = round(therapy * rng.uniform(0.6, 0.98), 4)
        m["min_pressure"] = round(rng.uniform(0.0, 3.0), 4)
        m["max_pressure"] = round(p95 + rng.uniform(2.0, 9.0), 4)
        m["avg_mask_pressure"] = round(therapy * rng.uniform(0.85, 0.98), 4)
        if "avg_epr_pressure" not in m:
            epr = rng.choice([0.0, 1.0, 2.0, 3.0])
            m["avg_epr_pressure"] = round(max(4.0, therapy - epr * rng.uniform(0.8, 1.0)), 4)
        m["therapy_mode"] = 0   # what a ResMed session stores (no parser fills it)

    def leak(self, m, tags, machine):
        rng = self.rng
        kind = tags["leak"]
        if kind == "typical":
            p95 = float(np.clip(self.e_leak_p95.sample(rng), 0.0, 23.9))
        else:
            lo, hi = LEAK_P95_BANDS[kind]
            p95 = float(rng.uniform(lo, hi))
        if machine == "philips":
            p95 += rng.uniform(20.0, 35.0)   # PRS1 reports total leak, vent flow included
        heavy = kind in ("high", "very_high")
        avg = p95 / (rng.uniform(1.4, 3.0) if heavy else rng.uniform(2.5, 8.0))
        m["leak_p95"] = round(p95, 4)
        m["avg_leak_rate"] = round(avg, 4)
        m["max_leak_rate"] = round(p95 * rng.uniform(1.2, 3.5) + rng.uniform(0.0, 5.0), 4)
        if machine != "ungraded":
            m["leak_p50"] = round(avg * rng.uniform(0.0, 0.9) if rng.random() < 0.6 else 0.0, 4)

    def breathing(self, m, machine):
        rng = self.rng
        # Clipped to adult ranges: one install's tails hold sensor glitches.
        rr = float(np.clip(self.e_rr.sample(rng) * rng.uniform(0.85, 1.25), 8.0, 28.0))
        tv = float(np.clip(self.e_tv.sample(rng) * rng.uniform(0.85, 1.4), 250.0, 750.0))
        m["avg_respiratory_rate"] = round(rr, 4)
        m["avg_tidal_volume"] = round(tv, 4)
        m["avg_minute_ventilation"] = round(rr * tv / 1000.0 * rng.uniform(0.95, 1.05), 4)
        if machine == "philips":
            return
        m["avg_flow_limitation"] = round(
            float(np.clip(self.e_fl.sample(rng) * rng.uniform(0.7, 2.5), 0.0, 0.6)), 4)
        snore = self.e_snore.sample(rng)
        if rng.random() < 0.08:
            snore = rng.uniform(0.2, 2.0)
        m["avg_snore"] = round(snore, 4)

    def drop_group(self, m):
        """A partial night: one group of figures was not recorded."""
        groups = {
            "no_pld": ["avg_therapy_pressure", "avg_mask_pressure", "avg_epr_pressure",
                       "avg_snore", "avg_target_ventilation", "avg_respiratory_rate",
                       "avg_tidal_volume", "avg_minute_ventilation", "avg_flow_limitation"],
            "no_waveform": ["avg_pressure", "min_pressure", "max_pressure"],
            "no_leak": ["avg_leak_rate", "max_leak_rate", "leak_p95", "leak_p50"],
            "no_pressure": ["avg_therapy_pressure", "pressure_p95", "avg_pressure",
                            "min_pressure", "max_pressure"],
        }
        name = list(groups)[self.rng.integers(len(groups))]
        for k in groups[name]:
            m.pop(k, None)
        return name

    def str_row(self, m, machine, mode):
        rng = self.rng
        family = {"resmed_apap": "autoset", "resmed_cpap": "cpap",
                  "resmed_bilevel": "bilevel", "resmed_asv": "asv"}[machine]
        mode = {"autoset": 1, "cpap": 0}.get(family, mode)
        p95 = m.get("pressure_p95") or m.get("avg_therapy_pressure") or rng.uniform(6.0, 12.0)
        s = {
            "family": family, "mode": mode,
            # The machine's own AHI; it differs from the computed one by its
            # own scoring, within a few tenths.
            "ahi": round(max(0.0, m["ahi"] + rng.normal(0.0, 0.1)), 2) if m["ahi"] > 0 else 0.0,
            "mask_events": int(2 * max(1, rng.poisson(1.5))),
            "leak_95": round((m.get("leak_p95") or rng.uniform(1.0, 20.0)) * rng.uniform(0.75, 1.05), 2),
            "mask_press_95": round(p95 + rng.uniform(0.3, 1.5), 2),
        }
        if family == "asv":
            epap = m.get("avg_epr_pressure") or rng.uniform(5.0, 10.0)
            s["asv_epap"] = round(epap, 2)
            s["asv_min_ps"] = round(rng.uniform(0.0, 3.0), 2)
            s["asv_max_ps"] = round(rng.uniform(8.0, 15.0), 2)
        if family in ("asv", "bilevel"):
            therapy = m.get("avg_therapy_pressure") or rng.uniform(8.0, 16.0)
            epap = m.get("avg_epr_pressure") or therapy - rng.uniform(3.0, 6.0)
            s["tgt_ipap_50"] = round(therapy, 2)
            s["tgt_epap_50"] = round(epap, 2)
            s["tgt_vent_50"] = round(rng.uniform(5.0, 9.0), 2)
        return s

    def oximetry(self, m, hours):
        rng = self.rng
        ahi = m["ahi"]
        baseline = float(np.clip(self.e_baseline.sample(rng), 93.0, 99.5))
        avg = baseline - rng.uniform(0.8, 2.5) - min(ahi, 60.0) * rng.uniform(0.02, 0.08)
        min_spo2 = avg - rng.uniform(2.0, 6.0) - min(ahi, 60.0) * rng.uniform(0.1, 0.35)
        odi = max(0.0, ahi * rng.uniform(0.5, 1.3) + rng.normal(0.0, 1.0))
        t90 = 0.0 if min_spo2 >= 90 else min(60.0, rng.uniform(0.1, 1.0) * (90 - min_spo2)
                                              * (1 + ahi / 15.0))
        t88 = 0.0 if min_spo2 >= 88 else t90 * rng.uniform(0.1, 0.6)
        hr = self.e_hr.sample(rng)
        return {
            "avg_spo2": round(avg, 2), "min_spo2": round(max(min_spo2, 70.0), 0),
            "spo2_baseline": round(baseline, 0), "odi_3pct": round(odi, 2),
            "time_below_90": round(t90, 2), "time_below_88": round(t88, 2),
            "avg_hr": round(hr, 2), "min_hr": int(round(hr - rng.uniform(6.0, 20.0))),
            "max_hr": int(round(hr + rng.uniform(12.0, 40.0))),
        }


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    ap.add_argument("--distributions", required=True)
    ap.add_argument("--count", type=int, required=True)
    ap.add_argument("--seed", type=int, default=13)
    ap.add_argument("--output", required=True)
    args = ap.parse_args()

    with open(args.distributions) as fh:
        dist = json.load(fh)
    rng = np.random.default_rng(args.seed)
    gen = Generator(dist, rng)
    start = dt.date(2023, 1, 1)
    span = (dt.date(2027, 12, 31) - start).days
    with open(args.output, "w") as out:
        for i in range(args.count):
            day = (start + dt.timedelta(days=int(rng.integers(span)))).isoformat()
            out.write(json.dumps(gen.night(i, day)) + "\n")
    print(f"{args.count} nights -> {args.output}", file=sys.stderr)


if __name__ == "__main__":
    main()
