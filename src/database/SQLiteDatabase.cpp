#include "database/SQLiteDatabase.h"
#include "database/SqlDialect.h"
#include <iostream>
#include <iomanip>
#include <sstream>
#include <cstring>

namespace hms_cpap {

// ---------------------------------------------------------------------------
// RAII wrapper for sqlite3_stmt
// ---------------------------------------------------------------------------
struct StmtGuard {
    sqlite3_stmt* stmt = nullptr;
    ~StmtGuard() { if (stmt) sqlite3_finalize(stmt); }
};

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

static double col_double(sqlite3_stmt* s, int i) {
    return sqlite3_column_type(s, i) == SQLITE_NULL ? 0.0 : sqlite3_column_double(s, i);
}

static int col_int(sqlite3_stmt* s, int i) {
    return sqlite3_column_type(s, i) == SQLITE_NULL ? 0 : sqlite3_column_int(s, i);
}

static std::string col_text(sqlite3_stmt* s, int i) {
    const unsigned char* t = sqlite3_column_text(s, i);
    return t ? reinterpret_cast<const char*>(t) : "";
}

static bool col_is_null(sqlite3_stmt* s, int i) {
    return sqlite3_column_type(s, i) == SQLITE_NULL;
}

static std::optional<double> col_opt_double(sqlite3_stmt* s, int i) {
    if (col_is_null(s, i)) return std::nullopt;
    return sqlite3_column_double(s, i);
}

static std::optional<int> col_opt_int(sqlite3_stmt* s, int i) {
    if (col_is_null(s, i)) return std::nullopt;
    return sqlite3_column_int(s, i);
}

// Bind helpers -- index is 1-based for sqlite3_bind_*
static void bind_text(sqlite3_stmt* s, int i, const std::string& v) {
    sqlite3_bind_text(s, i, v.c_str(), static_cast<int>(v.size()), SQLITE_TRANSIENT);
}
static void bind_int(sqlite3_stmt* s, int i, int v) { sqlite3_bind_int(s, i, v); }
static void bind_int64(sqlite3_stmt* s, int i, int64_t v) { sqlite3_bind_int64(s, i, v); }
static void bind_double(sqlite3_stmt* s, int i, double v) { sqlite3_bind_double(s, i, v); }
static void bind_null(sqlite3_stmt* s, int i) { sqlite3_bind_null(s, i); }

static void bind_opt_double(sqlite3_stmt* s, int i, const std::optional<double>& v, double fallback = 0.0) {
    bind_double(s, i, v.value_or(fallback));
}
static void bind_opt_int(sqlite3_stmt* s, int i, const std::optional<int>& v, int fallback = 0) {
    bind_int(s, i, v.value_or(fallback));
}

// ---------------------------------------------------------------------------
// Ctor / Dtor
// ---------------------------------------------------------------------------

SQLiteDatabase::SQLiteDatabase(const std::string& db_path)
    : db_path_(db_path) {}

SQLiteDatabase::~SQLiteDatabase() {
    disconnect();
}

// ---------------------------------------------------------------------------
// Format timestamp
// ---------------------------------------------------------------------------

std::string SQLiteDatabase::fmtTimestamp(const std::chrono::system_clock::time_point& tp) {
    auto t = std::chrono::system_clock::to_time_t(tp);
    std::tm* tm = std::localtime(&t);
    std::ostringstream oss;
    oss << std::put_time(tm, "%Y-%m-%d %H:%M:%S");
    return oss.str();
}

// ---------------------------------------------------------------------------
// Connection management
// ---------------------------------------------------------------------------

bool SQLiteDatabase::connect() {
    std::lock_guard<std::recursive_mutex> lock(mutex_);

    if (db_) return true;  // already open

    int rc = sqlite3_open(db_path_.c_str(), &db_);
    if (rc != SQLITE_OK) {
        std::cerr << "SQLite: Failed to open " << db_path_ << ": "
                  << sqlite3_errmsg(db_) << std::endl;
        db_ = nullptr;
        return false;
    }

    // WAL mode for better concurrent-read performance
    exec("PRAGMA journal_mode=WAL");
    exec("PRAGMA foreign_keys=ON");
    exec("PRAGMA busy_timeout=5000");

    createSchema();

    std::cout << "SQLite: Connected to " << db_path_ << std::endl;
    return true;
}

void SQLiteDatabase::disconnect() {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    if (db_) {
        sqlite3_close(db_);
        db_ = nullptr;
        std::cout << "SQLite: Disconnected" << std::endl;
    }
}

bool SQLiteDatabase::isConnected() const {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    return db_ != nullptr;
}

bool SQLiteDatabase::exec(const std::string& sql) {
    char* err = nullptr;
    int rc = sqlite3_exec(db_, sql.c_str(), nullptr, nullptr, &err);
    if (rc != SQLITE_OK) {
        std::cerr << "SQLite exec error: " << (err ? err : "unknown") << std::endl;
        sqlite3_free(err);
        return false;
    }
    return true;
}

void* SQLiteDatabase::rawConnection() {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    return static_cast<void*>(db_);
}

// ---------------------------------------------------------------------------
// Schema
// ---------------------------------------------------------------------------

void SQLiteDatabase::createSchema() {
    // cpap_devices
    exec(R"(
        CREATE TABLE IF NOT EXISTS cpap_devices (
            device_id       TEXT PRIMARY KEY,
            device_name     TEXT,
            serial_number   TEXT,
            model_id        INTEGER DEFAULT 0,
            version_id      INTEGER DEFAULT 0,
            last_seen       TEXT DEFAULT (datetime('now')),
            created_at      TEXT DEFAULT (datetime('now'))
        )
    )");

    // cpap_sessions
    exec(R"(
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
        )
    )");

    // cpap_session_metrics
    exec(R"(
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
        )
    )");

    // cpap_breathing_summary
    exec(R"(
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
        )
    )");

    // cpap_events
    exec(R"(
        CREATE TABLE IF NOT EXISTS cpap_events (
            id                INTEGER PRIMARY KEY AUTOINCREMENT,
            session_id        INTEGER NOT NULL,
            event_type        TEXT,
            event_timestamp   TEXT NOT NULL,
            duration_seconds  REAL DEFAULT 0,
            details           TEXT,
            UNIQUE (session_id, event_timestamp),
            FOREIGN KEY (session_id) REFERENCES cpap_sessions(id) ON DELETE CASCADE
        )
    )");

    // cpap_vitals
    exec(R"(
        CREATE TABLE IF NOT EXISTS cpap_vitals (
            id          INTEGER PRIMARY KEY AUTOINCREMENT,
            session_id  INTEGER NOT NULL,
            timestamp   TEXT NOT NULL,
            spo2        REAL,
            heart_rate  INTEGER,
            UNIQUE (session_id, timestamp),
            FOREIGN KEY (session_id) REFERENCES cpap_sessions(id) ON DELETE CASCADE
        )
    )");

    // cpap_calculated_metrics
    exec(R"(
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
        )
    )");

    // cpap_daily_summary (STR records)
    exec(R"(
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
        )
    )");

    // cpap_summaries (AI-generated)
    exec(R"(
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
        )
    )");

    // Indexes
    exec("CREATE INDEX IF NOT EXISTS idx_cpap_sessions_device ON cpap_sessions(device_id)");
    exec("CREATE INDEX IF NOT EXISTS idx_cpap_sessions_start ON cpap_sessions(device_id, session_start)");
    exec("CREATE INDEX IF NOT EXISTS idx_cpap_summaries_device_period ON cpap_summaries(device_id, period, range_end DESC)");

    std::cout << "SQLite: Schema created/verified" << std::endl;
}

