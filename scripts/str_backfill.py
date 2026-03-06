#!/usr/bin/env python3
"""
STR.edf Backfill — parse ResMed STR files and upsert daily summaries into PostgreSQL.

Usage:
    python3 str_backfill.py <str_file1.edf> [str_file2.edf ...]

Uses a custom lenient EDF parser (pyedflib rejects ResMed's non-standard Physical Dimension).
Matches the C++ parser logic exactly:
  - 10 MaskOn/MaskOff slots per record (minutes since noon)
  - Leak L/s -> L/min (*60)
  - SpO2 -1 -> 0
  - Duration <= 0 -> skip (no therapy)
"""

import sys
import json
import struct
from datetime import datetime, timedelta
import psycopg2
from psycopg2.extras import execute_values

DB_CONN = "host=localhost port=5432 dbname=cpap_monitoring user=maestro password=maestro_postgres_2026_secure"
DEVICE_ID = "cpap_resmed_23243570851"


class EDFSignal:
    def __init__(self):
        self.label = ""
        self.phys_min = 0.0
        self.phys_max = 0.0
        self.dig_min = 0
        self.dig_max = 0
        self.samples_per_record = 0
        self.gain = 1.0
        self.offset = 0.0


class EDFFile:
    def __init__(self):
        self.start_datetime = None
        self.num_signals = 0
        self.num_records = 0
        self.record_duration = 0
        self.signals = []
        self._data = {}  # signal_index -> list of physical values

    @staticmethod
    def open(filepath):
        edf = EDFFile()
        with open(filepath, "rb") as f:
            # Fixed header (256 bytes)
            f.read(8)  # version
            f.read(80)  # patient
            f.read(80)  # recording
            start_date = f.read(8).decode("ascii").strip()
            start_time = f.read(8).decode("ascii").strip()
            header_bytes = int(f.read(8).decode("ascii").strip())
            f.read(44)  # reserved
            edf.num_records = int(f.read(8).decode("ascii").strip())
            edf.record_duration = float(f.read(8).decode("ascii").strip())
            edf.num_signals = int(f.read(4).decode("ascii").strip())

            # Parse start datetime
            dd, mm, yy = start_date.split(".")
            hh, mi, ss = start_time.split(".")
            yy = int(yy)
            year = yy + 2000 if yy < 85 else yy + 1900
            edf.start_datetime = datetime(year, int(mm), int(dd), int(hh), int(mi), int(ss))

            ns = edf.num_signals

            # Signal headers (each field is ns * field_size bytes)
            labels = [f.read(16).decode("ascii").strip() for _ in range(ns)]
            f.read(80 * ns)  # transducer type
            f.read(8 * ns)   # physical dimension
            phys_mins = [float(f.read(8).decode("ascii").strip()) for _ in range(ns)]
            phys_maxs = [float(f.read(8).decode("ascii").strip()) for _ in range(ns)]
            dig_mins = [int(f.read(8).decode("ascii").strip()) for _ in range(ns)]
            dig_maxs = [int(f.read(8).decode("ascii").strip()) for _ in range(ns)]
            f.read(80 * ns)  # prefiltering
            samples_per_record = [int(f.read(8).decode("ascii").strip()) for _ in range(ns)]
            f.read(32 * ns)  # reserved

            for i in range(ns):
                sig = EDFSignal()
                sig.label = labels[i]
                sig.phys_min = phys_mins[i]
                sig.phys_max = phys_maxs[i]
                sig.dig_min = dig_mins[i]
                sig.dig_max = dig_maxs[i]
                sig.samples_per_record = samples_per_record[i]
                dig_range = sig.dig_max - sig.dig_min
                if dig_range != 0:
                    sig.gain = (sig.phys_max - sig.phys_min) / dig_range
                    sig.offset = sig.phys_min - sig.gain * sig.dig_min
                edf.signals.append(sig)

            # Read data records
            f.seek(header_bytes)
            for i in range(ns):
                edf._data[i] = []

            for _ in range(edf.num_records):
                for i in range(ns):
                    n = edf.signals[i].samples_per_record
                    raw = f.read(n * 2)
                    values = struct.unpack(f"<{n}h", raw)
                    for v in values:
                        edf._data[i].append(edf.signals[i].gain * v + edf.signals[i].offset)

        return edf

    def find_signal_exact(self, name):
        for i, sig in enumerate(self.signals):
            if sig.label == name:
                return i
        return -1

    def read_signal(self, name):
        idx = self.find_signal_exact(name)
        if idx < 0:
            return []
        return self._data[idx]


