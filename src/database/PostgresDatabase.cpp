#ifdef WITH_POSTGRESQL

#include "database/PostgresDatabase.h"

namespace hms_cpap {

PostgresDatabase::PostgresDatabase(const std::string& connection_string)
    : db_(std::make_unique<DatabaseService>(connection_string)) {}

// -- Connection management ----------------------------------------------------

bool PostgresDatabase::connect() { return db_->connect(); }
void PostgresDatabase::disconnect() { db_->disconnect(); }
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

} // namespace hms_cpap

#endif // WITH_POSTGRESQL
