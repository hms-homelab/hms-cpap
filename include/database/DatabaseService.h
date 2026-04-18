#pragma once
#ifdef WITH_POSTGRESQL

#include "database/IDatabase.h"
#include <pqxx/pqxx>
#include <memory>
#include <string>
#include <mutex>
#include <iostream>

namespace hms_cpap {

/**
 * DatabaseService - PostgreSQL database client for CPAP data
 *
 * Stores CPAP session data in cpap_monitoring database:
 * - Session metadata
 * - Breathing summaries
 * - Respiratory events
 * - Vital signs (SpO2, HR)
 * - Aggregated metrics
 *
 * Thread-safe with connection pooling and automatic reconnection.
 */
class DatabaseService : public IDatabase {
public:
    /**
     * Constructor
     *
     * @param connection_string PostgreSQL connection string
     */
    explicit DatabaseService(const std::string& connection_string);

    /**
     * Destructor
     */
    ~DatabaseService();

    // Disable copy
    DatabaseService(const DatabaseService&) = delete;
    DatabaseService& operator=(const DatabaseService&) = delete;

    DbType dbType() const override { return DbType::POSTGRESQL; }

    bool connect() override;
    void disconnect() override;
    bool isConnected() const override;

    /**
     * Save complete CPAP session to database
     *
     * Saves all session data in a single transaction:
     * - Device metadata (upsert)
     * - Session record
     * - Breathing summaries
     * - Events
     * - Vitals
     * - Session metrics
     *
     * @param session CPAPSession object
     * @return true if saved successfully
     */
    // -- IDatabase overrides ----------------------------------------------------

    bool saveSession(const CPAPSession& session) override;
    bool updateDeviceLastSeen(const std::string& device_id) override;

    std::optional<std::chrono::system_clock::time_point>
        getLastSessionStart(const std::string& device_id) override;

    bool sessionExists(const std::string& device_id,
                      const std::chrono::system_clock::time_point& session_start) override;

    std::map<std::string, int> getCheckpointFileSizes(const std::string& device_id,
                                                       const std::chrono::system_clock::time_point& session_start) override;

    bool updateCheckpointFileSizes(const std::string& device_id,
                                    const std::chrono::system_clock::time_point& session_start,
                                    const std::map<std::string, int>& file_sizes) override;

    bool isForceCompleted(const std::string& device_id,
                          const std::chrono::system_clock::time_point& session_start) override;

    bool setForceCompleted(const std::string& device_id,
                           const std::chrono::system_clock::time_point& session_start) override;

    bool markSessionCompleted(const std::string& device_id,
                              const std::chrono::system_clock::time_point& session_start) override;

    bool reopenSession(const std::string& device_id,
                       const std::chrono::system_clock::time_point& session_start) override;

    std::optional<SessionMetrics> getSessionMetrics(const std::string& device_id,
                                                    const std::chrono::system_clock::time_point& session_start) override;

    int deleteSessionsByDateFolder(const std::string& device_id,
                                   const std::string& date_folder) override;

    bool saveSTRDailyRecords(const std::vector<STRDailyRecord>& records) override;
    std::optional<std::string> getLastSTRDate(const std::string& device_id) override;

    std::optional<SessionMetrics> getNightlyMetrics(const std::string& device_id,
                                                    const std::chrono::system_clock::time_point& session_start) override;

    std::vector<SessionMetrics> getMetricsForDateRange(const std::string& device_id,
                                                        int days_back) override;

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
                             const cpapdash::parser::OximetrySession& session) override {
        (void)device_id; (void)session;
        std::cerr << "PostgreSQL: saveOximetrySession not yet implemented" << std::endl;
        return false;
    }

    bool oximetrySessionExists(const std::string& device_id,
                               const std::string& filename) override {
        (void)device_id; (void)filename;
        return false;
    }

    bool saveLiveOximetrySample(const std::string&, const std::string&,
                                 int, int, int) override { return false; }

    void* rawConnection() override {
        std::lock_guard<std::recursive_mutex> lock(mutex_);
        ensureConnection();
        return conn_.get();
    }

    // -- PostgreSQL-specific (not in IDatabase) --------------------------------

    bool executeRaw(const std::string& sql);

    /// Typed accessor for PostgreSQL callers that need the real pqxx::connection*
    pqxx::connection* pgConnection() {
        return static_cast<pqxx::connection*>(rawConnection());
    }

private:
    std::string connection_string_;
    std::unique_ptr<pqxx::connection> conn_;
    mutable std::recursive_mutex mutex_;

    /**
     * Ensure connection is alive (reconnect if needed)
     *
     * @return true if connection is ready
     */
    bool ensureConnection();

    /**
     * Upsert device record
     *
     * @param work Transaction
     * @param session Session data
     */
    void upsertDevice(pqxx::work& work, const CPAPSession& session);

    /**
     * Insert session record
     *
     * @param work Transaction
     * @param session Session data
     * @return session_id
     */
    int insertSession(pqxx::work& work, const CPAPSession& session);

    /**
     * Insert breathing summaries
     *
     * @param work Transaction
     * @param session_id Session ID
     * @param summaries Breathing summaries
     */
    void insertBreathingSummaries(pqxx::work& work, int session_id,
                                   const std::vector<BreathingSummary>& summaries);

    /**
     * Insert events
     *
     * @param work Transaction
     * @param session_id Session ID
     * @param events Events
     */
    void insertEvents(pqxx::work& work, int session_id,
                      const std::vector<CPAPEvent>& events);

    /**
     * Insert vitals
     *
     * @param work Transaction
     * @param session_id Session ID
     * @param vitals Vitals
     */
    void insertVitals(pqxx::work& work, int session_id,
                      const std::vector<CPAPVitals>& vitals);

    /**
     * Insert session metrics
     *
     * @param work Transaction
     * @param session_id Session ID
     * @param metrics Session metrics
     */
    void insertSessionMetrics(pqxx::work& work, int session_id,
                              const SessionMetrics& metrics);

    /**
     * Insert calculated respiratory metrics (OSCAR-style)
     *
     * @param work Transaction
     * @param session_id Session ID
     * @param summaries Breathing summaries with calculated metrics
     */
    void insertCalculatedMetrics(pqxx::work& work, int session_id,
                                  const std::vector<BreathingSummary>& summaries);
};

} // namespace hms_cpap

#endif // WITH_POSTGRESQL
