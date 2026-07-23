#ifdef WITH_POSTGRESQL
#include "utils/TimeCompat.h"
#include "database/DatabaseService.h"
#include <libpq-fe.h>
#include <iostream>
#include <iomanip>
#include <sstream>

namespace hms_cpap {

DatabaseService::DatabaseService(const std::string& connection_string)
    : connection_string_(connection_string) {
}

DatabaseService::~DatabaseService() {
    disconnect();
}

bool DatabaseService::connect() {
    std::lock_guard<std::recursive_mutex> lock(mutex_);

    try {
        conn_ = std::make_unique<pqxx::connection>(connection_string_);

        if (conn_->is_open()) {
            std::cout << "✅ DB: Connected to PostgreSQL (" << conn_->dbname() << ")" << std::endl;

            // Auto-migrate: add force_completed column if missing
            try {
                pqxx::work txn(*conn_);
                txn.exec("ALTER TABLE cpap_sessions ADD COLUMN IF NOT EXISTS force_completed BOOLEAN DEFAULT FALSE");
                txn.commit();
            } catch (...) {
                // Column may already exist or table not yet created — ignore
            }

            // Auto-migrate v2.0.0: PLD and ASV support
            try {
                pqxx::work txn(*conn_);
                // PLD-derived columns on cpap_session_metrics
                txn.exec("ALTER TABLE cpap_session_metrics ADD COLUMN IF NOT EXISTS avg_mask_pressure FLOAT");
                txn.exec("ALTER TABLE cpap_session_metrics ADD COLUMN IF NOT EXISTS avg_epr_pressure FLOAT");
                txn.exec("ALTER TABLE cpap_session_metrics ADD COLUMN IF NOT EXISTS avg_snore FLOAT");
                txn.exec("ALTER TABLE cpap_session_metrics ADD COLUMN IF NOT EXISTS leak_p50 FLOAT");
                txn.exec("ALTER TABLE cpap_session_metrics ADD COLUMN IF NOT EXISTS leak_p95 FLOAT");
                txn.exec("ALTER TABLE cpap_session_metrics ADD COLUMN IF NOT EXISTS avg_leak_rate FLOAT");
                txn.exec("ALTER TABLE cpap_session_metrics ADD COLUMN IF NOT EXISTS max_leak_rate FLOAT");
                // ASV-specific
                txn.exec("ALTER TABLE cpap_session_metrics ADD COLUMN IF NOT EXISTS avg_target_ventilation FLOAT");
                txn.exec("ALTER TABLE cpap_session_metrics ADD COLUMN IF NOT EXISTS therapy_mode INT");
                // PLD columns on cpap_calculated_metrics (per-minute)
                txn.exec("ALTER TABLE cpap_calculated_metrics ADD COLUMN IF NOT EXISTS mask_pressure FLOAT");
                txn.exec("ALTER TABLE cpap_calculated_metrics ADD COLUMN IF NOT EXISTS epr_pressure FLOAT");
                txn.exec("ALTER TABLE cpap_calculated_metrics ADD COLUMN IF NOT EXISTS snore_index FLOAT");
                txn.exec("ALTER TABLE cpap_calculated_metrics ADD COLUMN IF NOT EXISTS target_ventilation FLOAT");
                txn.commit();
                std::cout << "  DB: v2.0.0 migration (PLD/ASV columns) applied" << std::endl;
            } catch (...) {
                // Tables may not exist yet — ignore
            }

            // Auto-migrate v2.2.0: advanced signal analysis (desat + breaths)
            try {
                pqxx::work txn(*conn_);
                txn.exec("ALTER TABLE cpap_session_metrics ADD COLUMN IF NOT EXISTS spo2_drops INT");
                txn.exec("ALTER TABLE cpap_session_metrics ADD COLUMN IF NOT EXISTS odi FLOAT");
                txn.exec(R"(
                    CREATE TABLE IF NOT EXISTS cpap_breaths (
                        id                SERIAL PRIMARY KEY,
                        session_id        INTEGER NOT NULL REFERENCES cpap_sessions(id) ON DELETE CASCADE,
                        onset             TIMESTAMP NOT NULL,
                        tidal_volume      REAL,
                        inspiratory_time  REAL,
                        expiratory_time   REAL,
                        flow_limitation   REAL,
                        UNIQUE (session_id, onset)
                    )
                )");
                txn.commit();
                std::cout << "  DB: v2.2.0 migration (desat + breaths) applied" << std::endl;
            } catch (...) {
                // Tables may not exist yet — ignore
            }

            // Auto-migrate v2.1.0: AI summaries table
            try {
                pqxx::work txn(*conn_);
                txn.exec(R"(
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
                        created_at      TIMESTAMP NOT NULL DEFAULT CURRENT_TIMESTAMP
                    )
                )");
                txn.exec(R"(
                    CREATE INDEX IF NOT EXISTS idx_cpap_summaries_device_period
                    ON cpap_summaries (device_id, period, range_end DESC)
                )");
                txn.commit();
                std::cout << "  DB: v2.1.0 migration (cpap_summaries table) applied" << std::endl;
            } catch (...) {}

            // Auto-migrate v2.2.0: Oximetry tables (O2 Ring)
            try {
                pqxx::work txn(*conn_);
                txn.exec(R"(
                    CREATE TABLE IF NOT EXISTS oximetry_sessions (
                        id                  SERIAL PRIMARY KEY,
                        device_id           TEXT NOT NULL,
                        filename            TEXT UNIQUE NOT NULL,
                        start_time          TIMESTAMP NOT NULL,
                        end_time            TIMESTAMP NOT NULL,
                        duration_seconds    INT,
                        sample_interval     DOUBLE PRECISION,
                        avg_spo2            DOUBLE PRECISION,
                        min_spo2            DOUBLE PRECISION,
                        spo2_baseline       DOUBLE PRECISION,
                        time_below_90       DOUBLE PRECISION,
                        time_below_88       DOUBLE PRECISION,
                        odi_3pct            DOUBLE PRECISION,
                        desat_count         INT,
                        avg_hr              DOUBLE PRECISION,
                        min_hr              INT,
                        max_hr              INT,
                        valid_samples       INT,
                        total_samples       INT,
                        cpap_session_date   TEXT,
                        created_at          TIMESTAMP DEFAULT CURRENT_TIMESTAMP
                    )
                )");
                txn.exec(R"(
                    CREATE TABLE IF NOT EXISTS oximetry_samples (
                        id                      SERIAL PRIMARY KEY,
                        oximetry_session_id     INT REFERENCES oximetry_sessions(id) ON DELETE CASCADE,
                        timestamp               TIMESTAMP NOT NULL,
                        spo2                    INT,
                        heart_rate              INT,
                        motion                  INT,
                        vibration               INT,
                        valid                   BOOLEAN,
                        source                  TEXT DEFAULT 'vld'
                    )
                )");
                txn.exec("CREATE INDEX IF NOT EXISTS idx_oximetry_samples_session ON oximetry_samples(oximetry_session_id)");
                txn.commit();
                std::cout << "  DB: v2.2.0 migration (oximetry tables) applied" << std::endl;
            } catch (...) {}

            // Auto-migrate SDD-004: equipment profiles, items and supply types.
            // Keep in lockstep with scripts/schema.sql — drift here is what forced v4.4.10.
            try {
                pqxx::work txn(*conn_);
                txn.exec(R"(
                    CREATE TABLE IF NOT EXISTS cpap_equipment_types (
                        id                          SERIAL PRIMARY KEY,
                        type_key                    TEXT NOT NULL UNIQUE,
                        label                       TEXT NOT NULL,
                        category                    TEXT NOT NULL CHECK (category IN ('machine','accessory')),
                        default_replace_after_days  INT,
                        is_system                   BOOLEAN NOT NULL DEFAULT FALSE,
                        active                      BOOLEAN NOT NULL DEFAULT TRUE,
                        created_at                  TIMESTAMPTZ NOT NULL DEFAULT NOW(),
                        updated_at                  TIMESTAMPTZ NOT NULL DEFAULT NOW()
                    )
                )");
                txn.exec(R"(
                    CREATE TABLE IF NOT EXISTS cpap_equipment_profiles (
                        id          SERIAL PRIMARY KEY,
                        client_uuid TEXT,
                        name        TEXT NOT NULL,
                        active      BOOLEAN NOT NULL DEFAULT TRUE,
                        deleted     BOOLEAN NOT NULL DEFAULT FALSE,
                        created_at  TIMESTAMPTZ NOT NULL DEFAULT NOW(),
                        updated_at  TIMESTAMPTZ NOT NULL DEFAULT NOW()
                    )
                )");
                txn.exec(R"(
                    CREATE TABLE IF NOT EXISTS cpap_equipment_items (
                        id                 SERIAL PRIMARY KEY,
                        profile_id         INTEGER NOT NULL
                                           REFERENCES cpap_equipment_profiles(id) ON DELETE CASCADE,
                        client_uuid        TEXT,
                        type_key           TEXT NOT NULL,
                        category           TEXT NOT NULL DEFAULT 'accessory',
                        brand              TEXT,
                        model              TEXT,
                        variant            TEXT,
                        started_using_at   TIMESTAMPTZ,
                        replace_after_days INT,
                        notes              TEXT,
                        active             BOOLEAN NOT NULL DEFAULT TRUE,
                        deleted            BOOLEAN NOT NULL DEFAULT FALSE,
                        created_at         TIMESTAMPTZ NOT NULL DEFAULT NOW(),
                        updated_at         TIMESTAMPTZ NOT NULL DEFAULT NOW()
                    )
                )");
                // client_uuid is only for optional cloud sync; NULL offline, unique when set
                txn.exec(R"(
                    CREATE UNIQUE INDEX IF NOT EXISTS uq_cpap_equipment_profiles_uuid
                    ON cpap_equipment_profiles(client_uuid) WHERE client_uuid IS NOT NULL
                )");
                // Same for items: without this a replayed sync double-inserts
                txn.exec(R"(
                    CREATE UNIQUE INDEX IF NOT EXISTS uq_cpap_equipment_items_uuid
                    ON cpap_equipment_items(client_uuid) WHERE client_uuid IS NOT NULL
                )");
                // HARD RULE: at most one live machine per profile
                txn.exec(R"(
                    CREATE UNIQUE INDEX IF NOT EXISTS uq_cpap_equipment_one_machine
                    ON cpap_equipment_items(profile_id)
                    WHERE category = 'machine' AND active AND NOT deleted
                )");
                txn.exec(R"(
                    CREATE INDEX IF NOT EXISTS idx_cpap_equipment_items_profile
                    ON cpap_equipment_items(profile_id)
                )");
                // Seed system types verbatim from the app's supply_defaults.dart so
                // local, cloud and app all compute identical due dates.
                txn.exec(R"(
                    INSERT INTO cpap_equipment_types
                        (type_key, label, category, default_replace_after_days, is_system, active)
                    VALUES
                        ('machine',    'Machine',    'machine',   NULL, TRUE, TRUE),
                        ('mask',       'Mask',       'accessory',   90, TRUE, TRUE),
                        ('tubing',     'Tubing',     'accessory',   90, TRUE, TRUE),
                        ('filter',     'Filter',     'accessory',   30, TRUE, TRUE),
                        ('humidifier', 'Humidifier', 'accessory',  180, TRUE, TRUE),
                        ('headgear',   'Headgear',   'accessory',  180, TRUE, TRUE)
                    ON CONFLICT (type_key) DO NOTHING
                )");
                txn.commit();
                std::cout << "  DB: SDD-004 migration (equipment tables) applied" << std::endl;
            } catch (const std::exception& e) {
                std::cerr << "DB: SDD-004 equipment migration failed: " << e.what() << std::endl;
            }

            return true;
        }
    } catch (const std::exception& e) {
        std::cerr << "❌ DB: Connection failed: " << e.what() << std::endl;
    }

    return false;
}

void DatabaseService::disconnect() {
    std::lock_guard<std::recursive_mutex> lock(mutex_);

    if (conn_ && conn_->is_open()) {
        conn_.reset();
        std::cout << "🔌 DB: Disconnected" << std::endl;
    }
}

bool DatabaseService::isConnected() const {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    return conn_ && conn_->is_open();
}

bool DatabaseService::ensureConnection() {
    if (isConnected()) {
        return true;
    }

    std::cout << "⚠️  DB: Connection lost, reconnecting..." << std::endl;
    return connect();
}

void DatabaseService::upsertDevice(pqxx::work& work, const CPAPSession& session) {
    std::string query = R"(
        INSERT INTO cpap_devices (device_id, device_name, serial_number, model_id, version_id, last_seen)
        VALUES ($1, $2, $3, $4, $5, CURRENT_TIMESTAMP)
        ON CONFLICT (device_id) DO UPDATE
        SET device_name = EXCLUDED.device_name,
            serial_number = EXCLUDED.serial_number,
            model_id = EXCLUDED.model_id,
            version_id = EXCLUDED.version_id,
            last_seen = CURRENT_TIMESTAMP
    )";

    work.exec_params(query,
        session.device_id,
        session.device_name,
        session.serial_number,
        session.model_id.value_or(0),
        session.version_id.value_or(0)
    );
}

