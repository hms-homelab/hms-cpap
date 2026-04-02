-- HMS-CPAP Database Schema
-- PostgreSQL 13+
--
-- This schema is automatically created by HMS-CPAP when using SQLite.
-- For PostgreSQL, run this file manually before first start:
--   psql -h localhost -U maestro -d cpap_monitoring -f scripts/schema.sql
--
-- Version: 3.2.0
-- Date: 2026-04-02

-- =============================================================================
-- Core Tables
-- =============================================================================

-- Device registry
CREATE TABLE IF NOT EXISTS cpap_devices (
    device_id       TEXT PRIMARY KEY,
    device_name     TEXT,
    serial_number   TEXT,
    model_id        INT DEFAULT 0,
    version_id      INT DEFAULT 0,
    last_seen       TIMESTAMP DEFAULT NOW(),
    created_at      TIMESTAMP DEFAULT NOW()
);

-- CPAP Sessions
CREATE TABLE IF NOT EXISTS cpap_sessions (
    id                SERIAL PRIMARY KEY,
    device_id         TEXT NOT NULL,
    session_start     TIMESTAMP NOT NULL,
    session_end       TIMESTAMP,
    duration_seconds  INT DEFAULT 0,
    data_records      INT DEFAULT 0,
    brp_file_path     TEXT,
    eve_file_path     TEXT,
    sad_file_path     TEXT,
    pld_file_path     TEXT,
    csl_file_path     TEXT,
    checkpoint_files  JSONB DEFAULT '{}'::jsonb,
    force_completed   BOOLEAN DEFAULT FALSE,
    session_status    VARCHAR(50) DEFAULT 'in_progress',
    created_at        TIMESTAMP DEFAULT NOW(),
    updated_at        TIMESTAMP DEFAULT NOW(),
    UNIQUE (device_id, session_start)
);

CREATE INDEX IF NOT EXISTS idx_cpap_sessions_device
    ON cpap_sessions(device_id);
CREATE INDEX IF NOT EXISTS idx_cpap_sessions_start
    ON cpap_sessions(device_id, session_start DESC);
CREATE INDEX IF NOT EXISTS idx_cpap_sessions_status
    ON cpap_sessions(device_id, session_status);
CREATE INDEX IF NOT EXISTS idx_cpap_sessions_checkpoint_files
    ON cpap_sessions USING GIN(checkpoint_files);

-- Session metrics (one row per session)
CREATE TABLE IF NOT EXISTS cpap_session_metrics (
    id                     SERIAL PRIMARY KEY,
    session_id             INT NOT NULL UNIQUE,
    total_events           INT DEFAULT 0,
    ahi                    FLOAT DEFAULT 0,
    obstructive_apneas     INT DEFAULT 0,
    central_apneas         INT DEFAULT 0,
    hypopneas              INT DEFAULT 0,
    reras                  INT DEFAULT 0,
    clear_airway_apneas    INT DEFAULT 0,
    avg_event_duration     FLOAT,
    max_event_duration     FLOAT,
    time_in_apnea_percent  FLOAT,
    avg_spo2               FLOAT,
    min_spo2               FLOAT,
    avg_heart_rate         INT,
    max_heart_rate         INT,
    min_heart_rate         INT,
    avg_mask_pressure      FLOAT,
    avg_epr_pressure       FLOAT,
    avg_snore              FLOAT,
    leak_p50               FLOAT,
    leak_p95               FLOAT,
    avg_leak_rate          FLOAT,
    max_leak_rate          FLOAT,
    avg_target_ventilation FLOAT,
    therapy_mode           INT,
    created_at             TIMESTAMP DEFAULT NOW(),
    FOREIGN KEY (session_id) REFERENCES cpap_sessions(id) ON DELETE CASCADE
);

-- Breathing summaries (per-interval aggregates)
CREATE TABLE IF NOT EXISTS cpap_breathing_summary (
    id              SERIAL PRIMARY KEY,
    session_id      INT NOT NULL,
    timestamp       TIMESTAMP NOT NULL,
    avg_flow_rate   FLOAT,
    max_flow_rate   FLOAT,
    min_flow_rate   FLOAT,
    avg_pressure    FLOAT,
    max_pressure    FLOAT,
    min_pressure    FLOAT,
    UNIQUE (session_id, timestamp),
    FOREIGN KEY (session_id) REFERENCES cpap_sessions(id) ON DELETE CASCADE
);

-- Events (apneas, hypopneas, RERAs)
CREATE TABLE IF NOT EXISTS cpap_events (
    id                SERIAL PRIMARY KEY,
    session_id        INT NOT NULL,
    event_type        TEXT,
    event_timestamp   TIMESTAMP NOT NULL,
    duration_seconds  FLOAT DEFAULT 0,
    details           TEXT,
    UNIQUE (session_id, event_timestamp),
    FOREIGN KEY (session_id) REFERENCES cpap_sessions(id) ON DELETE CASCADE
);