// ---------------------------------------------------------------------------
// saveSession
// ---------------------------------------------------------------------------

bool SQLiteDatabase::saveSession(const CPAPSession& session) {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    if (!db_) { std::cerr << "SQLite: Not connected" << std::endl; return false; }

    exec("BEGIN TRANSACTION");

    try {
        upsertDevice(session);

        int64_t session_id = insertSession(session);
        std::cout << "SQLite: Session ID: " << session_id << std::endl;

        if (!session.breathing_summary.empty()) {
            insertBreathingSummaries(session_id, session.breathing_summary);
            insertCalculatedMetrics(session_id, session.breathing_summary);
        }

        if (!session.events.empty()) {
            insertEvents(session_id, session.events);
        }

        if (!session.vitals.empty()) {
            insertVitals(session_id, session.vitals);
        }

        if (session.metrics.has_value()) {
            insertSessionMetrics(session_id, session.metrics.value());
        }

        exec("COMMIT");
        std::cout << "SQLite: Session saved successfully" << std::endl;
        return true;

    } catch (const std::exception& e) {
        exec("ROLLBACK");
        std::cerr << "SQLite: Failed to save session: " << e.what() << std::endl;
        return false;
    }
}

// ---------------------------------------------------------------------------
// upsertDevice
// ---------------------------------------------------------------------------

void SQLiteDatabase::upsertDevice(const CPAPSession& session) {
    const char* sql = R"(
        INSERT INTO cpap_devices (device_id, device_name, serial_number, model_id, version_id, last_seen)
        VALUES (?, ?, ?, ?, ?, datetime('now'))
        ON CONFLICT (device_id) DO UPDATE SET
            device_name   = excluded.device_name,
            serial_number = excluded.serial_number,
            model_id      = excluded.model_id,
            version_id    = excluded.version_id,
            last_seen     = datetime('now')
    )";

    StmtGuard g;
    sqlite3_prepare_v2(db_, sql, -1, &g.stmt, nullptr);
    bind_text(g.stmt, 1, session.device_id);
    bind_text(g.stmt, 2, session.device_name);
    bind_text(g.stmt, 3, session.serial_number);
    bind_int(g.stmt, 4, session.model_id.value_or(0));
    bind_int(g.stmt, 5, session.version_id.value_or(0));

    if (sqlite3_step(g.stmt) != SQLITE_DONE) {
        std::cerr << "SQLite: upsertDevice error: " << sqlite3_errmsg(db_) << std::endl;
    }
}

// ---------------------------------------------------------------------------
// insertSession
// ---------------------------------------------------------------------------

int64_t SQLiteDatabase::insertSession(const CPAPSession& session) {
    std::string start_str = fmtTimestamp(session.session_start.value());

    const char* sql = R"(
        INSERT INTO cpap_sessions
            (device_id, session_start, session_end, duration_seconds, data_records,
             brp_file_path, eve_file_path, sad_file_path, pld_file_path, csl_file_path)
        VALUES (?, ?, NULL, ?, ?, ?, ?, ?, ?, ?)
        ON CONFLICT (device_id, session_start) DO UPDATE SET
            duration_seconds = excluded.duration_seconds,
            data_records     = excluded.data_records,
            brp_file_path    = excluded.brp_file_path,
            eve_file_path    = excluded.eve_file_path,
            sad_file_path    = excluded.sad_file_path,
            pld_file_path    = excluded.pld_file_path,
            csl_file_path    = excluded.csl_file_path
    )";

    StmtGuard g;
    sqlite3_prepare_v2(db_, sql, -1, &g.stmt, nullptr);
    bind_text(g.stmt, 1, session.device_id);
    bind_text(g.stmt, 2, start_str);
    bind_int(g.stmt, 3, session.duration_seconds.value_or(0));
    bind_int(g.stmt, 4, session.data_records);
    bind_text(g.stmt, 5, session.brp_file_path.value_or(""));
    bind_text(g.stmt, 6, session.eve_file_path.value_or(""));
    bind_text(g.stmt, 7, session.sad_file_path.value_or(""));
    bind_text(g.stmt, 8, session.pld_file_path.value_or(""));
    bind_text(g.stmt, 9, session.csl_file_path.value_or(""));

    if (sqlite3_step(g.stmt) != SQLITE_DONE) {
        std::cerr << "SQLite: insertSession error: " << sqlite3_errmsg(db_) << std::endl;
    }

    // ON CONFLICT upsert does not change last_insert_rowid, so look up by unique key
    StmtGuard g2;
    sqlite3_prepare_v2(db_, "SELECT id FROM cpap_sessions WHERE device_id = ? AND session_start = ?",
                        -1, &g2.stmt, nullptr);
    bind_text(g2.stmt, 1, session.device_id);
    bind_text(g2.stmt, 2, start_str);

    int64_t id = 0;
    if (sqlite3_step(g2.stmt) == SQLITE_ROW) {
        id = sqlite3_column_int64(g2.stmt, 0);
    }
    return id;
}

// ---------------------------------------------------------------------------
// insertBreathingSummaries
// ---------------------------------------------------------------------------

void SQLiteDatabase::insertBreathingSummaries(int64_t session_id,
                                               const std::vector<BreathingSummary>& summaries) {
    if (summaries.empty()) return;

    const char* sql = R"(
        INSERT OR IGNORE INTO cpap_breathing_summary
            (session_id, timestamp, avg_flow_rate, max_flow_rate, min_flow_rate,
             avg_pressure, max_pressure, min_pressure)
        VALUES (?, ?, ?, ?, ?, ?, ?, ?)
    )";

    StmtGuard g;
    sqlite3_prepare_v2(db_, sql, -1, &g.stmt, nullptr);

    for (const auto& s : summaries) {
        sqlite3_reset(g.stmt);
        sqlite3_clear_bindings(g.stmt);

        bind_int64(g.stmt, 1, session_id);
        bind_text(g.stmt, 2, fmtTimestamp(s.timestamp));
        bind_double(g.stmt, 3, s.avg_flow_rate);
        bind_double(g.stmt, 4, s.max_flow_rate);
        bind_double(g.stmt, 5, s.min_flow_rate);
        bind_double(g.stmt, 6, s.avg_pressure);
        bind_double(g.stmt, 7, s.max_pressure);
        bind_double(g.stmt, 8, s.min_pressure);

        if (sqlite3_step(g.stmt) != SQLITE_DONE) {
            std::cerr << "SQLite: insertBreathingSummaries error: " << sqlite3_errmsg(db_) << std::endl;
        }
    }
}

// ---------------------------------------------------------------------------
// insertEvents
// ---------------------------------------------------------------------------