int DatabaseService::insertSession(pqxx::work& work, const CPAPSession& session) {
    // Convert time_point to string
    auto start_time_t = std::chrono::system_clock::to_time_t(session.session_start.value());
    std::tm* start_tm = std::localtime(&start_time_t);
    std::ostringstream start_oss;
    start_oss << std::put_time(start_tm, "%Y-%m-%d %H:%M:%S");

    // session_end is intentionally never written here — markSessionCompleted() owns it.
    // Writing it from EDF data would pre-empt the IS NULL guard in markSessionCompleted
    // and prevent the LLM summary from firing.
    std::string query = R"(
        INSERT INTO cpap_sessions (device_id, session_start, session_end, duration_seconds, data_records,
                                   brp_file_path, eve_file_path, sad_file_path, pld_file_path, csl_file_path)
        VALUES ($1, $2, NULL, $3, $4, $5, $6, $7, $8, $9)
        ON CONFLICT (device_id, session_start) DO UPDATE
        SET duration_seconds = GREATEST(cpap_sessions.duration_seconds, EXCLUDED.duration_seconds),
            data_records = GREATEST(cpap_sessions.data_records, EXCLUDED.data_records),
            brp_file_path = EXCLUDED.brp_file_path,
            eve_file_path = EXCLUDED.eve_file_path,
            sad_file_path = EXCLUDED.sad_file_path,
            pld_file_path = EXCLUDED.pld_file_path,
            csl_file_path = EXCLUDED.csl_file_path
        RETURNING id
    )";

    auto result = work.exec_params(query,
        session.device_id,
        start_oss.str(),
        session.duration_seconds.value_or(0),
        session.data_records,
        session.brp_file_path.value_or(""),
        session.eve_file_path.value_or(""),
        session.sad_file_path.value_or(""),
        session.pld_file_path.value_or(""),
        session.csl_file_path.value_or("")
    );

    return result[0][0].as<int>();
}

void DatabaseService::insertBreathingSummaries(pqxx::work& work, int session_id,
                                                const std::vector<BreathingSummary>& summaries) {
    if (summaries.empty()) return;

    // Batch insert for performance
    std::ostringstream query;
    query << "INSERT INTO cpap_breathing_summary "
          << "(session_id, timestamp, avg_flow_rate, max_flow_rate, min_flow_rate, "
          << "avg_pressure, max_pressure, min_pressure) VALUES ";

    for (size_t i = 0; i < summaries.size(); ++i) {
        const auto& s = summaries[i];

        auto time_t = std::chrono::system_clock::to_time_t(s.timestamp);
        std::tm* tm = std::localtime(&time_t);
        std::ostringstream oss;
        oss << std::put_time(tm, "%Y-%m-%d %H:%M:%S");

        query << "(" << session_id << ", '" << oss.str() << "', "
              << s.avg_flow_rate << ", " << s.max_flow_rate << ", " << s.min_flow_rate << ", "
              << s.avg_pressure << ", " << s.max_pressure << ", " << s.min_pressure << ")";

        if (i < summaries.size() - 1) {
            query << ", ";
        }
    }

    // Use ON CONFLICT DO NOTHING to avoid duplicates when re-downloading sessions (48h window)
    query << " ON CONFLICT (session_id, timestamp) DO NOTHING";
    work.exec(query.str());
}

void DatabaseService::insertEvents(pqxx::work& work, int session_id,
                                    const std::vector<CPAPEvent>& events) {
    if (events.empty()) return;

    for (const auto& event : events) {
        auto time_t = std::chrono::system_clock::to_time_t(event.timestamp);
        std::tm* tm = std::localtime(&time_t);
        std::ostringstream oss;
        oss << std::put_time(tm, "%Y-%m-%d %H:%M:%S");

        std::string query = R"(
            INSERT INTO cpap_events (session_id, event_type, event_timestamp, duration_seconds, details)
            VALUES ($1, $2, $3, $4, $5)
            ON CONFLICT (session_id, event_timestamp) DO NOTHING
        )";

        work.exec_params(query,
            session_id,
            eventTypeToString(event.event_type),
            oss.str(),
            event.duration_seconds,
            event.details.value_or("")
        );
    }
}

void DatabaseService::insertDesaturations(pqxx::work& work, int session_id,
                                          const std::vector<DesatEvent>& desats) {
    if (desats.empty()) return;
    for (const auto& d : desats) {
        auto tt = std::chrono::system_clock::to_time_t(d.onset);
        std::ostringstream oss;
        oss << std::put_time(std::localtime(&tt), "%Y-%m-%d %H:%M:%S");
        char details[96];
        std::snprintf(details, sizeof(details), "{\"nadir\":%.1f,\"depth\":%.1f}", d.nadir, d.depth);
        work.exec_params(R"(
            INSERT INTO cpap_events (session_id, event_type, event_timestamp, duration_seconds, details)
            VALUES ($1, 'Desaturation', $2, $3, $4)
            ON CONFLICT (session_id, event_timestamp) DO NOTHING
        )", session_id, oss.str(), d.duration_seconds, std::string(details));
    }
}

void DatabaseService::insertBreaths(pqxx::work& work, int session_id,
                                    const std::vector<Breath>& breaths) {
    if (breaths.empty()) return;
    for (const auto& b : breaths) {
        auto tt = std::chrono::system_clock::to_time_t(b.onset);
        std::ostringstream oss;
        oss << std::put_time(std::localtime(&tt), "%Y-%m-%d %H:%M:%S");
        work.exec_params(R"(
            INSERT INTO cpap_breaths
                (session_id, onset, tidal_volume, inspiratory_time, expiratory_time, flow_limitation)
            VALUES ($1, $2, $3, $4, $5, $6)
            ON CONFLICT (session_id, onset) DO NOTHING
        )", session_id, oss.str(), b.tidal_volume, b.inspiratory_time, b.expiratory_time, b.flow_limitation);
    }
}

void DatabaseService::insertVitals(pqxx::work& work, int session_id,
                                    const std::vector<CPAPVitals>& vitals) {
    if (vitals.empty()) return;

    // Batch insert for performance (vitals can be large - 1 per second)
    // Use ON CONFLICT DO NOTHING to avoid duplicates when re-parsing growing files
    std::ostringstream query;
    query << "INSERT INTO cpap_vitals (session_id, timestamp, spo2, heart_rate) VALUES ";

    for (size_t i = 0; i < vitals.size(); ++i) {
        const auto& v = vitals[i];

        auto time_t = std::chrono::system_clock::to_time_t(v.timestamp);
        std::tm* tm = std::localtime(&time_t);
        std::ostringstream oss;
        oss << std::put_time(tm, "%Y-%m-%d %H:%M:%S");

        query << "(" << session_id << ", '" << oss.str() << "', ";

        if (v.spo2.has_value()) {
            query << v.spo2.value();
        } else {
            query << "NULL";
        }

        query << ", ";

        if (v.heart_rate.has_value()) {
            query << v.heart_rate.value();
        } else {
            query << "NULL";
        }

        query << ")";

        if (i < vitals.size() - 1) {
            query << ", ";
        }
    }

    query << " ON CONFLICT (session_id, timestamp) DO NOTHING";
    work.exec(query.str());
}

void DatabaseService::insertSessionMetrics(pqxx::work& work, int session_id,
                                            const SessionMetrics& metrics) {
    std::string query = R"(
        INSERT INTO cpap_session_metrics
        (session_id, total_events, ahi, obstructive_apneas, central_apneas, hypopneas, reras, clear_airway_apneas,
         avg_spo2, min_spo2, avg_heart_rate, max_heart_rate, min_heart_rate,
         avg_mask_pressure, avg_epr_pressure, avg_snore, leak_p50, leak_p95, avg_leak_rate, max_leak_rate,
         avg_target_ventilation, therapy_mode, spo2_drops, odi)
        VALUES ($1, $2, $3, $4, $5, $6, $7, $8, $9, $10, $11, $12, $13,
                $14, $15, $16, $17, $18, $19, $20, $21, $22, $23, $24)
        ON CONFLICT (session_id) DO UPDATE
        SET total_events = EXCLUDED.total_events,
            ahi = EXCLUDED.ahi,
            obstructive_apneas = EXCLUDED.obstructive_apneas,
            central_apneas = EXCLUDED.central_apneas,
            hypopneas = EXCLUDED.hypopneas,
            reras = EXCLUDED.reras,
            clear_airway_apneas = EXCLUDED.clear_airway_apneas,
            avg_spo2 = EXCLUDED.avg_spo2,
            min_spo2 = EXCLUDED.min_spo2,
            avg_heart_rate = EXCLUDED.avg_heart_rate,
            max_heart_rate = EXCLUDED.max_heart_rate,
            min_heart_rate = EXCLUDED.min_heart_rate,
            avg_mask_pressure = EXCLUDED.avg_mask_pressure,
            avg_epr_pressure = EXCLUDED.avg_epr_pressure,
            avg_snore = EXCLUDED.avg_snore,
            leak_p50 = EXCLUDED.leak_p50,
            leak_p95 = EXCLUDED.leak_p95,
            avg_leak_rate = EXCLUDED.avg_leak_rate,
            max_leak_rate = EXCLUDED.max_leak_rate,
            avg_target_ventilation = EXCLUDED.avg_target_ventilation,
            therapy_mode = EXCLUDED.therapy_mode,
            spo2_drops = EXCLUDED.spo2_drops,
            odi = EXCLUDED.odi
    )";

    // Helper for optional<double> -> NULL or value
    auto opt_dbl = [](const std::optional<double>& v) -> std::optional<double> { return v; };
    auto opt_int = [](const std::optional<int>& v) -> std::optional<int> { return v; };

    work.exec_params(query,
        session_id,
        metrics.total_events,
        metrics.ahi,
        metrics.obstructive_apneas,
        metrics.central_apneas,
        metrics.hypopneas,
        metrics.reras,
        metrics.clear_airway_apneas,
        metrics.avg_spo2.value_or(0),
        metrics.min_spo2.value_or(0),
        metrics.avg_heart_rate.value_or(0),
        metrics.max_heart_rate.value_or(0),
        metrics.min_heart_rate.value_or(0),
        metrics.avg_mask_pressure.value_or(0),
        metrics.avg_epr_pressure.value_or(0),
        metrics.avg_snore.value_or(0),
        metrics.leak_p50.value_or(0),
        metrics.leak_p95.value_or(0),
        metrics.avg_leak_rate.value_or(0),
        metrics.max_leak_rate.value_or(0),
        metrics.avg_target_ventilation.value_or(0),
        metrics.therapy_mode.value_or(0),
        metrics.spo2_drops.value_or(0),
        metrics.odi.value_or(0)
    );
}

void DatabaseService::insertCalculatedMetrics(pqxx::work& work, int session_id,
                                                const std::vector<BreathingSummary>& summaries) {
    if (summaries.empty()) return;

    // Only insert summaries that have calculated metrics (BRP or PLD derived)
    std::vector<const BreathingSummary*> with_metrics;
    for (const auto& s : summaries) {
        if (s.respiratory_rate.has_value() || s.tidal_volume.has_value() ||
            s.minute_ventilation.has_value() || s.flow_limitation.has_value() ||
            s.mask_pressure.has_value() || s.snore_index.has_value() ||
            s.target_ventilation.has_value()) {
            with_metrics.push_back(&s);
        }
    }

    if (with_metrics.empty()) return;

    // Batch insert for performance
    std::ostringstream query;
    query << "INSERT INTO cpap_calculated_metrics "
          << "(session_id, timestamp, respiratory_rate, tidal_volume, minute_ventilation, "
          << "inspiratory_time, expiratory_time, ie_ratio, flow_limitation, leak_rate, "
          << "flow_p95, flow_p90, pressure_p95, pressure_p90, "
          << "mask_pressure, epr_pressure, snore_index, target_ventilation) VALUES ";

    for (size_t i = 0; i < with_metrics.size(); ++i) {
        const auto& s = *with_metrics[i];

        auto time_t = std::chrono::system_clock::to_time_t(s.timestamp);
        std::tm* tm = std::localtime(&time_t);
        std::ostringstream oss;
        oss << std::put_time(tm, "%Y-%m-%d %H:%M:%S");

        query << "(" << session_id << ", '" << oss.str() << "', ";

        // Respiratory rate
        if (s.respiratory_rate.has_value()) {
            query << s.respiratory_rate.value();
        } else {
            query << "NULL";
        }
        query << ", ";

        // Tidal volume
        if (s.tidal_volume.has_value()) {
            query << s.tidal_volume.value();
        } else {
            query << "NULL";
        }
        query << ", ";

        // Minute ventilation
        if (s.minute_ventilation.has_value()) {
            query << s.minute_ventilation.value();
        } else {
            query << "NULL";
        }
        query << ", ";

        // Inspiratory time
        if (s.inspiratory_time.has_value()) {
            query << s.inspiratory_time.value();
        } else {
            query << "NULL";
        }
        query << ", ";

        // Expiratory time
        if (s.expiratory_time.has_value()) {
            query << s.expiratory_time.value();
        } else {
            query << "NULL";
        }
        query << ", ";

        // I:E ratio
        if (s.ie_ratio.has_value()) {
            query << s.ie_ratio.value();
        } else {
            query << "NULL";
        }
        query << ", ";

        // Flow limitation
        if (s.flow_limitation.has_value()) {
            query << s.flow_limitation.value();
        } else {
            query << "NULL";
        }
        query << ", ";

        // Leak rate
        if (s.leak_rate.has_value()) {
            query << s.leak_rate.value();
        } else {
            query << "NULL";
        }
        query << ", ";

        // Flow P95
        if (s.flow_p95.has_value()) {
            query << s.flow_p95.value();
        } else {
            query << "NULL";
        }
        query << ", ";

        // Flow P90 (not currently calculated, placeholder)
        query << "NULL, ";

        // Pressure P95
        if (s.pressure_p95.has_value()) {
            query << s.pressure_p95.value();
        } else {
            query << "NULL";
        }
        query << ", ";

        // Pressure P90 (not currently calculated, placeholder)
        query << "NULL, ";

        // PLD-derived: mask_pressure
        if (s.mask_pressure.has_value()) {
            query << s.mask_pressure.value();
        } else {
            query << "NULL";
        }
        query << ", ";

        // PLD-derived: epr_pressure
        if (s.epr_pressure.has_value()) {
            query << s.epr_pressure.value();
        } else {
            query << "NULL";
        }
        query << ", ";

        // PLD-derived: snore_index
        if (s.snore_index.has_value()) {
            query << s.snore_index.value();
        } else {
            query << "NULL";
        }
        query << ", ";

        // PLD-derived: target_ventilation (ASV only)
        if (s.target_ventilation.has_value()) {
            query << s.target_ventilation.value();
        } else {
            query << "NULL";
        }

        query << ")";

        if (i < with_metrics.size() - 1) {
            query << ", ";
        }
    }

    query << " ON CONFLICT (session_id, timestamp) DO NOTHING";
    work.exec(query.str());
}

