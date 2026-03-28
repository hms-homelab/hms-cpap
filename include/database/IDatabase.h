#pragma once

#include "models/CPAPModels.h"
#include <chrono>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace hms_cpap {

/// Database backend type
enum class DbType { SQLITE, MYSQL, POSTGRESQL };

/**
 * IDatabase - Pure virtual interface for CPAP database operations.
 *
 * Extracted from DatabaseService's public API so that alternative backends
 * (SQLite, MySQL) can be swapped in without touching business logic.
 * All types are standard C++ -- no pqxx headers required.
 */
class IDatabase {
public:
    virtual ~IDatabase() = default;

    /// Which backend is behind this interface
    virtual DbType dbType() const = 0;

    // -- Connection management ------------------------------------------------

    virtual bool connect() = 0;
    virtual void disconnect() = 0;
    virtual bool isConnected() const = 0;

    // -- Session CRUD ---------------------------------------------------------

    virtual bool saveSession(const CPAPSession& session) = 0;

    virtual bool sessionExists(const std::string& device_id,
                               const std::chrono::system_clock::time_point& session_start) = 0;

    virtual std::optional<std::chrono::system_clock::time_point>
        getLastSessionStart(const std::string& device_id) = 0;

    virtual std::optional<SessionMetrics> getSessionMetrics(
        const std::string& device_id,
        const std::chrono::system_clock::time_point& session_start) = 0;

    virtual bool markSessionCompleted(const std::string& device_id,
                                      const std::chrono::system_clock::time_point& session_start) = 0;

    virtual bool reopenSession(const std::string& device_id,
                               const std::chrono::system_clock::time_point& session_start) = 0;

    virtual int deleteSessionsByDateFolder(const std::string& device_id,
                                           const std::string& date_folder) = 0;

    // -- Force-complete -------------------------------------------------------

    virtual bool isForceCompleted(const std::string& device_id,
                                  const std::chrono::system_clock::time_point& session_start) = 0;

    virtual bool setForceCompleted(const std::string& device_id,
                                   const std::chrono::system_clock::time_point& session_start) = 0;

    // -- Checkpoint file sizes ------------------------------------------------

    virtual std::map<std::string, int> getCheckpointFileSizes(
        const std::string& device_id,
        const std::chrono::system_clock::time_point& session_start) = 0;

    virtual bool updateCheckpointFileSizes(
        const std::string& device_id,
        const std::chrono::system_clock::time_point& session_start,
        const std::map<std::string, int>& file_sizes) = 0;

    // -- Device ---------------------------------------------------------------

    virtual bool updateDeviceLastSeen(const std::string& device_id) = 0;

    // -- STR daily records ----------------------------------------------------

    virtual bool saveSTRDailyRecords(const std::vector<STRDailyRecord>& records) = 0;

    virtual std::optional<std::string> getLastSTRDate(const std::string& device_id) = 0;

    // -- Nightly / range metrics ----------------------------------------------

    virtual std::optional<SessionMetrics> getNightlyMetrics(
        const std::string& device_id,
        const std::chrono::system_clock::time_point& session_start) = 0;

    virtual std::vector<SessionMetrics> getMetricsForDateRange(
        const std::string& device_id, int days_back) = 0;

    // -- Summaries ------------------------------------------------------------

    virtual bool saveSummary(const std::string& device_id,
                             const std::string& period,
                             const std::string& range_start,
                             const std::string& range_end,
                             int nights_count,
                             double avg_ahi,
                             double avg_usage_hours,
                             double compliance_pct,
                             const std::string& summary_text) = 0;

    // -- Raw connection -------------------------------------------------------

    /**
     * Access the underlying native connection handle.
     * Returns void*; callers cast to the backend-specific type
     * (e.g., pqxx::connection* for PostgreSQL).
     */
    virtual void* rawConnection() = 0;
};

} // namespace hms_cpap
