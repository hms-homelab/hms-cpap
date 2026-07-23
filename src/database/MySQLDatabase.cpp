#ifdef WITH_MYSQL

#include "database/MySQLDatabase.h"
#include "database/SqlDialect.h"
#include "utils/TimeCompat.h"  // timegm_utc() for started_epoch
#include <iostream>
#include <iomanip>
#include <sstream>
#include <cstring>
#include <cstdlib>
#include <ctime>
#include <memory>
#include <stdexcept>

namespace hms_cpap {

// ---------------------------------------------------------------------------
// RAII wrapper for MYSQL_STMT
// ---------------------------------------------------------------------------
struct MysqlStmtGuard {
    MYSQL_STMT* stmt = nullptr;
    ~MysqlStmtGuard() { if (stmt) mysql_stmt_close(stmt); }
};

// ---------------------------------------------------------------------------
// RAII wrapper for MYSQL_RES (result metadata)
// ---------------------------------------------------------------------------
struct MysqlResGuard {
    MYSQL_RES* res = nullptr;
    ~MysqlResGuard() { if (res) mysql_free_result(res); }
};

// ---------------------------------------------------------------------------
// Bind-param builder helper
// ---------------------------------------------------------------------------

/// Holds ownership of buffers that MYSQL_BIND references point to.
/// Must outlive the mysql_stmt_execute() call.
struct ParamBinder {
    std::vector<MYSQL_BIND> binds;
    // Storage for string data, null indicators, lengths
    struct Slot {
        std::string str_val;
        long long   ll_val = 0;
        int         int_val = 0;
        double      dbl_val = 0.0;
        my_bool     is_null = 0;
        unsigned long length = 0;
    };
    std::vector<Slot> slots;

    explicit ParamBinder(size_t n) : binds(n), slots(n) {
        std::memset(binds.data(), 0, sizeof(MYSQL_BIND) * n);
    }

    void bindText(int idx, const std::string& v) {
        auto& s = slots[idx];
        s.str_val = v;
        s.length = static_cast<unsigned long>(s.str_val.size());
        s.is_null = 0;
        auto& b = binds[idx];
        b.buffer_type = MYSQL_TYPE_STRING;
        b.buffer = const_cast<char*>(s.str_val.c_str());
        b.buffer_length = s.length;
        b.length = &s.length;
        b.is_null = &s.is_null;
    }

    void bindInt(int idx, int v) {
        auto& s = slots[idx];
        s.int_val = v;
        s.is_null = 0;
        auto& b = binds[idx];
        b.buffer_type = MYSQL_TYPE_LONG;
        b.buffer = &s.int_val;
        b.is_null = &s.is_null;
    }

    void bindInt64(int idx, int64_t v) {
        auto& s = slots[idx];
        s.ll_val = static_cast<long long>(v);
        s.is_null = 0;
        auto& b = binds[idx];
        b.buffer_type = MYSQL_TYPE_LONGLONG;
        b.buffer = &s.ll_val;
        b.is_null = &s.is_null;
    }

    void bindDouble(int idx, double v) {
        auto& s = slots[idx];
        s.dbl_val = v;
        s.is_null = 0;
        auto& b = binds[idx];
        b.buffer_type = MYSQL_TYPE_DOUBLE;
        b.buffer = &s.dbl_val;
        b.is_null = &s.is_null;
    }

    void bindNull(int idx) {
        auto& s = slots[idx];
        s.is_null = 1;
        auto& b = binds[idx];
        b.buffer_type = MYSQL_TYPE_NULL;
        b.is_null = &s.is_null;
    }

    void bindOptDouble(int idx, const std::optional<double>& v, double fallback = 0.0) {
        bindDouble(idx, v.value_or(fallback));
    }

    void bindOptInt(int idx, const std::optional<int>& v, int fallback = 0) {
        bindInt(idx, v.value_or(fallback));
    }

    /// SDD-004: "" is stored as SQL NULL (client_uuid, started_using_at)
    void bindTextOrNull(int idx, const std::string& v) {
        if (v.empty()) bindNull(idx);
        else           bindText(idx, v);
    }

    /// SDD-004: -1 is stored as SQL NULL (replace_after_days, default_replace_after_days)
    void bindDaysOrNull(int idx, int days) {
        if (days < 0) bindNull(idx);
        else          bindInt(idx, days);
    }

    MYSQL_BIND* data() { return binds.data(); }
};

// ---------------------------------------------------------------------------
// Result-row fetch helper
// ---------------------------------------------------------------------------

/// Manages MYSQL_BIND output buffers for fetching result rows.
struct ResultBinder {
    std::vector<MYSQL_BIND> binds;
    struct Col {
        double      dbl_val = 0.0;
        long long   ll_val = 0;
        int         int_val = 0;
        char        str_buf[256] = {};
        unsigned long length = 0;
        my_bool     is_null = 0;
        my_bool     error = 0;
    };
    std::vector<Col> cols;
    /// Backing storage for columns wider than Col::str_buf (see bindColStringN).
    std::vector<std::unique_ptr<char[]>> wide_bufs;

    explicit ResultBinder(size_t n) : binds(n), cols(n) {
        std::memset(binds.data(), 0, sizeof(MYSQL_BIND) * n);
    }

    void bindColDouble(int idx) {
        auto& c = cols[idx];
        auto& b = binds[idx];
        b.buffer_type = MYSQL_TYPE_DOUBLE;
        b.buffer = &c.dbl_val;
        b.is_null = &c.is_null;
        b.error = &c.error;
    }

    void bindColInt(int idx) {
        auto& c = cols[idx];
        auto& b = binds[idx];
        b.buffer_type = MYSQL_TYPE_LONG;
        b.buffer = &c.int_val;
        b.is_null = &c.is_null;
        b.error = &c.error;
    }

    void bindColInt64(int idx) {
        auto& c = cols[idx];
        auto& b = binds[idx];
        b.buffer_type = MYSQL_TYPE_LONGLONG;
        b.buffer = &c.ll_val;
        b.is_null = &c.is_null;
        b.error = &c.error;
    }

    void bindColString(int idx, size_t max_len = 256) {
        auto& c = cols[idx];
        auto& b = binds[idx];
        b.buffer_type = MYSQL_TYPE_STRING;
        b.buffer = c.str_buf;
        b.buffer_length = max_len > sizeof(c.str_buf) ? sizeof(c.str_buf) : max_len;
        b.length = &c.length;
        b.is_null = &c.is_null;
        b.error = &c.error;
    }

    /// Like bindColString but for columns that do not fit in Col::str_buf
    /// (e.g. free-text notes). Storage lives in wide_bufs and outlives the fetch.
    void bindColStringN(int idx, size_t max_len) {
        wide_bufs.emplace_back(new char[max_len]());
        auto& c = cols[idx];
        auto& b = binds[idx];
        b.buffer_type = MYSQL_TYPE_STRING;
        b.buffer = wide_bufs.back().get();
        b.buffer_length = static_cast<unsigned long>(max_len);
        b.length = &c.length;
        b.is_null = &c.is_null;
        b.error = &c.error;
    }

    double colDouble(int idx) const {
        return cols[idx].is_null ? 0.0 : cols[idx].dbl_val;
    }
    int colInt(int idx) const {
        return cols[idx].is_null ? 0 : cols[idx].int_val;
    }
    int64_t colInt64(int idx) const {
        return cols[idx].is_null ? 0 : static_cast<int64_t>(cols[idx].ll_val);
    }
    std::string colText(int idx) const {
        if (cols[idx].is_null) return "";
        // *length is the server-side length, which can exceed our buffer when the
        // value was truncated -- clamp so we never read past the binding.
        unsigned long len = cols[idx].length;
        if (len > binds[idx].buffer_length) len = binds[idx].buffer_length;
        return std::string(static_cast<const char*>(binds[idx].buffer), len);
    }
    bool colIsNull(int idx) const { return cols[idx].is_null != 0; }

    std::optional<double> colOptDouble(int idx) const {
        if (cols[idx].is_null) return std::nullopt;
        return cols[idx].dbl_val;
    }
    std::optional<int> colOptInt(int idx) const {
        if (cols[idx].is_null) return std::nullopt;
        return cols[idx].int_val;
    }

    MYSQL_BIND* data() { return binds.data(); }
};

// ---------------------------------------------------------------------------
// Ctor / Dtor
// ---------------------------------------------------------------------------

MySQLDatabase::MySQLDatabase(const std::string& host,
                             unsigned int port,
                             const std::string& user,
                             const std::string& password,
                             const std::string& database)
    : host_(host), port_(port), user_(user), password_(password), database_(database) {}

MySQLDatabase::~MySQLDatabase() {
    disconnect();
}

// ---------------------------------------------------------------------------
// Format timestamp
// ---------------------------------------------------------------------------

std::string MySQLDatabase::fmtTimestamp(const std::chrono::system_clock::time_point& tp) {
    auto t = std::chrono::system_clock::to_time_t(tp);
    std::tm* tm = std::localtime(&t);
    std::ostringstream oss;
    oss << std::put_time(tm, "%Y-%m-%d %H:%M:%S");
    return oss.str();
}

// ---------------------------------------------------------------------------
// Connection management
// ---------------------------------------------------------------------------

bool MySQLDatabase::connect() {
    std::lock_guard<std::recursive_mutex> lock(mutex_);

    if (conn_) return true;  // already open

    conn_ = mysql_init(nullptr);
    if (!conn_) {
        std::cerr << "MySQL: mysql_init() failed" << std::endl;
        return false;
    }

    // Enable auto-reconnect
    my_bool reconnect = 1;
    mysql_options(conn_, MYSQL_OPT_RECONNECT, &reconnect);

    // Set connection timeout
    unsigned int timeout = 10;
    mysql_options(conn_, MYSQL_OPT_CONNECT_TIMEOUT, &timeout);

    if (!mysql_real_connect(conn_, host_.c_str(), user_.c_str(), password_.c_str(),
                            database_.c_str(), port_, nullptr, 0)) {
        std::cerr << "MySQL: Connection failed: " << mysql_error(conn_) << std::endl;
        mysql_close(conn_);
        conn_ = nullptr;
        return false;
    }

    // Use UTF-8
    mysql_set_character_set(conn_, "utf8mb4");

    // Pin the session to UTC. SDD-004 renders created_at/updated_at with a literal
    // trailing 'Z', and those columns default to NOW() — on a server running local
    // time that would stamp a local wall clock and label it UTC, so the same row
    // would compare differently against the SQLite/Postgres backends and corrupt
    // the sync layer's last-write-wins. Postgres converts explicitly and SQLite's
    // datetime('now') is UTC by definition; this makes MySQL agree.
    if (mysql_query(conn_, "SET time_zone = '+00:00'") != 0) {
        std::cerr << "MySQL: WARNING - could not set session time_zone to UTC: "
                  << mysql_error(conn_) << std::endl;
    }

    createSchema();

    std::cout << "MySQL: Connected to " << host_ << ":" << port_
              << "/" << database_ << std::endl;
    return true;
}

void MySQLDatabase::disconnect() {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    if (conn_) {
        mysql_close(conn_);
        conn_ = nullptr;
        std::cout << "MySQL: Disconnected" << std::endl;
    }
}

bool MySQLDatabase::isConnected() const {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    return conn_ != nullptr;
}

bool MySQLDatabase::exec(const std::string& sql) {
    if (mysql_query(conn_, sql.c_str()) != 0) {
        std::cerr << "MySQL exec error: " << mysql_error(conn_) << std::endl;
        return false;
    }
    // Consume any result set
    MYSQL_RES* res = mysql_store_result(conn_);
    if (res) mysql_free_result(res);
    return true;
}

void* MySQLDatabase::rawConnection() {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    return static_cast<void*>(conn_);
}

// ---------------------------------------------------------------------------
// Schema
// ---------------------------------------------------------------------------

void MySQLDatabase::createSchema() {
    // cpap_devices
    exec(R"(
        CREATE TABLE IF NOT EXISTS cpap_devices (
            device_id       VARCHAR(255) PRIMARY KEY,
            device_name     VARCHAR(255),
            serial_number   VARCHAR(255),
            model_id        INT DEFAULT 0,
            version_id      INT DEFAULT 0,
            last_seen       DATETIME DEFAULT NOW(),
            created_at      DATETIME DEFAULT NOW()
        ) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4
    )");

    // cpap_sessions
    exec(R"(
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
        ) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4
    )");