bool DatabaseService::saveSession(const CPAPSession& session) {
    if (!ensureConnection()) {
        std::cerr << "❌ DB: Not connected, cannot save session" << std::endl;
        return false;
    }

    std::lock_guard<std::recursive_mutex> lock(mutex_);

    try {
        pqxx::work txn(*conn_);

        std::cout << "💾 DB: Saving session..." << std::endl;

        // 1. Upsert device
        upsertDevice(txn, session);

        // 2. Insert session record
        int session_id = insertSession(txn, session);
        std::cout << "   Session ID: " << session_id << std::endl;

        // 3. Insert breathing summaries
        if (!session.breathing_summary.empty()) {
            insertBreathingSummaries(txn, session_id, session.breathing_summary);
            std::cout << "   Breathing summaries: " << session.breathing_summary.size() << std::endl;

            // 3a. Insert calculated respiratory metrics
            insertCalculatedMetrics(txn, session_id, session.breathing_summary);
            std::cout << "   Calculated metrics saved (RR, TV, MV, Ti/Te, I:E, FL, P95)" << std::endl;
        }

        // 4. Insert events
        if (!session.events.empty()) {
            insertEvents(txn, session_id, session.events);
            std::cout << "   Events: " << session.events.size() << std::endl;
        }

        // 4a. Insert SpO2 desaturations (as Desaturation events)
        if (!session.desaturations.empty()) {
            insertDesaturations(txn, session_id, session.desaturations);
            std::cout << "   Desaturations: " << session.desaturations.size() << std::endl;
        }

        // 4b. Insert breath-by-breath detail
        if (!session.breaths.empty()) {
            insertBreaths(txn, session_id, session.breaths);
            std::cout << "   Breaths: " << session.breaths.size() << std::endl;
        }

        // 5. Insert vitals
        if (!session.vitals.empty()) {
            insertVitals(txn, session_id, session.vitals);
            std::cout << "   Vitals: " << session.vitals.size() << std::endl;
        }

        // 6. Insert session metrics
        if (session.metrics.has_value()) {
            insertSessionMetrics(txn, session_id, session.metrics.value());
            std::cout << "   Metrics saved (AHI: " << session.metrics->ahi << ")" << std::endl;
        }

        txn.commit();

        std::cout << "✅ DB: Session saved successfully" << std::endl;
        return true;

    } catch (const std::exception& e) {
        std::cerr << "❌ DB: Failed to save session: " << e.what() << std::endl;
        return false;
    }
}

bool DatabaseService::updateDeviceLastSeen(const std::string& device_id) {
    if (!ensureConnection()) {
        return false;
    }

    std::lock_guard<std::recursive_mutex> lock(mutex_);

    try {
        pqxx::work txn(*conn_);

        std::string query = "UPDATE cpap_devices SET last_seen = CURRENT_TIMESTAMP WHERE device_id = $1";
        txn.exec_params(query, device_id);

        txn.commit();
        return true;

    } catch (const std::exception& e) {
        std::cerr << "❌ DB: Failed to update device: " << e.what() << std::endl;
        return false;
    }
}

std::optional<std::chrono::system_clock::time_point>
DatabaseService::getLastSessionStart(const std::string& device_id) {
    std::lock_guard<std::recursive_mutex> lock(mutex_);

    if (!ensureConnection()) {
        std::cerr << "DB: getLastSessionStart failed - no connection" << std::endl;
        return std::nullopt;
    }

    try {
        pqxx::work txn(*conn_);

        std::string query = R"(
            SELECT session_start
            FROM cpap_sessions
            WHERE device_id = $1
            ORDER BY session_start DESC
            LIMIT 1
        )";

        pqxx::result result = txn.exec_params(query, device_id);

        if (result.empty()) {
            std::cout << "DB: No previous sessions for device " << device_id << std::endl;
            return std::nullopt;
        }

        // Parse PostgreSQL timestamp (format: "YYYY-MM-DD HH:MM:SS")
        std::string timestamp_str = result[0][0].as<std::string>();
        std::tm tm = {};
        std::istringstream ss(timestamp_str);
        ss >> std::get_time(&tm, "%Y-%m-%d %H:%M:%S");

        if (ss.fail()) {
            std::cerr << "DB: Failed to parse timestamp: " << timestamp_str << std::endl;
            return std::nullopt;
        }

        tm.tm_isdst = -1;  // Let mktime auto-detect DST
        auto tp = std::chrono::system_clock::from_time_t(std::mktime(&tm));

        std::cout << "DB: Last session for " << device_id
                  << " at " << timestamp_str << std::endl;

        return tp;

    } catch (const std::exception& e) {
        std::cerr << "DB: getLastSessionStart error: " << e.what() << std::endl;
        return std::nullopt;
    }
}

std::optional<std::chrono::system_clock::time_point>
DatabaseService::getSessionStartForSleepDay(const std::string& device_id,
                                             const std::string& sleep_day,
                                             bool open_only) {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    if (!ensureConnection()) return std::nullopt;
    try {
        pqxx::work txn(*conn_);
        std::string query = R"(
            SELECT session_start
            FROM cpap_sessions
            WHERE device_id = $1
              AND DATE(session_start - INTERVAL '12 hours') = $2::date
        )";
        if (open_only) query += " AND session_end IS NULL";
        query += " ORDER BY session_start LIMIT 1";

        auto result = txn.exec_params(query, device_id, sleep_day);
        if (result.empty()) return std::nullopt;

        std::string ts = result[0][0].as<std::string>();
        std::tm tm = {};
        std::istringstream ss(ts);
        ss >> std::get_time(&tm, "%Y-%m-%d %H:%M:%S");
        if (ss.fail()) return std::nullopt;
        tm.tm_isdst = -1;
        return std::chrono::system_clock::from_time_t(std::mktime(&tm));
    } catch (const std::exception& e) {
        std::cerr << "DB: getSessionStartForSleepDay error: " << e.what() << std::endl;
        return std::nullopt;
    }
}

bool DatabaseService::sessionExists(const std::string& device_id,
                                    const std::chrono::system_clock::time_point& session_start) {
    std::lock_guard<std::recursive_mutex> lock(mutex_);

    if (!ensureConnection()) {
        std::cerr << "DB: sessionExists failed - no connection" << std::endl;
        return false;  // Assume doesn't exist if can't check
    }

    try {
        pqxx::work txn(*conn_);

        // Format session_start as PostgreSQL timestamp
        auto start_time_t = std::chrono::system_clock::to_time_t(session_start);
        std::tm* start_tm = std::localtime(&start_time_t);
        std::ostringstream start_oss;
        start_oss << std::put_time(start_tm, "%Y-%m-%d %H:%M:%S");

        // 5-second tolerance for minor timestamp parsing variations only
        std::string query = R"(
            SELECT EXISTS(
                SELECT 1
                FROM cpap_sessions
                WHERE device_id = $1
                  AND session_start BETWEEN $2::timestamp - INTERVAL '5 seconds'
                                        AND $2::timestamp + INTERVAL '5 seconds'
            )
        )";

        pqxx::result result = txn.exec_params(query, device_id, start_oss.str());

        bool exists = result[0][0].as<bool>();

        if (exists) {
            std::cout << "DB: Session " << start_oss.str() << " already exists in DB" << std::endl;
        }

        return exists;

    } catch (const std::exception& e) {
        std::cerr << "DB: sessionExists error: " << e.what() << std::endl;
        return false;  // Assume doesn't exist if error
    }
}

std::map<std::string, int> DatabaseService::getCheckpointFileSizes(
    const std::string& device_id,
    const std::chrono::system_clock::time_point& session_start) {

    std::lock_guard<std::recursive_mutex> lock(mutex_);

    if (!ensureConnection()) {
        std::cerr << "DB: getCheckpointFileSizes failed - no connection" << std::endl;
        return {};
    }

    try {
        pqxx::work txn(*conn_);

        // Format session_start as PostgreSQL timestamp
        auto start_time_t = std::chrono::system_clock::to_time_t(session_start);
        std::tm* start_tm = std::localtime(&start_time_t);
        std::ostringstream start_oss;
        start_oss << std::put_time(start_tm, "%Y-%m-%d %H:%M:%S");

        // 5-second tolerance for minor timestamp parsing variations only
        std::string query = R"(
            SELECT checkpoint_files
            FROM cpap_sessions
            WHERE device_id = $1
              AND session_start BETWEEN $2::timestamp - INTERVAL '5 seconds'
                                    AND $2::timestamp + INTERVAL '5 seconds'
            LIMIT 1
        )";

        pqxx::result result = txn.exec_params(query, device_id, start_oss.str());

        if (result.empty() || result[0][0].is_null()) {
            return {};  // Not found or NULL
        }

        // Parse JSONB to map
        std::string json_str = result[0][0].as<std::string>();
        std::map<std::string, int> file_sizes;

        // Simple JSON parsing for {"file1": 123, "file2": 456}
        size_t pos = 0;
        while ((pos = json_str.find("\"", pos)) != std::string::npos) {
            size_t name_start = pos + 1;
            size_t name_end = json_str.find("\"", name_start);
            if (name_end == std::string::npos) break;

            std::string filename = json_str.substr(name_start, name_end - name_start);

            size_t colon_pos = json_str.find(":", name_end);
            if (colon_pos == std::string::npos) break;

            size_t value_start = colon_pos + 1;
            size_t value_end = json_str.find_first_of(",}", value_start);
            if (value_end == std::string::npos) break;

            std::string value_str = json_str.substr(value_start, value_end - value_start);
            // Trim whitespace
            value_str.erase(0, value_str.find_first_not_of(" \t"));
            value_str.erase(value_str.find_last_not_of(" \t") + 1);

            file_sizes[filename] = std::stoi(value_str);
            pos = value_end;
        }

        return file_sizes;

    } catch (const std::exception& e) {
        std::cerr << "DB: getCheckpointFileSizes error: " << e.what() << std::endl;
        return {};
    }
}

std::map<std::string, int> DatabaseService::getCheckpointFilesByFolder(
    const std::string& device_id,
    const std::string& date_folder) {

    std::lock_guard<std::recursive_mutex> lock(mutex_);

    if (!ensureConnection()) {
        std::cerr << "DB: getCheckpointFilesByFolder failed - no connection" << std::endl;
        return {};
    }

    try {
        pqxx::work txn(*conn_);

        // Search checkpoint_files for any filename matching the date folder
        // OR the next day (cross-midnight sessions stored in previous day's folder)
        // e.g., folder 20260418 may contain files named 20260419_...
        int year, month, day;
        if (sscanf(date_folder.c_str(), "%4d%2d%2d", &year, &month, &day) != 3) return {};

        // Build next day string
        std::tm tm = {};
        tm.tm_year = year - 1900; tm.tm_mon = month - 1; tm.tm_mday = day + 1;
        tm.tm_isdst = -1;
        std::mktime(&tm);
        char next_day[9];
        std::strftime(next_day, sizeof(next_day), "%Y%m%d", &tm);

        std::string like1 = "%" + date_folder + "%";
        std::string like2 = "%" + std::string(next_day) + "%";

        std::string query = R"(
            SELECT checkpoint_files
            FROM cpap_sessions
            WHERE device_id = $1
              AND checkpoint_files IS NOT NULL
              AND (checkpoint_files::text LIKE $2 OR checkpoint_files::text LIKE $3)
        )";

        pqxx::result result = txn.exec_params(query, device_id, like1, like2);

        std::map<std::string, int> all_files;

        for (const auto& row : result) {
            if (row[0].is_null()) continue;

            std::string json_str = row[0].as<std::string>();

            // Parse JSONB: {"file1": 123, "file2": 456}
            size_t pos = 0;
            while ((pos = json_str.find("\"", pos)) != std::string::npos) {
                size_t name_start = pos + 1;
                size_t name_end = json_str.find("\"", name_start);
                if (name_end == std::string::npos) break;

                std::string filename = json_str.substr(name_start, name_end - name_start);

                size_t colon_pos = json_str.find(":", name_end);
                if (colon_pos == std::string::npos) break;

                size_t value_start = colon_pos + 1;
                size_t value_end = json_str.find_first_of(",}", value_start);
                if (value_end == std::string::npos) break;

                std::string value_str = json_str.substr(value_start, value_end - value_start);
                value_str.erase(0, value_str.find_first_not_of(" \t"));
                value_str.erase(value_str.find_last_not_of(" \t") + 1);

                all_files[filename] = std::stoi(value_str);
                pos = value_end;
            }
        }

        return all_files;

    } catch (const std::exception& e) {
        std::cerr << "DB: getCheckpointFilesByFolder error: " << e.what() << std::endl;
        return {};
    }
}

bool DatabaseService::updateCheckpointFileSizes(
    const std::string& device_id,
    const std::chrono::system_clock::time_point& session_start,
    const std::map<std::string, int>& file_sizes) {

    std::lock_guard<std::recursive_mutex> lock(mutex_);

    if (!ensureConnection()) {
        std::cerr << "DB: updateCheckpointFileSizes failed - no connection" << std::endl;
        return false;
    }

    try {
        pqxx::work txn(*conn_);

        // Format session_start as PostgreSQL timestamp
        auto start_time_t = std::chrono::system_clock::to_time_t(session_start);
        std::tm* start_tm = std::localtime(&start_time_t);
        std::ostringstream start_oss;
        start_oss << std::put_time(start_tm, "%Y-%m-%d %H:%M:%S");

        // Build JSON string
        std::ostringstream json_oss;
        json_oss << "{";
        bool first = true;
        for (const auto& [filename, size_kb] : file_sizes) {
            if (!first) json_oss << ",";
            json_oss << "\"" << filename << "\":" << size_kb;
            first = false;
        }
        json_oss << "}";

        // 5-second tolerance for minor timestamp parsing variations only
        std::string query = R"(
            UPDATE cpap_sessions
            SET checkpoint_files = $3::jsonb, updated_at = CURRENT_TIMESTAMP
            WHERE device_id = $1
              AND session_start BETWEEN $2::timestamp - INTERVAL '5 seconds'
                                    AND $2::timestamp + INTERVAL '5 seconds'
        )";

        txn.exec_params(query, device_id, start_oss.str(), json_oss.str());
        txn.commit();

        std::cout << "DB: Updated checkpoint_files (" << file_sizes.size() << " files)" << std::endl;
        return true;

    } catch (const std::exception& e) {
        std::cerr << "DB: updateCheckpointFileSizes error: " << e.what() << std::endl;
        return false;
    }
}

