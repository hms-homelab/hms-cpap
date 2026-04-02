-- HMS-CPAP Database Schema — SQLite
--
-- SQLite schema is auto-created by HMS-CPAP on first run.
-- This file is provided for reference and manual database setup.
--
-- Version: 3.2.0
-- Date: 2026-04-02

-- Device registry
CREATE TABLE IF NOT EXISTS cpap_devices (
    device_id       TEXT PRIMARY KEY,
    device_name     TEXT,
    serial_number   TEXT,
    model_id        INTEGER DEFAULT 0,
    version_id      INTEGER DEFAULT 0,
    last_seen       TEXT DEFAULT (datetime('now')),
    created_at      TEXT DEFAULT (datetime('now'))
);

-- CPAP Sessions
CREATE TABLE IF NOT EXISTS cpap_sessions (
    id                INTEGER PRIMARY KEY AUTOINCREMENT,
    device_id         TEXT NOT NULL,
    session_start     TEXT NOT NULL,
    session_end       TEXT,
    duration_seconds  INTEGER DEFAULT 0,
    data_records      INTEGER DEFAULT 0,
    brp_file_path     TEXT,
    eve_file_path     TEXT,
    sad_file_path     TEXT,
    pld_file_path     TEXT,
    csl_file_path     TEXT,
    checkpoint_files  TEXT,
    force_completed   INTEGER DEFAULT 0,
    created_at        TEXT DEFAULT (datetime('now')),
    updated_at        TEXT DEFAULT (datetime('now')),
    UNIQUE (device_id, session_start)
);

CREATE INDEX IF NOT EXISTS idx_cpap_sessions_device
    ON cpap_sessions(device_id);
CREATE INDEX IF NOT EXISTS idx_cpap_sessions_start
    ON cpap_sessions(device_id, session_start);

-- Session metrics (one row per session)
CREATE TABLE IF NOT EXISTS cpap_session_metrics (
    id                     INTEGER PRIMARY KEY AUTOINCREMENT,
    session_id             INTEGER NOT NULL UNIQUE,
    total_events           INTEGER DEFAULT 0,
    ahi                    REAL DEFAULT 0,
    obstructive_apneas     INTEGER DEFAULT 0,
    central_apneas         INTEGER DEFAULT 0,
    hypopneas              INTEGER DEFAULT 0,
    reras                  INTEGER DEFAULT 0,
    clear_airway_apneas    INTEGER DEFAULT 0,
    avg_event_duration     REAL,
    max_event_duration     REAL,
    time_in_apnea_percent  REAL,
    avg_spo2               REAL,
    min_spo2               REAL,
    avg_heart_rate         INTEGER,
    max_heart_rate         INTEGER,
    min_heart_rate         INTEGER,
    avg_mask_pressure      REAL,
    avg_epr_pressure       REAL,
    avg_snore              REAL,
    leak_p50               REAL,
    leak_p95               REAL,
    avg_leak_rate          REAL,
    max_leak_rate          REAL,
    avg_target_ventilation REAL,
    therapy_mode           INTEGER,
    created_at             TEXT DEFAULT (datetime('now')),
    FOREIGN KEY (session_id) REFERENCES cpap_sessions(id) ON DELETE CASCADE
);

-- Breathing summaries
CREATE TABLE IF NOT EXISTS cpap_breathing_summary (
    id              INTEGER PRIMARY KEY AUTOINCREMENT,
    session_id      INTEGER NOT NULL,
    timestamp       TEXT NOT NULL,
    avg_flow_rate   REAL,
    max_flow_rate   REAL,
    min_flow_rate   REAL,
    avg_pressure    REAL,
    max_pressure    REAL,
    min_pressure    REAL,
    UNIQUE (session_id, timestamp),
    FOREIGN KEY (session_id) REFERENCES cpap_sessions(id) ON DELETE CASCADE
);

