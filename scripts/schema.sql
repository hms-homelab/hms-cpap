-- HMS-CPAP Database Schema
-- PostgreSQL 13+
--
-- This schema is automatically created by HMS-CPAP on first run.
-- This file is provided for reference and manual database setup.
--
-- Version: 1.1.1
-- Date: 2026-02-14

-- =============================================================================
-- Main Tables
-- =============================================================================

-- CPAP Sessions table
-- Stores therapy session data from ResMed devices
CREATE TABLE IF NOT EXISTS cpap_sessions (
    id SERIAL PRIMARY KEY,

    -- Device identification
    device_id VARCHAR(255) NOT NULL,
    device_name VARCHAR(255),

    -- Session timing
    session_start TIMESTAMP NOT NULL,
    session_end TIMESTAMP,
    duration_seconds INT DEFAULT 0,

    -- Session metadata
    session_status VARCHAR(50) DEFAULT 'in_progress',  -- 'in_progress', 'completed'
    force_completed BOOLEAN DEFAULT FALSE,              -- Manual override: skip in burst cycle

    -- Checkpoint file tracking (JSONB)
    -- Stores individual file sizes for stop detection
    -- Example: {"20260209_003608_BRP.edf": 576, "20260209_003609_PLD.edf": 55}
    checkpoint_files JSONB DEFAULT '{}'::jsonb,

    -- File paths (permanent archive references)
    brp_file_path TEXT,    -- Breathing waveform data
    pld_file_path TEXT,    -- Pressure/leak data
    sad_file_path TEXT,    -- Session statistics
    eve_file_path TEXT,    -- Event data
    csl_file_path TEXT,    -- Session log

    -- Therapy metrics (real-time from checkpoints)
    ahi FLOAT DEFAULT 0,                    -- Apnea-Hypopnea Index (events/hour)
    leak_rate FLOAT DEFAULT 0,              -- L/min
    pressure_current FLOAT DEFAULT 0,       -- cmH2O
    pressure_min FLOAT DEFAULT 0,
    pressure_max FLOAT DEFAULT 0,
    pressure_avg FLOAT DEFAULT 0,

    -- Events (from EVE file)
    total_apneas INT DEFAULT 0,
    obstructive_apneas INT DEFAULT 0,
    central_apneas INT DEFAULT 0,
    hypopneas INT DEFAULT 0,
    rera INT DEFAULT 0,                     -- Respiratory Effort Related Arousal

    -- Vitals (from SAD file)
    spo2_avg FLOAT DEFAULT 0,               -- SpO2 average %
    spo2_min FLOAT DEFAULT 0,               -- SpO2 minimum %
    heart_rate_avg FLOAT DEFAULT 0,         -- bpm

    -- Breathing summary
    respiratory_rate FLOAT DEFAULT 0,       -- breaths/min
    tidal_volume FLOAT DEFAULT 0,           -- mL
    minute_ventilation FLOAT DEFAULT 0,     -- L/min

    -- Timestamps
    created_at TIMESTAMP DEFAULT NOW(),
    updated_at TIMESTAMP DEFAULT NOW(),

    -- Indexes
    UNIQUE (device_id, session_start)
);

-- Index for fast session lookups
CREATE INDEX IF NOT EXISTS idx_cpap_sessions_device_start
    ON cpap_sessions(device_id, session_start DESC);

-- Index for status queries
CREATE INDEX IF NOT EXISTS idx_cpap_sessions_status
    ON cpap_sessions(device_id, session_status);

-- Index for checkpoint file tracking (JSONB)
CREATE INDEX IF NOT EXISTS idx_cpap_sessions_checkpoint_files
    ON cpap_sessions USING GIN(checkpoint_files);

-- =============================================================================
-- Functions
-- =============================================================================

-- Update timestamp trigger
CREATE OR REPLACE FUNCTION update_updated_at_column()
RETURNS TRIGGER AS $$
BEGIN
    NEW.updated_at = NOW();
    RETURN NEW;
END;
$$ language 'plpgsql';

CREATE TRIGGER update_cpap_sessions_updated_at
    BEFORE UPDATE ON cpap_sessions
    FOR EACH ROW
    EXECUTE FUNCTION update_updated_at_column();

-- =============================================================================
-- Optional: Device Registry Table
-- =============================================================================