bool DatabaseService::markSessionCompleted(
    const std::string& device_id,
    const std::chrono::system_clock::time_point& session_start) {

    std::lock_guard<std::recursive_mutex> lock(mutex_);

    if (!ensureConnection()) {
        std::cerr << "DB: markSessionCompleted failed - no connection" << std::endl;
        return false;
    }

    try {
        pqxx::work txn(*conn_);

        // Format session_start as PostgreSQL timestamp
        auto start_time_t = std::chrono::system_clock::to_time_t(session_start);
        std::tm* start_tm = std::localtime(&start_time_t);
        std::ostringstream start_oss;
        start_oss << std::put_time(start_tm, "%Y-%m-%d %H:%M:%S");

        // Set session_end to now (session stopped growing)
        std::string query = R"(
            UPDATE cpap_sessions
            SET session_end = CURRENT_TIMESTAMP, updated_at = CURRENT_TIMESTAMP
            WHERE device_id = $1
              AND session_start BETWEEN $2::timestamp - INTERVAL '5 seconds'
                                    AND $2::timestamp + INTERVAL '5 seconds'
              AND session_end IS NULL
        )";

        auto result = txn.exec_params(query, device_id, start_oss.str());
        txn.commit();

        if (result.affected_rows() > 0) {
            std::cout << "✅ DB: Marked session " << start_oss.str() << " as COMPLETED" << std::endl;
            return true;
        } else {
            std::cout << "ℹ️  DB: Session already has session_end set" << std::endl;
            return false;
        }

    } catch (const std::exception& e) {
        std::cerr << "DB: markSessionCompleted error: " << e.what() << std::endl;
        return false;
    }
}

bool DatabaseService::reopenSession(
    const std::string& device_id,
    const std::chrono::system_clock::time_point& session_start) {

    std::lock_guard<std::recursive_mutex> lock(mutex_);

    if (!ensureConnection()) {
        std::cerr << "DB: reopenSession failed - no connection" << std::endl;
        return false;
    }

    try {
        pqxx::work txn(*conn_);

        auto start_time_t = std::chrono::system_clock::to_time_t(session_start);
        std::tm* start_tm = std::localtime(&start_time_t);
        std::ostringstream start_oss;
        start_oss << std::put_time(start_tm, "%Y-%m-%d %H:%M:%S");

        std::string query = R"(
            UPDATE cpap_sessions
            SET session_end = NULL, updated_at = CURRENT_TIMESTAMP
            WHERE device_id = $1
              AND session_start BETWEEN $2::timestamp - INTERVAL '5 seconds'
                                    AND $2::timestamp + INTERVAL '5 seconds'
              AND session_end IS NOT NULL
        )";

        auto result = txn.exec_params(query, device_id, start_oss.str());
        txn.commit();

        if (result.affected_rows() > 0) {
            std::cout << "DB: Reopened session " << start_oss.str()
                      << " (session_end cleared, session resumed)" << std::endl;
            return true;
        }
        return false;

    } catch (const std::exception& e) {
        std::cerr << "DB: reopenSession error: " << e.what() << std::endl;
        return false;
    }
}

bool DatabaseService::isForceCompleted(
    const std::string& device_id,
    const std::chrono::system_clock::time_point& session_start) {

    std::lock_guard<std::recursive_mutex> lock(mutex_);
    if (!ensureConnection()) return false;

    try {
        pqxx::work txn(*conn_);
        auto start_time_t = std::chrono::system_clock::to_time_t(session_start);
        std::tm* start_tm = std::localtime(&start_time_t);
        std::ostringstream oss;
        oss << std::put_time(start_tm, "%Y-%m-%d %H:%M:%S");

        auto result = txn.exec_params(
            "SELECT COALESCE(force_completed, FALSE) FROM cpap_sessions "
            "WHERE device_id = $1 "
            "AND session_start BETWEEN $2::timestamp - INTERVAL '5 seconds' "
            "AND $2::timestamp + INTERVAL '5 seconds' "
            "LIMIT 1",
            device_id, oss.str());
        txn.commit();

        return !result.empty() && result[0][0].as<bool>(false);
    } catch (const std::exception& e) {
        std::cerr << "DB: isForceCompleted error: " << e.what() << std::endl;
        return false;
    }
}

bool DatabaseService::setForceCompleted(
    const std::string& device_id,
    const std::chrono::system_clock::time_point& session_start) {

    std::lock_guard<std::recursive_mutex> lock(mutex_);
    if (!ensureConnection()) return false;

    try {
        pqxx::work txn(*conn_);
        auto start_time_t = std::chrono::system_clock::to_time_t(session_start);
        std::tm* start_tm = std::localtime(&start_time_t);
        std::ostringstream oss;
        oss << std::put_time(start_tm, "%Y-%m-%d %H:%M:%S");

        auto result = txn.exec_params(
            "UPDATE cpap_sessions SET force_completed = TRUE, updated_at = CURRENT_TIMESTAMP "
            "WHERE device_id = $1 "
            "AND session_start BETWEEN $2::timestamp - INTERVAL '5 seconds' "
            "AND $2::timestamp + INTERVAL '5 seconds'",
            device_id, oss.str());
        txn.commit();

        if (result.affected_rows() > 0) {
            std::cout << "DB: Session " << oss.str() << " marked force_completed" << std::endl;
            return true;
        }
        return false;
    } catch (const std::exception& e) {
        std::cerr << "DB: setForceCompleted error: " << e.what() << std::endl;
        return false;
    }
}

std::optional<SessionMetrics> DatabaseService::getSessionMetrics(
    const std::string& device_id,
    const std::chrono::system_clock::time_point& session_start) {

    std::lock_guard<std::recursive_mutex> lock(mutex_);

    if (!ensureConnection()) {
        std::cerr << "DB: getSessionMetrics failed - no connection" << std::endl;
        return std::nullopt;
    }

    try {
        pqxx::work txn(*conn_);

        auto start_time_t = std::chrono::system_clock::to_time_t(session_start);
        std::tm* start_tm = std::localtime(&start_time_t);
        std::ostringstream start_oss;
        start_oss << std::put_time(start_tm, "%Y-%m-%d %H:%M:%S");

        std::string query = R"(
            SELECT sm.total_events, sm.ahi, sm.obstructive_apneas, sm.central_apneas,
                   sm.hypopneas, sm.reras, sm.clear_airway_apneas,
                   sm.avg_event_duration, sm.max_event_duration, sm.time_in_apnea_percent,
                   sm.avg_spo2, sm.min_spo2, sm.avg_heart_rate, sm.max_heart_rate, sm.min_heart_rate,
                   round(s.duration_seconds / 3600.0, 4) AS usage_hours,
                   round(s.duration_seconds / 3600.0 * 100.0 / 8.0, 4) AS usage_percent,
                   c.avg_leak, c.max_leak, c.avg_rr, c.avg_tv, c.avg_mv,
                   c.avg_it, c.avg_et, c.avg_ie, c.avg_fl, c.fp95, c.pp95
            FROM cpap_session_metrics sm
            JOIN cpap_sessions s ON s.id = sm.session_id
            LEFT JOIN (
                SELECT session_id,
                       AVG(leak_rate) AS avg_leak, MAX(leak_rate) AS max_leak,
                       AVG(respiratory_rate) AS avg_rr, AVG(tidal_volume) AS avg_tv,
                       AVG(minute_ventilation) AS avg_mv, AVG(inspiratory_time) AS avg_it,
                       AVG(expiratory_time) AS avg_et, AVG(ie_ratio) AS avg_ie,
                       AVG(flow_limitation) AS avg_fl, AVG(flow_p95) AS fp95,
                       AVG(pressure_p95) AS pp95
                FROM cpap_calculated_metrics GROUP BY session_id
            ) c ON c.session_id = sm.session_id
            WHERE s.device_id = $1
              AND s.session_start BETWEEN $2::timestamp - INTERVAL '5 seconds'
                                      AND $2::timestamp + INTERVAL '5 seconds'
        )";

        auto result = txn.exec_params(query, device_id, start_oss.str());
        txn.commit();

        if (result.empty()) return std::nullopt;

        const auto& row = result[0];
        SessionMetrics m;
        m.total_events        = row["total_events"].as<int>(0);
        m.ahi                 = row["ahi"].as<double>(0.0);
        m.obstructive_apneas  = row["obstructive_apneas"].as<int>(0);
        m.central_apneas      = row["central_apneas"].as<int>(0);
        m.hypopneas           = row["hypopneas"].as<int>(0);
        m.reras               = row["reras"].as<int>(0);
        m.clear_airway_apneas = row["clear_airway_apneas"].as<int>(0);

        if (!row["avg_event_duration"].is_null())
            m.avg_event_duration = row["avg_event_duration"].as<double>();
        if (!row["max_event_duration"].is_null())
            m.max_event_duration = row["max_event_duration"].as<double>();
        if (!row["time_in_apnea_percent"].is_null())
            m.time_in_apnea_percent = row["time_in_apnea_percent"].as<double>();
        if (!row["usage_hours"].is_null())
            m.usage_hours = row["usage_hours"].as<double>();
        if (!row["usage_percent"].is_null())
            m.usage_percent = row["usage_percent"].as<double>();
        if (!row["avg_leak"].is_null())
            m.avg_leak_rate = row["avg_leak"].as<double>();
        if (!row["max_leak"].is_null())
            m.max_leak_rate = row["max_leak"].as<double>();
        if (!row["avg_rr"].is_null())
            m.avg_respiratory_rate = row["avg_rr"].as<double>();
        if (!row["avg_tv"].is_null())
            m.avg_tidal_volume = row["avg_tv"].as<double>();
        if (!row["avg_mv"].is_null())
            m.avg_minute_ventilation = row["avg_mv"].as<double>();
        if (!row["avg_it"].is_null())
            m.avg_inspiratory_time = row["avg_it"].as<double>();
        if (!row["avg_et"].is_null())
            m.avg_expiratory_time = row["avg_et"].as<double>();
        if (!row["avg_ie"].is_null())
            m.avg_ie_ratio = row["avg_ie"].as<double>();
        if (!row["avg_fl"].is_null())
            m.avg_flow_limitation = row["avg_fl"].as<double>();
        if (!row["fp95"].is_null())
            m.flow_p95 = row["fp95"].as<double>();
        if (!row["pp95"].is_null())
            m.pressure_p95 = row["pp95"].as<double>();
        if (!row["avg_spo2"].is_null() && row["avg_spo2"].as<double>(0.0) > 0)
            m.avg_spo2 = row["avg_spo2"].as<double>();
        if (!row["avg_heart_rate"].is_null() && row["avg_heart_rate"].as<int>(0) > 0)
            m.avg_heart_rate = row["avg_heart_rate"].as<int>();

        return m;

    } catch (const std::exception& e) {
        std::cerr << "DB: getSessionMetrics error: " << e.what() << std::endl;
        return std::nullopt;
    }
}