    // cpap_session_metrics
    exec(R"(
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
            spo2_drops             INT,
            odi                    DOUBLE,
            created_at             DATETIME DEFAULT NOW(),
            FOREIGN KEY (session_id) REFERENCES cpap_sessions(id) ON DELETE CASCADE
        ) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4
    )");

    // cpap_breathing_summary
    exec(R"(
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
        ) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4
    )");

    // cpap_events
    exec(R"(
        CREATE TABLE IF NOT EXISTS cpap_events (
            id                INT AUTO_INCREMENT PRIMARY KEY,
            session_id        INT NOT NULL,
            event_type        VARCHAR(64),
            event_timestamp   DATETIME NOT NULL,
            duration_seconds  DOUBLE DEFAULT 0,
            details           TEXT,
            UNIQUE KEY uq_session_event_ts (session_id, event_timestamp),
            FOREIGN KEY (session_id) REFERENCES cpap_sessions(id) ON DELETE CASCADE
        ) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4
    )");

    // cpap_breaths
    exec(R"(
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
        ) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4
    )");

    // cpap_vitals
    exec(R"(
        CREATE TABLE IF NOT EXISTS cpap_vitals (
            id          INT AUTO_INCREMENT PRIMARY KEY,
            session_id  INT NOT NULL,
            timestamp   DATETIME NOT NULL,
            spo2        DOUBLE,
            heart_rate  INT,
            UNIQUE KEY uq_session_vital_ts (session_id, timestamp),
            FOREIGN KEY (session_id) REFERENCES cpap_sessions(id) ON DELETE CASCADE
        ) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4
    )");

    // cpap_calculated_metrics
    exec(R"(
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
        ) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4
    )");

    // cpap_daily_summary (STR records)
    exec(R"(
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
        ) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4
    )");

    // cpap_summaries (AI-generated)
    exec(R"(
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
        ) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4
    )");

    // -- SDD-004: equipment profiles + supplies -------------------------------
    // Local-first mirror of the cloud model (hms-cpapdash-api SDD-035), minus
    // user_id: hms-cpap is single-household. A profile ("setup") owns exactly one
    // machine plus its accessories; supply wear is COMPUTED on read, never stored.
    // client_uuid exists only so optional cloud sync is idempotent; it is unused
    // offline. Keep this in lockstep with scripts/schema_mysql.sql.
    //
    // started_using_at is VARCHAR, not DATETIME, on purpose: it is an ISO-8601
    // string that must round-trip byte-for-byte with the SQLite/Postgres backends
    // and the phone app so all three compute identical due dates.

    // cpap_equipment_types -- catalog: seeded system defaults + user customs
    exec(R"(
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
        ) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4
    )");

    // cpap_equipment_profiles -- named setups.
    // MySQL UNIQUE allows repeated NULLs, so uq_eq_profile_uuid is exactly the
    // "UNIQUE WHERE client_uuid IS NOT NULL" the other backends express as a
    // partial index.
    exec(R"(
        CREATE TABLE IF NOT EXISTS cpap_equipment_profiles (
            id           INT AUTO_INCREMENT PRIMARY KEY,
            client_uuid  VARCHAR(64),
            name         VARCHAR(128) NOT NULL,
            active       TINYINT(1) DEFAULT 1,
            deleted      TINYINT(1) DEFAULT 0,
            created_at   DATETIME DEFAULT NOW(),
            updated_at   DATETIME DEFAULT NOW() ON UPDATE NOW(),
            UNIQUE KEY uq_eq_profile_uuid (client_uuid)
        ) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4
    )");

    // cpap_equipment_items -- the gear; category is denormalized from the type.
    // HARD RULE "at most one live machine per profile" CANNOT be an index here:
    // MySQL has no partial/filtered indexes. profileHasMachine() is the guard.
    exec(R"(
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
        ) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4
    )");

    // Seed the six system types verbatim from the app's supply_defaults.dart, so
    // local, cloud and the phone app all compute the same due dates.
    // INSERT IGNORE + UNIQUE(type_key) makes this idempotent.
    exec(R"(
        INSERT IGNORE INTO cpap_equipment_types
            (type_key, label, category, default_replace_after_days, is_system)
        VALUES
            ('machine',    'Machine',    'machine',   NULL, 1),
            ('mask',       'Mask',       'accessory',   90, 1),
            ('tubing',     'Tubing',     'accessory',   90, 1),
            ('filter',     'Filter',     'accessory',   30, 1),
            ('humidifier', 'Humidifier', 'accessory',  180, 1),
            ('headgear',   'Headgear',   'accessory',  180, 1)
    )");

    // Indexes
    exec("CREATE INDEX idx_cpap_sessions_device ON cpap_sessions(device_id)");
    exec("CREATE INDEX idx_cpap_sessions_start ON cpap_sessions(device_id, session_start)");
    exec("CREATE INDEX idx_cpap_summaries_device_period ON cpap_summaries(device_id, period, range_end DESC)");

    std::cout << "MySQL: Schema created/verified" << std::endl;
}

// ---------------------------------------------------------------------------
// saveSession
// ---------------------------------------------------------------------------

bool MySQLDatabase::saveSession(const CPAPSession& session) {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    if (!conn_) { std::cerr << "MySQL: Not connected" << std::endl; return false; }

    exec("START TRANSACTION");

    try {
        upsertDevice(session);

        int64_t session_id = insertSession(session);
        std::cout << "MySQL: Session ID: " << session_id << std::endl;

        if (!session.breathing_summary.empty()) {
            insertBreathingSummaries(session_id, session.breathing_summary);
            insertCalculatedMetrics(session_id, session.breathing_summary);
        }

        if (!session.events.empty()) {
            insertEvents(session_id, session.events);
        }

        if (!session.desaturations.empty()) {
            insertDesaturations(session_id, session.desaturations);
        }

        if (!session.breaths.empty()) {
            insertBreaths(session_id, session.breaths);
        }

        if (!session.vitals.empty()) {
            insertVitals(session_id, session.vitals);
        }

        if (session.metrics.has_value()) {
            insertSessionMetrics(session_id, session.metrics.value());
        }

        exec("COMMIT");
        std::cout << "MySQL: Session saved successfully" << std::endl;
        return true;

    } catch (const std::exception& e) {
        exec("ROLLBACK");
        std::cerr << "MySQL: Failed to save session: " << e.what() << std::endl;
        return false;
    }
}

// ---------------------------------------------------------------------------
// upsertDevice
// ---------------------------------------------------------------------------

void MySQLDatabase::upsertDevice(const CPAPSession& session) {
    const char* sql = R"(
        INSERT INTO cpap_devices (device_id, device_name, serial_number, model_id, version_id, last_seen)
        VALUES (?, ?, ?, ?, ?, NOW())
        ON DUPLICATE KEY UPDATE
            device_name   = VALUES(device_name),
            serial_number = VALUES(serial_number),
            model_id      = VALUES(model_id),
            version_id    = VALUES(version_id),
            last_seen     = NOW()
    )";

    MysqlStmtGuard g;
    g.stmt = mysql_stmt_init(conn_);
    if (mysql_stmt_prepare(g.stmt, sql, std::strlen(sql)) != 0) {
        std::cerr << "MySQL: upsertDevice prepare error: " << mysql_stmt_error(g.stmt) << std::endl;
        return;
    }

    ParamBinder p(5);
    p.bindText(0, session.device_id);
    p.bindText(1, session.device_name);
    p.bindText(2, session.serial_number);
    p.bindInt(3, session.model_id.value_or(0));
    p.bindInt(4, session.version_id.value_or(0));

    mysql_stmt_bind_param(g.stmt, p.data());
    if (mysql_stmt_execute(g.stmt) != 0) {
        std::cerr << "MySQL: upsertDevice error: " << mysql_stmt_error(g.stmt) << std::endl;
    }
}

// ---------------------------------------------------------------------------
// insertSession
// ---------------------------------------------------------------------------

int64_t MySQLDatabase::insertSession(const CPAPSession& session) {
    std::string start_str = fmtTimestamp(session.session_start.value());

    const char* sql = R"(
        INSERT INTO cpap_sessions
            (device_id, session_start, session_end, duration_seconds, data_records,
             brp_file_path, eve_file_path, sad_file_path, pld_file_path, csl_file_path)
        VALUES (?, ?, NULL, ?, ?, ?, ?, ?, ?, ?)
        ON DUPLICATE KEY UPDATE
            duration_seconds = VALUES(duration_seconds),
            data_records     = VALUES(data_records),
            brp_file_path    = VALUES(brp_file_path),
            eve_file_path    = VALUES(eve_file_path),
            sad_file_path    = VALUES(sad_file_path),
            pld_file_path    = VALUES(pld_file_path),
            csl_file_path    = VALUES(csl_file_path)
    )";

    MysqlStmtGuard g;
    g.stmt = mysql_stmt_init(conn_);
    if (mysql_stmt_prepare(g.stmt, sql, std::strlen(sql)) != 0) {
        std::cerr << "MySQL: insertSession prepare error: " << mysql_stmt_error(g.stmt) << std::endl;
        return 0;
    }

    ParamBinder p(9);
    p.bindText(0, session.device_id);
    p.bindText(1, start_str);
    p.bindInt(2, session.duration_seconds.value_or(0));
    p.bindInt(3, session.data_records);
    p.bindText(4, session.brp_file_path.value_or(""));
    p.bindText(5, session.eve_file_path.value_or(""));
    p.bindText(6, session.sad_file_path.value_or(""));
    p.bindText(7, session.pld_file_path.value_or(""));
    p.bindText(8, session.csl_file_path.value_or(""));

    mysql_stmt_bind_param(g.stmt, p.data());
    if (mysql_stmt_execute(g.stmt) != 0) {
        std::cerr << "MySQL: insertSession error: " << mysql_stmt_error(g.stmt) << std::endl;
        return 0;
    }

    // ON DUPLICATE KEY UPDATE: LAST_INSERT_ID() may not reflect actual row.
    // Look up by unique key.
    const char* lookup_sql = "SELECT id FROM cpap_sessions WHERE device_id = ? AND session_start = ?";
    MysqlStmtGuard g2;
    g2.stmt = mysql_stmt_init(conn_);
    mysql_stmt_prepare(g2.stmt, lookup_sql, std::strlen(lookup_sql));

    ParamBinder p2(2);
    p2.bindText(0, session.device_id);
    p2.bindText(1, start_str);
    mysql_stmt_bind_param(g2.stmt, p2.data());
    mysql_stmt_execute(g2.stmt);

    ResultBinder r(1);
    r.bindColInt64(0);
    mysql_stmt_bind_result(g2.stmt, r.data());
    mysql_stmt_store_result(g2.stmt);

    int64_t id = 0;
    if (mysql_stmt_fetch(g2.stmt) == 0) {
        id = r.colInt64(0);
    }
    return id;
}

// ---------------------------------------------------------------------------
// insertBreathingSummaries
// ---------------------------------------------------------------------------

void MySQLDatabase::insertBreathingSummaries(int64_t session_id,
                                              const std::vector<BreathingSummary>& summaries) {
    if (summaries.empty()) return;

    const char* sql = R"(
        INSERT IGNORE INTO cpap_breathing_summary
            (session_id, timestamp, avg_flow_rate, max_flow_rate, min_flow_rate,
             avg_pressure, max_pressure, min_pressure)
        VALUES (?, ?, ?, ?, ?, ?, ?, ?)
    )";

    MysqlStmtGuard g;
    g.stmt = mysql_stmt_init(conn_);
    if (mysql_stmt_prepare(g.stmt, sql, std::strlen(sql)) != 0) {
        std::cerr << "MySQL: insertBreathingSummaries prepare error: " << mysql_stmt_error(g.stmt) << std::endl;
        return;
    }

    for (const auto& s : summaries) {
        ParamBinder p(8);
        p.bindInt64(0, session_id);
        p.bindText(1, fmtTimestamp(s.timestamp));
        p.bindDouble(2, s.avg_flow_rate);
        p.bindDouble(3, s.max_flow_rate);
        p.bindDouble(4, s.min_flow_rate);
        p.bindDouble(5, s.avg_pressure);
        p.bindDouble(6, s.max_pressure);
        p.bindDouble(7, s.min_pressure);

        mysql_stmt_bind_param(g.stmt, p.data());
        if (mysql_stmt_execute(g.stmt) != 0) {
            std::cerr << "MySQL: insertBreathingSummaries error: " << mysql_stmt_error(g.stmt) << std::endl;
        }
    }
}

// ---------------------------------------------------------------------------
// insertEvents
// ---------------------------------------------------------------------------

void MySQLDatabase::insertEvents(int64_t session_id, const std::vector<CPAPEvent>& events) {
    if (events.empty()) return;

    const char* sql = R"(
        INSERT IGNORE INTO cpap_events
            (session_id, event_type, event_timestamp, duration_seconds, details)
        VALUES (?, ?, ?, ?, ?)
    )";

    MysqlStmtGuard g;
    g.stmt = mysql_stmt_init(conn_);
    if (mysql_stmt_prepare(g.stmt, sql, std::strlen(sql)) != 0) {
        std::cerr << "MySQL: insertEvents prepare error: " << mysql_stmt_error(g.stmt) << std::endl;
        return;
    }

    for (const auto& e : events) {
        ParamBinder p(5);
        p.bindInt64(0, session_id);
        p.bindText(1, eventTypeToString(e.event_type));
        p.bindText(2, fmtTimestamp(e.timestamp));
        p.bindDouble(3, e.duration_seconds);
        p.bindText(4, e.details.value_or(""));

        mysql_stmt_bind_param(g.stmt, p.data());
        if (mysql_stmt_execute(g.stmt) != 0) {
            std::cerr << "MySQL: insertEvents error: " << mysql_stmt_error(g.stmt) << std::endl;
        }
    }
}

void MySQLDatabase::insertDesaturations(int64_t session_id, const std::vector<DesatEvent>& desats) {
    if (desats.empty()) return;
    const char* sql = R"(
        INSERT IGNORE INTO cpap_events
            (session_id, event_type, event_timestamp, duration_seconds, details)
        VALUES (?, 'Desaturation', ?, ?, ?)
    )";
    MysqlStmtGuard g;
    g.stmt = mysql_stmt_init(conn_);
    if (mysql_stmt_prepare(g.stmt, sql, std::strlen(sql)) != 0) {
        std::cerr << "MySQL: insertDesaturations prepare error: " << mysql_stmt_error(g.stmt) << std::endl;
        return;
    }
    for (const auto& d : desats) {
        char details[96];
        std::snprintf(details, sizeof(details), "{\"nadir\":%.1f,\"depth\":%.1f}", d.nadir, d.depth);
        ParamBinder p(4);
        p.bindInt64(0, session_id);
        p.bindText(1, fmtTimestamp(d.onset));
        p.bindDouble(2, d.duration_seconds);
        p.bindText(3, std::string(details));
        mysql_stmt_bind_param(g.stmt, p.data());
        if (mysql_stmt_execute(g.stmt) != 0) {
            std::cerr << "MySQL: insertDesaturations error: " << mysql_stmt_error(g.stmt) << std::endl;
        }
    }
}

void MySQLDatabase::insertBreaths(int64_t session_id, const std::vector<Breath>& breaths) {
    if (breaths.empty()) return;
    const char* sql = R"(
        INSERT IGNORE INTO cpap_breaths
            (session_id, onset, tidal_volume, inspiratory_time, expiratory_time, flow_limitation)
        VALUES (?, ?, ?, ?, ?, ?)
    )";
    MysqlStmtGuard g;
    g.stmt = mysql_stmt_init(conn_);
    if (mysql_stmt_prepare(g.stmt, sql, std::strlen(sql)) != 0) {
        std::cerr << "MySQL: insertBreaths prepare error: " << mysql_stmt_error(g.stmt) << std::endl;
        return;
    }
    for (const auto& b : breaths) {
        ParamBinder p(6);
        p.bindInt64(0, session_id);
        p.bindText(1, fmtTimestamp(b.onset));
        p.bindDouble(2, b.tidal_volume);
        p.bindDouble(3, b.inspiratory_time);
        p.bindDouble(4, b.expiratory_time);
        p.bindDouble(5, b.flow_limitation);
        mysql_stmt_bind_param(g.stmt, p.data());
        if (mysql_stmt_execute(g.stmt) != 0) {
            std::cerr << "MySQL: insertBreaths error: " << mysql_stmt_error(g.stmt) << std::endl;
        }
    }
}

