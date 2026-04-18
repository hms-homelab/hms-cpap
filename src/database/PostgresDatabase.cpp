#ifdef WITH_POSTGRESQL

#include "database/PostgresDatabase.h"
#include <iostream>

namespace hms_cpap {

PostgresDatabase::PostgresDatabase(const std::string& connection_string)
    : db_(std::make_unique<DatabaseService>(connection_string)), conn_str_(connection_string) {}

// -- Connection management ----------------------------------------------------

bool PostgresDatabase::connect() { return db_->connect(); }
void PostgresDatabase::disconnect() {
    if (query_conn_) { PQfinish(query_conn_); query_conn_ = nullptr; }
    db_->disconnect();
}
bool PostgresDatabase::isConnected() const { return db_->isConnected(); }

// -- Session CRUD -------------------------------------------------------------

bool PostgresDatabase::saveSession(const CPAPSession& session) {
    return db_->saveSession(session);
}

bool PostgresDatabase::sessionExists(const std::string& device_id,
                                     const std::chrono::system_clock::time_point& session_start) {
    return db_->sessionExists(device_id, session_start);
}

std::optional<std::chrono::system_clock::time_point>
PostgresDatabase::getLastSessionStart(const std::string& device_id) {
    return db_->getLastSessionStart(device_id);
}

std::optional<SessionMetrics> PostgresDatabase::getSessionMetrics(
    const std::string& device_id,
    const std::chrono::system_clock::time_point& session_start) {
    return db_->getSessionMetrics(device_id, session_start);
}

bool PostgresDatabase::markSessionCompleted(const std::string& device_id,
                                            const std::chrono::system_clock::time_point& session_start) {
    return db_->markSessionCompleted(device_id, session_start);
}

bool PostgresDatabase::reopenSession(const std::string& device_id,
                                     const std::chrono::system_clock::time_point& session_start) {
    return db_->reopenSession(device_id, session_start);
}

int PostgresDatabase::deleteSessionsByDateFolder(const std::string& device_id,
                                                 const std::string& date_folder) {
    return db_->deleteSessionsByDateFolder(device_id, date_folder);
}

// -- Force-complete -----------------------------------------------------------

bool PostgresDatabase::isForceCompleted(const std::string& device_id,
                                        const std::chrono::system_clock::time_point& session_start) {
    return db_->isForceCompleted(device_id, session_start);
}

bool PostgresDatabase::setForceCompleted(const std::string& device_id,
                                         const std::chrono::system_clock::time_point& session_start) {
    return db_->setForceCompleted(device_id, session_start);
}

// -- Checkpoint file sizes ----------------------------------------------------

std::map<std::string, int> PostgresDatabase::getCheckpointFileSizes(
    const std::string& device_id,
    const std::chrono::system_clock::time_point& session_start) {
    return db_->getCheckpointFileSizes(device_id, session_start);
}

bool PostgresDatabase::updateCheckpointFileSizes(
    const std::string& device_id,
    const std::chrono::system_clock::time_point& session_start,
    const std::map<std::string, int>& file_sizes) {
    return db_->updateCheckpointFileSizes(device_id, session_start, file_sizes);
}

// -- Device -------------------------------------------------------------------

bool PostgresDatabase::updateDeviceLastSeen(const std::string& device_id) {
    return db_->updateDeviceLastSeen(device_id);
}

// -- STR daily records --------------------------------------------------------

bool PostgresDatabase::saveSTRDailyRecords(const std::vector<STRDailyRecord>& records) {
    return db_->saveSTRDailyRecords(records);
}

std::optional<std::string> PostgresDatabase::getLastSTRDate(const std::string& device_id) {
    return db_->getLastSTRDate(device_id);
}

// -- Nightly / range metrics --------------------------------------------------

std::optional<SessionMetrics> PostgresDatabase::getNightlyMetrics(
    const std::string& device_id,
    const std::chrono::system_clock::time_point& session_start) {
    return db_->getNightlyMetrics(device_id, session_start);
}

std::vector<SessionMetrics> PostgresDatabase::getMetricsForDateRange(
    const std::string& device_id, int days_back) {
    return db_->getMetricsForDateRange(device_id, days_back);
}

// -- Summaries ----------------------------------------------------------------

bool PostgresDatabase::saveSummary(const std::string& device_id,
                                   const std::string& period,
                                   const std::string& range_start,
                                   const std::string& range_end,
                                   int nights_count,
                                   double avg_ahi,
                                   double avg_usage_hours,
                                   double compliance_pct,
                                   const std::string& summary_text) {
    return db_->saveSummary(device_id, period, range_start, range_end,
                            nights_count, avg_ahi, avg_usage_hours,
                            compliance_pct, summary_text);
}

// -- Raw connection -----------------------------------------------------------

void* PostgresDatabase::rawConnection() {
    return static_cast<void*>(db_->rawConnection());
}

// -- Generic query (pure libpq — avoids pqxx cross-compile SEGV) -------------

bool PostgresDatabase::ensureQueryConn() {
    if (query_conn_ && PQstatus(query_conn_) == CONNECTION_OK) return true;
    if (query_conn_) PQfinish(query_conn_);
    query_conn_ = PQconnectdb(conn_str_.c_str());
    if (PQstatus(query_conn_) != CONNECTION_OK) {
        std::cerr << "executeQuery: PQ connect failed: " << PQerrorMessage(query_conn_) << std::endl;
        PQfinish(query_conn_);
        query_conn_ = nullptr;
        return false;
    }
    return true;
}

Json::Value PostgresDatabase::executeQuery(const std::string& sql,
                                           const std::vector<std::string>& params) {
    Json::Value arr(Json::arrayValue);
    std::lock_guard<std::mutex> lock(query_mutex_);
    if (!ensureQueryConn()) return arr;

    // Build C arrays for PQexecParams
    std::vector<const char*> pvals;
    pvals.reserve(params.size());
    for (auto& p : params) pvals.push_back(p.c_str());

    // Debug: log query with params substituted
    if (sql.find("oximetry") != std::string::npos) {
        std::string debug_sql = sql;
        for (size_t i = 0; i < params.size(); i++) {
            std::string placeholder = "$" + std::to_string(i + 1);
            auto pos = debug_sql.find(placeholder);
            if (pos != std::string::npos)
                debug_sql.replace(pos, placeholder.size(), "'" + params[i] + "'");
        }
        std::cout << "PG executeQuery [oximetry]: " << debug_sql << std::endl;
    }

    PGresult* res = PQexecParams(query_conn_, sql.c_str(),
                                 static_cast<int>(params.size()),
                                 nullptr, pvals.data(), nullptr, nullptr, 0);

    // Debug: log result for oximetry queries
    if (sql.find("oximetry") != std::string::npos) {
        std::cout << "PG result: status=" << PQresStatus(PQresultStatus(res))
                  << " rows=" << PQntuples(res) << std::endl;
    }

    if (!res || (PQresultStatus(res) != PGRES_TUPLES_OK &&
                 PQresultStatus(res) != PGRES_COMMAND_OK)) {
        std::cerr << "executeQuery error: " << PQerrorMessage(query_conn_) << std::endl;
        if (res) PQclear(res);
        return arr;
    }

    int nrows = PQntuples(res);
    int ncols = PQnfields(res);
    for (int r = 0; r < nrows; ++r) {
        Json::Value obj;
        for (int c = 0; c < ncols; ++c) {
            const char* col = PQfname(res, c);
            if (PQgetisnull(res, r, c))
                obj[col] = Json::nullValue;
            else
                obj[col] = PQgetvalue(res, r, c);
        }
        arr.append(obj);
    }
    PQclear(res);
    return arr;
}

} // namespace hms_cpap

#endif // WITH_POSTGRESQL
