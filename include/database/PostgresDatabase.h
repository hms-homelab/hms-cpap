#pragma once
#ifdef WITH_POSTGRESQL

#include "database/IDatabase.h"
#include "database/DatabaseService.h"
#include <memory>
#include <string>

namespace hms_cpap {

/**
 * PostgresDatabase - IDatabase implementation that delegates to DatabaseService.
 *
 * Thin wrapper: every method forwards to the existing DatabaseService,
 * which already handles PostgreSQL via pqxx.  No SQL translation needed.
 */
class PostgresDatabase : public IDatabase {
public:
    explicit PostgresDatabase(const std::string& connection_string);

    DbType dbType() const override { return DbType::POSTGRESQL; }

    // -- Connection management ------------------------------------------------

    bool connect() override;
    void disconnect() override;
    bool isConnected() const override;

    // -- Session CRUD ---------------------------------------------------------

    bool saveSession(const CPAPSession& session) override;

    bool sessionExists(const std::string& device_id,
                       const std::chrono::system_clock::time_point& session_start) override;

    std::optional<std::chrono::system_clock::time_point>
        getLastSessionStart(const std::string& device_id) override;

    std::optional<SessionMetrics> getSessionMetrics(
        const std::string& device_id,
        const std::chrono::system_clock::time_point& session_start) override;

    bool markSessionCompleted(const std::string& device_id,
                              const std::chrono::system_clock::time_point& session_start) override;

    bool reopenSession(const std::string& device_id,
                       const std::chrono::system_clock::time_point& session_start) override;

    int deleteSessionsByDateFolder(const std::string& device_id,
                                   const std::string& date_folder) override;

    // -- Force-complete -------------------------------------------------------

    bool isForceCompleted(const std::string& device_id,
                          const std::chrono::system_clock::time_point& session_start) override;

    bool setForceCompleted(const std::string& device_id,
                           const std::chrono::system_clock::time_point& session_start) override;

    // -- Checkpoint file sizes ------------------------------------------------

    std::map<std::string, int> getCheckpointFileSizes(
        const std::string& device_id,
        const std::chrono::system_clock::time_point& session_start) override;

    bool updateCheckpointFileSizes(
        const std::string& device_id,
        const std::chrono::system_clock::time_point& session_start,
        const std::map<std::string, int>& file_sizes) override;

    // -- Device ---------------------------------------------------------------

    bool updateDeviceLastSeen(const std::string& device_id) override;

    // -- STR daily records ----------------------------------------------------

    bool saveSTRDailyRecords(const std::vector<STRDailyRecord>& records) override;

    std::optional<std::string> getLastSTRDate(const std::string& device_id) override;

    // -- Nightly / range metrics ----------------------------------------------

    std::optional<SessionMetrics> getNightlyMetrics(
        const std::string& device_id,
        const std::chrono::system_clock::time_point& session_start) override;

    std::vector<SessionMetrics> getMetricsForDateRange(
        const std::string& device_id, int days_back) override;

    // -- Summaries ------------------------------------------------------------

    bool saveSummary(const std::string& device_id,
                     const std::string& period,
                     const std::string& range_start,
                     const std::string& range_end,
                     int nights_count,
                     double avg_ahi,
                     double avg_usage_hours,
                     double compliance_pct,
                     const std::string& summary_text) override;

    // -- Raw connection -------------------------------------------------------

    void* rawConnection() override;

    // -- Generic query --------------------------------------------------------

    Json::Value executeQuery(const std::string& sql,
                             const std::vector<std::string>& params = {}) override;

private:
    std::unique_ptr<DatabaseService> db_;
};

} // namespace hms_cpap

#endif // WITH_POSTGRESQL