std::optional<SessionMetrics> DatabaseService::getNightlyMetrics(
    const std::string& device_id,
    const std::chrono::system_clock::time_point& session_start) {

    std::lock_guard<std::recursive_mutex> lock(mutex_);

    if (!ensureConnection()) {
        std::cerr << "DB: getNightlyMetrics failed - no connection" << std::endl;
        return std::nullopt;
    }

    try {
        pqxx::work txn(*conn_);

        auto start_time_t = std::chrono::system_clock::to_time_t(session_start);
        std::tm* start_tm = std::localtime(&start_time_t);
        std::ostringstream start_oss;
        start_oss << std::put_time(start_tm, "%Y-%m-%d %H:%M:%S");

        // Sleep day = DATE(session_start - 12h), groups evening→morning of one night.
        // Events use MAX because all BRP sessions share the same nightly EVE file.
        // Duration is summed across all therapy periods in the night.
        // AHI recomputed from MAX(total_events) / SUM(duration).
        std::string query = R"(
            WITH night AS (
                SELECT DATE(session_start - INTERVAL '12 hours') AS sleep_day
                FROM cpap_sessions
                WHERE device_id = $1
                  AND session_start BETWEEN $2::timestamp - INTERVAL '5 seconds'
                                        AND $2::timestamp + INTERVAL '5 seconds'
                LIMIT 1
            )
            SELECT
                SUM(s.duration_seconds)                         AS total_seconds,
                MAX(sm.total_events)                            AS total_events,
                MAX(sm.obstructive_apneas)                      AS obstructive_apneas,
                MAX(sm.central_apneas)                          AS central_apneas,
                MAX(sm.hypopneas)                               AS hypopneas,
                MAX(sm.reras)                                   AS reras,
                MAX(sm.clear_airway_apneas)                     AS clear_airway_apneas,
                MAX(sm.avg_event_duration)                      AS avg_event_duration,
                MAX(sm.max_event_duration)                      AS max_event_duration,
                CASE WHEN SUM(s.duration_seconds) > 0
                     THEN round((SUM(s.duration_seconds) / 3600.0)::numeric, 4)
                     ELSE 0 END                                 AS usage_hours,
                CASE WHEN SUM(s.duration_seconds) > 0
                     THEN round((SUM(s.duration_seconds) / 3600.0 * 100.0 / 8.0)::numeric, 4)
                     ELSE 0 END                                 AS usage_percent,
                CASE WHEN SUM(s.duration_seconds) > 0
                     THEN round(((COALESCE(MAX(sm.obstructive_apneas), 0) + COALESCE(MAX(sm.central_apneas), 0)
                                + COALESCE(MAX(sm.hypopneas), 0) + COALESCE(MAX(sm.clear_airway_apneas), 0))
                              * 3600.0 / SUM(s.duration_seconds))::numeric, 4)
                     ELSE 0 END                                 AS ahi,
                CASE WHEN SUM(s.duration_seconds) > 0 AND MAX(sm.avg_event_duration) IS NOT NULL
                     THEN round((MAX(sm.total_events) * MAX(sm.avg_event_duration)
                              / SUM(s.duration_seconds) * 100.0)::numeric, 4)
                     ELSE 0 END                                 AS time_in_apnea_pct,
                AVG(c.avg_leak)  AS avg_leak,  MAX(c.max_leak) AS max_leak,
                AVG(c.avg_rr)    AS avg_rr,    AVG(c.avg_tv)   AS avg_tv,
                AVG(c.avg_mv)    AS avg_mv,    AVG(c.avg_it)   AS avg_it,
                AVG(c.avg_et)    AS avg_et,    AVG(c.avg_ie)   AS avg_ie,
                AVG(c.avg_fl)    AS avg_fl,    AVG(c.fp95)     AS fp95,
                AVG(c.pp95)      AS pp95,
                AVG(c.avg_mask_press) AS avg_mask_pressure,
                AVG(c.avg_epr_press)  AS avg_epr_pressure,
                AVG(c.avg_snore_idx)  AS avg_snore,
                AVG(c.avg_tgt_vent)   AS avg_target_ventilation,
                AVG(b.avg_press) AS avg_pressure,
                MAX(b.max_press) AS max_pressure,
                MIN(b.min_press) AS min_pressure,
                MAX(sm.leak_p50) AS leak_p50,
                MAX(sm.leak_p95) AS leak_p95_sess,
                MAX(sm.therapy_mode) AS therapy_mode
            FROM cpap_sessions s
            JOIN cpap_session_metrics sm ON sm.session_id = s.id
            LEFT JOIN (
                SELECT session_id,
                       AVG(leak_rate) AS avg_leak, MAX(leak_rate) AS max_leak,
                       AVG(respiratory_rate) AS avg_rr, AVG(tidal_volume) AS avg_tv,
                       AVG(minute_ventilation) AS avg_mv, AVG(inspiratory_time) AS avg_it,
                       AVG(expiratory_time) AS avg_et, AVG(ie_ratio) AS avg_ie,
                       AVG(flow_limitation) AS avg_fl, AVG(flow_p95) AS fp95,
                       AVG(pressure_p95) AS pp95,
                       AVG(mask_pressure) AS avg_mask_press,
                       AVG(epr_pressure) AS avg_epr_press,
                       AVG(snore_index) AS avg_snore_idx,
                       AVG(target_ventilation) AS avg_tgt_vent
                FROM cpap_calculated_metrics GROUP BY session_id
            ) c ON c.session_id = sm.session_id
            LEFT JOIN (
                SELECT session_id,
                       AVG(avg_pressure) AS avg_press,
                       MAX(max_pressure) AS max_press,
                       MIN(min_pressure) AS min_press
                FROM cpap_breathing_summary GROUP BY session_id
            ) b ON b.session_id = sm.session_id
            WHERE s.device_id = $1
              AND DATE(s.session_start - INTERVAL '12 hours') = (SELECT sleep_day FROM night)
        )";

        auto result = txn.exec_params(query, device_id, start_oss.str());
        txn.commit();

        if (result.empty()) return std::nullopt;

        const auto& row = result[0];
        if (row["total_seconds"].is_null()) return std::nullopt;

        SessionMetrics m;
        m.total_events        = row["total_events"].as<int>(0);
        m.ahi                 = row["ahi"].as<double>(0.0);
        m.obstructive_apneas  = row["obstructive_apneas"].as<int>(0);
        m.central_apneas      = row["central_apneas"].as<int>(0);
        m.hypopneas           = row["hypopneas"].as<int>(0);
        m.reras               = row["reras"].as<int>(0);
        m.clear_airway_apneas = row["clear_airway_apneas"].as<int>(0);

        if (!row["avg_event_duration"].is_null())
            m.avg_event_duration = row["avg_event_duration"].as<double>();
        if (!row["max_event_duration"].is_null())
            m.max_event_duration = row["max_event_duration"].as<double>();
        if (!row["time_in_apnea_pct"].is_null())
            m.time_in_apnea_percent = row["time_in_apnea_pct"].as<double>();
        if (!row["usage_hours"].is_null())
            m.usage_hours = row["usage_hours"].as<double>();
        if (!row["usage_percent"].is_null())
            m.usage_percent = row["usage_percent"].as<double>();
        if (!row["avg_leak"].is_null())
            m.avg_leak_rate = row["avg_leak"].as<double>();
        if (!row["max_leak"].is_null())
            m.max_leak_rate = row["max_leak"].as<double>();
        if (!row["avg_rr"].is_null())
            m.avg_respiratory_rate = row["avg_rr"].as<double>();
        if (!row["avg_tv"].is_null())
            m.avg_tidal_volume = row["avg_tv"].as<double>();
        if (!row["avg_mv"].is_null())
            m.avg_minute_ventilation = row["avg_mv"].as<double>();
        if (!row["avg_it"].is_null())
            m.avg_inspiratory_time = row["avg_it"].as<double>();
        if (!row["avg_et"].is_null())
            m.avg_expiratory_time = row["avg_et"].as<double>();
        if (!row["avg_ie"].is_null())
            m.avg_ie_ratio = row["avg_ie"].as<double>();
        if (!row["avg_fl"].is_null())
            m.avg_flow_limitation = row["avg_fl"].as<double>();
        if (!row["fp95"].is_null())
            m.flow_p95 = row["fp95"].as<double>();
        if (!row["pp95"].is_null())
            m.pressure_p95 = row["pp95"].as<double>();
        if (!row["avg_pressure"].is_null())
            m.avg_pressure = row["avg_pressure"].as<double>();
        if (!row["max_pressure"].is_null())
            m.max_pressure = row["max_pressure"].as<double>();
        if (!row["min_pressure"].is_null())
            m.min_pressure = row["min_pressure"].as<double>();
        // PLD-derived
        if (!row["avg_mask_pressure"].is_null())
            m.avg_mask_pressure = row["avg_mask_pressure"].as<double>();
        if (!row["avg_epr_pressure"].is_null())
            m.avg_epr_pressure = row["avg_epr_pressure"].as<double>();
        if (!row["avg_snore"].is_null())
            m.avg_snore = row["avg_snore"].as<double>();
        if (!row["avg_target_ventilation"].is_null() && row["avg_target_ventilation"].as<double>(0) > 0)
            m.avg_target_ventilation = row["avg_target_ventilation"].as<double>();
        if (!row["leak_p50"].is_null())
            m.leak_p50 = row["leak_p50"].as<double>();
        if (!row["leak_p95_sess"].is_null())
            m.leak_p95 = row["leak_p95_sess"].as<double>();
        if (!row["therapy_mode"].is_null())
            m.therapy_mode = row["therapy_mode"].as<int>();

        return m;

    } catch (const std::exception& e) {
        std::cerr << "DB: getNightlyMetrics error: " << e.what() << std::endl;
        return std::nullopt;
    }
}

std::vector<SessionMetrics> DatabaseService::getMetricsForDateRange(
    const std::string& device_id, int days_back) {

    std::lock_guard<std::recursive_mutex> lock(mutex_);

    if (!ensureConnection()) {
        std::cerr << "DB: getMetricsForDateRange failed - no connection" << std::endl;
        return {};
    }

    try {
        auto cutoff = std::chrono::system_clock::now() - std::chrono::hours(days_back * 24);
        auto cutoff_t = std::chrono::system_clock::to_time_t(cutoff);
        std::tm* cutoff_tm = std::localtime(&cutoff_t);
        std::ostringstream cutoff_oss;
        cutoff_oss << std::put_time(cutoff_tm, "%Y-%m-%d %H:%M:%S");



        // One row per sleep-night, same aggregation as getNightlyMetrics.
        // Oldest-first so the LLM sees the trend chronologically.
        std::string query = R"(
            SELECT
                DATE(s.session_start - INTERVAL '12 hours')     AS sleep_day,
                SUM(s.duration_seconds)                         AS total_seconds,
                MAX(sm.total_events)                            AS total_events,
                MAX(sm.obstructive_apneas)                      AS obstructive_apneas,
                MAX(sm.central_apneas)                          AS central_apneas,
                MAX(sm.hypopneas)                               AS hypopneas,
                MAX(sm.reras)                                   AS reras,
                MAX(sm.clear_airway_apneas)                     AS clear_airway_apneas,
                MAX(sm.avg_event_duration)                      AS avg_event_duration,
                MAX(sm.max_event_duration)                      AS max_event_duration,
                CASE WHEN SUM(s.duration_seconds) > 0
                     THEN round((SUM(s.duration_seconds) / 3600.0)::numeric, 4)
                     ELSE 0 END                                 AS usage_hours,
                CASE WHEN SUM(s.duration_seconds) > 0
                     THEN round((SUM(s.duration_seconds) / 3600.0 * 100.0 / 8.0)::numeric, 4)
                     ELSE 0 END                                 AS usage_percent,
                CASE WHEN SUM(s.duration_seconds) > 0
                     THEN round(((COALESCE(MAX(sm.obstructive_apneas), 0) + COALESCE(MAX(sm.central_apneas), 0)
                                + COALESCE(MAX(sm.hypopneas), 0) + COALESCE(MAX(sm.clear_airway_apneas), 0))
                              * 3600.0 / SUM(s.duration_seconds))::numeric, 4)
                     ELSE 0 END                                 AS ahi,
                AVG(c.avg_leak) AS avg_leak, MAX(c.max_leak)    AS max_leak,
                AVG(c.avg_rr)  AS avg_rr,   AVG(c.avg_tv)      AS avg_tv,
                AVG(c.avg_mv)  AS avg_mv,   AVG(c.avg_fl)      AS avg_fl,
                AVG(c.pp95)    AS pp95,
                AVG(c.avg_mask_press) AS avg_mask_pressure,
                AVG(c.avg_epr_press)  AS avg_epr_pressure,
                AVG(c.avg_snore_idx)  AS avg_snore,
                AVG(c.avg_tgt_vent)   AS avg_target_ventilation,
                AVG(b.avg_press) AS avg_pressure,
                MAX(b.max_press) AS max_pressure,
                MIN(b.min_press) AS min_pressure,
                MAX(sm.leak_p50) AS leak_p50,
                MAX(sm.leak_p95) AS leak_p95_sess,
                MAX(sm.therapy_mode) AS therapy_mode
            FROM cpap_sessions s
            JOIN cpap_session_metrics sm ON sm.session_id = s.id
            LEFT JOIN (
                SELECT session_id,
                       AVG(leak_rate) AS avg_leak, MAX(leak_rate) AS max_leak,
                       AVG(respiratory_rate) AS avg_rr, AVG(tidal_volume) AS avg_tv,
                       AVG(minute_ventilation) AS avg_mv, AVG(flow_limitation) AS avg_fl,
                       AVG(pressure_p95) AS pp95,
                       AVG(mask_pressure) AS avg_mask_press,
                       AVG(epr_pressure) AS avg_epr_press,
                       AVG(snore_index) AS avg_snore_idx,
                       AVG(target_ventilation) AS avg_tgt_vent
                FROM cpap_calculated_metrics GROUP BY session_id
            ) c ON c.session_id = sm.session_id
            LEFT JOIN (
                SELECT session_id,
                       AVG(avg_pressure) AS avg_press,
                       MAX(max_pressure) AS max_press,
                       MIN(min_pressure) AS min_press
                FROM cpap_breathing_summary GROUP BY session_id
            ) b ON b.session_id = sm.session_id
            WHERE s.device_id = ')" + device_id + R"('
              AND s.session_start >= ')" + cutoff_oss.str() + R"('::timestamp
              AND s.session_end IS NOT NULL
            GROUP BY DATE(s.session_start - INTERVAL '12 hours')
            ORDER BY sleep_day ASC
        )";
        // Use libpq C API directly to avoid pqxx cross-compiler ABI issues
        // with field::as<T>() template instantiations (SEGV on ARM).
        PGconn* pgconn = PQconnectdb(connection_string_.c_str());
        if (PQstatus(pgconn) != CONNECTION_OK) {
            std::cerr << "DB: libpq connect failed: " << PQerrorMessage(pgconn) << std::endl;
            PQfinish(pgconn);
            return {};
        }
        PGresult* pgr = PQexec(pgconn, query.c_str());

        if (PQresultStatus(pgr) != PGRES_TUPLES_OK) {
            std::cerr << "DB: getMetricsForDateRange query failed: "
                      << PQresultErrorMessage(pgr) << std::endl;
            PQclear(pgr);
            return {};
        }

        int nrows = PQntuples(pgr);
        int ncols = PQnfields(pgr);


        auto pgStr = [&](int row, int col) -> std::string {
            return PQgetisnull(pgr, row, col) ? "" : PQgetvalue(pgr, row, col);
        };
        auto pgInt = [&](int row, int col, int def = 0) -> int {
            return PQgetisnull(pgr, row, col) ? def : std::stoi(PQgetvalue(pgr, row, col));
        };
        auto pgDbl = [&](int row, int col, double def = 0.0) -> double {
            return PQgetisnull(pgr, row, col) ? def : std::stod(PQgetvalue(pgr, row, col));
        };
        auto pgNull = [&](int row, int col) -> bool {
            return PQgetisnull(pgr, row, col) != 0;
        };

        std::vector<SessionMetrics> nights;
        for (int r = 0; r < nrows; r++) {
            if (pgNull(r, 1)) continue;

            SessionMetrics m;
            m.sleep_day           = pgStr(r, 0);
            m.total_events        = pgInt(r, 2);
            m.ahi                 = pgDbl(r, 12);
            m.obstructive_apneas  = pgInt(r, 3);
            m.central_apneas      = pgInt(r, 4);
            m.hypopneas           = pgInt(r, 5);
            m.reras               = pgInt(r, 6);
            m.clear_airway_apneas = pgInt(r, 7);

            if (!pgNull(r, 8))  m.avg_event_duration     = pgDbl(r, 8);
            if (!pgNull(r, 9))  m.max_event_duration     = pgDbl(r, 9);
            if (!pgNull(r, 10)) m.usage_hours            = pgDbl(r, 10);
            if (!pgNull(r, 11)) m.usage_percent          = pgDbl(r, 11);
            if (!pgNull(r, 13)) m.avg_leak_rate          = pgDbl(r, 13);
            if (!pgNull(r, 14)) m.max_leak_rate          = pgDbl(r, 14);
            if (!pgNull(r, 15)) m.avg_respiratory_rate   = pgDbl(r, 15);
            if (!pgNull(r, 16)) m.avg_tidal_volume       = pgDbl(r, 16);
            if (!pgNull(r, 17)) m.avg_minute_ventilation = pgDbl(r, 17);
            if (!pgNull(r, 18)) m.avg_flow_limitation    = pgDbl(r, 18);
            if (!pgNull(r, 19)) m.pressure_p95           = pgDbl(r, 19);
            if (!pgNull(r, 24)) m.avg_pressure           = pgDbl(r, 24);
            if (!pgNull(r, 25)) m.max_pressure           = pgDbl(r, 25);
            if (!pgNull(r, 26)) m.min_pressure           = pgDbl(r, 26);
            if (!pgNull(r, 20)) m.avg_mask_pressure      = pgDbl(r, 20);
            if (!pgNull(r, 21)) m.avg_epr_pressure       = pgDbl(r, 21);
            if (!pgNull(r, 22)) m.avg_snore              = pgDbl(r, 22);
            if (!pgNull(r, 23) && pgDbl(r, 23) > 0)
                m.avg_target_ventilation = pgDbl(r, 23);
            if (!pgNull(r, 27)) m.leak_p50               = pgDbl(r, 27);
            if (!pgNull(r, 28)) m.leak_p95               = pgDbl(r, 28);
            if (!pgNull(r, 29)) m.therapy_mode           = pgInt(r, 29);

            nights.push_back(std::move(m));
        }
        PQclear(pgr);
        PQfinish(pgconn);

        std::cout << "DB: getMetricsForDateRange(" << days_back << " days) returned "
                  << nights.size() << " nights" << std::endl;
        return nights;

    } catch (const std::exception& e) {
        std::cerr << "DB: getMetricsForDateRange error: " << e.what() << std::endl;
        return {};
    }
}