-- Vitals (SpO2, heart rate)
CREATE TABLE IF NOT EXISTS cpap_vitals (
    id          SERIAL PRIMARY KEY,
    session_id  INT NOT NULL,
    timestamp   TIMESTAMP NOT NULL,
    spo2        FLOAT,
    heart_rate  INT,
    UNIQUE (session_id, timestamp),
    FOREIGN KEY (session_id) REFERENCES cpap_sessions(id) ON DELETE CASCADE
);

-- Calculated metrics (per-minute respiratory data)
CREATE TABLE IF NOT EXISTS cpap_calculated_metrics (
    id                   SERIAL PRIMARY KEY,
    session_id           INT NOT NULL,
    timestamp            TIMESTAMP NOT NULL,
    respiratory_rate     FLOAT,
    tidal_volume         FLOAT,
    minute_ventilation   FLOAT,
    inspiratory_time     FLOAT,
    expiratory_time      FLOAT,
    ie_ratio             FLOAT,
    flow_limitation      FLOAT,
    leak_rate            FLOAT,
    flow_p95             FLOAT,
    flow_p90             FLOAT,
    pressure_p95         FLOAT,
    pressure_p90         FLOAT,
    mask_pressure        FLOAT,
    epr_pressure         FLOAT,
    snore_index          FLOAT,
    target_ventilation   FLOAT,
    UNIQUE (session_id, timestamp),
    FOREIGN KEY (session_id) REFERENCES cpap_sessions(id) ON DELETE CASCADE
);

-- STR daily summary (therapy day records from STR.EDF)
CREATE TABLE IF NOT EXISTS cpap_daily_summary (
    id                SERIAL PRIMARY KEY,
    device_id         TEXT NOT NULL,
    record_date       DATE NOT NULL,
    mask_pairs        JSONB DEFAULT '[]',
    mask_events       INT DEFAULT 0,
    duration_minutes  FLOAT DEFAULT 0,
    patient_hours     FLOAT DEFAULT 0,
    ahi               FLOAT, hi FLOAT, ai FLOAT, oai FLOAT, cai FLOAT, uai FLOAT,
    rin               FLOAT, csr FLOAT,
    mask_press_50     FLOAT, mask_press_95 FLOAT, mask_press_max FLOAT,
    leak_50           FLOAT, leak_95 FLOAT, leak_max FLOAT,
    spo2_50           FLOAT, spo2_95 FLOAT,
    resp_rate_50      FLOAT, tid_vol_50 FLOAT, min_vent_50 FLOAT,
    mode              INT, epr_level FLOAT, pressure_setting FLOAT,
    fault_device      INT DEFAULT 0,
    fault_alarm       INT DEFAULT 0,
    created_at        TIMESTAMP DEFAULT NOW(),
    updated_at        TIMESTAMP DEFAULT NOW(),
    UNIQUE (device_id, record_date)
);

-- AI-generated summaries (daily, weekly, monthly)
CREATE TABLE IF NOT EXISTS cpap_summaries (
    id              SERIAL PRIMARY KEY,
    device_id       TEXT NOT NULL,
    period          TEXT NOT NULL CHECK (period IN ('daily', 'weekly', 'monthly')),
    range_start     DATE NOT NULL,
    range_end       DATE NOT NULL,
    nights_count    INT NOT NULL DEFAULT 1,
    avg_ahi         DOUBLE PRECISION,
    avg_usage_hours DOUBLE PRECISION,
    compliance_pct  DOUBLE PRECISION,
    summary_text    TEXT NOT NULL,
    created_at      TIMESTAMP NOT NULL DEFAULT NOW()
);

CREATE INDEX IF NOT EXISTS idx_cpap_summaries_device_period
    ON cpap_summaries(device_id, period, range_end DESC);

-- =============================================================================
-- Triggers
-- =============================================================================

CREATE OR REPLACE FUNCTION update_updated_at_column()
RETURNS TRIGGER AS $$
BEGIN
    NEW.updated_at = NOW();
    RETURN NEW;
END;
$$ language 'plpgsql';

DO $$ BEGIN
    CREATE TRIGGER update_cpap_sessions_updated_at
        BEFORE UPDATE ON cpap_sessions
        FOR EACH ROW EXECUTE FUNCTION update_updated_at_column();
EXCEPTION WHEN duplicate_object THEN NULL;
END $$;

DO $$ BEGIN
    CREATE TRIGGER update_cpap_daily_summary_updated_at
        BEFORE UPDATE ON cpap_daily_summary
        FOR EACH ROW EXECUTE FUNCTION update_updated_at_column();
EXCEPTION WHEN duplicate_object THEN NULL;
END $$;