def parse_str(filepath):
    """Parse STR.edf and return list of daily record tuples."""
    edf = EDFFile.open(filepath)

    if int(edf.record_duration) != 86400:
        print(f"  Unexpected record_duration={edf.record_duration}")
        return []

    print(f"  {edf.num_records} days, {edf.num_signals} signals, start={edf.start_datetime}")

    duration_data = edf.read_signal("Duration")
    patient_hours_data = edf.read_signal("PatientHours")
    mask_events_data = edf.read_signal("MaskEvents")
    ahi_data = edf.read_signal("AHI")
    hi_data = edf.read_signal("HI")
    ai_data = edf.read_signal("AI")
    oai_data = edf.read_signal("OAI")
    cai_data = edf.read_signal("CAI")
    uai_data = edf.read_signal("UAI")
    rin_data = edf.read_signal("RIN")
    csr_data = edf.read_signal("CSR")

    mask_press_50_data = edf.read_signal("MaskPress.50")
    mask_press_95_data = edf.read_signal("MaskPress.95")
    mask_press_max_data = edf.read_signal("MaskPress.Max")

    leak_50_data = edf.read_signal("Leak.50")
    leak_95_data = edf.read_signal("Leak.95")
    leak_max_data = edf.read_signal("Leak.Max")

    spo2_50_data = edf.read_signal("SpO2.50")
    spo2_95_data = edf.read_signal("SpO2.95")

    resp_rate_50_data = edf.read_signal("RespRate.50")
    tid_vol_50_data = edf.read_signal("TidVol.50")
    min_vent_50_data = edf.read_signal("MinVent.50")

    mode_data = edf.read_signal("Mode")
    epr_level_data = edf.read_signal("S.EPR.Level")
    press_setting_data = edf.read_signal("S.C.Press")

    fault_device_data = edf.read_signal("Fault.Device")
    fault_alarm_data = edf.read_signal("Fault.Alarm")

    mask_on_data = edf.read_signal("MaskOn")
    mask_off_data = edf.read_signal("MaskOff")

    def val(data, i):
        return data[i] if 0 <= i < len(data) else 0.0

    records = []
    for rec in range(edf.num_records):
        dur = val(duration_data, rec)
        if dur <= 0:
            continue

        record_date = edf.start_datetime + timedelta(days=rec)
        date_str = record_date.strftime("%Y-%m-%d")

        lk50 = val(leak_50_data, rec) * 60.0
        lk95 = val(leak_95_data, rec) * 60.0
        lkmax = val(leak_max_data, rec) * 60.0

        sp50 = val(spo2_50_data, rec)
        sp95 = val(spo2_95_data, rec)
        sp50 = sp50 if sp50 > 0 else 0
        sp95 = sp95 if sp95 > 0 else 0

        # MaskOn/MaskOff pairs
        mask_pairs = []
        if mask_on_data and mask_off_data:
            base = rec * 10
            for slot in range(10):
                idx = base + slot
                if idx >= len(mask_on_data):
                    break
                on_min = mask_on_data[idx]
                off_min = mask_off_data[idx]
                if on_min < 0 or off_min < 0:
                    continue
                if on_min == 0 and off_min == 0:
                    continue
                if on_min == off_min:
                    continue
                on_dt = record_date + timedelta(minutes=on_min)
                off_dt = record_date + timedelta(minutes=off_min)
                mask_pairs.append({
                    "on": on_dt.strftime("%Y-%m-%dT%H:%M:%S"),
                    "off": off_dt.strftime("%Y-%m-%dT%H:%M:%S"),
                })

        records.append((
            DEVICE_ID, date_str, json.dumps(mask_pairs),
            int(val(mask_events_data, rec)), dur, val(patient_hours_data, rec),
            val(ahi_data, rec), val(hi_data, rec), val(ai_data, rec),
            val(oai_data, rec), val(cai_data, rec), val(uai_data, rec),
            val(rin_data, rec), val(csr_data, rec),
            val(mask_press_50_data, rec), val(mask_press_95_data, rec),
            val(mask_press_max_data, rec),
            lk50, lk95, lkmax,
            sp50, sp95,
            val(resp_rate_50_data, rec), val(tid_vol_50_data, rec),
            val(min_vent_50_data, rec),
            int(val(mode_data, rec)), val(epr_level_data, rec),
            val(press_setting_data, rec),
            int(val(fault_device_data, rec)), int(val(fault_alarm_data, rec)),
        ))

    print(f"  Parsed {len(records)} therapy days")
    return records