void SQLiteDatabase::insertEvents(int64_t session_id, const std::vector<CPAPEvent>& events) {
    if (events.empty()) return;

    const char* sql = R"(
        INSERT OR IGNORE INTO cpap_events
            (session_id, event_type, event_timestamp, duration_seconds, details)
        VALUES (?, ?, ?, ?, ?)
    )";

    StmtGuard g;
    sqlite3_prepare_v2(db_, sql, -1, &g.stmt, nullptr);

    for (const auto& e : events) {
        sqlite3_reset(g.stmt);
        sqlite3_clear_bindings(g.stmt);

        bind_int64(g.stmt, 1, session_id);
        bind_text(g.stmt, 2, eventTypeToString(e.event_type));
        bind_text(g.stmt, 3, fmtTimestamp(e.timestamp));
        bind_double(g.stmt, 4, e.duration_seconds);
        bind_text(g.stmt, 5, e.details.value_or(""));

        if (sqlite3_step(g.stmt) != SQLITE_DONE) {
            std::cerr << "SQLite: insertEvents error: " << sqlite3_errmsg(db_) << std::endl;
        }
    }
}

// ---------------------------------------------------------------------------
// insertVitals
// ---------------------------------------------------------------------------

void SQLiteDatabase::insertVitals(int64_t session_id, const std::vector<CPAPVitals>& vitals) {
    if (vitals.empty()) return;

    const char* sql = R"(
        INSERT OR IGNORE INTO cpap_vitals (session_id, timestamp, spo2, heart_rate)
        VALUES (?, ?, ?, ?)
    )";

    StmtGuard g;
    sqlite3_prepare_v2(db_, sql, -1, &g.stmt, nullptr);

    for (const auto& v : vitals) {
        sqlite3_reset(g.stmt);
        sqlite3_clear_bindings(g.stmt);

        bind_int64(g.stmt, 1, session_id);
        bind_text(g.stmt, 2, fmtTimestamp(v.timestamp));
        if (v.spo2.has_value()) bind_double(g.stmt, 3, v.spo2.value());
        else bind_null(g.stmt, 3);
        if (v.heart_rate.has_value()) bind_int(g.stmt, 4, v.heart_rate.value());
        else bind_null(g.stmt, 4);

        if (sqlite3_step(g.stmt) != SQLITE_DONE) {
            std::cerr << "SQLite: insertVitals error: " << sqlite3_errmsg(db_) << std::endl;
        }
    }
}

// ---------------------------------------------------------------------------
// insertSessionMetrics
// ---------------------------------------------------------------------------

void SQLiteDatabase::insertSessionMetrics(int64_t session_id, const SessionMetrics& m) {
    const char* sql = R"(
        INSERT INTO cpap_session_metrics
            (session_id, total_events, ahi, obstructive_apneas, central_apneas,
             hypopneas, reras, clear_airway_apneas,
             avg_spo2, min_spo2, avg_heart_rate, max_heart_rate, min_heart_rate,
             avg_mask_pressure, avg_epr_pressure, avg_snore,
             leak_p50, leak_p95, avg_leak_rate, max_leak_rate,
             avg_target_ventilation, therapy_mode)
        VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?)
        ON CONFLICT (session_id) DO UPDATE SET
            total_events           = excluded.total_events,
            ahi                    = excluded.ahi,
            obstructive_apneas     = excluded.obstructive_apneas,
            central_apneas         = excluded.central_apneas,
            hypopneas              = excluded.hypopneas,
            reras                  = excluded.reras,
            clear_airway_apneas    = excluded.clear_airway_apneas,
            avg_spo2               = excluded.avg_spo2,
            min_spo2               = excluded.min_spo2,
            avg_heart_rate         = excluded.avg_heart_rate,
            max_heart_rate         = excluded.max_heart_rate,
            min_heart_rate         = excluded.min_heart_rate,
            avg_mask_pressure      = excluded.avg_mask_pressure,
            avg_epr_pressure       = excluded.avg_epr_pressure,
            avg_snore              = excluded.avg_snore,
            leak_p50               = excluded.leak_p50,
            leak_p95               = excluded.leak_p95,
            avg_leak_rate          = excluded.avg_leak_rate,
            max_leak_rate          = excluded.max_leak_rate,
            avg_target_ventilation = excluded.avg_target_ventilation,
            therapy_mode           = excluded.therapy_mode
    )";

    StmtGuard g;
    sqlite3_prepare_v2(db_, sql, -1, &g.stmt, nullptr);

    bind_int64(g.stmt, 1, session_id);
    bind_int(g.stmt, 2, m.total_events);
    bind_double(g.stmt, 3, m.ahi);
    bind_int(g.stmt, 4, m.obstructive_apneas);
    bind_int(g.stmt, 5, m.central_apneas);
    bind_int(g.stmt, 6, m.hypopneas);
    bind_int(g.stmt, 7, m.reras);
    bind_int(g.stmt, 8, m.clear_airway_apneas);
    bind_opt_double(g.stmt, 9, m.avg_spo2);
    bind_opt_double(g.stmt, 10, m.min_spo2);
    bind_opt_int(g.stmt, 11, m.avg_heart_rate);
    bind_opt_int(g.stmt, 12, m.max_heart_rate);
    bind_opt_int(g.stmt, 13, m.min_heart_rate);
    bind_opt_double(g.stmt, 14, m.avg_mask_pressure);
    bind_opt_double(g.stmt, 15, m.avg_epr_pressure);
    bind_opt_double(g.stmt, 16, m.avg_snore);
    bind_opt_double(g.stmt, 17, m.leak_p50);
    bind_opt_double(g.stmt, 18, m.leak_p95);
    bind_opt_double(g.stmt, 19, m.avg_leak_rate);
    bind_opt_double(g.stmt, 20, m.max_leak_rate);
    bind_opt_double(g.stmt, 21, m.avg_target_ventilation);
    bind_opt_int(g.stmt, 22, m.therapy_mode);

    if (sqlite3_step(g.stmt) != SQLITE_DONE) {
        std::cerr << "SQLite: insertSessionMetrics error: " << sqlite3_errmsg(db_) << std::endl;
    }
}

// ---------------------------------------------------------------------------
// insertCalculatedMetrics
// ---------------------------------------------------------------------------

