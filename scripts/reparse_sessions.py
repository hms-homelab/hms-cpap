#!/usr/bin/env python3
"""
Reparse sessions for a date range.

Deletes existing session data (sessions + cascaded metrics/events/vitals)
for the given date range, so the next burst cycle on the Pi re-discovers
and re-parses those dates from ezShare.

Usage:
    python3 reparse_sessions.py 2025-08-15 2025-09-15
    python3 reparse_sessions.py 2026-02-12              # single day

Matches sessions by their date folder (from brp_file_path), not session_start,
because sessions starting at e.g. 23:00 on Sep 9 are in folder 20250909 but
have session_start on Sep 9, while sessions starting at 00:10 on Sep 10 are
also in folder 20250909 but have session_start on Sep 10.

All child tables (events, metrics, vitals, breathing_summary, calculated_metrics)
cascade-delete automatically via FK constraints.
"""

import sys
from datetime import datetime, timedelta
import psycopg2

DB_CONN = "host=localhost port=5432 dbname=cpap_monitoring user=maestro password=maestro_postgres_2026_secure"
DEVICE_ID = "cpap_resmed_23243570851"


def date_range(start, end):
    """Generate YYYYMMDD strings for each day in range."""
    current = start
    while current <= end:
        yield current.strftime("%Y%m%d")
        current += timedelta(days=1)


def main():
    if len(sys.argv) < 2:
        print(f"Usage: {sys.argv[0]} <start_date> [end_date]")
        print(f"  Dates in YYYY-MM-DD format")
        print(f"  Single date: reparse that day only")
        sys.exit(1)

    start_date = sys.argv[1]
    end_date = sys.argv[2] if len(sys.argv) > 2 else start_date

    try:
        start = datetime.strptime(start_date, "%Y-%m-%d")
        end = datetime.strptime(end_date, "%Y-%m-%d")
    except ValueError:
        print("Error: dates must be YYYY-MM-DD format")
        sys.exit(1)

    if end < start:
        start, end = end, start

    # Build list of date folders to match
    folders = list(date_range(start, end))
    folder_patterns = [f"%DATALOG/{f}/%" for f in folders]

    conn = psycopg2.connect(DB_CONN)
    try:
        cur = conn.cursor()

        # Find sessions whose BRP file path matches any of the date folders
        conditions = " OR ".join(["brp_file_path::text LIKE %s"] * len(folder_patterns))
        query = f"""
            SELECT id, session_start, duration_seconds,
                   brp_file_path, eve_file_path
            FROM cpap_sessions
            WHERE device_id = %s AND ({conditions})
            ORDER BY session_start
        """
        cur.execute(query, [DEVICE_ID] + folder_patterns)

        sessions = cur.fetchall()
        if not sessions:
            print(f"No sessions found for date folders {folders[0]} to {folders[-1]}")
            sys.exit(0)

        print(f"Found {len(sessions)} session(s) to delete and reparse:\n")
        for sid, ss, dur, brp, eve in sessions:
            dur_h = (dur or 0) / 3600
            print(f"  id={sid}  start={ss}  dur={dur_h:.1f}h  brp={brp or 'none'}")

        # Count child rows
        session_ids = [s[0] for s in sessions]
        placeholders = ",".join(["%s"] * len(session_ids))

        for table in ["cpap_events", "cpap_session_metrics", "cpap_calculated_metrics",
                       "cpap_vitals", "cpap_breathing_summary"]:
            cur.execute(f"SELECT COUNT(*) FROM {table} WHERE session_id IN ({placeholders})", session_ids)
            count = cur.fetchone()[0]
            if count > 0:
                print(f"  + {count} rows in {table} (cascade delete)")

        print(f"\nThis will DELETE these sessions and all related data.")
        print(f"The Pi will re-discover and re-parse them on the next burst cycle.")
        confirm = input("\nProceed? [y/N] ").strip().lower()
        if confirm != "y":
            print("Aborted.")
            sys.exit(0)

        # Delete sessions (child rows cascade)
        cur.execute(
            f"DELETE FROM cpap_sessions WHERE device_id = %s AND id IN ({placeholders})",
            [DEVICE_ID] + session_ids
        )
        deleted = cur.rowcount
        conn.commit()

        print(f"\nDeleted {deleted} session(s) and all cascaded data.")
        print(f"The Pi will re-parse these on the next burst cycle.")
        print(f"Monitor: ssh aamat@192.168.2.73 'journalctl -u hms-cpap -f'")

    finally:
        conn.close()


if __name__ == "__main__":
    main()