def upsert_records(records):
    """Upsert records into cpap_daily_summary."""
    conn = psycopg2.connect(DB_CONN)
    try:
        cur = conn.cursor()
        sql = """
            INSERT INTO cpap_daily_summary
                (device_id, record_date, mask_pairs, mask_events, duration_minutes, patient_hours,
                 ahi, hi, ai, oai, cai, uai, rin, csr,
                 mask_press_50, mask_press_95, mask_press_max,
                 leak_50, leak_95, leak_max,
                 spo2_50, spo2_95,
                 resp_rate_50, tid_vol_50, min_vent_50,
                 mode, epr_level, pressure_setting,
                 fault_device, fault_alarm, updated_at)
            VALUES %s
            ON CONFLICT (device_id, record_date) DO UPDATE SET
                mask_pairs = EXCLUDED.mask_pairs,
                mask_events = EXCLUDED.mask_events,
                duration_minutes = EXCLUDED.duration_minutes,
                patient_hours = EXCLUDED.patient_hours,
                ahi = EXCLUDED.ahi, hi = EXCLUDED.hi, ai = EXCLUDED.ai,
                oai = EXCLUDED.oai, cai = EXCLUDED.cai, uai = EXCLUDED.uai,
                rin = EXCLUDED.rin, csr = EXCLUDED.csr,
                mask_press_50 = EXCLUDED.mask_press_50,
                mask_press_95 = EXCLUDED.mask_press_95,
                mask_press_max = EXCLUDED.mask_press_max,
                leak_50 = EXCLUDED.leak_50, leak_95 = EXCLUDED.leak_95,
                leak_max = EXCLUDED.leak_max,
                spo2_50 = EXCLUDED.spo2_50, spo2_95 = EXCLUDED.spo2_95,
                resp_rate_50 = EXCLUDED.resp_rate_50,
                tid_vol_50 = EXCLUDED.tid_vol_50,
                min_vent_50 = EXCLUDED.min_vent_50,
                mode = EXCLUDED.mode, epr_level = EXCLUDED.epr_level,
                pressure_setting = EXCLUDED.pressure_setting,
                fault_device = EXCLUDED.fault_device,
                fault_alarm = EXCLUDED.fault_alarm,
                updated_at = NOW()
        """
        template = "(%s, %s, %s::jsonb, %s, %s, %s, %s, %s, %s, %s, %s, %s, %s, %s, %s, %s, %s, %s, %s, %s, %s, %s, %s, %s, %s, %s, %s, %s, %s, %s, NOW())"
        execute_values(cur, sql, records, template=template, page_size=100)
        conn.commit()
        print(f"  Upserted {len(records)} records")
    finally:
        conn.close()


def main():
    if len(sys.argv) < 2:
        print(f"Usage: {sys.argv[0]} <str1.edf> [str2.edf ...]")
        sys.exit(1)

    all_records = []
    for filepath in sys.argv[1:]:
        print(f"\nParsing {filepath}...")
        records = parse_str(filepath)
        all_records.extend(records)

    if not all_records:
        print("No therapy days found.")
        sys.exit(1)

    # Deduplicate by (device_id, record_date) — keep last occurrence (current > old)
    seen = {}
    for r in all_records:
        key = (r[0], r[1])
        seen[key] = r
    deduped = list(seen.values())
    deduped.sort(key=lambda r: r[1])

    print(f"\nTotal unique therapy days: {len(deduped)}")
    print(f"Date range: {deduped[0][1]} to {deduped[-1][1]}")

    print("\nUpserting to PostgreSQL...")
    upsert_records(deduped)

    print("\nBackfill complete.")


if __name__ == "__main__":
    main()