void SQLiteDatabase::insertCalculatedMetrics(int64_t session_id,
                                              const std::vector<BreathingSummary>& summaries) {
    if (summaries.empty()) return;

    const char* sql = R"(
        INSERT OR IGNORE INTO cpap_calculated_metrics
            (session_id, timestamp, respiratory_rate, tidal_volume, minute_ventilation,
             inspiratory_time, expiratory_time, ie_ratio, flow_limitation, leak_rate,
             flow_p95, flow_p90, pressure_p95, pressure_p90,
             mask_pressure, epr_pressure, snore_index, target_ventilation)
        VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?)
    )";

    StmtGuard g;
    sqlite3_prepare_v2(db_, sql, -1, &g.stmt, nullptr);

    for (const auto& s : summaries) {
        // Only insert if has any calculated metric
        if (!s.respiratory_rate && !s.tidal_volume && !s.minute_ventilation &&
            !s.flow_limitation && !s.mask_pressure && !s.snore_index &&
            !s.target_ventilation) continue;

        sqlite3_reset(g.stmt);
        sqlite3_clear_bindings(g.stmt);

        bind_int64(g.stmt, 1, session_id);
        bind_text(g.stmt, 2, fmtTimestamp(s.timestamp));

        auto bind_opt = [&](int idx, const std::optional<double>& v) {
            if (v) bind_double(g.stmt, idx, *v);
            else bind_null(g.stmt, idx);
        };

        bind_opt(3, s.respiratory_rate);
        bind_opt(4, s.tidal_volume);
        bind_opt(5, s.minute_ventilation);
        bind_opt(6, s.inspiratory_time);
        bind_opt(7, s.expiratory_time);
        bind_opt(8, s.ie_ratio);
        bind_opt(9, s.flow_limitation);
        bind_opt(10, s.leak_rate);
        bind_opt(11, s.flow_p95);
        bind_null(g.stmt, 12);  // flow_p90 placeholder
        bind_opt(13, s.pressure_p95);
        bind_null(g.stmt, 14);  // pressure_p90 placeholder
        bind_opt(15, s.mask_pressure);
        bind_opt(16, s.epr_pressure);
        bind_opt(17, s.snore_index);
        bind_opt(18, s.target_ventilation);

        if (sqlite3_step(g.stmt) != SQLITE_DONE) {
            std::cerr << "SQLite: insertCalculatedMetrics error: " << sqlite3_errmsg(db_) << std::endl;
        }
    }
}

// ---------------------------------------------------------------------------
// sessionExists
// ---------------------------------------------------------------------------

bool SQLiteDatabase::sessionExists(const std::string& device_id,
                                    const std::chrono::system_clock::time_point& session_start) {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    if (!db_) return false;

    std::string ts = fmtTimestamp(session_start);

    const char* sql = R"(
        SELECT EXISTS(
            SELECT 1 FROM cpap_sessions
            WHERE device_id = ?
              AND session_start BETWEEN datetime(?, '-5 seconds') AND datetime(?, '+5 seconds')
        )
    )";

    StmtGuard g;
    sqlite3_prepare_v2(db_, sql, -1, &g.stmt, nullptr);
    bind_text(g.stmt, 1, device_id);
    bind_text(g.stmt, 2, ts);
    bind_text(g.stmt, 3, ts);

    bool exists = false;
    if (sqlite3_step(g.stmt) == SQLITE_ROW) {
        exists = sqlite3_column_int(g.stmt, 0) != 0;
    }

    if (exists) {
        std::cout << "SQLite: Session " << ts << " already exists" << std::endl;
    }
    return exists;
}

// ---------------------------------------------------------------------------
// getLastSessionStart
// ---------------------------------------------------------------------------

std::optional<std::chrono::system_clock::time_point>
SQLiteDatabase::getLastSessionStart(const std::string& device_id) {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    if (!db_) return std::nullopt;

    const char* sql = R"(
        SELECT session_start FROM cpap_sessions
        WHERE device_id = ?
        ORDER BY session_start DESC
        LIMIT 1
    )";

    StmtGuard g;
    sqlite3_prepare_v2(db_, sql, -1, &g.stmt, nullptr);
    bind_text(g.stmt, 1, device_id);

    if (sqlite3_step(g.stmt) != SQLITE_ROW) {
        std::cout << "SQLite: No previous sessions for " << device_id << std::endl;
        return std::nullopt;
    }

    std::string ts_str = col_text(g.stmt, 0);
    std::tm tm = {};
    std::istringstream ss(ts_str);
    ss >> std::get_time(&tm, "%Y-%m-%d %H:%M:%S");
    if (ss.fail()) {
        std::cerr << "SQLite: Failed to parse timestamp: " << ts_str << std::endl;
        return std::nullopt;
    }
    tm.tm_isdst = -1;
    auto tp = std::chrono::system_clock::from_time_t(std::mktime(&tm));
    std::cout << "SQLite: Last session for " << device_id << " at " << ts_str << std::endl;
    return tp;
}

// ---------------------------------------------------------------------------
// getSessionMetrics
// ---------------------------------------------------------------------------

std::optional<SessionMetrics> SQLiteDatabase::getSessionMetrics(
    const std::string& device_id,
    const std::chrono::system_clock::time_point& session_start) {

    std::lock_guard<std::recursive_mutex> lock(mutex_);
    if (!db_) return std::nullopt;

    std::string ts = fmtTimestamp(session_start);

    const char* sql = R"(
        SELECT sm.total_events, sm.ahi, sm.obstructive_apneas, sm.central_apneas,
               sm.hypopneas, sm.reras, sm.clear_airway_apneas,
               sm.avg_event_duration, sm.max_event_duration, sm.time_in_apnea_percent,
               sm.avg_spo2, sm.min_spo2, sm.avg_heart_rate, sm.max_heart_rate, sm.min_heart_rate,
               ROUND(s.duration_seconds / 3600.0, 4) AS usage_hours,
               ROUND(s.duration_seconds / 3600.0 * 100.0 / 8.0, 4) AS usage_percent,
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
        WHERE s.device_id = ?
          AND s.session_start BETWEEN datetime(?, '-5 seconds') AND datetime(?, '+5 seconds')
    )";

    StmtGuard g;
    sqlite3_prepare_v2(db_, sql, -1, &g.stmt, nullptr);
    bind_text(g.stmt, 1, device_id);
    bind_text(g.stmt, 2, ts);
    bind_text(g.stmt, 3, ts);

    if (sqlite3_step(g.stmt) != SQLITE_ROW) return std::nullopt;

    // Column order:
    //  0=total_events  1=ahi  2=OA  3=CA  4=hyp  5=rera  6=CA_clear
    //  7=avg_evt_dur  8=max_evt_dur  9=time_apnea_pct
    // 10=avg_spo2  11=min_spo2  12=avg_hr  13=max_hr  14=min_hr
    // 15=usage_hours  16=usage_pct
    // 17=avg_leak  18=max_leak  19=avg_rr  20=avg_tv  21=avg_mv
    // 22=avg_it  23=avg_et  24=avg_ie  25=avg_fl  26=fp95  27=pp95
    SessionMetrics m;
    m.total_events        = col_int(g.stmt, 0);
    m.ahi                 = col_double(g.stmt, 1);
    m.obstructive_apneas  = col_int(g.stmt, 2);
    m.central_apneas      = col_int(g.stmt, 3);
    m.hypopneas           = col_int(g.stmt, 4);
    m.reras               = col_int(g.stmt, 5);
    m.clear_airway_apneas = col_int(g.stmt, 6);

    m.avg_event_duration   = col_opt_double(g.stmt, 7);
    m.max_event_duration   = col_opt_double(g.stmt, 8);
    m.time_in_apnea_percent = col_opt_double(g.stmt, 9);
    m.usage_hours          = col_opt_double(g.stmt, 15);
    m.usage_percent        = col_opt_double(g.stmt, 16);
    m.avg_leak_rate        = col_opt_double(g.stmt, 17);
    m.max_leak_rate        = col_opt_double(g.stmt, 18);
    m.avg_respiratory_rate = col_opt_double(g.stmt, 19);
    m.avg_tidal_volume     = col_opt_double(g.stmt, 20);
    m.avg_minute_ventilation = col_opt_double(g.stmt, 21);
    m.avg_inspiratory_time = col_opt_double(g.stmt, 22);
    m.avg_expiratory_time  = col_opt_double(g.stmt, 23);
    m.avg_ie_ratio         = col_opt_double(g.stmt, 24);
    m.avg_flow_limitation  = col_opt_double(g.stmt, 25);
    m.flow_p95             = col_opt_double(g.stmt, 26);
    m.pressure_p95         = col_opt_double(g.stmt, 27);

    if (!col_is_null(g.stmt, 10) && col_double(g.stmt, 10) > 0)
        m.avg_spo2 = col_double(g.stmt, 10);
    if (!col_is_null(g.stmt, 12) && col_int(g.stmt, 12) > 0)
        m.avg_heart_rate = col_int(g.stmt, 12);

    return m;
}