// ---------------------------------------------------------------------------
// insertVitals
// ---------------------------------------------------------------------------

void MySQLDatabase::insertVitals(int64_t session_id, const std::vector<CPAPVitals>& vitals) {
    if (vitals.empty()) return;

    const char* sql = R"(
        INSERT IGNORE INTO cpap_vitals (session_id, timestamp, spo2, heart_rate)
        VALUES (?, ?, ?, ?)
    )";

    MysqlStmtGuard g;
    g.stmt = mysql_stmt_init(conn_);
    if (mysql_stmt_prepare(g.stmt, sql, std::strlen(sql)) != 0) {
        std::cerr << "MySQL: insertVitals prepare error: " << mysql_stmt_error(g.stmt) << std::endl;
        return;
    }

    for (const auto& v : vitals) {
        ParamBinder p(4);
        p.bindInt64(0, session_id);
        p.bindText(1, fmtTimestamp(v.timestamp));
        if (v.spo2.has_value()) p.bindDouble(2, v.spo2.value());
        else p.bindNull(2);
        if (v.heart_rate.has_value()) p.bindInt(3, v.heart_rate.value());
        else p.bindNull(3);

        mysql_stmt_bind_param(g.stmt, p.data());
        if (mysql_stmt_execute(g.stmt) != 0) {
            std::cerr << "MySQL: insertVitals error: " << mysql_stmt_error(g.stmt) << std::endl;
        }
    }
}

// ---------------------------------------------------------------------------
// insertSessionMetrics
// ---------------------------------------------------------------------------

void MySQLDatabase::insertSessionMetrics(int64_t session_id, const SessionMetrics& m) {
    const char* sql = R"(
        INSERT INTO cpap_session_metrics
            (session_id, total_events, ahi, obstructive_apneas, central_apneas,
             hypopneas, reras, clear_airway_apneas,
             avg_spo2, min_spo2, avg_heart_rate, max_heart_rate, min_heart_rate,
             avg_mask_pressure, avg_epr_pressure, avg_snore,
             leak_p50, leak_p95, avg_leak_rate, max_leak_rate,
             avg_target_ventilation, therapy_mode, spo2_drops, odi)
        VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?)
        ON DUPLICATE KEY UPDATE
            total_events           = VALUES(total_events),
            ahi                    = VALUES(ahi),
            obstructive_apneas     = VALUES(obstructive_apneas),
            central_apneas         = VALUES(central_apneas),
            hypopneas              = VALUES(hypopneas),
            reras                  = VALUES(reras),
            clear_airway_apneas    = VALUES(clear_airway_apneas),
            avg_spo2               = VALUES(avg_spo2),
            min_spo2               = VALUES(min_spo2),
            avg_heart_rate         = VALUES(avg_heart_rate),
            max_heart_rate         = VALUES(max_heart_rate),
            min_heart_rate         = VALUES(min_heart_rate),
            avg_mask_pressure      = VALUES(avg_mask_pressure),
            avg_epr_pressure       = VALUES(avg_epr_pressure),
            avg_snore              = VALUES(avg_snore),
            leak_p50               = VALUES(leak_p50),
            leak_p95               = VALUES(leak_p95),
            avg_leak_rate          = VALUES(avg_leak_rate),
            max_leak_rate          = VALUES(max_leak_rate),
            avg_target_ventilation = VALUES(avg_target_ventilation),
            therapy_mode           = VALUES(therapy_mode),
            spo2_drops             = VALUES(spo2_drops),
            odi                    = VALUES(odi)
    )";

    MysqlStmtGuard g;
    g.stmt = mysql_stmt_init(conn_);
    if (mysql_stmt_prepare(g.stmt, sql, std::strlen(sql)) != 0) {
        std::cerr << "MySQL: insertSessionMetrics prepare error: " << mysql_stmt_error(g.stmt) << std::endl;
        return;
    }

    ParamBinder p(24);
    p.bindInt64(0, session_id);
    p.bindInt(1, m.total_events);
    p.bindDouble(2, m.ahi);
    p.bindInt(3, m.obstructive_apneas);
    p.bindInt(4, m.central_apneas);
    p.bindInt(5, m.hypopneas);
    p.bindInt(6, m.reras);
    p.bindInt(7, m.clear_airway_apneas);
    p.bindOptDouble(8, m.avg_spo2);
    p.bindOptDouble(9, m.min_spo2);
    p.bindOptInt(10, m.avg_heart_rate);
    p.bindOptInt(11, m.max_heart_rate);
    p.bindOptInt(12, m.min_heart_rate);
    p.bindOptDouble(13, m.avg_mask_pressure);
    p.bindOptDouble(14, m.avg_epr_pressure);
    p.bindOptDouble(15, m.avg_snore);
    p.bindOptDouble(16, m.leak_p50);
    p.bindOptDouble(17, m.leak_p95);
    p.bindOptDouble(18, m.avg_leak_rate);
    p.bindOptDouble(19, m.max_leak_rate);
    p.bindOptDouble(20, m.avg_target_ventilation);
    p.bindOptInt(21, m.therapy_mode);
    p.bindOptInt(22, m.spo2_drops);
    p.bindOptDouble(23, m.odi);

    mysql_stmt_bind_param(g.stmt, p.data());
    if (mysql_stmt_execute(g.stmt) != 0) {
        std::cerr << "MySQL: insertSessionMetrics error: " << mysql_stmt_error(g.stmt) << std::endl;
    }
}

// ---------------------------------------------------------------------------
// insertCalculatedMetrics
// ---------------------------------------------------------------------------

void MySQLDatabase::insertCalculatedMetrics(int64_t session_id,
                                             const std::vector<BreathingSummary>& summaries) {
    if (summaries.empty()) return;

    const char* sql = R"(
        INSERT IGNORE INTO cpap_calculated_metrics
            (session_id, timestamp, respiratory_rate, tidal_volume, minute_ventilation,
             inspiratory_time, expiratory_time, ie_ratio, flow_limitation, leak_rate,
             flow_p95, flow_p90, pressure_p95, pressure_p90,
             mask_pressure, epr_pressure, snore_index, target_ventilation)
        VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?)
    )";

    MysqlStmtGuard g;
    g.stmt = mysql_stmt_init(conn_);
    if (mysql_stmt_prepare(g.stmt, sql, std::strlen(sql)) != 0) {
        std::cerr << "MySQL: insertCalculatedMetrics prepare error: " << mysql_stmt_error(g.stmt) << std::endl;
        return;
    }

    for (const auto& s : summaries) {
        // Only insert if has any calculated metric
        if (!s.respiratory_rate && !s.tidal_volume && !s.minute_ventilation &&
            !s.flow_limitation && !s.mask_pressure && !s.snore_index &&
            !s.target_ventilation) continue;

        ParamBinder p(18);
        p.bindInt64(0, session_id);
        p.bindText(1, fmtTimestamp(s.timestamp));

        auto bind_opt = [&](int idx, const std::optional<double>& v) {
            if (v) p.bindDouble(idx, *v);
            else p.bindNull(idx);
        };

        bind_opt(2, s.respiratory_rate);
        bind_opt(3, s.tidal_volume);
        bind_opt(4, s.minute_ventilation);
        bind_opt(5, s.inspiratory_time);
        bind_opt(6, s.expiratory_time);
        bind_opt(7, s.ie_ratio);
        bind_opt(8, s.flow_limitation);
        bind_opt(9, s.leak_rate);
        bind_opt(10, s.flow_p95);
        p.bindNull(11);  // flow_p90 placeholder
        bind_opt(12, s.pressure_p95);
        p.bindNull(13);  // pressure_p90 placeholder
        bind_opt(14, s.mask_pressure);
        bind_opt(15, s.epr_pressure);
        bind_opt(16, s.snore_index);
        bind_opt(17, s.target_ventilation);

        mysql_stmt_bind_param(g.stmt, p.data());
        if (mysql_stmt_execute(g.stmt) != 0) {
            std::cerr << "MySQL: insertCalculatedMetrics error: " << mysql_stmt_error(g.stmt) << std::endl;
        }
    }
}

// ---------------------------------------------------------------------------
// sessionExists
// ---------------------------------------------------------------------------

bool MySQLDatabase::sessionExists(const std::string& device_id,
                                   const std::chrono::system_clock::time_point& session_start) {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    if (!conn_) return false;

    std::string ts = fmtTimestamp(session_start);

    const char* sql = R"(
        SELECT EXISTS(
            SELECT 1 FROM cpap_sessions
            WHERE device_id = ?
              AND session_start BETWEEN DATE_SUB(CAST(? AS DATETIME), INTERVAL 5 SECOND)
                                    AND DATE_ADD(CAST(? AS DATETIME), INTERVAL 5 SECOND)
        )
    )";

    MysqlStmtGuard g;
    g.stmt = mysql_stmt_init(conn_);
    mysql_stmt_prepare(g.stmt, sql, std::strlen(sql));

    ParamBinder p(3);
    p.bindText(0, device_id);
    p.bindText(1, ts);
    p.bindText(2, ts);
    mysql_stmt_bind_param(g.stmt, p.data());
    mysql_stmt_execute(g.stmt);

    ResultBinder r(1);
    r.bindColInt(0);
    mysql_stmt_bind_result(g.stmt, r.data());
    mysql_stmt_store_result(g.stmt);

    bool exists = false;
    if (mysql_stmt_fetch(g.stmt) == 0) {
        exists = r.colInt(0) != 0;
    }

    if (exists) {
        std::cout << "MySQL: Session " << ts << " already exists" << std::endl;
    }
    return exists;
}

// ---------------------------------------------------------------------------
// getLastSessionStart
// ---------------------------------------------------------------------------

std::optional<std::chrono::system_clock::time_point>
MySQLDatabase::getLastSessionStart(const std::string& device_id) {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    if (!conn_) return std::nullopt;

    const char* sql = R"(
        SELECT DATE_FORMAT(session_start, '%Y-%m-%d %H:%i:%s') FROM cpap_sessions
        WHERE device_id = ?
        ORDER BY session_start DESC
        LIMIT 1
    )";

    MysqlStmtGuard g;
    g.stmt = mysql_stmt_init(conn_);
    mysql_stmt_prepare(g.stmt, sql, std::strlen(sql));

    ParamBinder p(1);
    p.bindText(0, device_id);
    mysql_stmt_bind_param(g.stmt, p.data());
    mysql_stmt_execute(g.stmt);

    ResultBinder r(1);
    r.bindColString(0);
    mysql_stmt_bind_result(g.stmt, r.data());
    mysql_stmt_store_result(g.stmt);

    if (mysql_stmt_fetch(g.stmt) != 0) {
        std::cout << "MySQL: No previous sessions for " << device_id << std::endl;
        return std::nullopt;
    }

    std::string ts_str = r.colText(0);
    std::tm tm = {};
    std::istringstream ss(ts_str);
    ss >> std::get_time(&tm, "%Y-%m-%d %H:%M:%S");
    if (ss.fail()) {
        std::cerr << "MySQL: Failed to parse timestamp: " << ts_str << std::endl;
        return std::nullopt;
    }
    tm.tm_isdst = -1;
    auto tp = std::chrono::system_clock::from_time_t(std::mktime(&tm));
    std::cout << "MySQL: Last session for " << device_id << " at " << ts_str << std::endl;
    return tp;
}

std::optional<std::chrono::system_clock::time_point>
MySQLDatabase::getSessionStartForSleepDay(const std::string&, const std::string&, bool) {
    return std::nullopt;
}

// ---------------------------------------------------------------------------
// getSessionMetrics
// ---------------------------------------------------------------------------

std::optional<SessionMetrics> MySQLDatabase::getSessionMetrics(
    const std::string& device_id,
    const std::chrono::system_clock::time_point& session_start) {

    std::lock_guard<std::recursive_mutex> lock(mutex_);
    if (!conn_) return std::nullopt;

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
          AND s.session_start BETWEEN DATE_SUB(CAST(? AS DATETIME), INTERVAL 5 SECOND)
                                  AND DATE_ADD(CAST(? AS DATETIME), INTERVAL 5 SECOND)
    )";

    MysqlStmtGuard g;
    g.stmt = mysql_stmt_init(conn_);
    mysql_stmt_prepare(g.stmt, sql, std::strlen(sql));

    ParamBinder p(3);
    p.bindText(0, device_id);
    p.bindText(1, ts);
    p.bindText(2, ts);
    mysql_stmt_bind_param(g.stmt, p.data());
    mysql_stmt_execute(g.stmt);

    // 28 output columns
    ResultBinder r(28);
    for (int i = 0; i < 28; ++i) r.bindColDouble(i);
    // Override int columns
    r.bindColInt(0);   // total_events
    r.bindColInt(2);   // OA
    r.bindColInt(3);   // CA
    r.bindColInt(4);   // hypopneas
    r.bindColInt(5);   // reras
    r.bindColInt(6);   // CA clear
    r.bindColInt(12);  // avg_heart_rate
    r.bindColInt(13);  // max_heart_rate
    r.bindColInt(14);  // min_heart_rate

    mysql_stmt_bind_result(g.stmt, r.data());
    mysql_stmt_store_result(g.stmt);

    if (mysql_stmt_fetch(g.stmt) != 0) return std::nullopt;

    SessionMetrics m;
    m.total_events        = r.colInt(0);
    m.ahi                 = r.colDouble(1);
    m.obstructive_apneas  = r.colInt(2);
    m.central_apneas      = r.colInt(3);
    m.hypopneas           = r.colInt(4);
    m.reras               = r.colInt(5);
    m.clear_airway_apneas = r.colInt(6);

    m.avg_event_duration    = r.colOptDouble(7);
    m.max_event_duration    = r.colOptDouble(8);
    m.time_in_apnea_percent = r.colOptDouble(9);
    m.usage_hours           = r.colOptDouble(15);
    m.usage_percent         = r.colOptDouble(16);
    m.avg_leak_rate         = r.colOptDouble(17);
    m.max_leak_rate         = r.colOptDouble(18);
    m.avg_respiratory_rate  = r.colOptDouble(19);
    m.avg_tidal_volume      = r.colOptDouble(20);
    m.avg_minute_ventilation = r.colOptDouble(21);
    m.avg_inspiratory_time  = r.colOptDouble(22);
    m.avg_expiratory_time   = r.colOptDouble(23);
    m.avg_ie_ratio          = r.colOptDouble(24);
    m.avg_flow_limitation   = r.colOptDouble(25);
    m.flow_p95              = r.colOptDouble(26);
    m.pressure_p95          = r.colOptDouble(27);

    if (!r.colIsNull(10) && r.colDouble(10) > 0)
        m.avg_spo2 = r.colDouble(10);
    if (!r.colIsNull(12) && r.colInt(12) > 0)
        m.avg_heart_rate = r.colInt(12);

    return m;
}

