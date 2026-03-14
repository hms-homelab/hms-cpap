#include "database/DatabaseService.h"
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

    std::string end_str = "NULL";
    if (session.session_end.has_value()) {
        auto end_time_t = std::chrono::system_clock::to_time_t(session.session_end.value());
        std::tm* end_tm = std::localtime(&end_time_t);
        std::ostringstream end_oss;
        end_oss << std::put_time(end_tm, "%Y-%m-%d %H:%M:%S");
        end_str = "'" + end_oss.str() + "'";
    }

    std::string query = R"(
        INSERT INTO cpap_sessions (device_id, session_start, session_end, duration_seconds, data_records,
                                   brp_file_path, eve_file_path, sad_file_path, pld_file_path, csl_file_path)
        VALUES ($1, $2, )" + end_str + R"(, $3, $4, $5, $6, $7, $8, $9)
        ON CONFLICT (device_id, session_start) DO UPDATE
        SET session_end = EXCLUDED.session_end,
            duration_seconds = EXCLUDED.duration_seconds,
            data_records = EXCLUDED.data_records,
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
         avg_spo2, min_spo2, avg_heart_rate, max_heart_rate, min_heart_rate)
        VALUES ($1, $2, $3, $4, $5, $6, $7, $8, $9, $10, $11, $12, $13)
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
            min_heart_rate = EXCLUDED.min_heart_rate
    )";

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
        metrics.min_heart_rate.value_or(0)
    );
}

void DatabaseService::insertCalculatedMetrics(pqxx::work& work, int session_id,
                                                const std::vector<BreathingSummary>& summaries) {
    if (summaries.empty()) return;

    // Only insert summaries that have calculated metrics
    std::vector<const BreathingSummary*> with_metrics;
    for (const auto& s : summaries) {
        if (s.respiratory_rate.has_value() || s.tidal_volume.has_value() ||
            s.minute_ventilation.has_value() || s.flow_limitation.has_value()) {
            with_metrics.push_back(&s);
        }
    }

    if (with_metrics.empty()) return;

    // Batch insert for performance
    std::ostringstream query;
    query << "INSERT INTO cpap_calculated_metrics "
          << "(session_id, timestamp, respiratory_rate, tidal_volume, minute_ventilation, "
          << "inspiratory_time, expiratory_time, ie_ratio, flow_limitation, leak_rate, "
          << "flow_p95, flow_p90, pressure_p95, pressure_p90) VALUES ";

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
        query << "NULL";

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

            // 3a. Insert calculated metrics (OSCAR-style)
            insertCalculatedMetrics(txn, session_id, session.breathing_summary);
            std::cout << "   Calculated metrics saved (RR, TV, MV, Ti/Te, I:E, FL, P95)" << std::endl;
        }

        // 4. Insert events
        if (!session.events.empty()) {
            insertEvents(txn, session_id, session.events);
            std::cout << "   Events: " << session.events.size() << std::endl;
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
                     THEN round((MAX(sm.total_events) * 3600.0 / SUM(s.duration_seconds))::numeric, 4)
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
                AVG(b.avg_press) AS avg_pressure,
                MAX(b.max_press) AS max_pressure,
                MIN(b.min_press) AS min_pressure
            FROM cpap_sessions s
            JOIN cpap_session_metrics sm ON sm.session_id = s.id
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

        return m;

    } catch (const std::exception& e) {
        std::cerr << "DB: getNightlyMetrics error: " << e.what() << std::endl;
        return std::nullopt;
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

} // namespace hms_cpap