// ---------------------------------------------------------------------------
// markSessionCompleted
// ---------------------------------------------------------------------------

bool SQLiteDatabase::markSessionCompleted(const std::string& device_id,
                                           const std::chrono::system_clock::time_point& session_start) {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    if (!db_) return false;

    std::string ts = fmtTimestamp(session_start);

    const char* sql = R"(
        UPDATE cpap_sessions
        SET session_end = datetime('now'), updated_at = datetime('now')
        WHERE device_id = ?
          AND session_start BETWEEN datetime(?, '-5 seconds') AND datetime(?, '+5 seconds')
          AND session_end IS NULL
    )";

    StmtGuard g;
    sqlite3_prepare_v2(db_, sql, -1, &g.stmt, nullptr);
    bind_text(g.stmt, 1, device_id);
    bind_text(g.stmt, 2, ts);
    bind_text(g.stmt, 3, ts);

    sqlite3_step(g.stmt);
    int changes = sqlite3_changes(db_);

    if (changes > 0) {
        std::cout << "SQLite: Marked session " << ts << " as COMPLETED" << std::endl;
        return true;
    }
    std::cout << "SQLite: Session already has session_end set" << std::endl;
    return false;
}

// ---------------------------------------------------------------------------
// reopenSession
// ---------------------------------------------------------------------------

bool SQLiteDatabase::reopenSession(const std::string& device_id,
                                    const std::chrono::system_clock::time_point& session_start) {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    if (!db_) return false;

    std::string ts = fmtTimestamp(session_start);

    const char* sql = R"(
        UPDATE cpap_sessions
        SET session_end = NULL, updated_at = datetime('now')
        WHERE device_id = ?
          AND session_start BETWEEN datetime(?, '-5 seconds') AND datetime(?, '+5 seconds')
          AND session_end IS NOT NULL
    )";

    StmtGuard g;
    sqlite3_prepare_v2(db_, sql, -1, &g.stmt, nullptr);
    bind_text(g.stmt, 1, device_id);
    bind_text(g.stmt, 2, ts);
    bind_text(g.stmt, 3, ts);

    sqlite3_step(g.stmt);
    int changes = sqlite3_changes(db_);

    if (changes > 0) {
        std::cout << "SQLite: Reopened session " << ts << std::endl;
        return true;
    }
    return false;
}

// ---------------------------------------------------------------------------
// deleteSessionsByDateFolder
// ---------------------------------------------------------------------------

int SQLiteDatabase::deleteSessionsByDateFolder(const std::string& device_id,
                                                const std::string& date_folder) {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    if (!db_) return -1;

    std::string pattern = "%DATALOG/" + date_folder + "/%";

    const char* sql = R"(
        DELETE FROM cpap_sessions
        WHERE device_id = ? AND brp_file_path LIKE ?
    )";

    StmtGuard g;
    sqlite3_prepare_v2(db_, sql, -1, &g.stmt, nullptr);
    bind_text(g.stmt, 1, device_id);
    bind_text(g.stmt, 2, pattern);

    if (sqlite3_step(g.stmt) != SQLITE_DONE) {
        std::cerr << "SQLite: deleteSessionsByDateFolder error: " << sqlite3_errmsg(db_) << std::endl;
        return -1;
    }
    return sqlite3_changes(db_);
}

// ---------------------------------------------------------------------------
// isForceCompleted / setForceCompleted
// ---------------------------------------------------------------------------

bool SQLiteDatabase::isForceCompleted(const std::string& device_id,
                                       const std::chrono::system_clock::time_point& session_start) {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    if (!db_) return false;

    std::string ts = fmtTimestamp(session_start);

    const char* sql = R"(
        SELECT COALESCE(force_completed, 0) FROM cpap_sessions
        WHERE device_id = ?
          AND session_start BETWEEN datetime(?, '-5 seconds') AND datetime(?, '+5 seconds')
        LIMIT 1
    )";

    StmtGuard g;
    sqlite3_prepare_v2(db_, sql, -1, &g.stmt, nullptr);
    bind_text(g.stmt, 1, device_id);
    bind_text(g.stmt, 2, ts);
    bind_text(g.stmt, 3, ts);

    if (sqlite3_step(g.stmt) == SQLITE_ROW) {
        return sqlite3_column_int(g.stmt, 0) != 0;
    }
    return false;
}

bool SQLiteDatabase::setForceCompleted(const std::string& device_id,
                                        const std::chrono::system_clock::time_point& session_start) {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    if (!db_) return false;

    std::string ts = fmtTimestamp(session_start);

    const char* sql = R"(
        UPDATE cpap_sessions SET force_completed = 1, updated_at = datetime('now')
        WHERE device_id = ?
          AND session_start BETWEEN datetime(?, '-5 seconds') AND datetime(?, '+5 seconds')
    )";

    StmtGuard g;
    sqlite3_prepare_v2(db_, sql, -1, &g.stmt, nullptr);
    bind_text(g.stmt, 1, device_id);
    bind_text(g.stmt, 2, ts);
    bind_text(g.stmt, 3, ts);

    sqlite3_step(g.stmt);
    int changes = sqlite3_changes(db_);

    if (changes > 0) {
        std::cout << "SQLite: Session " << ts << " marked force_completed" << std::endl;
        return true;
    }
    return false;
}

// ---------------------------------------------------------------------------
// getCheckpointFileSizes / updateCheckpointFileSizes
// ---------------------------------------------------------------------------

std::map<std::string, int> SQLiteDatabase::getCheckpointFileSizes(
    const std::string& device_id,
    const std::chrono::system_clock::time_point& session_start) {

    std::lock_guard<std::recursive_mutex> lock(mutex_);
    if (!db_) return {};

    std::string ts = fmtTimestamp(session_start);

    const char* sql = R"(
        SELECT checkpoint_files FROM cpap_sessions
        WHERE device_id = ?
          AND session_start BETWEEN datetime(?, '-5 seconds') AND datetime(?, '+5 seconds')
        LIMIT 1
    )";

    StmtGuard g;
    sqlite3_prepare_v2(db_, sql, -1, &g.stmt, nullptr);
    bind_text(g.stmt, 1, device_id);
    bind_text(g.stmt, 2, ts);
    bind_text(g.stmt, 3, ts);

    if (sqlite3_step(g.stmt) != SQLITE_ROW || col_is_null(g.stmt, 0)) {
        return {};
    }

    // Parse simple JSON {"file1":123,"file2":456}
    std::string json_str = col_text(g.stmt, 0);
    std::map<std::string, int> file_sizes;

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

        file_sizes[filename] = std::stoi(value_str);
        pos = value_end;
    }

    return file_sizes;
}

