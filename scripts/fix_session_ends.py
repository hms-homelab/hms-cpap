#!/usr/bin/env python3
"""
Fix sessions with NULL session_end by reading the EDF header of the
session's own BRP file to calculate the actual data end time.

session_end = EDF start time + (actual_records * record_duration)

Usage:
    python3 fix_session_ends.py
"""

import os
import struct
from datetime import datetime, timedelta
import psycopg2

DB_CONN = "host=localhost port=5432 dbname=cpap_monitoring user=maestro password=maestro_postgres_2026_secure"
DEVICE_ID = "cpap_resmed_23243570851"
ARCHIVE_BASE = "/mnt/public/cpap_data"


def read_edf_session_end(filepath):
    """Read EDF header and calculate session end from data."""
    with open(filepath, "rb") as f:
        hdr = f.read(256)

    # Start date (bytes 168-176): dd.mm.yy
    date_str = hdr[168:176].decode().strip()
    # Start time (bytes 176-184): hh.mm.ss
    time_str = hdr[176:184].decode().strip()
    # Num data records (bytes 236-244)
    num_records_str = hdr[236:244].decode().strip()
    # Record duration (bytes 244-252)
    record_duration_str = hdr[244:252].decode().strip()

    # Parse start time
    dd, mm, yy = date_str.split(".")
    hh, mi, ss = time_str.split(".")
    year = 2000 + int(yy) if int(yy) < 85 else 1900 + int(yy)
    start = datetime(year, int(mm), int(dd), int(hh), int(mi), int(ss))

    record_duration = float(record_duration_str)
    num_records = int(num_records_str)

    # num_records=-1 means the header wasn't finalized. Calculate from file size.
    if num_records < 0:
        file_size = os.path.getsize(filepath)
        # Read number of signals to calculate header size
        num_signals = int(hdr[252:256].decode().strip())
        header_size = 256 + num_signals * 256
        # Read samples per record for each signal
        f2 = open(filepath, "rb")
        f2.seek(256 + num_signals * 216)  # offset to "nr of samples" field
        samples_per_record = 0
        for i in range(num_signals):
            spr = int(f2.read(8).decode().strip())
            samples_per_record += spr
        f2.close()
        data_size = file_size - header_size
        bytes_per_record = samples_per_record * 2  # 16-bit samples
        if bytes_per_record > 0:
            num_records = data_size // bytes_per_record

    if num_records <= 0:
        return None, None

    end = start + timedelta(seconds=num_records * record_duration)
    duration = int(num_records * record_duration)
    return end, duration


def main():
    conn = psycopg2.connect(DB_CONN)
    cur = conn.cursor()

    cur.execute("""
        SELECT id, session_start, brp_file_path, checkpoint_files
        FROM cpap_sessions
        WHERE device_id = %s AND session_end IS NULL
        ORDER BY session_start
    """, (DEVICE_ID,))

    rows = cur.fetchall()
    print(f"Found {len(rows)} session(s) without session_end\n")

    fixed = 0
    for session_id, session_start, brp_path, checkpoint_files in rows:
        if not brp_path:
            print(f"  id={session_id}  start={session_start}  SKIP (no brp_file_path)")
            continue

        # Find the last BRP for this session from checkpoint_files or brp_file_path
        date_folder = os.path.dirname(brp_path)
        full_folder = os.path.join(ARCHIVE_BASE, date_folder)

        # Get all BRP files belonging to this session from checkpoint_files
        session_brps = []
        if checkpoint_files and isinstance(checkpoint_files, dict):
            for fname in sorted(checkpoint_files.keys()):
                if "_BRP.edf" in fname:
                    full_path = os.path.join(full_folder, fname)
                    if os.path.exists(full_path):
                        session_brps.append(full_path)

        # Fallback to the brp_file_path
        if not session_brps:
            full_path = os.path.join(ARCHIVE_BASE, brp_path)
            if os.path.exists(full_path):
                session_brps = [full_path]

        if not session_brps:
            print(f"  id={session_id}  start={session_start}  SKIP (BRP not found)")
            continue

        # Use the LAST BRP file for this session to get session_end
        last_brp = session_brps[-1]
        session_end, duration = read_edf_session_end(last_brp)

        if not session_end:
            print(f"  id={session_id}  start={session_start}  SKIP (no records in {os.path.basename(last_brp)})")
            continue

        # Duration should be from session_start to session_end
        duration = int((session_end - session_start).total_seconds())

        if duration <= 0:
            print(f"  id={session_id}  start={session_start}  SKIP (end={session_end} <= start)")
            continue

        cur.execute("""
            UPDATE cpap_sessions
            SET session_end = %s, duration_seconds = %s
            WHERE id = %s
        """, (session_end, duration, session_id))

        print(f"  id={session_id}  start={session_start}  end={session_end}  dur={duration/3600:.1f}h  brp={os.path.basename(last_brp)}")
        fixed += 1

    conn.commit()
    cur.close()
    conn.close()

    print(f"\nFixed {fixed}/{len(rows)} session(s)")


if __name__ == "__main__":
    main()