bool DatabaseService::saveSummary(
    const std::string& device_id,
    const std::string& period,
    const std::string& range_start,
    const std::string& range_end,
    int nights_count,
    double avg_ahi,
    double avg_usage_hours,
    double compliance_pct,
    const std::string& summary_text) {

    std::lock_guard<std::recursive_mutex> lock(mutex_);
    if (!ensureConnection()) return false;

    try {
        pqxx::work txn(*conn_);
        std::string query =
            "INSERT INTO cpap_summaries"
            " (device_id, period, range_start, range_end, nights_count,"
            "  avg_ahi, avg_usage_hours, compliance_pct, summary_text)"
            " VALUES (" + txn.quote(device_id) + ", " + txn.quote(period) + ", "
            + txn.quote(range_start) + "::date, " + txn.quote(range_end) + "::date, "
            + std::to_string(nights_count) + ", "
            + std::to_string(avg_ahi) + ", "
            + std::to_string(avg_usage_hours) + ", "
            + std::to_string(compliance_pct) + ", "
            + txn.quote(summary_text) + ")";
        txn.exec(query);
        txn.commit();
        std::cout << "DB: Saved " << period << " summary (" << range_start
                  << " to " << range_end << ", " << nights_count << " nights)" << std::endl;
        return true;
    } catch (const std::exception& e) {
        std::cerr << "DB: saveSummary error: " << e.what() << std::endl;
        return false;
    }
}

int DatabaseService::deleteSessionsByDateFolder(const std::string& device_id,
                                                 const std::string& date_folder) {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    if (!ensureConnection()) return -1;

    try {
        pqxx::work work(*conn_);

        // Match sessions whose brp_file_path contains the date folder
        std::string pattern = "%DATALOG/" + date_folder + "/%";
        auto result = work.exec_params(
            "DELETE FROM cpap_sessions "
            "WHERE device_id = $1 AND brp_file_path::text LIKE $2",
            device_id, pattern);

        int deleted = static_cast<int>(result.affected_rows());
        work.commit();
        return deleted;

    } catch (const std::exception& e) {
        std::cerr << "DB: Failed to delete sessions for " << date_folder
                  << ": " << e.what() << std::endl;
        return -1;
    }
}

bool DatabaseService::saveSTRDailyRecords(const std::vector<STRDailyRecord>& records) {
    if (records.empty()) return true;

    if (!ensureConnection()) {
        std::cerr << "DB: saveSTRDailyRecords failed - no connection" << std::endl;
        return false;
    }

    std::lock_guard<std::recursive_mutex> lock(mutex_);

    try {
        pqxx::work txn(*conn_);

        // Ensure table exists
        txn.exec(R"(
            CREATE TABLE IF NOT EXISTS cpap_daily_summary (
                id SERIAL PRIMARY KEY,
                device_id TEXT NOT NULL,
                record_date DATE NOT NULL,
                mask_pairs JSONB DEFAULT '[]',
                mask_events INT DEFAULT 0,
                duration_minutes FLOAT DEFAULT 0,
                patient_hours FLOAT DEFAULT 0,
                ahi FLOAT, hi FLOAT, ai FLOAT, oai FLOAT, cai FLOAT, uai FLOAT,
                rin FLOAT, csr FLOAT,
                mask_press_50 FLOAT, mask_press_95 FLOAT, mask_press_max FLOAT,
                leak_50 FLOAT, leak_95 FLOAT, leak_max FLOAT,
                spo2_50 FLOAT, spo2_95 FLOAT,
                resp_rate_50 FLOAT, tid_vol_50 FLOAT, min_vent_50 FLOAT,
                mode INT, epr_level FLOAT, pressure_setting FLOAT,
                fault_device INT DEFAULT 0, fault_alarm INT DEFAULT 0,
                created_at TIMESTAMP DEFAULT NOW(),
                updated_at TIMESTAMP DEFAULT NOW(),
                UNIQUE (device_id, record_date)
            )
        )");

        for (const auto& r : records) {
            auto date_t = std::chrono::system_clock::to_time_t(r.record_date);
            std::tm* tm = std::localtime(&date_t);
            std::ostringstream date_oss;
            date_oss << std::put_time(tm, "%Y-%m-%d");

            // Build mask_pairs JSON
            std::ostringstream pairs_json;
            pairs_json << "[";
            for (size_t i = 0; i < r.mask_pairs.size(); ++i) {
                if (i > 0) pairs_json << ",";
                auto on_t = std::chrono::system_clock::to_time_t(r.mask_pairs[i].first);
                auto off_t = std::chrono::system_clock::to_time_t(r.mask_pairs[i].second);
                std::tm on_tm = *std::localtime(&on_t);
                std::tm off_tm = *std::localtime(&off_t);
                std::ostringstream on_oss, off_oss;
                on_oss << std::put_time(&on_tm, "%Y-%m-%dT%H:%M:%S");
                off_oss << std::put_time(&off_tm, "%Y-%m-%dT%H:%M:%S");
                pairs_json << "{\"on\":\"" << on_oss.str() << "\",\"off\":\"" << off_oss.str() << "\"}";
            }
            pairs_json << "]";

            std::string query = R"(
                INSERT INTO cpap_daily_summary
                    (device_id, record_date, mask_pairs, mask_events, duration_minutes, patient_hours,
                     ahi, hi, ai, oai, cai, uai, rin, csr,
                     mask_press_50, mask_press_95, mask_press_max,
                     leak_50, leak_95, leak_max,
                     spo2_50, spo2_95,
                     resp_rate_50, tid_vol_50, min_vent_50,
                     mode, epr_level, pressure_setting,
                     fault_device, fault_alarm, updated_at)
                VALUES ($1, $2, $3::jsonb, $4, $5, $6,
                        $7, $8, $9, $10, $11, $12, $13, $14,
                        $15, $16, $17,
                        $18, $19, $20,
                        $21, $22,
                        $23, $24, $25,
                        $26, $27, $28,
                        $29, $30, NOW())
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
            )";

            txn.exec_params(query,
                r.device_id, date_oss.str(), pairs_json.str(),
                r.mask_events, r.duration_minutes, r.patient_hours,
                r.ahi, r.hi, r.ai, r.oai, r.cai, r.uai, r.rin, r.csr,
                r.mask_press_50, r.mask_press_95, r.mask_press_max,
                r.leak_50, r.leak_95, r.leak_max,
                r.spo2_50, r.spo2_95,
                r.resp_rate_50, r.tid_vol_50, r.min_vent_50,
                r.mode, r.epr_level, r.pressure_setting,
                r.fault_device, r.fault_alarm
            );
        }

        txn.commit();
        std::cout << "DB: Saved " << records.size() << " STR daily records" << std::endl;
        return true;

    } catch (const std::exception& e) {
        std::cerr << "DB: saveSTRDailyRecords error: " << e.what() << std::endl;
        return false;
    }
}

std::optional<std::string> DatabaseService::getLastSTRDate(const std::string& device_id) {
    std::lock_guard<std::recursive_mutex> lock(mutex_);

    if (!ensureConnection()) return std::nullopt;

    try {
        pqxx::work txn(*conn_);
        auto result = txn.exec_params(
            "SELECT MAX(record_date)::text FROM cpap_daily_summary WHERE device_id = $1",
            device_id
        );
        txn.commit();

        if (result.empty() || result[0][0].is_null()) return std::nullopt;
        return result[0][0].as<std::string>();

    } catch (const std::exception& e) {
        std::cerr << "DB: getLastSTRDate error: " << e.what() << std::endl;
        return std::nullopt;
    }
}

bool DatabaseService::executeRaw(const std::string& sql) {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    if (!ensureConnection()) return false;

    try {
        pqxx::work txn(*conn_);
        txn.exec(sql);
        txn.commit();
        return true;
    } catch (const std::exception& e) {
        std::cerr << "DB: executeRaw error: " << e.what() << std::endl;
        return false;
    }
}

// ── Oximetry (O2 Ring) ──────────────────────────────────────────────────

static std::string fmtTs(std::chrono::system_clock::time_point tp) {
    auto tt = std::chrono::system_clock::to_time_t(tp);
    std::tm tm{}; gmtime_r(&tt, &tm);
    std::ostringstream oss;
    oss << std::put_time(&tm, "%Y-%m-%d %H:%M:%S");
    return oss.str();
}

bool DatabaseService::saveOximetrySession(const std::string& device_id,
                                           const cpapdash::parser::OximetrySession& session) {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    if (!ensureConnection()) return false;

    try {
        pqxx::work txn(*conn_);

        // Upsert session
        auto r = txn.exec_params(R"(
            INSERT INTO oximetry_sessions
                (device_id, filename, start_time, end_time, duration_seconds,
                 sample_interval, avg_spo2, min_spo2, spo2_baseline,
                 time_below_90, time_below_88, odi_3pct, desat_count,
                 avg_hr, min_hr, max_hr, valid_samples, total_samples,
                 cpap_session_date)
            VALUES ($1, $2, $3, $4, $5, $6, $7, $8, $9, $10, $11, $12, $13, $14, $15, $16, $17, $18, $19)
            ON CONFLICT (filename) DO UPDATE SET
                end_time = EXCLUDED.end_time,
                duration_seconds = EXCLUDED.duration_seconds,
                avg_spo2 = EXCLUDED.avg_spo2,
                min_spo2 = EXCLUDED.min_spo2,
                spo2_baseline = EXCLUDED.spo2_baseline,
                time_below_90 = EXCLUDED.time_below_90,
                time_below_88 = EXCLUDED.time_below_88,
                odi_3pct = EXCLUDED.odi_3pct,
                desat_count = EXCLUDED.desat_count,
                avg_hr = EXCLUDED.avg_hr,
                min_hr = EXCLUDED.min_hr,
                max_hr = EXCLUDED.max_hr,
                valid_samples = EXCLUDED.valid_samples,
                total_samples = EXCLUDED.total_samples
            RETURNING id
        )",
            device_id, session.filename,
            fmtTs(session.start_time),
            fmtTs(session.end_time),
            session.duration_seconds, session.sample_interval,
            session.metrics.avg_spo2, session.metrics.min_spo2,
            session.metrics.spo2_baseline,
            session.metrics.time_below_90_pct, session.metrics.time_below_88_pct,
            session.metrics.odi_3pct, session.metrics.desat_count_3pct,
            session.metrics.avg_hr, session.metrics.min_hr, session.metrics.max_hr,
            session.metrics.valid_samples, session.metrics.total_samples,
            session.date_str()
        );

        int session_id = r[0][0].as<int>();

        // Delete old samples, reinsert
        txn.exec_params("DELETE FROM oximetry_samples WHERE oximetry_session_id = $1", session_id);

        if (!session.samples.empty()) {
            std::ostringstream q;
            q << "INSERT INTO oximetry_samples (oximetry_session_id, timestamp, spo2, heart_rate, motion, vibration, valid, source) VALUES ";

            for (size_t i = 0; i < session.samples.size(); i++) {
                const auto& s = session.samples[i];
                auto tt = std::chrono::system_clock::to_time_t(s.timestamp);
                std::tm* tm = std::localtime(&tt);
                std::ostringstream ts;
                ts << std::put_time(tm, "%Y-%m-%d %H:%M:%S");

                q << "(" << session_id << ", '" << ts.str() << "', "
                  << (int)s.spo2 << ", " << (int)s.heart_rate << ", "
                  << (int)s.motion << ", " << (int)s.vibration << ", "
                  << (s.valid() ? "true" : "false") << ", 'vld')";

                if (i < session.samples.size() - 1) q << ", ";
            }

            txn.exec(q.str());
        }

        txn.commit();
        std::cout << "O2Ring: Saved to PostgreSQL (" << session.filename
                  << ", " << session.samples.size() << " samples)" << std::endl;
        return true;

    } catch (const std::exception& e) {
        std::cerr << "O2Ring: PostgreSQL save failed: " << e.what() << std::endl;
        return false;
    }
}