bool SQLiteDatabase::updateCheckpointFileSizes(
    const std::string& device_id,
    const std::chrono::system_clock::time_point& session_start,
    const std::map<std::string, int>& file_sizes) {

    std::lock_guard<std::recursive_mutex> lock(mutex_);
    if (!db_) return false;

    std::string ts = fmtTimestamp(session_start);

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

    const char* sql = R"(
        UPDATE cpap_sessions
        SET checkpoint_files = ?, updated_at = datetime('now')
        WHERE device_id = ?
          AND session_start BETWEEN datetime(?, '-5 seconds') AND datetime(?, '+5 seconds')
    )";

    StmtGuard g;
    sqlite3_prepare_v2(db_, sql, -1, &g.stmt, nullptr);
    bind_text(g.stmt, 1, json_oss.str());
    bind_text(g.stmt, 2, device_id);
    bind_text(g.stmt, 3, ts);
    bind_text(g.stmt, 4, ts);

    if (sqlite3_step(g.stmt) != SQLITE_DONE) {
        std::cerr << "SQLite: updateCheckpointFileSizes error: " << sqlite3_errmsg(db_) << std::endl;
        return false;
    }

    std::cout << "SQLite: Updated checkpoint_files (" << file_sizes.size() << " files)" << std::endl;
    return true;
}

// ---------------------------------------------------------------------------
// updateDeviceLastSeen
// ---------------------------------------------------------------------------

bool SQLiteDatabase::updateDeviceLastSeen(const std::string& device_id) {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    if (!db_) return false;

    const char* sql = "UPDATE cpap_devices SET last_seen = datetime('now') WHERE device_id = ?";

    StmtGuard g;
    sqlite3_prepare_v2(db_, sql, -1, &g.stmt, nullptr);
    bind_text(g.stmt, 1, device_id);

    if (sqlite3_step(g.stmt) != SQLITE_DONE) {
        std::cerr << "SQLite: updateDeviceLastSeen error: " << sqlite3_errmsg(db_) << std::endl;
        return false;
    }
    return true;
}

// ---------------------------------------------------------------------------
// saveSTRDailyRecords
// ---------------------------------------------------------------------------

bool SQLiteDatabase::saveSTRDailyRecords(const std::vector<STRDailyRecord>& records) {
    if (records.empty()) return true;

    std::lock_guard<std::recursive_mutex> lock(mutex_);
    if (!db_) return false;

    exec("BEGIN TRANSACTION");

    try {
        const char* sql = R"(
            INSERT INTO cpap_daily_summary
                (device_id, record_date, mask_pairs, mask_events, duration_minutes, patient_hours,
                 ahi, hi, ai, oai, cai, uai, rin, csr,
                 mask_press_50, mask_press_95, mask_press_max,
                 leak_50, leak_95, leak_max,
                 spo2_50, spo2_95,
                 resp_rate_50, tid_vol_50, min_vent_50,
                 mode, epr_level, pressure_setting,
                 fault_device, fault_alarm, updated_at)
            VALUES (?, ?, ?, ?, ?, ?,
                    ?, ?, ?, ?, ?, ?, ?, ?,
                    ?, ?, ?,
                    ?, ?, ?,
                    ?, ?,
                    ?, ?, ?,
                    ?, ?, ?,
                    ?, ?, datetime('now'))
            ON CONFLICT (device_id, record_date) DO UPDATE SET
                mask_pairs       = excluded.mask_pairs,
                mask_events      = excluded.mask_events,
                duration_minutes = excluded.duration_minutes,
                patient_hours    = excluded.patient_hours,
                ahi = excluded.ahi, hi = excluded.hi, ai = excluded.ai,
                oai = excluded.oai, cai = excluded.cai, uai = excluded.uai,
                rin = excluded.rin, csr = excluded.csr,
                mask_press_50    = excluded.mask_press_50,
                mask_press_95    = excluded.mask_press_95,
                mask_press_max   = excluded.mask_press_max,
                leak_50 = excluded.leak_50, leak_95 = excluded.leak_95,
                leak_max = excluded.leak_max,
                spo2_50 = excluded.spo2_50, spo2_95 = excluded.spo2_95,
                resp_rate_50     = excluded.resp_rate_50,
                tid_vol_50       = excluded.tid_vol_50,
                min_vent_50      = excluded.min_vent_50,
                mode = excluded.mode, epr_level = excluded.epr_level,
                pressure_setting = excluded.pressure_setting,
                fault_device     = excluded.fault_device,
                fault_alarm      = excluded.fault_alarm,
                updated_at       = datetime('now')
        )";

        StmtGuard g;
        sqlite3_prepare_v2(db_, sql, -1, &g.stmt, nullptr);

        for (const auto& r : records) {
            sqlite3_reset(g.stmt);
            sqlite3_clear_bindings(g.stmt);

            // Format date
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

            bind_text(g.stmt, 1, r.device_id);
            bind_text(g.stmt, 2, date_oss.str());
            bind_text(g.stmt, 3, pairs_json.str());
            bind_int(g.stmt, 4, r.mask_events);
            bind_double(g.stmt, 5, r.duration_minutes);
            bind_double(g.stmt, 6, r.patient_hours);
            bind_double(g.stmt, 7, r.ahi);
            bind_double(g.stmt, 8, r.hi);
            bind_double(g.stmt, 9, r.ai);
            bind_double(g.stmt, 10, r.oai);
            bind_double(g.stmt, 11, r.cai);
            bind_double(g.stmt, 12, r.uai);
            bind_double(g.stmt, 13, r.rin);
            bind_double(g.stmt, 14, r.csr);
            bind_double(g.stmt, 15, r.mask_press_50);
            bind_double(g.stmt, 16, r.mask_press_95);
            bind_double(g.stmt, 17, r.mask_press_max);
            bind_double(g.stmt, 18, r.leak_50);
            bind_double(g.stmt, 19, r.leak_95);
            bind_double(g.stmt, 20, r.leak_max);
            bind_double(g.stmt, 21, r.spo2_50);
            bind_double(g.stmt, 22, r.spo2_95);
            bind_double(g.stmt, 23, r.resp_rate_50);
            bind_double(g.stmt, 24, r.tid_vol_50);
            bind_double(g.stmt, 25, r.min_vent_50);
            bind_int(g.stmt, 26, r.mode);
            bind_double(g.stmt, 27, r.epr_level);
            bind_double(g.stmt, 28, r.pressure_setting);
            bind_int(g.stmt, 29, r.fault_device);
            bind_int(g.stmt, 30, r.fault_alarm);

            if (sqlite3_step(g.stmt) != SQLITE_DONE) {
                std::cerr << "SQLite: saveSTRDailyRecords error: " << sqlite3_errmsg(db_) << std::endl;
            }
        }

        exec("COMMIT");
        std::cout << "SQLite: Saved " << records.size() << " STR daily records" << std::endl;
        return true;

    } catch (const std::exception& e) {
        exec("ROLLBACK");
        std::cerr << "SQLite: saveSTRDailyRecords error: " << e.what() << std::endl;
        return false;
    }
}