// ---------------------------------------------------------------------------
// markSessionCompleted
// ---------------------------------------------------------------------------

bool MySQLDatabase::markSessionCompleted(const std::string& device_id,
                                          const std::chrono::system_clock::time_point& session_start) {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    if (!conn_) return false;

    std::string ts = fmtTimestamp(session_start);

    const char* sql = R"(
        UPDATE cpap_sessions
        SET session_end = NOW(), updated_at = NOW()
        WHERE device_id = ?
          AND session_start BETWEEN DATE_SUB(CAST(? AS DATETIME), INTERVAL 5 SECOND)
                                AND DATE_ADD(CAST(? AS DATETIME), INTERVAL 5 SECOND)
          AND session_end IS NULL
    )";

    MysqlStmtGuard g;
    g.stmt = mysql_stmt_init(conn_);
    mysql_stmt_prepare(g.stmt, sql, std::strlen(sql));

    ParamBinder p(3);
    p.bindText(0, device_id);
    p.bindText(1, ts);
    p.bindText(2, ts);
    mysql_stmt_bind_param(g.stmt, p.data());
    mysql_stmt_execute(g.stmt);

    my_ulonglong changes = mysql_stmt_affected_rows(g.stmt);

    if (changes > 0) {
        std::cout << "MySQL: Marked session " << ts << " as COMPLETED" << std::endl;
        return true;
    }
    std::cout << "MySQL: Session already has session_end set" << std::endl;
    return false;
}

// ---------------------------------------------------------------------------
// reopenSession
// ---------------------------------------------------------------------------

bool MySQLDatabase::reopenSession(const std::string& device_id,
                                   const std::chrono::system_clock::time_point& session_start) {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    if (!conn_) return false;

    std::string ts = fmtTimestamp(session_start);

    const char* sql = R"(
        UPDATE cpap_sessions
        SET session_end = NULL, updated_at = NOW()
        WHERE device_id = ?
          AND session_start BETWEEN DATE_SUB(CAST(? AS DATETIME), INTERVAL 5 SECOND)
                                AND DATE_ADD(CAST(? AS DATETIME), INTERVAL 5 SECOND)
          AND session_end IS NOT NULL
    )";

    MysqlStmtGuard g;
    g.stmt = mysql_stmt_init(conn_);
    mysql_stmt_prepare(g.stmt, sql, std::strlen(sql));

    ParamBinder p(3);
    p.bindText(0, device_id);
    p.bindText(1, ts);
    p.bindText(2, ts);
    mysql_stmt_bind_param(g.stmt, p.data());
    mysql_stmt_execute(g.stmt);

    my_ulonglong changes = mysql_stmt_affected_rows(g.stmt);

    if (changes > 0) {
        std::cout << "MySQL: Reopened session " << ts << std::endl;
        return true;
    }
    return false;
}

// ---------------------------------------------------------------------------
// deleteSessionsByDateFolder
// ---------------------------------------------------------------------------

int MySQLDatabase::deleteSessionsByDateFolder(const std::string& device_id,
                                               const std::string& date_folder) {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    if (!conn_) return -1;

    std::string pattern = "%DATALOG/" + date_folder + "/%";

    const char* sql = R"(
        DELETE FROM cpap_sessions
        WHERE device_id = ? AND brp_file_path LIKE ?
    )";

    MysqlStmtGuard g;
    g.stmt = mysql_stmt_init(conn_);
    mysql_stmt_prepare(g.stmt, sql, std::strlen(sql));

    ParamBinder p(2);
    p.bindText(0, device_id);
    p.bindText(1, pattern);
    mysql_stmt_bind_param(g.stmt, p.data());

    if (mysql_stmt_execute(g.stmt) != 0) {
        std::cerr << "MySQL: deleteSessionsByDateFolder error: " << mysql_stmt_error(g.stmt) << std::endl;
        return -1;
    }
    return static_cast<int>(mysql_stmt_affected_rows(g.stmt));
}

// ---------------------------------------------------------------------------
// isForceCompleted / setForceCompleted
// ---------------------------------------------------------------------------

bool MySQLDatabase::isForceCompleted(const std::string& device_id,
                                      const std::chrono::system_clock::time_point& session_start) {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    if (!conn_) return false;

    std::string ts = fmtTimestamp(session_start);

    const char* sql = R"(
        SELECT COALESCE(force_completed, 0) FROM cpap_sessions
        WHERE device_id = ?
          AND session_start BETWEEN DATE_SUB(CAST(? AS DATETIME), INTERVAL 5 SECOND)
                                AND DATE_ADD(CAST(? AS DATETIME), INTERVAL 5 SECOND)
        LIMIT 1
    )";

    MysqlStmtGuard g;
    g.stmt = mysql_stmt_init(conn_);
    mysql_stmt_prepare(g.stmt, sql, std::strlen(sql));

    ParamBinder p(3);
    p.bindText(0, device_id);
    p.bindText(1, ts);
    p.bindText(2, ts);
    mysql_stmt_bind_param(g.stmt, p.data());
    mysql_stmt_execute(g.stmt);

    ResultBinder r(1);
    r.bindColInt(0);
    mysql_stmt_bind_result(g.stmt, r.data());
    mysql_stmt_store_result(g.stmt);

    if (mysql_stmt_fetch(g.stmt) == 0) {
        return r.colInt(0) != 0;
    }
    return false;
}

bool MySQLDatabase::setForceCompleted(const std::string& device_id,
                                       const std::chrono::system_clock::time_point& session_start) {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    if (!conn_) return false;

    std::string ts = fmtTimestamp(session_start);

    const char* sql = R"(
        UPDATE cpap_sessions SET force_completed = 1, updated_at = NOW()
        WHERE device_id = ?
          AND session_start BETWEEN DATE_SUB(CAST(? AS DATETIME), INTERVAL 5 SECOND)
                                AND DATE_ADD(CAST(? AS DATETIME), INTERVAL 5 SECOND)
    )";

    MysqlStmtGuard g;
    g.stmt = mysql_stmt_init(conn_);
    mysql_stmt_prepare(g.stmt, sql, std::strlen(sql));

    ParamBinder p(3);
    p.bindText(0, device_id);
    p.bindText(1, ts);
    p.bindText(2, ts);
    mysql_stmt_bind_param(g.stmt, p.data());
    mysql_stmt_execute(g.stmt);

    my_ulonglong changes = mysql_stmt_affected_rows(g.stmt);

    if (changes > 0) {
        std::cout << "MySQL: Session " << ts << " marked force_completed" << std::endl;
        return true;
    }
    return false;
}

// ---------------------------------------------------------------------------
// getCheckpointFileSizes / updateCheckpointFileSizes
// ---------------------------------------------------------------------------

std::map<std::string, int> MySQLDatabase::getCheckpointFileSizes(
    const std::string& device_id,
    const std::chrono::system_clock::time_point& session_start) {

    std::lock_guard<std::recursive_mutex> lock(mutex_);
    if (!conn_) return {};

    std::string ts = fmtTimestamp(session_start);

    const char* sql = R"(
        SELECT checkpoint_files FROM cpap_sessions
        WHERE device_id = ?
          AND session_start BETWEEN DATE_SUB(CAST(? AS DATETIME), INTERVAL 5 SECOND)
                                AND DATE_ADD(CAST(? AS DATETIME), INTERVAL 5 SECOND)
        LIMIT 1
    )";

    MysqlStmtGuard g;
    g.stmt = mysql_stmt_init(conn_);
    mysql_stmt_prepare(g.stmt, sql, std::strlen(sql));

    ParamBinder p(3);
    p.bindText(0, device_id);
    p.bindText(1, ts);
    p.bindText(2, ts);
    mysql_stmt_bind_param(g.stmt, p.data());
    mysql_stmt_execute(g.stmt);

    // checkpoint_files is JSON, fetched as string
    ResultBinder r(1);
    r.bindColString(0);
    mysql_stmt_bind_result(g.stmt, r.data());
    mysql_stmt_store_result(g.stmt);

    if (mysql_stmt_fetch(g.stmt) != 0 || r.colIsNull(0)) {
        return {};
    }

    // Parse simple JSON {"file1":123,"file2":456}
    std::string json_str = r.colText(0);
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

bool MySQLDatabase::updateCheckpointFileSizes(
    const std::string& device_id,
    const std::chrono::system_clock::time_point& session_start,
    const std::map<std::string, int>& file_sizes) {

    std::lock_guard<std::recursive_mutex> lock(mutex_);
    if (!conn_) return false;

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
        SET checkpoint_files = ?, updated_at = NOW()
        WHERE device_id = ?
          AND session_start BETWEEN DATE_SUB(CAST(? AS DATETIME), INTERVAL 5 SECOND)
                                AND DATE_ADD(CAST(? AS DATETIME), INTERVAL 5 SECOND)
    )";

    MysqlStmtGuard g;
    g.stmt = mysql_stmt_init(conn_);
    mysql_stmt_prepare(g.stmt, sql, std::strlen(sql));

    ParamBinder p(4);
    p.bindText(0, json_oss.str());
    p.bindText(1, device_id);
    p.bindText(2, ts);
    p.bindText(3, ts);
    mysql_stmt_bind_param(g.stmt, p.data());

    if (mysql_stmt_execute(g.stmt) != 0) {
        std::cerr << "MySQL: updateCheckpointFileSizes error: " << mysql_stmt_error(g.stmt) << std::endl;
        return false;
    }

    std::cout << "MySQL: Updated checkpoint_files (" << file_sizes.size() << " files)" << std::endl;
    return true;
}

// ---------------------------------------------------------------------------
// updateDeviceLastSeen
// ---------------------------------------------------------------------------

bool MySQLDatabase::updateDeviceLastSeen(const std::string& device_id) {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    if (!conn_) return false;

    const char* sql = "UPDATE cpap_devices SET last_seen = NOW() WHERE device_id = ?";

    MysqlStmtGuard g;
    g.stmt = mysql_stmt_init(conn_);
    mysql_stmt_prepare(g.stmt, sql, std::strlen(sql));

    ParamBinder p(1);
    p.bindText(0, device_id);
    mysql_stmt_bind_param(g.stmt, p.data());

    if (mysql_stmt_execute(g.stmt) != 0) {
        std::cerr << "MySQL: updateDeviceLastSeen error: " << mysql_stmt_error(g.stmt) << std::endl;
        return false;
    }
    return true;
}

// ---------------------------------------------------------------------------
// saveSTRDailyRecords
// ---------------------------------------------------------------------------

