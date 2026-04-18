#pragma once

#include "database/IDatabase.h"
#include <sqlite3.h>
#include <mutex>
#include <string>

namespace hms_cpap {

/**
 * SQLiteDatabase - SQLite3 backend implementing IDatabase.
 *
 * Uses sqlite3 C API with prepared statements.
 * Thread-safe via recursive_mutex (same pattern as DatabaseService).
 */
class SQLiteDatabase : public IDatabase {
public:
    /**
     * @param db_path Filesystem path for the SQLite database file.
     */
    explicit SQLiteDatabase(const std::string& db_path);
    ~SQLiteDatabase() override;

    // Disable copy
    SQLiteDatabase(const SQLiteDatabase&) = delete;
    SQLiteDatabase& operator=(const SQLiteDatabase&) = delete;

    // -- IDatabase interface ---------------------------------------------------

    DbType dbType() const override { return DbType::SQLITE; }

    bool connect() override;
    void disconnect() override;
    bool isConnected() const override;

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

    bool isForceCompleted(const std::string& device_id,
                          const std::chrono::system_clock::time_point& session_start) override;

    bool setForceCompleted(const std::string& device_id,
                           const std::chrono::system_clock::time_point& session_start) override;

    std::map<std::string, int> getCheckpointFileSizes(
        const std::string& device_id,
        const std::chrono::system_clock::time_point& session_start) override;

    bool updateCheckpointFileSizes(
        const std::string& device_id,
        const std::chrono::system_clock::time_point& session_start,
        const std::map<std::string, int>& file_sizes) override;

    bool updateDeviceLastSeen(const std::string& device_id) override;

    bool saveSTRDailyRecords(const std::vector<STRDailyRecord>& records) override;

    std::optional<std::string> getLastSTRDate(const std::string& device_id) override;

    std::optional<SessionMetrics> getNightlyMetrics(
        const std::string& device_id,
        const std::chrono::system_clock::time_point& session_start) override;

    std::vector<SessionMetrics> getMetricsForDateRange(
        const std::string& device_id, int days_back) override;

    bool saveSummary(const std::string& device_id,
                     const std::string& period,
                     const std::string& range_start,
                     const std::string& range_end,
                     int nights_count,
                     double avg_ahi,
                     double avg_usage_hours,
                     double compliance_pct,
                     const std::string& summary_text) override;

    bool saveOximetrySession(const std::string& device_id,
                             const cpapdash::parser::OximetrySession& session) override;

    bool oximetrySessionExists(const std::string& device_id,
                               const std::string& filename) override;

    bool saveLiveOximetrySample(const std::string& device_id,
                                 const std::string& date,
                                 int spo2, int hr, int motion) override;

    void* rawConnection() override;

    // -- Generic query --------------------------------------------------------

    Json::Value executeQuery(const std::string& sql,
                             const std::vector<std::string>& params = {}) override;

private:
    std::string db_path_;
    sqlite3* db_ = nullptr;
    mutable std::recursive_mutex mutex_;

    /// Create all tables (called from connect())
    void createSchema();

    /// Execute SQL with no result rows
    bool exec(const std::string& sql);

    /// Format a time_point as "YYYY-MM-DD HH:MM:SS"
    static std::string fmtTimestamp(const std::chrono::system_clock::time_point& tp);

    /// Upsert device during saveSession
    void upsertDevice(const CPAPSession& session);

    /// Insert/upsert session record, return rowid
    int64_t insertSession(const CPAPSession& session);

    /// Insert breathing summaries (batch)
    void insertBreathingSummaries(int64_t session_id,
                                  const std::vector<BreathingSummary>& summaries);

    /// Insert events
    void insertEvents(int64_t session_id, const std::vector<CPAPEvent>& events);

    /// Insert vitals (batch)
    void insertVitals(int64_t session_id, const std::vector<CPAPVitals>& vitals);

    /// Insert/upsert session metrics
    void insertSessionMetrics(int64_t session_id, const SessionMetrics& metrics);

    /// Insert calculated metrics (OSCAR-style, batch)
    void insertCalculatedMetrics(int64_t session_id,
                                  const std::vector<BreathingSummary>& summaries);

    /// Parse a row from getNightlyMetrics / getMetricsForDateRange into SessionMetrics
    SessionMetrics parseMetricsRow(sqlite3_stmt* stmt, int col_offset = 0);
};

} // namespace hms_cpap