// ---------------------------------------------------------------------------
// getLastSTRDate
// ---------------------------------------------------------------------------

std::optional<std::string> SQLiteDatabase::getLastSTRDate(const std::string& device_id) {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    if (!db_) return std::nullopt;

    const char* sql = "SELECT MAX(record_date) FROM cpap_daily_summary WHERE device_id = ?";

    StmtGuard g;
    sqlite3_prepare_v2(db_, sql, -1, &g.stmt, nullptr);
    bind_text(g.stmt, 1, device_id);

    if (sqlite3_step(g.stmt) == SQLITE_ROW && !col_is_null(g.stmt, 0)) {
        return col_text(g.stmt, 0);
    }
    return std::nullopt;
}

// ---------------------------------------------------------------------------
// getNightlyMetrics
// ---------------------------------------------------------------------------

std::optional<SessionMetrics> SQLiteDatabase::getNightlyMetrics(
    const std::string& device_id,
    const std::chrono::system_clock::time_point& session_start) {

    std::lock_guard<std::recursive_mutex> lock(mutex_);
    if (!db_) return std::nullopt;

    std::string ts = fmtTimestamp(session_start);

    // SQLite version of the PG nightly aggregation query.
    // sleep_day = date(session_start, '-12 hours')
    const char* sql = R"(
        WITH night AS (
            SELECT date(session_start, '-12 hours') AS sleep_day
            FROM cpap_sessions
            WHERE device_id = ?
              AND session_start BETWEEN datetime(?, '-5 seconds') AND datetime(?, '+5 seconds')
            LIMIT 1
        )
        SELECT
            SUM(s.duration_seconds)                          AS total_seconds,
            MAX(sm.total_events)                             AS total_events,
            MAX(sm.obstructive_apneas)                       AS obstructive_apneas,
            MAX(sm.central_apneas)                           AS central_apneas,
            MAX(sm.hypopneas)                                AS hypopneas,
            MAX(sm.reras)                                    AS reras,
            MAX(sm.clear_airway_apneas)                      AS clear_airway_apneas,
            MAX(sm.avg_event_duration)                       AS avg_event_duration,
            MAX(sm.max_event_duration)                       AS max_event_duration,
            CASE WHEN SUM(s.duration_seconds) > 0
                 THEN ROUND(SUM(s.duration_seconds) / 3600.0, 4)
                 ELSE 0 END                                  AS usage_hours,
            CASE WHEN SUM(s.duration_seconds) > 0
                 THEN ROUND(SUM(s.duration_seconds) / 3600.0 * 100.0 / 8.0, 4)
                 ELSE 0 END                                  AS usage_percent,
            CASE WHEN SUM(s.duration_seconds) > 0
                 THEN ROUND(MAX(sm.total_events) * 3600.0 / SUM(s.duration_seconds), 4)
                 ELSE 0 END                                  AS ahi,
            CASE WHEN SUM(s.duration_seconds) > 0 AND MAX(sm.avg_event_duration) IS NOT NULL
                 THEN ROUND(MAX(sm.total_events) * MAX(sm.avg_event_duration)
                          / SUM(s.duration_seconds) * 100.0, 4)
                 ELSE 0 END                                  AS time_in_apnea_pct,
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
        WHERE s.device_id = ?
          AND date(s.session_start, '-12 hours') = (SELECT sleep_day FROM night)
    )";

    StmtGuard g;
    sqlite3_prepare_v2(db_, sql, -1, &g.stmt, nullptr);
    bind_text(g.stmt, 1, device_id);
    bind_text(g.stmt, 2, ts);
    bind_text(g.stmt, 3, ts);
    bind_text(g.stmt, 4, device_id);

    if (sqlite3_step(g.stmt) != SQLITE_ROW) return std::nullopt;
    if (col_is_null(g.stmt, 0)) return std::nullopt;  // total_seconds

    // Column order:
    //  0=total_seconds  1=total_events  2=OA  3=CA  4=hyp  5=rera  6=CA_clear
    //  7=avg_evt_dur  8=max_evt_dur  9=usage_hours  10=usage_pct  11=ahi
    // 12=time_apnea_pct  13=avg_leak  14=max_leak  15=avg_rr  16=avg_tv
    // 17=avg_mv  18=avg_it  19=avg_et  20=avg_ie  21=avg_fl  22=fp95  23=pp95
    // 24=avg_mask_pressure  25=avg_epr_pressure  26=avg_snore  27=avg_target_ventilation
    // 28=avg_pressure  29=max_pressure  30=min_pressure
    // 31=leak_p50  32=leak_p95_sess  33=therapy_mode
    SessionMetrics m;
    m.total_events        = col_int(g.stmt, 1);
    m.ahi                 = col_double(g.stmt, 11);
    m.obstructive_apneas  = col_int(g.stmt, 2);
    m.central_apneas      = col_int(g.stmt, 3);
    m.hypopneas           = col_int(g.stmt, 4);
    m.reras               = col_int(g.stmt, 5);
    m.clear_airway_apneas = col_int(g.stmt, 6);

    m.avg_event_duration    = col_opt_double(g.stmt, 7);
    m.max_event_duration    = col_opt_double(g.stmt, 8);
    m.time_in_apnea_percent = col_opt_double(g.stmt, 12);
    m.usage_hours           = col_opt_double(g.stmt, 9);
    m.usage_percent         = col_opt_double(g.stmt, 10);
    m.avg_leak_rate         = col_opt_double(g.stmt, 13);
    m.max_leak_rate         = col_opt_double(g.stmt, 14);
    m.avg_respiratory_rate  = col_opt_double(g.stmt, 15);
    m.avg_tidal_volume      = col_opt_double(g.stmt, 16);
    m.avg_minute_ventilation = col_opt_double(g.stmt, 17);
    m.avg_inspiratory_time  = col_opt_double(g.stmt, 18);
    m.avg_expiratory_time   = col_opt_double(g.stmt, 19);
    m.avg_ie_ratio          = col_opt_double(g.stmt, 20);
    m.avg_flow_limitation   = col_opt_double(g.stmt, 21);
    m.flow_p95              = col_opt_double(g.stmt, 22);
    m.pressure_p95          = col_opt_double(g.stmt, 23);
    m.avg_pressure          = col_opt_double(g.stmt, 28);
    m.max_pressure          = col_opt_double(g.stmt, 29);
    m.min_pressure          = col_opt_double(g.stmt, 30);
    m.avg_mask_pressure     = col_opt_double(g.stmt, 24);
    m.avg_epr_pressure      = col_opt_double(g.stmt, 25);
    m.avg_snore             = col_opt_double(g.stmt, 26);
    if (!col_is_null(g.stmt, 27) && col_double(g.stmt, 27) > 0)
        m.avg_target_ventilation = col_double(g.stmt, 27);
    m.leak_p50              = col_opt_double(g.stmt, 31);
    m.leak_p95              = col_opt_double(g.stmt, 32);
    m.therapy_mode          = col_opt_int(g.stmt, 33);

    return m;
}

// ---------------------------------------------------------------------------
// getMetricsForDateRange
// ---------------------------------------------------------------------------