bool MySQLDatabase::saveSTRDailyRecords(const std::vector<STRDailyRecord>& records) {
    if (records.empty()) return true;

    std::lock_guard<std::recursive_mutex> lock(mutex_);
    if (!conn_) return false;

    exec("START TRANSACTION");

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
                    ?, ?, NOW())
            ON DUPLICATE KEY UPDATE
                mask_pairs       = VALUES(mask_pairs),
                mask_events      = VALUES(mask_events),
                duration_minutes = VALUES(duration_minutes),
                patient_hours    = VALUES(patient_hours),
                ahi = VALUES(ahi), hi = VALUES(hi), ai = VALUES(ai),
                oai = VALUES(oai), cai = VALUES(cai), uai = VALUES(uai),
                rin = VALUES(rin), csr = VALUES(csr),
                mask_press_50    = VALUES(mask_press_50),
                mask_press_95    = VALUES(mask_press_95),
                mask_press_max   = VALUES(mask_press_max),
                leak_50 = VALUES(leak_50), leak_95 = VALUES(leak_95),
                leak_max = VALUES(leak_max),
                spo2_50 = VALUES(spo2_50), spo2_95 = VALUES(spo2_95),
                resp_rate_50     = VALUES(resp_rate_50),
                tid_vol_50       = VALUES(tid_vol_50),
                min_vent_50      = VALUES(min_vent_50),
                mode = VALUES(mode), epr_level = VALUES(epr_level),
                pressure_setting = VALUES(pressure_setting),
                fault_device     = VALUES(fault_device),
                fault_alarm      = VALUES(fault_alarm),
                updated_at       = NOW()
        )";

        MysqlStmtGuard g;
        g.stmt = mysql_stmt_init(conn_);
        if (mysql_stmt_prepare(g.stmt, sql, std::strlen(sql)) != 0) {
            std::cerr << "MySQL: saveSTRDailyRecords prepare error: " << mysql_stmt_error(g.stmt) << std::endl;
            exec("ROLLBACK");
            return false;
        }

        for (const auto& rec : records) {
            // Format date
            auto date_t = std::chrono::system_clock::to_time_t(rec.record_date);
            std::tm* tm = std::localtime(&date_t);
            std::ostringstream date_oss;
            date_oss << std::put_time(tm, "%Y-%m-%d");

            // Build mask_pairs JSON
            std::ostringstream pairs_json;
            pairs_json << "[";
            for (size_t i = 0; i < rec.mask_pairs.size(); ++i) {
                if (i > 0) pairs_json << ",";
                auto on_t = std::chrono::system_clock::to_time_t(rec.mask_pairs[i].first);
                auto off_t = std::chrono::system_clock::to_time_t(rec.mask_pairs[i].second);
                std::tm on_tm = *std::localtime(&on_t);
                std::tm off_tm = *std::localtime(&off_t);
                std::ostringstream on_oss, off_oss;
                on_oss << std::put_time(&on_tm, "%Y-%m-%dT%H:%M:%S");
                off_oss << std::put_time(&off_tm, "%Y-%m-%dT%H:%M:%S");
                pairs_json << "{\"on\":\"" << on_oss.str() << "\",\"off\":\"" << off_oss.str() << "\"}";
            }
            pairs_json << "]";

            ParamBinder p(30);
            p.bindText(0, rec.device_id);
            p.bindText(1, date_oss.str());
            p.bindText(2, pairs_json.str());
            p.bindInt(3, rec.mask_events);
            p.bindDouble(4, rec.duration_minutes);
            p.bindDouble(5, rec.patient_hours);
            p.bindDouble(6, rec.ahi);
            p.bindDouble(7, rec.hi);
            p.bindDouble(8, rec.ai);
            p.bindDouble(9, rec.oai);
            p.bindDouble(10, rec.cai);
            p.bindDouble(11, rec.uai);
            p.bindDouble(12, rec.rin);
            p.bindDouble(13, rec.csr);
            p.bindDouble(14, rec.mask_press_50);
            p.bindDouble(15, rec.mask_press_95);
            p.bindDouble(16, rec.mask_press_max);
            p.bindDouble(17, rec.leak_50);
            p.bindDouble(18, rec.leak_95);
            p.bindDouble(19, rec.leak_max);
            p.bindDouble(20, rec.spo2_50);
            p.bindDouble(21, rec.spo2_95);
            p.bindDouble(22, rec.resp_rate_50);
            p.bindDouble(23, rec.tid_vol_50);
            p.bindDouble(24, rec.min_vent_50);
            p.bindInt(25, rec.mode);
            p.bindDouble(26, rec.epr_level);
            p.bindDouble(27, rec.pressure_setting);
            p.bindInt(28, rec.fault_device);
            p.bindInt(29, rec.fault_alarm);

            mysql_stmt_bind_param(g.stmt, p.data());
            if (mysql_stmt_execute(g.stmt) != 0) {
                std::cerr << "MySQL: saveSTRDailyRecords error: " << mysql_stmt_error(g.stmt) << std::endl;
            }
        }

        exec("COMMIT");
        std::cout << "MySQL: Saved " << records.size() << " STR daily records" << std::endl;
        return true;

    } catch (const std::exception& e) {
        exec("ROLLBACK");
        std::cerr << "MySQL: saveSTRDailyRecords error: " << e.what() << std::endl;
        return false;
    }
}

// ---------------------------------------------------------------------------
// getLastSTRDate
// ---------------------------------------------------------------------------

std::optional<std::string> MySQLDatabase::getLastSTRDate(const std::string& device_id) {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    if (!conn_) return std::nullopt;

    const char* sql = "SELECT DATE_FORMAT(MAX(record_date), '%Y-%m-%d') FROM cpap_daily_summary WHERE device_id = ?";

    MysqlStmtGuard g;
    g.stmt = mysql_stmt_init(conn_);
    mysql_stmt_prepare(g.stmt, sql, std::strlen(sql));

    ParamBinder p(1);
    p.bindText(0, device_id);
    mysql_stmt_bind_param(g.stmt, p.data());
    mysql_stmt_execute(g.stmt);

    ResultBinder r(1);
    r.bindColString(0);
    mysql_stmt_bind_result(g.stmt, r.data());
    mysql_stmt_store_result(g.stmt);

    if (mysql_stmt_fetch(g.stmt) == 0 && !r.colIsNull(0)) {
        return r.colText(0);
    }
    return std::nullopt;
}

// ---------------------------------------------------------------------------
// aggregateDailySummaryFromSessions
// ---------------------------------------------------------------------------

bool MySQLDatabase::aggregateDailySummaryFromSessions(const std::string& device_id) {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    if (!conn_) return false;

    // NULLIF(...,0) on pressure/spo2/epr: PrismaParser doesn't aggregate WMEDF
    // signal samples yet (issue #15), so those columns are always 0 in
    // cpap_session_metrics -- store NULL rather than a misleading zero.
    //
    // ahi is a duration-weighted average of the parser's own per-session m.ahi,
    // NOT re-derived from summing obstructive/central/clear_airway/hypopneas: the
    // parser counts a further generic "unclassified apnea" event type that is
    // folded into m.ahi but never persisted to any typed column (see
    // cpapdash-parser ParsedSession::calculateMetrics, `apnea_other`), so
    // reconstructing ahi from the typed sums alone silently undercounts it. ai is
    // then ahi - hi so the AHI = AI + HI identity holds for the row; ai absorbs
    // that untracked bucket as a residual, same as it does inside m.ahi itself.
    const char* sql = R"(
        INSERT INTO cpap_daily_summary
            (device_id, record_date, duration_minutes, patient_hours,
             ahi, hi, ai, oai, cai, uai, rin, mask_events, mask_pairs,
             mask_press_50, leak_50, leak_95, spo2_50, epr_level, mode, updated_at)
        SELECT
            s.device_id,
            DATE(DATE_SUB(s.session_start, INTERVAL 12 HOUR)) AS record_date,
            ROUND(SUM(s.duration_seconds) / 60.0, 1),
            ROUND(SUM(s.duration_seconds) / 3600.0, 2),
            ROUND(SUM(COALESCE(m.ahi,0) * s.duration_seconds / 3600.0)
                / NULLIF(SUM(s.duration_seconds) / 3600.0, 0), 2) AS ahi,
            ROUND(SUM(COALESCE(m.hypopneas,0)) / NULLIF(SUM(s.duration_seconds) / 3600.0, 0), 2) AS hi,
            ROUND(SUM(COALESCE(m.ahi,0) * s.duration_seconds / 3600.0) / NULLIF(SUM(s.duration_seconds) / 3600.0, 0)
                - SUM(COALESCE(m.hypopneas,0)) / NULLIF(SUM(s.duration_seconds) / 3600.0, 0), 2) AS ai,
            ROUND(SUM(COALESCE(m.obstructive_apneas,0)) / NULLIF(SUM(s.duration_seconds) / 3600.0, 0), 2) AS oai,
            ROUND(SUM(COALESCE(m.central_apneas,0)) / NULLIF(SUM(s.duration_seconds) / 3600.0, 0), 2) AS cai,
            ROUND(SUM(COALESCE(m.clear_airway_apneas,0)) / NULLIF(SUM(s.duration_seconds) / 3600.0, 0), 2) AS uai,
            ROUND(SUM(COALESCE(m.reras,0)) / NULLIF(SUM(s.duration_seconds) / 3600.0, 0), 2) AS rin,
            SUM(COALESCE(m.total_events,0)) AS mask_events,
            '[]' AS mask_pairs,
            ROUND(AVG(NULLIF(m.avg_mask_pressure, 0)), 1),
            ROUND(AVG(NULLIF(m.leak_p50, 0)), 2),
            ROUND(AVG(NULLIF(m.leak_p95, 0)), 2),
            ROUND(AVG(NULLIF(m.avg_spo2, 0)), 1),
            ROUND(AVG(NULLIF(m.avg_epr_pressure, 0)), 2),
            MAX(COALESCE(m.therapy_mode, 0)),
            NOW()
        FROM cpap_sessions s
        JOIN cpap_session_metrics m ON m.session_id = s.id
        WHERE s.device_id = ?
        GROUP BY s.device_id, DATE(DATE_SUB(s.session_start, INTERVAL 12 HOUR))
        ON DUPLICATE KEY UPDATE
            duration_minutes = VALUES(duration_minutes),
            patient_hours    = VALUES(patient_hours),
            ahi = VALUES(ahi), hi = VALUES(hi), ai = VALUES(ai),
            oai = VALUES(oai), cai = VALUES(cai), uai = VALUES(uai), rin = VALUES(rin),
            mask_events      = VALUES(mask_events),
            mask_pairs       = VALUES(mask_pairs),
            mask_press_50    = VALUES(mask_press_50),
            leak_50 = VALUES(leak_50), leak_95 = VALUES(leak_95),
            spo2_50          = VALUES(spo2_50),
            epr_level        = VALUES(epr_level),
            mode             = VALUES(mode),
            updated_at       = NOW()
    )";

    MysqlStmtGuard g;
    g.stmt = mysql_stmt_init(conn_);
    if (mysql_stmt_prepare(g.stmt, sql, std::strlen(sql)) != 0) {
        std::cerr << "MySQL: aggregateDailySummaryFromSessions prepare error: "
                  << mysql_stmt_error(g.stmt) << std::endl;
        return false;
    }

    ParamBinder p(1);
    p.bindText(0, device_id);
    mysql_stmt_bind_param(g.stmt, p.data());

    if (mysql_stmt_execute(g.stmt) != 0) {
        std::cerr << "MySQL: aggregateDailySummaryFromSessions error: "
                  << mysql_stmt_error(g.stmt) << std::endl;
        return false;
    }
    return true;
}

// ---------------------------------------------------------------------------
// getNightlyMetrics
// ---------------------------------------------------------------------------

std::optional<SessionMetrics> MySQLDatabase::getNightlyMetrics(
    const std::string& device_id,
    const std::chrono::system_clock::time_point& session_start) {

    std::lock_guard<std::recursive_mutex> lock(mutex_);
    if (!conn_) return std::nullopt;

    std::string ts = fmtTimestamp(session_start);

    // MySQL version of the nightly aggregation query.
    // sleep_day = DATE(DATE_SUB(session_start, INTERVAL 12 HOUR))
    const char* sql = R"(
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
                 THEN ROUND((COALESCE(MAX(sm.obstructive_apneas), 0) + COALESCE(MAX(sm.central_apneas), 0)
                            + COALESCE(MAX(sm.hypopneas), 0) + COALESCE(MAX(sm.clear_airway_apneas), 0))
                          * 3600.0 / SUM(s.duration_seconds), 4)
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
          AND DATE(DATE_SUB(s.session_start, INTERVAL 12 HOUR)) = (
              SELECT DATE(DATE_SUB(session_start, INTERVAL 12 HOUR))
              FROM cpap_sessions
              WHERE device_id = ?
                AND session_start BETWEEN DATE_SUB(CAST(? AS DATETIME), INTERVAL 5 SECOND)
                                      AND DATE_ADD(CAST(? AS DATETIME), INTERVAL 5 SECOND)
              LIMIT 1
          )
    )";

    MysqlStmtGuard g;
    g.stmt = mysql_stmt_init(conn_);
    mysql_stmt_prepare(g.stmt, sql, std::strlen(sql));

    ParamBinder p(4);
    p.bindText(0, device_id);
    p.bindText(1, device_id);
    p.bindText(2, ts);
    p.bindText(3, ts);
    mysql_stmt_bind_param(g.stmt, p.data());
    mysql_stmt_execute(g.stmt);

    // 34 output columns (indices 0-33)
    ResultBinder r(34);
    for (int i = 0; i < 34; ++i) r.bindColDouble(i);
    // Override int columns
    r.bindColInt(1);   // total_events
    r.bindColInt(2);   // OA
    r.bindColInt(3);   // CA
    r.bindColInt(4);   // hypopneas
    r.bindColInt(5);   // reras
    r.bindColInt(6);   // CA_clear
    r.bindColInt(33);  // therapy_mode

    mysql_stmt_bind_result(g.stmt, r.data());
    mysql_stmt_store_result(g.stmt);

    if (mysql_stmt_fetch(g.stmt) != 0) return std::nullopt;
    if (r.colIsNull(0)) return std::nullopt;  // total_seconds

    // Column order:
    //  0=total_seconds  1=total_events  2=OA  3=CA  4=hyp  5=rera  6=CA_clear
    //  7=avg_evt_dur  8=max_evt_dur  9=usage_hours  10=usage_pct  11=ahi
    // 12=time_apnea_pct  13=avg_leak  14=max_leak  15=avg_rr  16=avg_tv
    // 17=avg_mv  18=avg_it  19=avg_et  20=avg_ie  21=avg_fl  22=fp95  23=pp95
    // 24=avg_mask_pressure  25=avg_epr_pressure  26=avg_snore  27=avg_target_ventilation
    // 28=avg_pressure  29=max_pressure  30=min_pressure
    // 31=leak_p50  32=leak_p95_sess  33=therapy_mode
    SessionMetrics m;
    m.total_events        = r.colInt(1);
    m.ahi                 = r.colDouble(11);
    m.obstructive_apneas  = r.colInt(2);
    m.central_apneas      = r.colInt(3);
    m.hypopneas           = r.colInt(4);
    m.reras               = r.colInt(5);
    m.clear_airway_apneas = r.colInt(6);

    m.avg_event_duration    = r.colOptDouble(7);
    m.max_event_duration    = r.colOptDouble(8);
    m.time_in_apnea_percent = r.colOptDouble(12);
    m.usage_hours           = r.colOptDouble(9);
    m.usage_percent         = r.colOptDouble(10);
    m.avg_leak_rate         = r.colOptDouble(13);
    m.max_leak_rate         = r.colOptDouble(14);
    m.avg_respiratory_rate  = r.colOptDouble(15);
    m.avg_tidal_volume      = r.colOptDouble(16);
    m.avg_minute_ventilation = r.colOptDouble(17);
    m.avg_inspiratory_time  = r.colOptDouble(18);
    m.avg_expiratory_time   = r.colOptDouble(19);
    m.avg_ie_ratio          = r.colOptDouble(20);
    m.avg_flow_limitation   = r.colOptDouble(21);
    m.flow_p95              = r.colOptDouble(22);
    m.pressure_p95          = r.colOptDouble(23);
    m.avg_pressure          = r.colOptDouble(28);
    m.max_pressure          = r.colOptDouble(29);
    m.min_pressure          = r.colOptDouble(30);
    m.avg_mask_pressure     = r.colOptDouble(24);
    m.avg_epr_pressure      = r.colOptDouble(25);
    m.avg_snore             = r.colOptDouble(26);
    if (!r.colIsNull(27) && r.colDouble(27) > 0)
        m.avg_target_ventilation = r.colDouble(27);
    m.leak_p50              = r.colOptDouble(31);
    m.leak_p95              = r.colOptDouble(32);
    m.therapy_mode          = r.colOptInt(33);

    return m;
}

