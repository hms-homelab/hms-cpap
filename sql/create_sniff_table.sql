-- Fysetc sniff mode: raw GPIO 33 PCNT pulse count data for bus pattern analysis
-- One row per 1-second batch (10 samples at 100ms each)
CREATE TABLE IF NOT EXISTS cpap_sniff_data (
    id SERIAL PRIMARY KEY,
    device_id VARCHAR(64) NOT NULL,
    timestamp TIMESTAMPTZ NOT NULL,
    uptime_sec INTEGER NOT NULL,
    seq INTEGER NOT NULL,
    pulse_counts SMALLINT[] NOT NULL,
    interval_ms SMALLINT NOT NULL DEFAULT 100,
    therapy_detected BOOLEAN NOT NULL DEFAULT FALSE,
    idle_ms INTEGER NOT NULL DEFAULT 0,
    created_at TIMESTAMPTZ NOT NULL DEFAULT NOW()
);

-- Index for time-range queries and pattern analysis
CREATE INDEX IF NOT EXISTS idx_sniff_device_time
    ON cpap_sniff_data (device_id, timestamp);

CREATE INDEX IF NOT EXISTS idx_sniff_therapy
    ON cpap_sniff_data (device_id, therapy_detected, timestamp);