std::vector<SessionMetrics> SQLiteDatabase::getMetricsForDateRange(
    const std::string& device_id, int days_back) {

    std::lock_guard<std::recursive_mutex> lock(mutex_);
    if (!db_) return {};

    // Compute cutoff timestamp string
    auto cutoff = std::chrono::system_clock::now() - std::chrono::hours(days_back * 24);
    std::string cutoff_str = fmtTimestamp(cutoff);

    // One row per sleep-night, same aggregation as getNightlyMetrics
    const char* sql = R"(
        SELECT
            date(s.session_start, '-12 hours')               AS sleep_day,
            SUM(s.duration_seconds)                          AS total_seconds,
            MAX(sm.total_events)                             AS total_events,
            MAX(sm.obstructive_apneas)                       AS obstructive_apneas,
            MAX(sm.central_apneas)                           AS central_apneas,
            MAX(sm.hypopneas)                                AS hypopneas,
            MAX(sm.reras)                                    AS reras,
            MAX(sm.clear_airway_apneas)                      AS clear_airway_apneas,
            MAX(sm.avg_event_duration)                       AS avg_event_duration,
            MAX(sm.max_event_duration)                       AS max_event_duration,
            CASE WHEN SUM(s.duration_seconds) > 0
                 THEN ROUND(SUM(s.duration_seconds) / 3600.0, 4)
                 ELSE 0 END                                  AS usage_hours,
            CASE WHEN SUM(s.duration_seconds) > 0
                 THEN ROUND(SUM(s.duration_seconds) / 3600.0 * 100.0 / 8.0, 4)
                 ELSE 0 END                                  AS usage_percent,
            CASE WHEN SUM(s.duration_seconds) > 0
                 THEN ROUND(MAX(sm.total_events) * 3600.0 / SUM(s.duration_seconds), 4)
                 ELSE 0 END                                  AS ahi,
            AVG(c.avg_leak) AS avg_leak, MAX(c.max_leak)     AS max_leak,
            AVG(c.avg_rr)  AS avg_rr,   AVG(c.avg_tv)       AS avg_tv,
            AVG(c.avg_mv)  AS avg_mv,   AVG(c.avg_fl)       AS avg_fl,
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
        WHERE s.device_id = ?
          AND s.session_start >= datetime(?)
          AND s.session_end IS NOT NULL
        GROUP BY date(s.session_start, '-12 hours')
        ORDER BY sleep_day ASC
    )";

    StmtGuard g;
    sqlite3_prepare_v2(db_, sql, -1, &g.stmt, nullptr);
    bind_text(g.stmt, 1, device_id);
    bind_text(g.stmt, 2, cutoff_str);

    // Column order matches SELECT:
    //  0=sleep_day  1=total_seconds  2=total_events  3=OA  4=CA  5=hyp  6=rera
    //  7=CA_clear  8=avg_evt_dur  9=max_evt_dur  10=usage_hours  11=usage_pct
    // 12=ahi  13=avg_leak  14=max_leak  15=avg_rr  16=avg_tv  17=avg_mv  18=avg_fl
    // 19=pp95  20=avg_mask_pressure  21=avg_epr_pressure  22=avg_snore
    // 23=avg_target_ventilation  24=avg_pressure  25=max_pressure  26=min_pressure
    // 27=leak_p50  28=leak_p95_sess  29=therapy_mode

    std::vector<SessionMetrics> nights;
    while (sqlite3_step(g.stmt) == SQLITE_ROW) {
        if (col_is_null(g.stmt, 1)) continue;  // total_seconds

        SessionMetrics m;
        m.sleep_day           = col_text(g.stmt, 0);
        m.total_events        = col_int(g.stmt, 2);
        m.ahi                 = col_double(g.stmt, 12);
        m.obstructive_apneas  = col_int(g.stmt, 3);
        m.central_apneas      = col_int(g.stmt, 4);
        m.hypopneas           = col_int(g.stmt, 5);
        m.reras               = col_int(g.stmt, 6);
        m.clear_airway_apneas = col_int(g.stmt, 7);

        m.avg_event_duration     = col_opt_double(g.stmt, 8);
        m.max_event_duration     = col_opt_double(g.stmt, 9);
        m.usage_hours            = col_opt_double(g.stmt, 10);
        m.usage_percent          = col_opt_double(g.stmt, 11);
        m.avg_leak_rate          = col_opt_double(g.stmt, 13);
        m.max_leak_rate          = col_opt_double(g.stmt, 14);
        m.avg_respiratory_rate   = col_opt_double(g.stmt, 15);
        m.avg_tidal_volume       = col_opt_double(g.stmt, 16);
        m.avg_minute_ventilation = col_opt_double(g.stmt, 17);
        m.avg_flow_limitation    = col_opt_double(g.stmt, 18);
        m.pressure_p95           = col_opt_double(g.stmt, 19);
        m.avg_pressure           = col_opt_double(g.stmt, 24);
        m.max_pressure           = col_opt_double(g.stmt, 25);
        m.min_pressure           = col_opt_double(g.stmt, 26);
        m.avg_mask_pressure      = col_opt_double(g.stmt, 20);
        m.avg_epr_pressure       = col_opt_double(g.stmt, 21);
        m.avg_snore              = col_opt_double(g.stmt, 22);
        if (!col_is_null(g.stmt, 23) && col_double(g.stmt, 23) > 0)
            m.avg_target_ventilation = col_double(g.stmt, 23);
        m.leak_p50               = col_opt_double(g.stmt, 27);
        m.leak_p95               = col_opt_double(g.stmt, 28);
        m.therapy_mode           = col_opt_int(g.stmt, 29);

        nights.push_back(std::move(m));
    }

    std::cout << "SQLite: getMetricsForDateRange(" << days_back << " days) returned "
              << nights.size() << " nights" << std::endl;
    return nights;
}

// ---------------------------------------------------------------------------
// saveSummary
// ---------------------------------------------------------------------------

bool SQLiteDatabase::saveSummary(
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
    if (!db_) return false;

    const char* sql = R"(
        INSERT INTO cpap_summaries
            (device_id, period, range_start, range_end, nights_count,
             avg_ahi, avg_usage_hours, compliance_pct, summary_text)
        VALUES (?, ?, date(?), date(?), ?, ?, ?, ?, ?)
    )";

    StmtGuard g;
    sqlite3_prepare_v2(db_, sql, -1, &g.stmt, nullptr);
    bind_text(g.stmt, 1, device_id);
    bind_text(g.stmt, 2, period);
    bind_text(g.stmt, 3, range_start);
    bind_text(g.stmt, 4, range_end);
    bind_int(g.stmt, 5, nights_count);
    bind_double(g.stmt, 6, avg_ahi);
    bind_double(g.stmt, 7, avg_usage_hours);
    bind_double(g.stmt, 8, compliance_pct);
    bind_text(g.stmt, 9, summary_text);

    if (sqlite3_step(g.stmt) != SQLITE_DONE) {
        std::cerr << "SQLite: saveSummary error: " << sqlite3_errmsg(db_) << std::endl;
        return false;
    }

    std::cout << "SQLite: Saved " << period << " summary (" << range_start
              << " to " << range_end << ", " << nights_count << " nights)" << std::endl;
    return true;
}

} // namespace hms_cpap