// ---------------------------------------------------------------------------
// getMetricsForDateRange
// ---------------------------------------------------------------------------

std::vector<SessionMetrics> MySQLDatabase::getMetricsForDateRange(
    const std::string& device_id, int days_back) {

    std::lock_guard<std::recursive_mutex> lock(mutex_);
    if (!conn_) return {};

    // Compute cutoff timestamp string
    auto cutoff = std::chrono::system_clock::now() - std::chrono::hours(days_back * 24);
    std::string cutoff_str = fmtTimestamp(cutoff);

    // One row per sleep-night, same aggregation as getNightlyMetrics
    const char* sql = R"(
        SELECT
            DATE_FORMAT(DATE(DATE_SUB(s.session_start, INTERVAL 12 HOUR)), '%Y-%m-%d') AS sleep_day,
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
                 THEN ROUND((COALESCE(MAX(sm.obstructive_apneas), 0) + COALESCE(MAX(sm.central_apneas), 0)
                            + COALESCE(MAX(sm.hypopneas), 0) + COALESCE(MAX(sm.clear_airway_apneas), 0))
                          * 3600.0 / SUM(s.duration_seconds), 4)
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
          AND s.session_start >= CAST(? AS DATETIME)
          AND s.session_end IS NOT NULL
        GROUP BY DATE(DATE_SUB(s.session_start, INTERVAL 12 HOUR))
        ORDER BY sleep_day ASC
    )";

    MysqlStmtGuard g;
    g.stmt = mysql_stmt_init(conn_);
    mysql_stmt_prepare(g.stmt, sql, std::strlen(sql));

    ParamBinder p(2);
    p.bindText(0, device_id);
    p.bindText(1, cutoff_str);
    mysql_stmt_bind_param(g.stmt, p.data());
    mysql_stmt_execute(g.stmt);

    // 30 output columns (0-29)
    ResultBinder r(30);
    r.bindColString(0);  // sleep_day
    for (int i = 1; i < 30; ++i) r.bindColDouble(i);
    // Override int columns
    r.bindColInt(2);   // total_events
    r.bindColInt(3);   // OA
    r.bindColInt(4);   // CA
    r.bindColInt(5);   // hypopneas
    r.bindColInt(6);   // reras
    r.bindColInt(7);   // CA_clear
    r.bindColInt(29);  // therapy_mode

    mysql_stmt_bind_result(g.stmt, r.data());
    mysql_stmt_store_result(g.stmt);

    std::vector<SessionMetrics> nights;
    while (mysql_stmt_fetch(g.stmt) == 0) {
        if (r.colIsNull(1)) continue;  // total_seconds

        SessionMetrics m;
        m.sleep_day           = r.colText(0);
        m.total_events        = r.colInt(2);
        m.ahi                 = r.colDouble(12);
        m.obstructive_apneas  = r.colInt(3);
        m.central_apneas      = r.colInt(4);
        m.hypopneas           = r.colInt(5);
        m.reras               = r.colInt(6);
        m.clear_airway_apneas = r.colInt(7);

        m.avg_event_duration     = r.colOptDouble(8);
        m.max_event_duration     = r.colOptDouble(9);
        m.usage_hours            = r.colOptDouble(10);
        m.usage_percent          = r.colOptDouble(11);
        m.avg_leak_rate          = r.colOptDouble(13);
        m.max_leak_rate          = r.colOptDouble(14);
        m.avg_respiratory_rate   = r.colOptDouble(15);
        m.avg_tidal_volume       = r.colOptDouble(16);
        m.avg_minute_ventilation = r.colOptDouble(17);
        m.avg_flow_limitation    = r.colOptDouble(18);
        m.pressure_p95           = r.colOptDouble(19);
        m.avg_pressure           = r.colOptDouble(24);
        m.max_pressure           = r.colOptDouble(25);
        m.min_pressure           = r.colOptDouble(26);
        m.avg_mask_pressure      = r.colOptDouble(20);
        m.avg_epr_pressure       = r.colOptDouble(21);
        m.avg_snore              = r.colOptDouble(22);
        if (!r.colIsNull(23) && r.colDouble(23) > 0)
            m.avg_target_ventilation = r.colDouble(23);
        m.leak_p50               = r.colOptDouble(27);
        m.leak_p95               = r.colOptDouble(28);
        m.therapy_mode           = r.colOptInt(29);

        nights.push_back(std::move(m));
    }

    std::cout << "MySQL: getMetricsForDateRange(" << days_back << " days) returned "
              << nights.size() << " nights" << std::endl;
    return nights;
}

// ---------------------------------------------------------------------------
// saveSummary
// ---------------------------------------------------------------------------

bool MySQLDatabase::saveSummary(
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
    if (!conn_) return false;

    const char* sql = R"(
        INSERT INTO cpap_summaries
            (device_id, period, range_start, range_end, nights_count,
             avg_ahi, avg_usage_hours, compliance_pct, summary_text)
        VALUES (?, ?, CAST(? AS DATE), CAST(? AS DATE), ?, ?, ?, ?, ?)
    )";

    MysqlStmtGuard g;
    g.stmt = mysql_stmt_init(conn_);
    mysql_stmt_prepare(g.stmt, sql, std::strlen(sql));

    ParamBinder p(9);
    p.bindText(0, device_id);
    p.bindText(1, period);
    p.bindText(2, range_start);
    p.bindText(3, range_end);
    p.bindInt(4, nights_count);
    p.bindDouble(5, avg_ahi);
    p.bindDouble(6, avg_usage_hours);
    p.bindDouble(7, compliance_pct);
    p.bindText(8, summary_text);

    mysql_stmt_bind_param(g.stmt, p.data());
    if (mysql_stmt_execute(g.stmt) != 0) {
        std::cerr << "MySQL: saveSummary error: " << mysql_stmt_error(g.stmt) << std::endl;
        return false;
    }

    std::cout << "MySQL: Saved " << period << " summary (" << range_start
              << " to " << range_end << ", " << nights_count << " nights)" << std::endl;
    return true;
}

// ---------------------------------------------------------------------------
// SDD-004: equipment profiles + supplies
//
// Conventions (shared by all three backends, see IDatabase.h):
//   replace_after_days == -1  <-> SQL NULL ("use the type default")
//   client_uuid == ""         <-> SQL NULL
//   started_using_at == ""    <-> SQL NULL; started_epoch is derived on read
// MySQL stores booleans as TINYINT(1) 0/1 and timestamps as DATETIME, which are
// read back through DATE_FORMAT so the ISO-ish strings match the other backends.
// ---------------------------------------------------------------------------

namespace {

// "YYYY-MM-DD", "YYYY-MM-DD HH:MM:SS" or "YYYY-MM-DDTHH:MM:SS[Z|+hh:mm]".
// Returns unix seconds (UTC), or 0 when unset/unparseable.
long long iso_to_epoch(const std::string& iso) {
    if (iso.size() < 10) return 0;

    std::tm tm{};
    tm.tm_year = std::atoi(iso.substr(0, 4).c_str()) - 1900;
    tm.tm_mon  = std::atoi(iso.substr(5, 2).c_str()) - 1;
    tm.tm_mday = std::atoi(iso.substr(8, 2).c_str());
    if (iso.size() >= 19) {
        tm.tm_hour = std::atoi(iso.substr(11, 2).c_str());
        tm.tm_min  = std::atoi(iso.substr(14, 2).c_str());
        tm.tm_sec  = std::atoi(iso.substr(17, 2).c_str());
    }
    if (tm.tm_mon < 0 || tm.tm_mon > 11 || tm.tm_mday < 1 || tm.tm_mday > 31) return 0;

    time_t t = timegm_utc(&tm);
    return t < 0 ? 0 : static_cast<long long>(t);
}

/// started_using_at is user-supplied and stored VARCHAR, so it may arrive
/// date-only ("2024-03-01"), space-separated, or already ISO-Z. Postgres
/// re-renders it as full ISO-Z on read; normalise here so the column round-trips
/// identically on every backend. "" stays "" (unset).
std::string normalize_started(const std::string& ts) {
    if (ts.empty()) return ts;
    if (ts.size() == 10) return ts + "T00:00:00Z";   // date-only
    if (ts.size() < 19) return ts;
    std::string out = ts.substr(0, 19);
    out[10] = 'T';
    out += 'Z';
    return out;
}

/// Equipment writes accept an ISO-8601 UTC override ("2026-07-19T11:00:00Z") that
/// must land in updated_at verbatim when mirroring a cloud row. A DATETIME column
/// will not take the 'T' separator or the trailing 'Z', so render it as
/// "YYYY-MM-DD HH:MM:SS". "" (no override) and anything that does not parse to
/// that shape return "", which makes the statement's NOW() fallback stamp the
/// current time instead of writing garbage into the column.
std::string eqTsOverride(const std::string& iso) {
    // Shared gate first, so every engine accepts and rejects exactly the same
    // strings; only the wire format differs. MySQL DATETIME will not take the
    // trailing "Z", so the canonical form is rewritten to "YYYY-MM-DD HH:MM:SS".
    const std::string canon = IDatabase::sanitizeUpdatedAtOverride(iso);
    if (canon.empty()) return {};
    std::string out = canon.substr(0, 19);
    out[10] = ' ';
    return out;
}

/// mysql_stmt_init + prepare with the file's usual error logging.
bool eqPrepare(MYSQL* conn, MysqlStmtGuard& g, const std::string& sql, const char* who) {
    g.stmt = mysql_stmt_init(conn);
    if (!g.stmt) {
        std::cerr << "MySQL: " << who << " stmt_init failed: " << mysql_error(conn) << std::endl;
        return false;
    }
    if (mysql_stmt_prepare(g.stmt, sql.c_str(), sql.size()) != 0) {
        std::cerr << "MySQL: " << who << " prepare error: " << mysql_stmt_error(g.stmt) << std::endl;
        return false;
    }
    return true;
}

/// SELECT EXISTS(...) with a single int parameter.
bool eqExistsById(MYSQL* conn, const char* sql, int id, const char* who) {
    MysqlStmtGuard g;
    if (!eqPrepare(conn, g, sql, who)) return false;

    ParamBinder p(1);
    p.bindInt(0, id);
    mysql_stmt_bind_param(g.stmt, p.data());
    if (mysql_stmt_execute(g.stmt) != 0) {
        std::cerr << "MySQL: " << who << " error: " << mysql_stmt_error(g.stmt) << std::endl;
        return false;
    }

    ResultBinder r(1);
    r.bindColInt(0);
    mysql_stmt_bind_result(g.stmt, r.data());
    mysql_stmt_store_result(g.stmt);

    bool exists = false;
    if (mysql_stmt_fetch(g.stmt) == 0) exists = r.colInt(0) != 0;
    return exists;
}

// MySQL reports 0 affected rows when an UPDATE writes identical values (and
// updated_at = NOW() has only second resolution), whereas SQLite counts matched
// rows. Treat "no change but the row is still there" as success so all three
// backends agree.
bool eqUpdateSucceeded(MYSQL* conn, MYSQL_STMT* stmt, const char* exists_sql,
                       int id, const char* who) {
    if (mysql_stmt_affected_rows(stmt) > 0) return true;
    return eqExistsById(conn, exists_sql, id, who);
}

// ISO-8601 UTC with a trailing Z, byte-identical to what SQLite and Postgres
// emit: the sync layer does last-write-wins by comparing these strings, so the
// shape has to match exactly. 'T' and 'Z' are literals (no % prefix).
const char* kEqTsFmt = "'%Y-%m-%dT%H:%i:%sZ'";

const std::string kTypeCols =
    "id, type_key, label, category, default_replace_after_days, is_system, active";

const std::string kProfileCols =
    std::string("id, client_uuid, name, active, deleted, ")
    + "DATE_FORMAT(created_at, " + kEqTsFmt + "), "
    + "DATE_FORMAT(updated_at, " + kEqTsFmt + ")";

const std::string kItemCols =
    std::string("id, profile_id, client_uuid, type_key, category, brand, model, variant, ")
    + "started_using_at, replace_after_days, notes, active, deleted, "
    + "DATE_FORMAT(created_at, " + kEqTsFmt + "), "
    + "DATE_FORMAT(updated_at, " + kEqTsFmt + ")";

// Column buffers are sized in BYTES, but the schema is utf8mb4 and the widths
// are CHARACTERS -- a multi-byte value would be silently truncated on fetch, so
// every text buffer below is 4 bytes per declared character.
constexpr size_t kUtf8mb4 = 4;

void bindTypeRow(ResultBinder& r) {
    r.bindColInt(0);                          // id
    r.bindColString(1, 64 * kUtf8mb4);        // type_key
    r.bindColStringN(2, 128 * kUtf8mb4);      // label
    r.bindColString(3, 32 * kUtf8mb4);        // category
    r.bindColInt(4);              // default_replace_after_days (nullable)
    r.bindColInt(5);              // is_system
    r.bindColInt(6);              // active
}

IDatabase::EquipmentType parseTypeRow(const ResultBinder& r) {
    IDatabase::EquipmentType t;
    t.id                         = r.colInt(0);
    t.type_key                   = r.colText(1);
    t.label                      = r.colText(2);
    t.category                   = r.colText(3);
    t.default_replace_after_days = r.colIsNull(4) ? -1 : r.colInt(4);
    t.is_system                  = r.colInt(5) != 0;
    t.active                     = r.colInt(6) != 0;
    return t;
}

void bindProfileRow(ResultBinder& r) {
    r.bindColInt(0);                          // id
    r.bindColString(1, 64 * kUtf8mb4);        // client_uuid
    r.bindColStringN(2, 128 * kUtf8mb4);      // name
    r.bindColInt(3);                          // active
    r.bindColInt(4);                          // deleted
    r.bindColString(5, 32);                   // created_at (ASCII ISO-8601)
    r.bindColString(6, 32);                   // updated_at (ASCII ISO-8601)
}

IDatabase::EquipmentProfile parseProfileRow(const ResultBinder& r) {
    IDatabase::EquipmentProfile p;
    p.id          = r.colInt(0);
    p.client_uuid = r.colText(1);
    p.name        = r.colText(2);
    p.active      = r.colInt(3) != 0;
    p.deleted     = r.colInt(4) != 0;
    p.created_at  = r.colText(5);
    p.updated_at  = r.colText(6);
    return p;
}

void bindItemRow(ResultBinder& r) {
    r.bindColInt(0);                          // id
    r.bindColInt(1);                          // profile_id
    r.bindColString(2, 64 * kUtf8mb4);        // client_uuid
    r.bindColString(3, 64 * kUtf8mb4);        // type_key
    r.bindColString(4, 32 * kUtf8mb4);        // category
    r.bindColStringN(5, 128 * kUtf8mb4);      // brand
    r.bindColStringN(6, 128 * kUtf8mb4);      // model
    r.bindColStringN(7, 128 * kUtf8mb4);      // variant
    r.bindColString(8, 32);                   // started_using_at (ASCII ISO-8601)
    r.bindColInt(9);                          // replace_after_days (nullable)
    r.bindColStringN(10, 1000 * kUtf8mb4);    // notes (VARCHAR(1000) utf8mb4)
    r.bindColInt(11);                         // active
    r.bindColInt(12);                         // deleted
    r.bindColString(13, 32);                  // created_at (ASCII ISO-8601)
    r.bindColString(14, 32);                  // updated_at (ASCII ISO-8601)
}

IDatabase::EquipmentItem parseItemRow(const ResultBinder& r) {
    IDatabase::EquipmentItem it;
    it.id                 = r.colInt(0);
    it.profile_id         = r.colInt(1);
    it.client_uuid        = r.colText(2);
    it.type_key           = r.colText(3);
    it.category           = r.colText(4);
    it.brand              = r.colText(5);
    it.model              = r.colText(6);
    it.variant            = r.colText(7);
    it.started_using_at   = normalize_started(r.colText(8));
    it.started_epoch      = iso_to_epoch(it.started_using_at);
    it.replace_after_days = r.colIsNull(9) ? -1 : r.colInt(9);
    it.notes              = r.colText(10);
    it.active             = r.colInt(11) != 0;
    it.deleted            = r.colInt(12) != 0;
    it.created_at         = r.colText(13);
    it.updated_at         = r.colText(14);
    return it;
}

} // namespace