bool DatabaseService::oximetrySessionExists(const std::string& device_id,
                                             const std::string& filename) {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    if (!ensureConnection()) return false;

    try {
        pqxx::work txn(*conn_);
        auto r = txn.exec_params(
            "SELECT 1 FROM oximetry_sessions WHERE device_id = $1 AND filename = $2",
            device_id, filename);
        txn.commit();
        return !r.empty();
    } catch (...) {
        return false;
    }
}

bool DatabaseService::saveLiveOximetrySample(const std::string& device_id,
                                              const std::string& date,
                                              int spo2, int hr, int motion) {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    if (!ensureConnection()) return false;

    try {
        pqxx::work txn(*conn_);

        std::string live_filename = "live_" + date + ".vld";

        // Get or create live session
        auto r = txn.exec_params(
            "SELECT id FROM oximetry_sessions WHERE device_id = $1 AND filename = $2",
            device_id, live_filename);

        int session_id;
        if (r.empty()) {
            auto ins = txn.exec_params(R"(
                INSERT INTO oximetry_sessions
                    (device_id, filename, start_time, end_time, duration_seconds,
                     sample_interval, valid_samples, total_samples)
                VALUES ($1, $2, NOW(), NOW(), 0, 0, 0, 0)
                RETURNING id
            )", device_id, live_filename);
            session_id = ins[0][0].as<int>();
        } else {
            session_id = r[0][0].as<int>();
        }

        // Insert sample
        txn.exec_params(R"(
            INSERT INTO oximetry_samples
                (oximetry_session_id, timestamp, spo2, heart_rate, motion, vibration, valid, source)
            VALUES ($1, NOW(), $2, $3, $4, 0, true, 'live')
        )", session_id, spo2, hr, motion);

        // Update session counts
        txn.exec_params(R"(
            UPDATE oximetry_sessions SET
                end_time = NOW(),
                duration_seconds = EXTRACT(EPOCH FROM (NOW() - start_time))::INT,
                total_samples = (SELECT COUNT(*) FROM oximetry_samples WHERE oximetry_session_id = $1),
                valid_samples = (SELECT COUNT(*) FROM oximetry_samples WHERE oximetry_session_id = $1 AND valid = true)
            WHERE id = $1
        )", session_id);

        txn.commit();
        return true;

    } catch (const std::exception& e) {
        std::cerr << "O2Ring: PostgreSQL live sample failed: " << e.what() << std::endl;
        return false;
    }
}

IDatabase::OxiSummary DatabaseService::getOximetrySummary(
    const std::string& device_id, const std::string& date,
    const std::string& next_day) {
    OxiSummary s;
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    if (!ensureConnection()) return s;

    try {
        pqxx::work txn(*conn_);
        auto r = txn.exec_params(
            "SELECT avg_spo2, min_spo2, spo2_baseline, odi_3pct, "
            "time_below_90, time_below_88, avg_hr, min_hr, max_hr, "
            "valid_samples, duration_seconds "
            "FROM oximetry_sessions WHERE device_id = $1 "
            "AND (cpap_session_date = $2 OR cpap_session_date = $3) "
            "AND duration_seconds > 60 "
            "ORDER BY duration_seconds DESC LIMIT 1",
            device_id, date, next_day);
        txn.commit();

        if (!r.empty()) {
            s.found = true;
            s.avg_spo2 = r[0]["avg_spo2"].as<double>(0);
            s.min_spo2 = r[0]["min_spo2"].as<double>(0);
            s.spo2_baseline = r[0]["spo2_baseline"].as<double>(0);
            s.odi_3pct = r[0]["odi_3pct"].as<double>(0);
            s.time_below_90 = r[0]["time_below_90"].as<double>(0);
            s.time_below_88 = r[0]["time_below_88"].as<double>(0);
            s.avg_hr = r[0]["avg_hr"].as<double>(0);
            s.min_hr = r[0]["min_hr"].as<int>(0);
            s.max_hr = r[0]["max_hr"].as<int>(0);
            s.valid_samples = r[0]["valid_samples"].as<int>(0);
            s.duration_seconds = r[0]["duration_seconds"].as<int>(0);
        }
    } catch (const std::exception& e) {
        std::cerr << "getOximetrySummary error: " << e.what() << std::endl;
    }

    return s;
}

IDatabase::OxiRangeSummary DatabaseService::getOximetryRangeSummary(
    const std::string& device_id, const std::string& start,
    const std::string& end) {
    OxiRangeSummary s;
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    if (!ensureConnection()) return s;

    try {
        pqxx::work txn(*conn_);
        auto r = txn.exec_params(
            "SELECT COUNT(*) as nights, "
            "ROUND(AVG(avg_spo2)::numeric, 1) as avg_spo2, "
            "MIN(min_spo2) as min_spo2, "
            "ROUND(AVG(odi_3pct)::numeric, 1) as avg_odi, "
            "ROUND(AVG(time_below_90)::numeric, 1) as avg_below_90, "
            "ROUND(AVG(avg_hr)::numeric, 0) as avg_hr "
            "FROM oximetry_sessions WHERE device_id = $1 "
            "AND cpap_session_date >= $2 AND cpap_session_date <= $3 "
            "AND duration_seconds > 60",
            device_id, start, end);
        txn.commit();

        if (!r.empty()) {
            s.nights = r[0]["nights"].as<int>(0);
            if (s.nights > 0) {
                s.found = true;
                s.avg_spo2 = r[0]["avg_spo2"].as<double>(0);
                s.min_spo2 = r[0]["min_spo2"].as<double>(0);
                s.avg_odi = r[0]["avg_odi"].as<double>(0);
                s.avg_below_90 = r[0]["avg_below_90"].as<double>(0);
                s.avg_hr = r[0]["avg_hr"].as<double>(0);
            }
        }
    } catch (const std::exception& e) {
        std::cerr << "getOximetryRangeSummary error: " << e.what() << std::endl;
    }

    return s;
}

std::vector<IDatabase::OxiNightlyPoint> DatabaseService::getOximetryNightlySpo2(
    const std::string& device_id, const std::string& start, const std::string& end) {
    std::vector<IDatabase::OxiNightlyPoint> pts;
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    if (!ensureConnection()) return pts;
    try {
        pqxx::work txn(*conn_);
        // One row per night: pick session with longest duration when multiple exist
        auto rows = txn.exec_params(
            "SELECT DISTINCT ON (cpap_session_date) cpap_session_date, avg_spo2, min_spo2 "
            "FROM oximetry_sessions "
            "WHERE device_id = $1 "
            "AND cpap_session_date >= $2 AND cpap_session_date <= $3 "
            "AND avg_spo2 IS NOT NULL AND duration_seconds > 60 "
            "ORDER BY cpap_session_date ASC, duration_seconds DESC",
            device_id, start, end);
        txn.commit();
        for (const auto& r : rows) {
            IDatabase::OxiNightlyPoint p;
            p.date     = r["cpap_session_date"].as<std::string>("");
            p.avg_spo2 = r["avg_spo2"].is_null() ? 0 : r["avg_spo2"].as<double>(0);
            p.min_spo2 = r["min_spo2"].is_null() ? 0 : r["min_spo2"].as<double>(0);
            pts.push_back(p);
        }
    } catch (const std::exception& e) {
        std::cerr << "getOximetryNightlySpo2 error: " << e.what() << std::endl;
    }
    return pts;
}

// ── Equipment profiles + supplies (SDD-004) ─────────────────────────────
//
// Conventions (shared with the SQLite/MySQL backends):
//   replace_after_days == -1  -> SQL NULL ("use the type default")
//   client_uuid == ""         -> SQL NULL
//   started_using_at == ""    -> SQL NULL; started_epoch == 0 when unset
//   deleted rows are tombstones: hidden from list*/get*, kept for sync,
//                               and tombstoning also clears active.
//   all instants are UTC: naive timestamp strings are read AS UTC on write and
//                         emitted as "YYYY-MM-DDTHH:MM:SSZ" on read, never in
//                         the server's local zone.

namespace {

// ISO-8601 UTC, matching what the app and the cloud API exchange.
constexpr const char* kIsoCols = R"(
    to_char(created_at AT TIME ZONE 'UTC', 'YYYY-MM-DD"T"HH24:MI:SS"Z"') AS created_at,
    to_char(updated_at AT TIME ZONE 'UTC', 'YYYY-MM-DD"T"HH24:MI:SS"Z"') AS updated_at
)";

std::string typeCols() {
    return " id, type_key, label, category,"
           " COALESCE(default_replace_after_days, -1) AS default_replace_after_days,"
           " is_system, active ";
}

std::string profileCols() {
    return std::string(" id, COALESCE(client_uuid, '') AS client_uuid, name, active, deleted,")
           + kIsoCols;
}

std::string itemCols() {
    return std::string(
        " id, profile_id, COALESCE(client_uuid, '') AS client_uuid, type_key, category,"
        " COALESCE(brand, '') AS brand, COALESCE(model, '') AS model,"
        " COALESCE(variant, '') AS variant,"
        " COALESCE(to_char(started_using_at AT TIME ZONE 'UTC',"
        "                  'YYYY-MM-DD\"T\"HH24:MI:SS\"Z\"'), '') AS started_using_at,"
        // Extracted through UTC explicitly so started_epoch is identical on every
        // backend (SQLite/MySQL derive it in C++ with timegm_utc); a bare EXTRACT
        // would follow the server's TimeZone and shift due dates by a day.
        " COALESCE(EXTRACT(EPOCH FROM (started_using_at AT TIME ZONE 'UTC'))::bigint, 0)"
        "     AS started_epoch,"
        " COALESCE(replace_after_days, -1) AS replace_after_days,"
        " COALESCE(notes, '') AS notes, active, deleted,") + kIsoCols;
}

// Templated on the row type: libpqxx 7.10+ yields pqxx::row_ref when iterating a
// result, older versions yield pqxx::row. Naming either one concretely breaks the
// other, and the Windows CI runner (vcpkg) ships the newer one while Linux does
// not — so this only failed on Windows.
template <typename Row>
IDatabase::EquipmentType rowToType(const Row& r) {
    IDatabase::EquipmentType t;
    t.id                         = r["id"].template as<int>(0);
    t.type_key                   = r["type_key"].template as<std::string>("");
    t.label                      = r["label"].template as<std::string>("");
    t.category                   = r["category"].template as<std::string>("");
    t.default_replace_after_days = r["default_replace_after_days"].template as<int>(-1);
    t.is_system                  = r["is_system"].template as<bool>(false);
    t.active                     = r["active"].template as<bool>(true);
    return t;
}

template <typename Row>
IDatabase::EquipmentProfile rowToProfile(const Row& r) {
    IDatabase::EquipmentProfile p;
    p.id          = r["id"].template as<int>(0);
    p.client_uuid = r["client_uuid"].template as<std::string>("");
    p.name        = r["name"].template as<std::string>("");
    p.active      = r["active"].template as<bool>(true);
    p.deleted     = r["deleted"].template as<bool>(false);
    p.created_at  = r["created_at"].template as<std::string>("");
    p.updated_at  = r["updated_at"].template as<std::string>("");
    return p;
}

template <typename Row>
IDatabase::EquipmentItem rowToItem(const Row& r) {
    IDatabase::EquipmentItem it;
    it.id                 = r["id"].template as<int>(0);
    it.profile_id         = r["profile_id"].template as<int>(0);
    it.client_uuid        = r["client_uuid"].template as<std::string>("");
    it.type_key           = r["type_key"].template as<std::string>("");
    it.category           = r["category"].template as<std::string>("");
    it.brand              = r["brand"].template as<std::string>("");
    it.model              = r["model"].template as<std::string>("");
    it.variant            = r["variant"].template as<std::string>("");
    it.started_using_at   = r["started_using_at"].template as<std::string>("");
    it.started_epoch      = r["started_epoch"].template as<long long>(0);
    it.replace_after_days = r["replace_after_days"].template as<int>(-1);
    it.notes              = r["notes"].template as<std::string>("");
    it.active             = r["active"].template as<bool>(true);
    it.deleted            = r["deleted"].template as<bool>(false);
    it.created_at         = r["created_at"].template as<std::string>("");
    it.updated_at         = r["updated_at"].template as<std::string>("");
    return it;
}

} // namespace

std::vector<IDatabase::EquipmentType> DatabaseService::listEquipmentTypes() {
    std::vector<IDatabase::EquipmentType> types;
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    if (!ensureConnection()) return types;

    try {
        pqxx::work txn(*conn_);
        auto rows = txn.exec("SELECT" + typeCols() +
                             "FROM cpap_equipment_types WHERE active = TRUE "
                             "ORDER BY is_system DESC, id");
        txn.commit();
        for (const auto& r : rows) types.push_back(rowToType(r));
    } catch (const std::exception& e) {
        std::cerr << "DB: listEquipmentTypes error: " << e.what() << std::endl;
    }
    return types;
}

