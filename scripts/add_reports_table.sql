-- Migration: add cpap_reports table to cpap_monitoring
-- Run: psql -h localhost -U maestro -d cpap_monitoring -f add_reports_table.sql

CREATE TABLE IF NOT EXISTS cpap_reports (
    id              SERIAL PRIMARY KEY,
    device_id       TEXT NOT NULL,
    range_start     DATE NOT NULL,
    range_end       DATE NOT NULL,
    nights_count    INTEGER,
    filename        TEXT NOT NULL,
    filepath        TEXT NOT NULL,
    status          TEXT NOT NULL DEFAULT 'pending',  -- pending, generating, ready, error
    error_msg       TEXT,
    created_at      TIMESTAMP DEFAULT NOW(),
    completed_at    TIMESTAMP
);

CREATE INDEX IF NOT EXISTS idx_cpap_reports_device ON cpap_reports (device_id, created_at DESC);
