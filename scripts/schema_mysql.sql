-- HMS-CPAP Database Schema — MySQL/MariaDB
--
-- MySQL schema is auto-created by HMS-CPAP on first run.
-- This file is provided for reference and manual database setup.
--
-- Version: 3.2.0
-- Date: 2026-04-02

-- Device registry
CREATE TABLE IF NOT EXISTS cpap_devices (
    device_id       VARCHAR(255) PRIMARY KEY,
    device_name     VARCHAR(255),
    serial_number   VARCHAR(255),
    model_id        INT DEFAULT 0,
    version_id      INT DEFAULT 0,
    last_seen       DATETIME DEFAULT NOW(),
    created_at      DATETIME DEFAULT NOW()
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4;

-- CPAP Sessions
CREATE TABLE IF NOT EXISTS cpap_sessions (
    id                INT AUTO_INCREMENT PRIMARY KEY,
    device_id         VARCHAR(255) NOT NULL,
    session_start     DATETIME NOT NULL,
    session_end       DATETIME,
    duration_seconds  INT DEFAULT 0,
    data_records      INT DEFAULT 0,
    brp_file_path     VARCHAR(512),
    eve_file_path     VARCHAR(512),
    sad_file_path     VARCHAR(512),
    pld_file_path     VARCHAR(512),
    csl_file_path     VARCHAR(512),
    checkpoint_files  JSON,
    force_completed   TINYINT DEFAULT 0,
    created_at        DATETIME DEFAULT NOW(),
    updated_at        DATETIME DEFAULT NOW() ON UPDATE NOW(),
    UNIQUE KEY uq_device_session (device_id, session_start)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4;

CREATE INDEX idx_cpap_sessions_device ON cpap_sessions(device_id);
CREATE INDEX idx_cpap_sessions_start ON cpap_sessions(device_id, session_start);

-- Session metrics (one row per session)
CREATE TABLE IF NOT EXISTS cpap_session_metrics (
    id                     INT AUTO_INCREMENT PRIMARY KEY,
    session_id             INT NOT NULL UNIQUE,
    total_events           INT DEFAULT 0,
    ahi                    DOUBLE DEFAULT 0,
    obstructive_apneas     INT DEFAULT 0,
    central_apneas         INT DEFAULT 0,
    hypopneas              INT DEFAULT 0,
    reras                  INT DEFAULT 0,
    clear_airway_apneas    INT DEFAULT 0,
    avg_event_duration     DOUBLE,
    max_event_duration     DOUBLE,
    time_in_apnea_percent  DOUBLE,
    avg_spo2               DOUBLE,
    min_spo2               DOUBLE,
    spo2_drops             INT,
    odi                    DOUBLE,
    avg_heart_rate         INT,
    max_heart_rate         INT,
    min_heart_rate         INT,
    avg_mask_pressure      DOUBLE,
    avg_epr_pressure       DOUBLE,
    avg_snore              DOUBLE,
    leak_p50               DOUBLE,
    leak_p95               DOUBLE,
    avg_leak_rate          DOUBLE,
    max_leak_rate          DOUBLE,
    avg_target_ventilation DOUBLE,
    therapy_mode           INT,
    created_at             DATETIME DEFAULT NOW(),
    FOREIGN KEY (session_id) REFERENCES cpap_sessions(id) ON DELETE CASCADE
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4;

-- Breathing summaries
CREATE TABLE IF NOT EXISTS cpap_breathing_summary (
    id              INT AUTO_INCREMENT PRIMARY KEY,
    session_id      INT NOT NULL,
    timestamp       DATETIME NOT NULL,
    avg_flow_rate   DOUBLE,
    max_flow_rate   DOUBLE,
    min_flow_rate   DOUBLE,
    avg_pressure    DOUBLE,
    max_pressure    DOUBLE,
    min_pressure    DOUBLE,
    UNIQUE KEY uq_session_ts (session_id, timestamp),
    FOREIGN KEY (session_id) REFERENCES cpap_sessions(id) ON DELETE CASCADE
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4;

-- Breath-by-breath detail (zero-crossing detection)
CREATE TABLE IF NOT EXISTS cpap_breaths (
    id                INT AUTO_INCREMENT PRIMARY KEY,
    session_id        INT NOT NULL,
    onset             DATETIME NOT NULL,
    tidal_volume      DOUBLE,
    inspiratory_time  DOUBLE,
    expiratory_time   DOUBLE,
    flow_limitation   DOUBLE,
    UNIQUE KEY uq_session_onset (session_id, onset),
    FOREIGN KEY (session_id) REFERENCES cpap_sessions(id) ON DELETE CASCADE
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4;

-- Events
CREATE TABLE IF NOT EXISTS cpap_events (
    id                INT AUTO_INCREMENT PRIMARY KEY,
    session_id        INT NOT NULL,
    event_type        VARCHAR(64),
    event_timestamp   DATETIME NOT NULL,
    duration_seconds  DOUBLE DEFAULT 0,
    details           TEXT,
    UNIQUE KEY uq_session_event_ts (session_id, event_timestamp),
    FOREIGN KEY (session_id) REFERENCES cpap_sessions(id) ON DELETE CASCADE
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4;

-- Vitals
CREATE TABLE IF NOT EXISTS cpap_vitals (
    id          INT AUTO_INCREMENT PRIMARY KEY,
    session_id  INT NOT NULL,
    timestamp   DATETIME NOT NULL,
    spo2        DOUBLE,
    heart_rate  INT,
    UNIQUE KEY uq_session_vital_ts (session_id, timestamp),
    FOREIGN KEY (session_id) REFERENCES cpap_sessions(id) ON DELETE CASCADE
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4;

-- Calculated metrics (per-minute)
CREATE TABLE IF NOT EXISTS cpap_calculated_metrics (
    id                   INT AUTO_INCREMENT PRIMARY KEY,
    session_id           INT NOT NULL,
    timestamp            DATETIME NOT NULL,
    respiratory_rate     DOUBLE,
    tidal_volume         DOUBLE,
    minute_ventilation   DOUBLE,
    inspiratory_time     DOUBLE,
    expiratory_time      DOUBLE,
    ie_ratio             DOUBLE,
    flow_limitation      DOUBLE,
    leak_rate            DOUBLE,
    flow_p95             DOUBLE,
    flow_p90             DOUBLE,
    pressure_p95         DOUBLE,
    pressure_p90         DOUBLE,
    mask_pressure        DOUBLE,
    epr_pressure         DOUBLE,
    snore_index          DOUBLE,
    target_ventilation   DOUBLE,
    UNIQUE KEY uq_session_calc_ts (session_id, timestamp),
    FOREIGN KEY (session_id) REFERENCES cpap_sessions(id) ON DELETE CASCADE
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4;

-- STR daily summary
CREATE TABLE IF NOT EXISTS cpap_daily_summary (
    id                INT AUTO_INCREMENT PRIMARY KEY,
    device_id         VARCHAR(255) NOT NULL,
    record_date       DATE NOT NULL,
    mask_pairs        JSON DEFAULT (JSON_ARRAY()),
    mask_events       INT DEFAULT 0,
    duration_minutes  DOUBLE DEFAULT 0,
    patient_hours     DOUBLE DEFAULT 0,
    ahi               DOUBLE, hi DOUBLE, ai DOUBLE, oai DOUBLE, cai DOUBLE, uai DOUBLE,
    rin               DOUBLE, csr DOUBLE,
    mask_press_50     DOUBLE, mask_press_95 DOUBLE, mask_press_max DOUBLE,
    leak_50           DOUBLE, leak_95 DOUBLE, leak_max DOUBLE,
    spo2_50           DOUBLE, spo2_95 DOUBLE,
    resp_rate_50      DOUBLE, tid_vol_50 DOUBLE, min_vent_50 DOUBLE,
    mode              INT, epr_level DOUBLE, pressure_setting DOUBLE,
    fault_device      INT DEFAULT 0,
    fault_alarm       INT DEFAULT 0,
    created_at        DATETIME DEFAULT NOW(),
    updated_at        DATETIME DEFAULT NOW() ON UPDATE NOW(),
    UNIQUE KEY uq_device_date (device_id, record_date)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4;

-- AI-generated summaries
CREATE TABLE IF NOT EXISTS cpap_summaries (
    id              INT AUTO_INCREMENT PRIMARY KEY,
    device_id       VARCHAR(255) NOT NULL,
    period          ENUM('daily', 'weekly', 'monthly') NOT NULL,
    range_start     DATE NOT NULL,
    range_end       DATE NOT NULL,
    nights_count    INT NOT NULL DEFAULT 1,
    avg_ahi         DOUBLE,
    avg_usage_hours DOUBLE,
    compliance_pct  DOUBLE,
    summary_text    TEXT NOT NULL,
    created_at      DATETIME NOT NULL DEFAULT NOW()
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4;

CREATE INDEX idx_cpap_summaries_device_period
    ON cpap_summaries(device_id, period, range_end DESC);

-- =============================================================================
-- Sleep Stage Inference (Phase 20)
-- =============================================================================

-- Per-epoch sleep stage predictions (30s epochs)
CREATE TABLE IF NOT EXISTS cpap_sleep_stages (
    id                 INT AUTO_INCREMENT PRIMARY KEY,
    session_id         INT NOT NULL,
    epoch_start_ts     DATETIME(3) NOT NULL,
    epoch_duration_sec INT NOT NULL DEFAULT 30,
    stage              TINYINT NOT NULL,         -- 0=Wake 1=Light 2=Deep 3=REM
    confidence         FLOAT NOT NULL,
    provisional        TINYINT(1) NOT NULL DEFAULT 0,
    model_version      VARCHAR(64) NOT NULL,
    UNIQUE KEY uq_session_epoch (session_id, epoch_start_ts),
    FOREIGN KEY (session_id) REFERENCES cpap_sessions(id) ON DELETE CASCADE
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4;

CREATE INDEX idx_cpap_sleep_stages_session
    ON cpap_sleep_stages(session_id);

-- =============================================================================
-- SDD-004: Equipment profiles + supplies
-- =============================================================================
-- Local-first mirror of the cloud model (hms-cpapdash-api SDD-035), minus
-- user_id: hms-cpap is single-household. A profile ("setup") owns exactly one
-- machine plus its accessories; supply wear is COMPUTED on read, never stored.
-- client_uuid exists only so optional cloud sync is idempotent; it is unused
-- offline. Keep this in lockstep with MySQLDatabase::createSchema().
--
-- started_using_at is VARCHAR, not DATETIME, on purpose: it is an ISO-8601
-- string that must round-trip byte-for-byte with the SQLite/Postgres backends
-- and the phone app so all three compute identical due dates.

-- Catalog: seeded system defaults + user customs
CREATE TABLE IF NOT EXISTS cpap_equipment_types (
    id                          INT AUTO_INCREMENT PRIMARY KEY,
    type_key                    VARCHAR(64) NOT NULL,
    label                       VARCHAR(128) NOT NULL,
    category                    VARCHAR(32) NOT NULL,
    default_replace_after_days  INT,
    is_system                   TINYINT(1) DEFAULT 0,
    active                      TINYINT(1) DEFAULT 1,
    created_at                  DATETIME DEFAULT NOW(),
    updated_at                  DATETIME DEFAULT NOW() ON UPDATE NOW(),
    UNIQUE KEY uq_eq_type_key (type_key)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4;

-- Named setups. MySQL UNIQUE allows repeated NULLs, so uq_eq_profile_uuid is
-- exactly the "UNIQUE WHERE client_uuid IS NOT NULL" the other backends express
-- as a partial index.
CREATE TABLE IF NOT EXISTS cpap_equipment_profiles (
    id           INT AUTO_INCREMENT PRIMARY KEY,
    client_uuid  VARCHAR(64),
    name         VARCHAR(128) NOT NULL,
    active       TINYINT(1) DEFAULT 1,
    deleted      TINYINT(1) DEFAULT 0,
    created_at   DATETIME DEFAULT NOW(),
    updated_at   DATETIME DEFAULT NOW() ON UPDATE NOW(),
    UNIQUE KEY uq_eq_profile_uuid (client_uuid)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4;

-- The gear; category is denormalized from the type.
-- HARD RULE "at most one live machine per profile" CANNOT be an index here:
-- MySQL has no partial/filtered indexes. IDatabase::profileHasMachine() is the
-- portable application guard the controller calls before any machine write.
CREATE TABLE IF NOT EXISTS cpap_equipment_items (
    id                  INT AUTO_INCREMENT PRIMARY KEY,
    profile_id          INT NOT NULL,
    client_uuid         VARCHAR(64),
    type_key            VARCHAR(64) NOT NULL,
    category            VARCHAR(32) NOT NULL DEFAULT 'accessory',
    brand               VARCHAR(128) DEFAULT '',
    model               VARCHAR(128) DEFAULT '',
    variant             VARCHAR(128),
    started_using_at    VARCHAR(32),
    replace_after_days  INT,
    notes               VARCHAR(1000),
    active              TINYINT(1) DEFAULT 1,
    deleted             TINYINT(1) DEFAULT 0,
    created_at          DATETIME DEFAULT NOW(),
    updated_at          DATETIME DEFAULT NOW() ON UPDATE NOW(),
    UNIQUE KEY uq_eq_item_uuid (client_uuid),
    KEY idx_eq_item_profile (profile_id, active),
    FOREIGN KEY (profile_id) REFERENCES cpap_equipment_profiles(id) ON DELETE CASCADE
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4;

-- Seed the six system types verbatim from the app's supply_defaults.dart, so
-- local, cloud and the phone app all compute the same due dates.
-- INSERT IGNORE + UNIQUE(type_key) makes this idempotent.
INSERT IGNORE INTO cpap_equipment_types
    (type_key, label, category, default_replace_after_days, is_system)
VALUES
    ('machine',    'Machine',    'machine',   NULL, 1),
    ('mask',       'Mask',       'accessory',   90, 1),
    ('tubing',     'Tubing',     'accessory',   90, 1),
    ('filter',     'Filter',     'accessory',   30, 1),
    ('humidifier', 'Humidifier', 'accessory',  180, 1),
    ('headgear',   'Headgear',   'accessory',  180, 1);