-- Device registry (optional, for multi-device deployments)
CREATE TABLE IF NOT EXISTS cpap_devices (
    id SERIAL PRIMARY KEY,
    device_id VARCHAR(255) UNIQUE NOT NULL,
    device_name VARCHAR(255) NOT NULL,
    device_model VARCHAR(255),
    serial_number VARCHAR(255),

    -- Last seen tracking
    last_seen TIMESTAMP,
    last_session_start TIMESTAMP,

    -- Metadata
    created_at TIMESTAMP DEFAULT NOW(),
    updated_at TIMESTAMP DEFAULT NOW()
);

CREATE TRIGGER update_cpap_devices_updated_at
    BEFORE UPDATE ON cpap_devices
    FOR EACH ROW
    EXECUTE FUNCTION update_updated_at_column();

-- =============================================================================
-- Sample Queries
-- =============================================================================

-- Get latest session for a device
-- SELECT * FROM cpap_sessions
-- WHERE device_id = 'resmed_airsense10'
-- ORDER BY session_start DESC LIMIT 1;

-- Get all sessions from last 7 days
-- SELECT * FROM cpap_sessions
-- WHERE session_start >= NOW() - INTERVAL '7 days'
-- ORDER BY session_start DESC;

-- Get average AHI over last month
-- SELECT AVG(ahi) as avg_ahi
-- FROM cpap_sessions
-- WHERE session_start >= NOW() - INTERVAL '30 days'
--   AND session_status = 'completed';

-- Get sessions with high leak rate
-- SELECT session_start, leak_rate
-- FROM cpap_sessions
-- WHERE leak_rate > 24
-- ORDER BY session_start DESC;

-- Check for stopped sessions (checkpoint tracking)
-- SELECT session_start, session_end, checkpoint_files
-- FROM cpap_sessions
-- WHERE session_end IS NOT NULL
-- ORDER BY session_start DESC LIMIT 10;

-- =============================================================================
-- Maintenance
-- =============================================================================

-- Clean up old sessions (optional, keep 1 year)
-- DELETE FROM cpap_sessions
-- WHERE session_start < NOW() - INTERVAL '1 year';

-- Vacuum and analyze for performance
-- VACUUM ANALYZE cpap_sessions;

-- Check table size
-- SELECT pg_size_pretty(pg_total_relation_size('cpap_sessions'));

-- =============================================================================
-- Notes
-- =============================================================================

-- 1. checkpoint_files JSONB column stores file sizes for intelligent polling
--    - Only BRP, PLD, SAD checkpoint files (not EVE/CSL)
--    - Used for session stop detection
--    - See: docs/CHECKPOINT_FILE_TRACKING.md
--
-- 2. File paths (brp_file_path, etc.) reference permanent archive locations
--    - Enables file retrieval without re-parsing
--    - Format: /data/cpap_archive/YYYYMMDD/HHMMSS/filename.edf
--
-- 3. Session status transitions:
--    - 'in_progress': Active session, checkpoint files growing
--    - 'completed': Session ended, EVE file present OR checkpoints stopped
--
-- 4. Timestamp tolerance: 2-5 seconds for session lookups
--    - Accounts for slight variations in session_start timestamps
--    - See DatabaseService::sessionExists() implementation
--
-- 5. AHI calculation: total_apneas / (duration_seconds / 3600)
--    - Automatically calculated when EVE file is parsed
--    - Updated in real-time as session progresses

-- =============================================================================
-- Migration from v1.0.0 to v1.1.0
-- =============================================================================

-- Add checkpoint_files column if upgrading from v1.0.0
-- ALTER TABLE cpap_sessions ADD COLUMN IF NOT EXISTS checkpoint_files JSONB DEFAULT '{}'::jsonb;
-- CREATE INDEX IF NOT EXISTS idx_cpap_sessions_checkpoint_files ON cpap_sessions USING GIN(checkpoint_files);

-- =============================================================================
-- Migration from v1.8.0 to v1.8.1
-- =============================================================================

-- Add force_completed flag (auto-migrated on connect, included here for reference)
-- ALTER TABLE cpap_sessions ADD COLUMN IF NOT EXISTS force_completed BOOLEAN DEFAULT FALSE;

-- =============================================================================
-- End of schema
-- =============================================================================
