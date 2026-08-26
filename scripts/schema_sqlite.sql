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
-- cpap_session_files (SDD-014): which files actually make up a session.
CREATE TABLE IF NOT EXISTS cpap_session_files (
    id          INTEGER PRIMARY KEY AUTOINCREMENT,
    session_id  INTEGER NOT NULL,
    kind        TEXT NOT NULL,
    rel_path    TEXT NOT NULL,
    UNIQUE (session_id, rel_path)
);
CREATE INDEX IF NOT EXISTS idx_session_files_session ON cpap_session_files(session_id);
CREATE INDEX IF NOT EXISTS idx_session_files_path ON cpap_session_files(rel_path);

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
    spo2_drops             INTEGER,
    odi                    REAL,
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

-- Breath-by-breath detail (zero-crossing detection)
CREATE TABLE IF NOT EXISTS cpap_breaths (
    id                INTEGER PRIMARY KEY AUTOINCREMENT,
    session_id        INTEGER NOT NULL,
    onset             TEXT NOT NULL,
    tidal_volume      REAL,
    inspiratory_time  REAL,
    expiratory_time   REAL,
    flow_limitation   REAL,
    UNIQUE (session_id, onset),
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
    -- Hours of therapy ON THIS DAY, from either writer.
    patient_hours     REAL DEFAULT 0,
    -- ResMed's lifetime PatientHours counter. STR-only, NULL otherwise.
    machine_hours     REAL,
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
-- SDD-020: what ResMed's own servers say about the same nights. Deliberately a
-- separate table from cpap_daily_summary, which is what OUR parser read off the
-- card; joining the two for display is the point, mixing them would destroy the
-- provenance that makes the comparison mean anything.
CREATE TABLE IF NOT EXISTS cpap_myair_records (
    record_date      TEXT PRIMARY KEY,
    total_usage_min  REAL DEFAULT 0,
    sleep_score      INTEGER DEFAULT 0,
    usage_score      INTEGER DEFAULT 0,
    ahi_score        INTEGER DEFAULT 0,
    mask_score       INTEGER DEFAULT 0,
    leak_score       INTEGER DEFAULT 0,
    ahi              REAL DEFAULT 0,
    mask_pair_count  INTEGER DEFAULT 0,
    leak_percentile  REAL DEFAULT 0,
    -- 0 when ResMed returned an all-zero night: they have NO DATA for that
    -- date, which is not the same as a night with no therapy.
    has_data         INTEGER DEFAULT 0,
    fetched_at       TEXT DEFAULT (datetime('now'))
);

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

-- =============================================================================
-- Sleep Stage Inference (Phase 20)
-- =============================================================================

-- Per-epoch sleep stage predictions (30s epochs)
CREATE TABLE IF NOT EXISTS cpap_sleep_stages (
    id                 INTEGER PRIMARY KEY AUTOINCREMENT,
    session_id         INTEGER NOT NULL,
    epoch_start_ts     TEXT NOT NULL,
    epoch_duration_sec INTEGER NOT NULL DEFAULT 30,
    stage              INTEGER NOT NULL,         -- 0=Wake 1=Light 2=Deep 3=REM
    confidence         REAL NOT NULL,
    provisional        INTEGER NOT NULL DEFAULT 0,
    model_version      TEXT NOT NULL,
    UNIQUE (session_id, epoch_start_ts),
    FOREIGN KEY (session_id) REFERENCES cpap_sessions(id) ON DELETE CASCADE
);

CREATE INDEX IF NOT EXISTS idx_cpap_sleep_stages_session
    ON cpap_sleep_stages(session_id);

-- =============================================================================
-- Equipment profiles + supplies (SDD-004)
-- =============================================================================
-- Local-first mirror of the cloud model (hms-cpapdash-api SDD-035), minus
-- user_id: hms-cpap is single-household. A profile ("setup") owns exactly one
-- machine plus its accessories; supply wear is COMPUTED on read, never stored.
-- client_uuid exists only so optional cloud sync is idempotent; unused offline.
-- Keep this in lockstep with SQLiteDatabase::createSchema().

-- Catalog: seeded system defaults + user customs
CREATE TABLE IF NOT EXISTS cpap_equipment_types (
    id                          INTEGER PRIMARY KEY AUTOINCREMENT,
    type_key                    TEXT NOT NULL UNIQUE,
    label                       TEXT NOT NULL,
    category                    TEXT NOT NULL,
    default_replace_after_days  INTEGER,
    is_system                   INTEGER DEFAULT 0,
    active                      INTEGER DEFAULT 1,
    created_at                  TEXT DEFAULT (datetime('now')),
    updated_at                  TEXT DEFAULT (datetime('now'))
);

-- Named setups
CREATE TABLE IF NOT EXISTS cpap_equipment_profiles (
    id           INTEGER PRIMARY KEY AUTOINCREMENT,
    client_uuid  TEXT,
    name         TEXT NOT NULL,
    active       INTEGER DEFAULT 1,
    deleted      INTEGER DEFAULT 0,
    created_at   TEXT DEFAULT (datetime('now')),
    updated_at   TEXT DEFAULT (datetime('now'))
);

-- The gear; category denormalized from the type so the one-machine-per-profile
-- index can enforce it at the DB level
CREATE TABLE IF NOT EXISTS cpap_equipment_items (
    id                  INTEGER PRIMARY KEY AUTOINCREMENT,
    profile_id          INTEGER NOT NULL REFERENCES cpap_equipment_profiles(id) ON DELETE CASCADE,
    client_uuid         TEXT,
    type_key            TEXT NOT NULL,
    category            TEXT NOT NULL DEFAULT 'accessory',
    brand               TEXT DEFAULT '',
    model               TEXT DEFAULT '',
    variant             TEXT,
    started_using_at    TEXT,
    replace_after_days  INTEGER,
    notes               TEXT,
    active              INTEGER DEFAULT 1,
    deleted             INTEGER DEFAULT 0,
    created_at          TEXT DEFAULT (datetime('now')),
    updated_at          TEXT DEFAULT (datetime('now'))
);

-- Seed the six system types verbatim from the app's supply_defaults.dart, so
-- local, cloud and the phone app all compute the same due dates.
INSERT OR IGNORE INTO cpap_equipment_types
    (type_key, label, category, default_replace_after_days, is_system)
VALUES
    ('machine',    'Machine',    'machine',   NULL, 1),
    ('mask',       'Mask',       'accessory',   90, 1),
    ('tubing',     'Tubing',     'accessory',   90, 1),
    ('filter',     'Filter',     'accessory',   30, 1),
    ('humidifier', 'Humidifier', 'accessory',  180, 1),
    ('headgear',   'Headgear',   'accessory',  180, 1);

CREATE UNIQUE INDEX IF NOT EXISTS idx_eq_profile_uuid
    ON cpap_equipment_profiles(client_uuid) WHERE client_uuid IS NOT NULL;
CREATE UNIQUE INDEX IF NOT EXISTS idx_eq_item_uuid
    ON cpap_equipment_items(client_uuid) WHERE client_uuid IS NOT NULL;
CREATE INDEX IF NOT EXISTS idx_eq_item_profile
    ON cpap_equipment_items(profile_id, active);
-- HARD RULE: at most one live machine per profile.
CREATE UNIQUE INDEX IF NOT EXISTS idx_eq_one_machine_per_profile
    ON cpap_equipment_items(profile_id)
    WHERE category = 'machine' AND active = 1 AND deleted = 0;

-- ── SDD-007: cleaning schedules ──────────────────────────────────────────────
-- The WASH half of equipment upkeep, deliberately separate from supplies: a mask
-- is REPLACED every 90 days and WIPED every day, and one interval cannot mean
-- both. Mirrors hms-cpapdash-api SDD-043 minus user_id (single household).
-- The seven presets are seeded verbatim from that SDD so a user running both
-- stacks sees one vocabulary.

CREATE TABLE IF NOT EXISTS cleaning_task_types (
    id                    INTEGER PRIMARY KEY AUTOINCREMENT,
    task_key              TEXT NOT NULL UNIQUE,
    label                 TEXT NOT NULL,
    applies_to_type_key   TEXT,
    default_interval_days INTEGER NOT NULL,
    is_system             INTEGER DEFAULT 0,
    active                INTEGER DEFAULT 1,
    created_at            TEXT DEFAULT (datetime('now','localtime'))
);

CREATE TABLE IF NOT EXISTS cleaning_tasks (
    id             INTEGER PRIMARY KEY AUTOINCREMENT,
    profile_id     INTEGER NOT NULL REFERENCES cpap_equipment_profiles(id) ON DELETE CASCADE,
    item_id        INTEGER REFERENCES cpap_equipment_items(id) ON DELETE SET NULL,
    client_uuid    TEXT,
    task_key       TEXT NOT NULL,
    label          TEXT NOT NULL,
    interval_days  INTEGER NOT NULL,
    time_minutes   INTEGER NOT NULL DEFAULT 510,
    start_date     TEXT NOT NULL,
    enabled        INTEGER DEFAULT 0,
    last_done_at   TEXT,
    deleted        INTEGER DEFAULT 0,
    created_at     TEXT DEFAULT (datetime('now','localtime')),
    updated_at     TEXT DEFAULT (datetime('now','localtime'))
);

CREATE INDEX IF NOT EXISTS idx_cleaning_tasks_profile
    ON cleaning_tasks(profile_id, deleted);
CREATE UNIQUE INDEX IF NOT EXISTS idx_cleaning_tasks_uuid
    ON cleaning_tasks(client_uuid) WHERE client_uuid IS NOT NULL;
CREATE UNIQUE INDEX IF NOT EXISTS idx_cleaning_tasks_profile_key
    ON cleaning_tasks(profile_id, task_key) WHERE deleted = 0;

-- SDD-008: one row per date folder on the card, recording whether the night's
-- FILES all arrived, which is a different question from whether the therapy
-- ended. Derived state about a transfer, not user data: rebuildable from the
-- card, never synced, safe to wipe. date_folder is the natural key, so a
-- re-scan updates in place instead of duplicating a night.
CREATE TABLE IF NOT EXISTS cpap_sync_folders (
    date_folder     TEXT PRIMARY KEY,
    files_listed    INTEGER DEFAULT 0,
    complete        INTEGER DEFAULT 0,
    stable          INTEGER DEFAULT 0,
    last_total_size INTEGER DEFAULT -1,
    last_file_count INTEGER DEFAULT -1,
    str_due         INTEGER DEFAULT 0,
    str_day         TEXT,
    sidecars_due    INTEGER DEFAULT 0,
    resync_size     INTEGER DEFAULT -1,
    resync_count    INTEGER DEFAULT 0,
    updated_at      TEXT DEFAULT (datetime('now','localtime'))
);
-- The sweep asks for outstanding debt every burst.
CREATE INDEX IF NOT EXISTS idx_sync_folders_debt
    ON cpap_sync_folders(str_due, sidecars_due);

INSERT OR IGNORE INTO cleaning_task_types
    (task_key, label, applies_to_type_key, default_interval_days, is_system)
VALUES
    ('mask_wipe',        'Wipe the mask cushion',         'mask',        1, 1),
    ('mask_wash',        'Wash the mask and cushion',     'mask',        7, 1),
    ('headgear_wash',    'Wash the headgear',             'headgear',    7, 1),
    ('tubing_wash',      'Wash the tubing',               'tubing',      7, 1),
    ('humidifier_empty', 'Empty and rinse the water tub', 'humidifier',  1, 1),
    ('humidifier_wash',  'Wash the water tub',            'humidifier',  7, 1),
    ('filter_check',     'Check the filter',              'filter',     30, 1);

-- Report jobs. Declared for parity with the PostgreSQL schema; note the report
-- queries cast with created_at::text, which SQLite does not accept, so reports
-- are not usable on this backend until those are made portable.
CREATE TABLE IF NOT EXISTS cpap_reports (
    id            INTEGER PRIMARY KEY AUTOINCREMENT,
    device_id     TEXT NOT NULL,
    range_start   TEXT,
    range_end     TEXT,
    nights_count  INTEGER,
    filename      TEXT,
    filepath      TEXT,
    status        TEXT NOT NULL DEFAULT 'pending',
    error_msg     TEXT,
    created_at    TEXT DEFAULT CURRENT_TIMESTAMP,
    completed_at  TEXT
);

CREATE INDEX IF NOT EXISTS idx_cpap_reports_device_created
    ON cpap_reports(device_id, created_at DESC);