std::optional<IDatabase::EquipmentType>
DatabaseService::resolveEquipmentType(const std::string& type_key) {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    if (!ensureConnection()) return std::nullopt;

    try {
        pqxx::work txn(*conn_);
        // Deliberately not filtered on active: existing items must still resolve
        // their interval after the user retires a type from the picker.
        auto rows = txn.exec_params("SELECT" + typeCols() +
                                    "FROM cpap_equipment_types WHERE type_key = $1",
                                    type_key);
        txn.commit();
        if (rows.empty()) return std::nullopt;
        return rowToType(rows[0]);
    } catch (const std::exception& e) {
        std::cerr << "DB: resolveEquipmentType error: " << e.what() << std::endl;
        return std::nullopt;
    }
}

int DatabaseService::addEquipmentType(const IDatabase::EquipmentType& t) {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    if (!ensureConnection()) return -1;

    try {
        pqxx::work txn(*conn_);
        auto r = txn.exec_params(R"(
            INSERT INTO cpap_equipment_types
                (type_key, label, category, default_replace_after_days, is_system, active)
            VALUES ($1, $2, $3, NULLIF($4::int, -1), $5, $6)
            ON CONFLICT (type_key) DO NOTHING
            RETURNING id
        )",
            t.type_key, t.label, t.category,
            t.default_replace_after_days, t.is_system, t.active);
        txn.commit();

        if (r.empty()) {
            std::cerr << "DB: addEquipmentType duplicate type_key '" << t.type_key << "'" << std::endl;
            return -1;
        }
        return r[0]["id"].as<int>(-1);
    } catch (const std::exception& e) {
        std::cerr << "DB: addEquipmentType error: " << e.what() << std::endl;
        return -1;
    }
}

bool DatabaseService::updateEquipmentType(int id, const IDatabase::EquipmentType& t) {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    if (!ensureConnection()) return false;

    try {
        pqxx::work txn(*conn_);
        // Seeded rows keep their key/category (items reference them); the interval
        // and label stay editable so a user can retune the resupply cadence.
        auto r = txn.exec_params(R"(
            UPDATE cpap_equipment_types SET
                type_key                   = CASE WHEN is_system THEN type_key ELSE $2 END,
                label                      = $3,
                category                   = CASE WHEN is_system THEN category ELSE $4 END,
                default_replace_after_days = NULLIF($5::int, -1),
                active                     = $6,
                updated_at                 = NOW()
            WHERE id = $1
        )",
            id, t.type_key, t.label, t.category, t.default_replace_after_days, t.active);
        txn.commit();
        return r.affected_rows() > 0;
    } catch (const std::exception& e) {
        std::cerr << "DB: updateEquipmentType error: " << e.what() << std::endl;
        return false;
    }
}

bool DatabaseService::deleteEquipmentType(int id) {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    if (!ensureConnection()) return false;

    try {
        pqxx::work txn(*conn_);
        auto r = txn.exec_params(
            "UPDATE cpap_equipment_types SET active = FALSE, updated_at = NOW() "
            "WHERE id = $1 AND is_system = FALSE",
            id);
        txn.commit();
        return r.affected_rows() > 0;
    } catch (const std::exception& e) {
        std::cerr << "DB: deleteEquipmentType error: " << e.what() << std::endl;
        return false;
    }
}

std::vector<IDatabase::EquipmentProfile>
DatabaseService::listEquipmentProfiles(bool include_deleted) {
    std::vector<IDatabase::EquipmentProfile> profiles;
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    if (!ensureConnection()) return profiles;

    try {
        pqxx::work txn(*conn_);
        std::string q = "SELECT" + profileCols() + "FROM cpap_equipment_profiles";
        if (!include_deleted) q += " WHERE deleted = FALSE";
        q += " ORDER BY id";
        auto rows = txn.exec(q);
        txn.commit();
        for (const auto& r : rows) profiles.push_back(rowToProfile(r));
    } catch (const std::exception& e) {
        std::cerr << "DB: listEquipmentProfiles error: " << e.what() << std::endl;
    }
    return profiles;
}

std::optional<IDatabase::EquipmentProfile> DatabaseService::getEquipmentProfile(int id) {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    if (!ensureConnection()) return std::nullopt;

    try {
        pqxx::work txn(*conn_);
        auto rows = txn.exec_params("SELECT" + profileCols() +
                                    "FROM cpap_equipment_profiles "
                                    "WHERE id = $1 AND deleted = FALSE",
                                    id);
        txn.commit();
        if (rows.empty()) return std::nullopt;
        return rowToProfile(rows[0]);
    } catch (const std::exception& e) {
        std::cerr << "DB: getEquipmentProfile error: " << e.what() << std::endl;
        return std::nullopt;
    }
}

int DatabaseService::upsertEquipmentProfile(const IDatabase::EquipmentProfile& p,
                                            const std::string& updated_at_override) {
    const std::string ts = sanitizeUpdatedAtOverride(updated_at_override);
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    if (!ensureConnection()) return -1;

    try {
        pqxx::work txn(*conn_);

        if (p.id <= 0) {
            auto r = txn.exec_params(R"(
                INSERT INTO cpap_equipment_profiles
                    (client_uuid, name, active, deleted, updated_at)
                VALUES (NULLIF($1::text, ''), $2, $3, $4,
                        COALESCE(NULLIF($5::text, '')::timestamptz, NOW()))
                RETURNING id
            )", p.client_uuid, p.name, p.active, p.deleted, ts);
            txn.commit();
            if (r.empty()) return -1;
            return r[0]["id"].as<int>(-1);
        }

        auto r = txn.exec_params(R"(
            UPDATE cpap_equipment_profiles SET
                client_uuid = NULLIF($2::text, ''),
                name        = $3,
                active      = $4,
                deleted     = $5,
                updated_at  = COALESCE(NULLIF($6::text, '')::timestamptz, NOW())
            WHERE id = $1
        )", p.id, p.client_uuid, p.name, p.active, p.deleted, ts);
        txn.commit();
        return r.affected_rows() > 0 ? p.id : -1;

    } catch (const std::exception& e) {
        std::cerr << "DB: upsertEquipmentProfile error: " << e.what() << std::endl;
        return -1;
    }
}

bool DatabaseService::tombstoneEquipmentProfile(int id,
                                                const std::string& updated_at_override) {
    const std::string ts = sanitizeUpdatedAtOverride(updated_at_override);
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    if (!ensureConnection()) return false;

    try {
        pqxx::work txn(*conn_);
        auto r = txn.exec_params(
            "UPDATE cpap_equipment_profiles "
            "SET deleted = TRUE, active = FALSE, "
            "    updated_at = COALESCE(NULLIF($2::text, '')::timestamptz, NOW()) "
            "WHERE id = $1 AND deleted = FALSE",
            id, ts);
        // Soft cascade: the FK cascade only fires on a hard DELETE, and the items
        // must disappear from the supply view with their profile. Clearing active
        // as well frees the one-live-machine slot for the profile.
        txn.exec_params(
            "UPDATE cpap_equipment_items "
            "SET deleted = TRUE, active = FALSE, "
            "    updated_at = COALESCE(NULLIF($2::text, '')::timestamptz, NOW()) "
            "WHERE profile_id = $1 AND deleted = FALSE",
            id, ts);
        txn.commit();
        return r.affected_rows() > 0;
    } catch (const std::exception& e) {
        std::cerr << "DB: tombstoneEquipmentProfile error: " << e.what() << std::endl;
        return false;
    }
}

int DatabaseService::ensureDefaultEquipmentProfile() {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    if (!ensureConnection()) return -1;

    try {
        pqxx::work txn(*conn_);
        auto rows = txn.exec(
            "SELECT id FROM cpap_equipment_profiles WHERE deleted = FALSE "
            "ORDER BY active DESC, id LIMIT 1");
        if (!rows.empty()) {
            int id = rows[0]["id"].as<int>(-1);
            txn.commit();
            return id;
        }

        auto ins = txn.exec(
            "INSERT INTO cpap_equipment_profiles (name) VALUES ('My CPAP') RETURNING id");
        txn.commit();
        if (ins.empty()) return -1;
        int id = ins[0]["id"].as<int>(-1);
        std::cout << "DB: Created default equipment profile 'My CPAP' (id " << id << ")" << std::endl;
        return id;

    } catch (const std::exception& e) {
        std::cerr << "DB: ensureDefaultEquipmentProfile error: " << e.what() << std::endl;
        return -1;
    }
}

std::vector<IDatabase::EquipmentItem> DatabaseService::listEquipmentItems(bool include_history) {
    std::vector<IDatabase::EquipmentItem> items;
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    if (!ensureConnection()) return items;

    try {
        pqxx::work txn(*conn_);
        std::string q = "SELECT" + itemCols() +
                        "FROM cpap_equipment_items WHERE deleted = FALSE";
        if (!include_history) q += " AND active = TRUE";
        // Machines first, then id -- same sequence as the SQLite/MySQL backends.
        q += " ORDER BY CASE WHEN category = 'machine' THEN 0 ELSE 1 END, id";
        auto rows = txn.exec(q);
        txn.commit();
        for (const auto& r : rows) items.push_back(rowToItem(r));
    } catch (const std::exception& e) {
        std::cerr << "DB: listEquipmentItems error: " << e.what() << std::endl;
    }
    return items;
}

std::optional<IDatabase::EquipmentItem> DatabaseService::getEquipmentItem(int id) {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    if (!ensureConnection()) return std::nullopt;

    try {
        pqxx::work txn(*conn_);
        auto rows = txn.exec_params("SELECT" + itemCols() +
                                    "FROM cpap_equipment_items "
                                    "WHERE id = $1 AND deleted = FALSE",
                                    id);
        txn.commit();
        if (rows.empty()) return std::nullopt;
        return rowToItem(rows[0]);
    } catch (const std::exception& e) {
        std::cerr << "DB: getEquipmentItem error: " << e.what() << std::endl;
        return std::nullopt;
    }
}

bool DatabaseService::profileHasMachine(int profile_id, int exclude_item_id) {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    if (!ensureConnection()) return false;

    try {
        pqxx::work txn(*conn_);
        auto rows = txn.exec_params(
            "SELECT 1 FROM cpap_equipment_items "
            "WHERE profile_id = $1 AND category = 'machine' "
            "AND active = TRUE AND deleted = FALSE AND id <> $2 LIMIT 1",
            profile_id, exclude_item_id);
        txn.commit();
        return !rows.empty();
    } catch (const std::exception& e) {
        std::cerr << "DB: profileHasMachine error: " << e.what() << std::endl;
        return false;
    }
}

int DatabaseService::upsertEquipmentItem(const IDatabase::EquipmentItem& item,
                                         const std::string& updated_at_override) {
    const std::string ts = sanitizeUpdatedAtOverride(updated_at_override);
    // Resolve the category from the type before opening our own transaction
    // (resolveEquipmentType runs its own txn; nesting one inside another throws).
    std::string category = item.category;
    if (category.empty()) {
        if (auto t = resolveEquipmentType(item.type_key)) category = t->category;
        if (category.empty()) category = "accessory";
    }

    std::lock_guard<std::recursive_mutex> lock(mutex_);
    if (!ensureConnection()) return -1;

    try {
        pqxx::work txn(*conn_);

        if (item.id <= 0) {
            auto r = txn.exec_params(R"(
                INSERT INTO cpap_equipment_items
                    (profile_id, client_uuid, type_key, category, brand, model, variant,
                     started_using_at, replace_after_days, notes, active, deleted,
                     updated_at)
                VALUES ($1, NULLIF($2::text, ''), $3, $4, $5, $6, $7,
                        NULLIF($8::text, '')::timestamp AT TIME ZONE 'UTC',
                        NULLIF($9::int, -1), $10, $11, $12,
                        COALESCE(NULLIF($13::text, '')::timestamptz, NOW()))
                RETURNING id
            )",
                item.profile_id, item.client_uuid, item.type_key, category,
                item.brand, item.model, item.variant,
                item.started_using_at, item.replace_after_days, item.notes,
                item.active, item.deleted, ts);
            txn.commit();
            if (r.empty()) return -1;
            return r[0]["id"].as<int>(-1);
        }

        auto r = txn.exec_params(R"(
            UPDATE cpap_equipment_items SET
                profile_id         = $2,
                client_uuid        = NULLIF($3::text, ''),
                type_key           = $4,
                category           = $5,
                brand              = $6,
                model              = $7,
                variant            = $8,
                started_using_at   = NULLIF($9::text, '')::timestamp AT TIME ZONE 'UTC',
                replace_after_days = NULLIF($10::int, -1),
                notes              = $11,
                active             = $12,
                deleted            = $13,
                updated_at         = COALESCE(NULLIF($14::text, '')::timestamptz, NOW())
            WHERE id = $1
        )",
            item.id, item.profile_id, item.client_uuid, item.type_key, category,
            item.brand, item.model, item.variant,
            item.started_using_at, item.replace_after_days, item.notes,
            item.active, item.deleted, ts);
        txn.commit();
        return r.affected_rows() > 0 ? item.id : -1;

    } catch (const std::exception& e) {
        // A unique_violation here is the one-machine-per-profile index doing its job.
        std::cerr << "DB: upsertEquipmentItem error: " << e.what() << std::endl;
        return -1;
    }
}

bool DatabaseService::tombstoneEquipmentItem(int id,
                                             const std::string& updated_at_override) {
    const std::string ts = sanitizeUpdatedAtOverride(updated_at_override);
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    if (!ensureConnection()) return false;

    try {
        pqxx::work txn(*conn_);
        auto r = txn.exec_params(
            "UPDATE cpap_equipment_items "
            "SET deleted = TRUE, active = FALSE, "
            "    updated_at = COALESCE(NULLIF($2::text, '')::timestamptz, NOW()) "
            "WHERE id = $1 AND deleted = FALSE",
            id, ts);
        txn.commit();
        return r.affected_rows() > 0;
    } catch (const std::exception& e) {
        std::cerr << "DB: tombstoneEquipmentItem error: " << e.what() << std::endl;
        return false;
    }
}

} // namespace hms_cpap

#endif // WITH_POSTGRESQL