-- Events
CREATE TABLE IF NOT EXISTS cpap_events (
    id                INTEGER PRIMARY KEY AUTOINCREMENT,
    session_id        INTEGER NOT NULL,
    event_type        TEXT,
    event_timestamp   TEXT NOT NULL,
    duration_seconds  REAL DEFAULT 0,
    details           TEXT,
    UNIQUE (session_id, event_timestamp),
    FOREIGN KEY (session_id) REFERENCES cpap_sessions(id) ON DELETE CASCADE
);

-- Vitals
CREATE TABLE IF NOT EXISTS cpap_vitals (
    id          INTEGER PRIMARY KEY AUTOINCREMENT,
    session_id  INTEGER NOT NULL,
    timestamp   TEXT NOT NULL,
    spo2        REAL,
    heart_rate  INTEGER,
    UNIQUE (session_id, timestamp),
    FOREIGN KEY (session_id) REFERENCES cpap_sessions(id) ON DELETE CASCADE
);

-- Calculated metrics (per-minute)
CREATE TABLE IF NOT EXISTS cpap_calculated_metrics (
    id                   INTEGER PRIMARY KEY AUTOINCREMENT,
    session_id           INTEGER NOT NULL,
    timestamp            TEXT NOT NULL,
    respiratory_rate     REAL,
    tidal_volume         REAL,
    minute_ventilation   REAL,
    inspiratory_time     REAL,
    expiratory_time      REAL,
    ie_ratio             REAL,
    flow_limitation      REAL,
    leak_rate            REAL,
    flow_p95             REAL,
    flow_p90             REAL,
    pressure_p95         REAL,
    pressure_p90         REAL,
    mask_pressure        REAL,
    epr_pressure         REAL,
    snore_index          REAL,
    target_ventilation   REAL,
    UNIQUE (session_id, timestamp),
    FOREIGN KEY (session_id) REFERENCES cpap_sessions(id) ON DELETE CASCADE
);

-- STR daily summary
CREATE TABLE IF NOT EXISTS cpap_daily_summary (
    id                INTEGER PRIMARY KEY AUTOINCREMENT,
    device_id         TEXT NOT NULL,
    record_date       TEXT NOT NULL,
    mask_pairs        TEXT DEFAULT '[]',
    mask_events       INTEGER DEFAULT 0,
    duration_minutes  REAL DEFAULT 0,
    patient_hours     REAL DEFAULT 0,
    ahi               REAL, hi REAL, ai REAL, oai REAL, cai REAL, uai REAL,
    rin               REAL, csr REAL,
    mask_press_50     REAL, mask_press_95 REAL, mask_press_max REAL,
    leak_50           REAL, leak_95 REAL, leak_max REAL,
    spo2_50           REAL, spo2_95 REAL,
    resp_rate_50      REAL, tid_vol_50 REAL, min_vent_50 REAL,
    mode              INTEGER, epr_level REAL, pressure_setting REAL,
    fault_device      INTEGER DEFAULT 0,
    fault_alarm       INTEGER DEFAULT 0,
    created_at        TEXT DEFAULT (datetime('now')),
    updated_at        TEXT DEFAULT (datetime('now')),
    UNIQUE (device_id, record_date)
);

-- AI-generated summaries
CREATE TABLE IF NOT EXISTS cpap_summaries (
    id              INTEGER PRIMARY KEY AUTOINCREMENT,
    device_id       TEXT NOT NULL,
    period          TEXT NOT NULL CHECK (period IN ('daily', 'weekly', 'monthly')),
    range_start     TEXT NOT NULL,
    range_end       TEXT NOT NULL,
    nights_count    INTEGER NOT NULL DEFAULT 1,
    avg_ahi         REAL,
    avg_usage_hours REAL,
    compliance_pct  REAL,
    summary_text    TEXT NOT NULL,
    created_at      TEXT NOT NULL DEFAULT (datetime('now'))
);

CREATE INDEX IF NOT EXISTS idx_cpap_summaries_device_period
    ON cpap_summaries(device_id, period, range_end DESC);