// -- Types --------------------------------------------------------------------

std::vector<IDatabase::EquipmentType> MySQLDatabase::listEquipmentTypes() {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    std::vector<EquipmentType> out;
    if (!conn_) return out;

    const std::string sql = "SELECT " + kTypeCols +
        " FROM cpap_equipment_types WHERE active = 1"
        " ORDER BY is_system DESC, id";

    MysqlStmtGuard g;
    if (!eqPrepare(conn_, g, sql, "listEquipmentTypes")) return out;

    if (mysql_stmt_execute(g.stmt) != 0) {
        std::cerr << "MySQL: listEquipmentTypes error: " << mysql_stmt_error(g.stmt) << std::endl;
        return out;
    }

    ResultBinder r(7);
    bindTypeRow(r);
    mysql_stmt_bind_result(g.stmt, r.data());
    mysql_stmt_store_result(g.stmt);

    while (mysql_stmt_fetch(g.stmt) == 0) out.push_back(parseTypeRow(r));
    return out;
}

std::optional<IDatabase::EquipmentType>
MySQLDatabase::resolveEquipmentType(const std::string& type_key) {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    if (!conn_) return std::nullopt;

    // Deliberately NOT filtered on active: an item may still reference a type
    // the user has since retired, and SupplyStatus needs its default interval.
    const std::string sql = "SELECT " + kTypeCols +
        " FROM cpap_equipment_types WHERE type_key = ?";

    MysqlStmtGuard g;
    if (!eqPrepare(conn_, g, sql, "resolveEquipmentType")) return std::nullopt;

    ParamBinder p(1);
    p.bindText(0, type_key);
    mysql_stmt_bind_param(g.stmt, p.data());
    if (mysql_stmt_execute(g.stmt) != 0) {
        std::cerr << "MySQL: resolveEquipmentType error: " << mysql_stmt_error(g.stmt) << std::endl;
        return std::nullopt;
    }

    ResultBinder r(7);
    bindTypeRow(r);
    mysql_stmt_bind_result(g.stmt, r.data());
    mysql_stmt_store_result(g.stmt);

    if (mysql_stmt_fetch(g.stmt) == 0) return parseTypeRow(r);
    return std::nullopt;
}

int MySQLDatabase::addEquipmentType(const EquipmentType& t) {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    if (!conn_) return -1;

    const char* sql = R"(
        INSERT INTO cpap_equipment_types
            (type_key, label, category, default_replace_after_days, is_system, active)
        VALUES (?, ?, ?, ?, ?, ?)
    )";

    MysqlStmtGuard g;
    if (!eqPrepare(conn_, g, sql, "addEquipmentType")) return -1;

    ParamBinder p(6);
    p.bindText(0, t.type_key);
    p.bindText(1, t.label);
    p.bindText(2, t.category);
    p.bindDaysOrNull(3, t.default_replace_after_days);
    p.bindInt(4, t.is_system ? 1 : 0);
    p.bindInt(5, t.active ? 1 : 0);
    mysql_stmt_bind_param(g.stmt, p.data());

    if (mysql_stmt_execute(g.stmt) != 0) {
        // Duplicate type_key lands here (UNIQUE constraint)
        std::cerr << "MySQL: addEquipmentType error: " << mysql_stmt_error(g.stmt) << std::endl;
        return -1;
    }
    return static_cast<int>(mysql_stmt_insert_id(g.stmt));
}

bool MySQLDatabase::updateEquipmentType(int id, const EquipmentType& t) {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    if (!conn_) return false;

    // is_system is never reassigned by an update, and on a seeded row type_key
    // and category are pinned as well: renaming the 'machine' type would orphan
    // every item referencing it.
    const char* sql = R"(
        UPDATE cpap_equipment_types SET
            type_key = CASE WHEN is_system = 1 THEN type_key ELSE ? END,
            label    = ?,
            category = CASE WHEN is_system = 1 THEN category ELSE ? END,
            default_replace_after_days = ?, active = ?,
            updated_at = NOW()
        WHERE id = ?
    )";

    MysqlStmtGuard g;
    if (!eqPrepare(conn_, g, sql, "updateEquipmentType")) return false;

    ParamBinder p(6);
    p.bindText(0, t.type_key);
    p.bindText(1, t.label);
    p.bindText(2, t.category);
    p.bindDaysOrNull(3, t.default_replace_after_days);
    p.bindInt(4, t.active ? 1 : 0);
    p.bindInt(5, id);
    mysql_stmt_bind_param(g.stmt, p.data());

    if (mysql_stmt_execute(g.stmt) != 0) {
        std::cerr << "MySQL: updateEquipmentType error: " << mysql_stmt_error(g.stmt) << std::endl;
        return false;
    }
    return eqUpdateSucceeded(conn_, g.stmt,
        "SELECT EXISTS(SELECT 1 FROM cpap_equipment_types WHERE id = ?)",
        id, "updateEquipmentType");
}

bool MySQLDatabase::deleteEquipmentType(int id) {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    if (!conn_) return false;

    // Soft delete, and never a seeded system row.
    const char* sql = R"(
        UPDATE cpap_equipment_types
           SET active = 0, updated_at = NOW()
         WHERE id = ? AND is_system = 0
    )";

    MysqlStmtGuard g;
    if (!eqPrepare(conn_, g, sql, "deleteEquipmentType")) return false;

    ParamBinder p(1);
    p.bindInt(0, id);
    mysql_stmt_bind_param(g.stmt, p.data());

    if (mysql_stmt_execute(g.stmt) != 0) {
        std::cerr << "MySQL: deleteEquipmentType error: " << mysql_stmt_error(g.stmt) << std::endl;
        return false;
    }
    return eqUpdateSucceeded(conn_, g.stmt,
        "SELECT EXISTS(SELECT 1 FROM cpap_equipment_types WHERE id = ? AND is_system = 0)",
        id, "deleteEquipmentType");
}

// -- Profiles -----------------------------------------------------------------

std::vector<IDatabase::EquipmentProfile>
MySQLDatabase::listEquipmentProfiles(bool include_deleted) {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    std::vector<EquipmentProfile> out;
    if (!conn_) return out;

    const std::string sql = "SELECT " + kProfileCols +
        " FROM cpap_equipment_profiles WHERE (deleted = 0 OR ? = 1) ORDER BY id";

    MysqlStmtGuard g;
    if (!eqPrepare(conn_, g, sql, "listEquipmentProfiles")) return out;

    ParamBinder p(1);
    p.bindInt(0, include_deleted ? 1 : 0);
    mysql_stmt_bind_param(g.stmt, p.data());

    if (mysql_stmt_execute(g.stmt) != 0) {
        std::cerr << "MySQL: listEquipmentProfiles error: " << mysql_stmt_error(g.stmt) << std::endl;
        return out;
    }

    ResultBinder r(7);
    bindProfileRow(r);
    mysql_stmt_bind_result(g.stmt, r.data());
    mysql_stmt_store_result(g.stmt);

    while (mysql_stmt_fetch(g.stmt) == 0) out.push_back(parseProfileRow(r));
    return out;
}

std::optional<IDatabase::EquipmentProfile> MySQLDatabase::getEquipmentProfile(int id) {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    if (!conn_) return std::nullopt;

    const std::string sql = "SELECT " + kProfileCols +
        // A tombstoned profile must read as absent: this is the controller's
        // existence check, not a sync accessor.
        " FROM cpap_equipment_profiles WHERE id = ? AND deleted = 0";

    MysqlStmtGuard g;
    if (!eqPrepare(conn_, g, sql, "getEquipmentProfile")) return std::nullopt;

    ParamBinder p(1);
    p.bindInt(0, id);
    mysql_stmt_bind_param(g.stmt, p.data());

    if (mysql_stmt_execute(g.stmt) != 0) {
        std::cerr << "MySQL: getEquipmentProfile error: " << mysql_stmt_error(g.stmt) << std::endl;
        return std::nullopt;
    }

    ResultBinder r(7);
    bindProfileRow(r);
    mysql_stmt_bind_result(g.stmt, r.data());
    mysql_stmt_store_result(g.stmt);

    if (mysql_stmt_fetch(g.stmt) == 0) return parseProfileRow(r);
    return std::nullopt;
}

int MySQLDatabase::upsertEquipmentProfile(const EquipmentProfile& p,
                                          const std::string& updated_at_override) {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    if (!conn_) return -1;

    const std::string ts = eqTsOverride(updated_at_override);

    if (p.id > 0) {
        const char* sql = R"(
            UPDATE cpap_equipment_profiles SET
                client_uuid = ?, name = ?, active = ?, deleted = ?,
                updated_at = COALESCE(NULLIF(?, ''), NOW())
            WHERE id = ?
        )";

        MysqlStmtGuard g;
        if (!eqPrepare(conn_, g, sql, "upsertEquipmentProfile")) return -1;

        ParamBinder b(6);
        b.bindTextOrNull(0, p.client_uuid);
        b.bindText(1, p.name);
        b.bindInt(2, p.active ? 1 : 0);
        b.bindInt(3, p.deleted ? 1 : 0);
        b.bindText(4, ts);
        b.bindInt(5, p.id);
        mysql_stmt_bind_param(g.stmt, b.data());

        if (mysql_stmt_execute(g.stmt) != 0) {
            std::cerr << "MySQL: upsertEquipmentProfile error: "
                      << mysql_stmt_error(g.stmt) << std::endl;
            return -1;
        }
        return eqUpdateSucceeded(conn_, g.stmt,
            "SELECT EXISTS(SELECT 1 FROM cpap_equipment_profiles WHERE id = ?)",
            p.id, "upsertEquipmentProfile") ? p.id : -1;
    }

    const char* sql = R"(
        INSERT INTO cpap_equipment_profiles
            (client_uuid, name, active, deleted, updated_at)
        VALUES (?, ?, ?, ?, COALESCE(NULLIF(?, ''), NOW()))
    )";

    MysqlStmtGuard g;
    if (!eqPrepare(conn_, g, sql, "upsertEquipmentProfile")) return -1;

    ParamBinder b(5);
    b.bindTextOrNull(0, p.client_uuid);
    b.bindText(1, p.name);
    b.bindInt(2, p.active ? 1 : 0);
    b.bindInt(3, p.deleted ? 1 : 0);
    b.bindText(4, ts);
    mysql_stmt_bind_param(g.stmt, b.data());

    if (mysql_stmt_execute(g.stmt) != 0) {
        std::cerr << "MySQL: upsertEquipmentProfile error: "
                  << mysql_stmt_error(g.stmt) << std::endl;
        return -1;
    }
    return static_cast<int>(mysql_stmt_insert_id(g.stmt));
}

bool MySQLDatabase::tombstoneEquipmentProfile(int id,
                                              const std::string& updated_at_override) {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    if (!conn_) return false;

    const std::string ts = eqTsOverride(updated_at_override);

    // Soft cascade: the FK cascade only fires on a hard DELETE, so the items of a
    // tombstoned profile are tombstoned explicitly. Clearing active as well frees
    // the one-live-machine slot for the profile.
    const char* sql_profile = R"(
        UPDATE cpap_equipment_profiles
           SET deleted = 1, active = 0,
               updated_at = COALESCE(NULLIF(?, ''), NOW())
         WHERE id = ? AND deleted = 0
    )";
    const char* sql_items = R"(
        UPDATE cpap_equipment_items
           SET deleted = 1, active = 0,
               updated_at = COALESCE(NULLIF(?, ''), NOW())
         WHERE profile_id = ? AND deleted = 0
    )";

    MysqlStmtGuard g;
    if (!eqPrepare(conn_, g, sql_profile, "tombstoneEquipmentProfile")) return false;

    ParamBinder p(2);
    p.bindText(0, ts);
    p.bindInt(1, id);
    mysql_stmt_bind_param(g.stmt, p.data());

    if (mysql_stmt_execute(g.stmt) != 0) {
        std::cerr << "MySQL: tombstoneEquipmentProfile error: "
                  << mysql_stmt_error(g.stmt) << std::endl;
        return false;
    }
    // deleted flips 0 -> 1, so a matched row always reports as affected here.
    if (mysql_stmt_affected_rows(g.stmt) == 0) return false;

    MysqlStmtGuard gi;
    if (eqPrepare(conn_, gi, sql_items, "tombstoneEquipmentProfile items")) {
        ParamBinder pi(2);
        pi.bindText(0, ts);
        pi.bindInt(1, id);
        mysql_stmt_bind_param(gi.stmt, pi.data());
        if (mysql_stmt_execute(gi.stmt) != 0) {
            std::cerr << "MySQL: tombstoneEquipmentProfile items error: "
                      << mysql_stmt_error(gi.stmt) << std::endl;
        }
    }
    return true;
}

