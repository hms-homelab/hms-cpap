#pragma once
#ifdef WITH_POSTGRESQL

#include "models/CPAPModels.h"
#include <pqxx/pqxx>
#include <memory>
#include <string>
#include <mutex>

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
class DatabaseService {
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

    /**
     * Connect to database
     *
     * @return true if connected successfully
     */
    bool connect();

    /**
     * Disconnect from database
     */
    void disconnect();

    /**
     * Check if connected
     *
     * @return true if connected
     */
    bool isConnected() const;

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
    bool saveSession(const CPAPSession& session);

    /**
     * Update device last_seen timestamp
     *
     * @param device_id Device identifier
     * @return true if updated successfully
     */
    bool updateDeviceLastSeen(const std::string& device_id);

    /**
     * Get the most recent session start timestamp for a device
     *
     * Used for delta collection: only download sessions newer than this.
     *
     * @param device_id Device identifier (e.g., "cpap_resmed_23243570851")
     * @return Optional timestamp of last session, nullopt if no sessions found
     */
    std::optional<std::chrono::system_clock::time_point>
        getLastSessionStart(const std::string& device_id);

    /**
     * Check if a session already exists in database
     *
     * Used to avoid re-downloading sessions we already have stored.
     *
     * @param device_id Device identifier
     * @param session_start Session start timestamp
     * @return true if session exists in DB
     */
    bool sessionExists(const std::string& device_id,
                      const std::chrono::system_clock::time_point& session_start);

    /**
     * Get last downloaded checkpoint file sizes for a session
     *
     * Returns individual checkpoint file sizes (BRP/PLD/SAD) as stored in DB.
     * Used to detect if specific files have grown or new files appeared.
     *
     * @param device_id Device identifier
     * @param session_start Session start timestamp
     * @return Map of filename -> size in KB (empty if not found)
     */
    std::map<std::string, int> getCheckpointFileSizes(const std::string& device_id,
                                                       const std::chrono::system_clock::time_point& session_start);

    /**
     * Update checkpoint file sizes for a session
     *
     * Stores individual checkpoint file sizes (BRP/PLD/SAD only) in JSONB.
     * CSL/EVE files are excluded as they're not used for session stop detection.
     *
     * @param device_id Device identifier
     * @param session_start Session start timestamp
     * @param file_sizes Map of checkpoint filename -> size in KB
     * @return true if updated successfully
     */
    bool updateCheckpointFileSizes(const std::string& device_id,
                                    const std::chrono::system_clock::time_point& session_start,
                                    const std::map<std::string, int>& file_sizes);

    /**
     * Mark session as COMPLETED
     *
     * Sets session_end to current timestamp when session stops growing.
     *
     * @param device_id Device identifier
     * @param session_start Session start timestamp
     * @return true if updated successfully
     */
    /**
     * Check if a session has been force-completed (skip in burst cycle)
     */
    bool isForceCompleted(const std::string& device_id,
                          const std::chrono::system_clock::time_point& session_start);

    /**
     * Set force_completed flag on a session (prevents re-parsing)
     */
    bool setForceCompleted(const std::string& device_id,
                           const std::chrono::system_clock::time_point& session_start);

    bool markSessionCompleted(const std::string& device_id,
                              const std::chrono::system_clock::time_point& session_start);

    /**
     * Clear session_end back to NULL when a completed session resumes
     * (checkpoint files start growing again after mask was put back on).
     * Returns true if session_end was cleared.
     */
    bool reopenSession(const std::string& device_id,
                       const std::chrono::system_clock::time_point& session_start);

    /**
     * Load session metrics from DB for MQTT republishing on session completion
     */
    std::optional<SessionMetrics> getSessionMetrics(const std::string& device_id,
                                                    const std::chrono::system_clock::time_point& session_start);

    /**
     * Delete sessions whose brp_file_path matches a date folder.
     *
     * Cascades to events, metrics, vitals, breathing_summary, calculated_metrics.
     *
     * @param device_id Device identifier
     * @param date_folder Date folder name (e.g., "20250818")
     * @return Number of sessions deleted (-1 on error)
     */
    int deleteSessionsByDateFolder(const std::string& device_id,
                                   const std::string& date_folder);

    /**
     * Save STR daily records to cpap_daily_summary table (upsert).
     *
     * @param records Vector of STRDailyRecord
     * @return true if saved successfully
     */
    bool saveSTRDailyRecords(const std::vector<STRDailyRecord>& records);

    /**
     * Get the last STR record date for a device.
     *
     * @param device_id Device identifier
     * @return "YYYY-MM-DD" string or nullopt
     */
    std::optional<std::string> getLastSTRDate(const std::string& device_id);

    /**
     * Load nightly aggregated metrics for the sleep day containing session_start.
     *
     * Sleep day = noon-to-noon window (DATE(session_start - 12h)).
     * All BRP sessions from the same night are summed for duration; event counts
     * use MAX (events come from shared EVE file, so they are identical across
     * all BRP-derived sessions in the same night).
     * AHI is recomputed as total_events / total_hours.
     */
    std::optional<SessionMetrics> getNightlyMetrics(const std::string& device_id,
                                                    const std::chrono::system_clock::time_point& session_start);

    /**
     * Get per-night metrics for a date range (for weekly/monthly summaries).
     *
     * Returns one SessionMetrics per sleep-night within [range_start, range_end].
     * Each entry also has usage_hours and the sleep_day date filled in.
     * Ordered oldest-first so the LLM can see the trend.
     *
     * @param device_id Device identifier
     * @param days_back Number of days to look back from today (7 = weekly, 30 = monthly)
     * @return Vector of per-night metrics, empty if no data
     */
    std::vector<SessionMetrics> getMetricsForDateRange(const std::string& device_id,
                                                        int days_back);

    /**
     * Save an AI-generated summary to the cpap_summaries table.
     *
     * @param device_id Device identifier
     * @param period "daily", "weekly", or "monthly"
     * @param range_start First sleep-night date covered
     * @param range_end Last sleep-night date covered
     * @param nights_count Number of nights in the range
     * @param avg_ahi Average AHI across the range
     * @param avg_usage_hours Average usage hours/night
     * @param compliance_pct Percentage of nights >= 4h
     * @param summary_text The LLM-generated summary text
     * @return true if saved successfully
     */
    bool saveSummary(const std::string& device_id,
                     const std::string& period,
                     const std::string& range_start,
                     const std::string& range_end,
                     int nights_count,
                     double avg_ahi,
                     double avg_usage_hours,
                     double compliance_pct,
                     const std::string& summary_text);

    /**
     * Execute arbitrary SQL (DDL or DML) in a single transaction.
     *
     * Used by FysetcSniffService for CREATE TABLE and INSERT statements.
     *
     * @param sql Raw SQL string
     * @return true if executed successfully
     */
    bool executeRaw(const std::string& sql);

    /**
     * Get raw pqxx connection (for QueryService read-only queries).
     * Caller must hold no other locks — acquires the recursive mutex.
     */
    pqxx::connection* rawConnection() {
        std::lock_guard<std::recursive_mutex> lock(mutex_);
        ensureConnection();
        return conn_.get();
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