int MySQLDatabase::ensureDefaultEquipmentProfile() {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    if (!conn_) return -1;

    const char* sql = R"(
        SELECT id FROM cpap_equipment_profiles WHERE deleted = 0
        ORDER BY active DESC, id LIMIT 1
    )";

    MysqlStmtGuard g;
    if (!eqPrepare(conn_, g, sql, "ensureDefaultEquipmentProfile")) return -1;

    if (mysql_stmt_execute(g.stmt) != 0) {
        std::cerr << "MySQL: ensureDefaultEquipmentProfile error: "
                  << mysql_stmt_error(g.stmt) << std::endl;
        return -1;
    }

    ResultBinder r(1);
    r.bindColInt(0);
    mysql_stmt_bind_result(g.stmt, r.data());
    mysql_stmt_store_result(g.stmt);

    if (mysql_stmt_fetch(g.stmt) == 0) return r.colInt(0);

    EquipmentProfile p;
    p.name = "My CPAP";
    int id = upsertEquipmentProfile(p, "");  // local write: stamp now()
    if (id > 0) {
        std::cout << "MySQL: Created default equipment profile 'My CPAP' (id "
                  << id << ")" << std::endl;
    }
    return id;
}

// -- Items --------------------------------------------------------------------

std::vector<IDatabase::EquipmentItem> MySQLDatabase::listEquipmentItems(bool include_history) {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    std::vector<EquipmentItem> out;
    if (!conn_) return out;

    // Tombstones stay hidden either way; include_history adds retired rows.
    const std::string sql = "SELECT " + kItemCols +
        " FROM cpap_equipment_items"
        " WHERE deleted = 0 AND (active = 1 OR ? = 1)"
        " ORDER BY CASE WHEN category = 'machine' THEN 0 ELSE 1 END, id";

    MysqlStmtGuard g;
    if (!eqPrepare(conn_, g, sql, "listEquipmentItems")) return out;

    ParamBinder p(1);
    p.bindInt(0, include_history ? 1 : 0);
    mysql_stmt_bind_param(g.stmt, p.data());

    if (mysql_stmt_execute(g.stmt) != 0) {
        std::cerr << "MySQL: listEquipmentItems error: " << mysql_stmt_error(g.stmt) << std::endl;
        return out;
    }

    ResultBinder r(15);
    bindItemRow(r);
    mysql_stmt_bind_result(g.stmt, r.data());
    mysql_stmt_store_result(g.stmt);

    while (mysql_stmt_fetch(g.stmt) == 0) out.push_back(parseItemRow(r));
    return out;
}

std::optional<IDatabase::EquipmentItem> MySQLDatabase::getEquipmentItem(int id) {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    if (!conn_) return std::nullopt;

    const std::string sql = "SELECT " + kItemCols +
        // Tombstones read as absent (see getEquipmentProfile).
        " FROM cpap_equipment_items WHERE id = ? AND deleted = 0";

    MysqlStmtGuard g;
    if (!eqPrepare(conn_, g, sql, "getEquipmentItem")) return std::nullopt;

    ParamBinder p(1);
    p.bindInt(0, id);
    mysql_stmt_bind_param(g.stmt, p.data());

    if (mysql_stmt_execute(g.stmt) != 0) {
        std::cerr << "MySQL: getEquipmentItem error: " << mysql_stmt_error(g.stmt) << std::endl;
        return std::nullopt;
    }

    ResultBinder r(15);
    bindItemRow(r);
    mysql_stmt_bind_result(g.stmt, r.data());
    mysql_stmt_store_result(g.stmt);

    if (mysql_stmt_fetch(g.stmt) == 0) return parseItemRow(r);
    return std::nullopt;
}

bool MySQLDatabase::profileHasMachine(int profile_id, int exclude_item_id) {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    if (!conn_) return false;

    // MySQL has no partial unique index, so this IS the enforcement of the
    // one-live-machine-per-profile rule on this backend.
    const char* sql = R"(
        SELECT EXISTS(
            SELECT 1 FROM cpap_equipment_items
             WHERE profile_id = ? AND category = 'machine'
               AND active = 1 AND deleted = 0
               AND id <> ?
        )
    )";

    MysqlStmtGuard g;
    if (!eqPrepare(conn_, g, sql, "profileHasMachine")) return false;

    ParamBinder p(2);
    p.bindInt(0, profile_id);
    p.bindInt(1, exclude_item_id);
    mysql_stmt_bind_param(g.stmt, p.data());

    if (mysql_stmt_execute(g.stmt) != 0) {
        std::cerr << "MySQL: profileHasMachine error: " << mysql_stmt_error(g.stmt) << std::endl;
        return false;
    }

    ResultBinder r(1);
    r.bindColInt(0);
    mysql_stmt_bind_result(g.stmt, r.data());
    mysql_stmt_store_result(g.stmt);

    bool has = false;
    if (mysql_stmt_fetch(g.stmt) == 0) has = r.colInt(0) != 0;
    return has;
}

int MySQLDatabase::upsertEquipmentItem(const EquipmentItem& item,
                                       const std::string& updated_at_override) {
    // An empty category is resolved from the type before anything is written --
    // storing '' would make category = 'machine' never match and silently disable
    // the one-machine-per-profile rule. Unknown types fall back to 'accessory'.
    std::string category = item.category;
    if (category.empty()) {
        if (auto t = resolveEquipmentType(item.type_key)) category = t->category;
        if (category.empty()) category = "accessory";
    }

    std::lock_guard<std::recursive_mutex> lock(mutex_);
    if (!conn_) return -1;

    const std::string ts = eqTsOverride(updated_at_override);

    // NOTE: unlike SQLite/Postgres there is no DB-level backstop for the
    // one-live-machine rule here -- MySQL cannot express a partial unique index.
    // Callers must consult profileHasMachine() before writing a machine row.
    if (item.id > 0) {
        const char* sql = R"(
            UPDATE cpap_equipment_items SET
                profile_id = ?, client_uuid = ?, type_key = ?, category = ?,
                brand = ?, model = ?, variant = ?, started_using_at = ?,
                replace_after_days = ?, notes = ?, active = ?, deleted = ?,
                updated_at = COALESCE(NULLIF(?, ''), NOW())
            WHERE id = ?
        )";

        MysqlStmtGuard g;
        if (!eqPrepare(conn_, g, sql, "upsertEquipmentItem")) return -1;

        ParamBinder p(14);
        p.bindInt(0, item.profile_id);
        p.bindTextOrNull(1, item.client_uuid);
        p.bindText(2, item.type_key);
        p.bindText(3, category);
        p.bindText(4, item.brand);
        p.bindText(5, item.model);
        p.bindText(6, item.variant);
        p.bindTextOrNull(7, item.started_using_at);
        p.bindDaysOrNull(8, item.replace_after_days);
        p.bindText(9, item.notes);
        p.bindInt(10, item.active ? 1 : 0);
        p.bindInt(11, item.deleted ? 1 : 0);
        p.bindText(12, ts);
        p.bindInt(13, item.id);
        mysql_stmt_bind_param(g.stmt, p.data());

        if (mysql_stmt_execute(g.stmt) != 0) {
            std::cerr << "MySQL: upsertEquipmentItem error: "
                      << mysql_stmt_error(g.stmt) << std::endl;
            return -1;
        }
        return eqUpdateSucceeded(conn_, g.stmt,
            "SELECT EXISTS(SELECT 1 FROM cpap_equipment_items WHERE id = ?)",
            item.id, "upsertEquipmentItem") ? item.id : -1;
    }

    const char* sql = R"(
        INSERT INTO cpap_equipment_items
            (profile_id, client_uuid, type_key, category, brand, model, variant,
             started_using_at, replace_after_days, notes, active, deleted, updated_at)
        VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?,
                COALESCE(NULLIF(?, ''), NOW()))
    )";

    MysqlStmtGuard g;
    if (!eqPrepare(conn_, g, sql, "upsertEquipmentItem")) return -1;

    ParamBinder p(13);
    p.bindInt(0, item.profile_id);
    p.bindTextOrNull(1, item.client_uuid);
    p.bindText(2, item.type_key);
    p.bindText(3, category);
    p.bindText(4, item.brand);
    p.bindText(5, item.model);
    p.bindText(6, item.variant);
    p.bindTextOrNull(7, item.started_using_at);
    p.bindDaysOrNull(8, item.replace_after_days);
    p.bindText(9, item.notes);
    p.bindInt(10, item.active ? 1 : 0);
    p.bindInt(11, item.deleted ? 1 : 0);
    p.bindText(12, ts);
    mysql_stmt_bind_param(g.stmt, p.data());

    if (mysql_stmt_execute(g.stmt) != 0) {
        std::cerr << "MySQL: upsertEquipmentItem error: "
                  << mysql_stmt_error(g.stmt) << std::endl;
        return -1;
    }
    return static_cast<int>(mysql_stmt_insert_id(g.stmt));
}

bool MySQLDatabase::tombstoneEquipmentItem(int id,
                                           const std::string& updated_at_override) {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    if (!conn_) return false;

    const char* sql = R"(
        UPDATE cpap_equipment_items
           SET deleted = 1, active = 0,
               updated_at = COALESCE(NULLIF(?, ''), NOW())
         WHERE id = ? AND deleted = 0
    )";

    MysqlStmtGuard g;
    if (!eqPrepare(conn_, g, sql, "tombstoneEquipmentItem")) return false;

    ParamBinder p(2);
    p.bindText(0, eqTsOverride(updated_at_override));
    p.bindInt(1, id);
    mysql_stmt_bind_param(g.stmt, p.data());

    if (mysql_stmt_execute(g.stmt) != 0) {
        std::cerr << "MySQL: tombstoneEquipmentItem error: "
                  << mysql_stmt_error(g.stmt) << std::endl;
        return false;
    }
    // deleted flips 0 -> 1, so a matched row always reports as affected here.
    return mysql_stmt_affected_rows(g.stmt) > 0;
}

// -- Generic query ------------------------------------------------------------

Json::Value MySQLDatabase::executeQuery(const std::string& sql,
                                        const std::vector<std::string>& params) {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    Json::Value arr(Json::arrayValue);
    if (!conn_) return arr;

    MYSQL_STMT* stmt = mysql_stmt_init(conn_);
    if (!stmt) return arr;

    // RAII cleanup
    struct StmtClose { MYSQL_STMT* s; ~StmtClose() { mysql_stmt_close(s); } } guard{stmt};

    if (mysql_stmt_prepare(stmt, sql.c_str(), sql.size()) != 0) {
        std::cerr << "MySQL::executeQuery prepare error: " << mysql_stmt_error(stmt) << std::endl;
        return arr;
    }

    // Bind input params (all as strings)
    std::vector<MYSQL_BIND> in_binds(params.size());
    std::vector<unsigned long> param_lengths(params.size());
    memset(in_binds.data(), 0, sizeof(MYSQL_BIND) * params.size());
    for (size_t i = 0; i < params.size(); ++i) {
        param_lengths[i] = params[i].size();
        in_binds[i].buffer_type = MYSQL_TYPE_STRING;
        in_binds[i].buffer = const_cast<char*>(params[i].c_str());
        in_binds[i].buffer_length = params[i].size();
        in_binds[i].length = &param_lengths[i];
    }
    if (!params.empty()) {
        mysql_stmt_bind_param(stmt, in_binds.data());
    }

    if (mysql_stmt_execute(stmt) != 0) {
        std::cerr << "MySQL::executeQuery execute error: " << mysql_stmt_error(stmt) << std::endl;
        return arr;
    }

    // Get result metadata
    MYSQL_RES* meta = mysql_stmt_result_metadata(stmt);
    if (!meta) return arr;  // no result set (not a SELECT)

    unsigned int num_cols = mysql_num_fields(meta);
    MYSQL_FIELD* fields = mysql_fetch_fields(meta);

    // Collect column names
    std::vector<std::string> col_names(num_cols);
    for (unsigned int c = 0; c < num_cols; ++c) {
        col_names[c] = fields[c].name;
    }

    // Bind output buffers (all as strings)
    std::vector<MYSQL_BIND> out_binds(num_cols);
    std::vector<std::vector<char>> buffers(num_cols, std::vector<char>(4096));
    std::vector<unsigned long> lengths(num_cols);
    std::vector<my_bool> nulls(num_cols);
    memset(out_binds.data(), 0, sizeof(MYSQL_BIND) * num_cols);
    for (unsigned int c = 0; c < num_cols; ++c) {
        out_binds[c].buffer_type = MYSQL_TYPE_STRING;
        out_binds[c].buffer = buffers[c].data();
        out_binds[c].buffer_length = buffers[c].size();
        out_binds[c].length = &lengths[c];
        out_binds[c].is_null = &nulls[c];
    }
    mysql_stmt_bind_result(stmt, out_binds.data());
    mysql_stmt_store_result(stmt);

    while (mysql_stmt_fetch(stmt) == 0) {
        Json::Value obj;
        for (unsigned int c = 0; c < num_cols; ++c) {
            if (nulls[c]) {
                obj[col_names[c]] = Json::nullValue;
            } else {
                obj[col_names[c]] = std::string(buffers[c].data(), lengths[c]);
            }
        }
        arr.append(obj);
    }

    mysql_free_result(meta);
    return arr;
}

} // namespace hms_cpap

#endif // WITH_MYSQL
