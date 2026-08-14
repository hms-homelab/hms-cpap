#pragma once
#ifdef WITH_POSTGRESQL

#include "database/IDatabase.h"
#include <pqxx/pqxx>
#include <libpq-fe.h>
#include <memory>
#include <string>
#include <mutex>
#include <vector>
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

    std::optional<std::chrono::system_clock::time_point>
        getSessionStartForSleepDay(const std::string& device_id,
                                   const std::string& sleep_day,
                                   bool open_only = false) override;

    bool sessionExists(const std::string& device_id,
                      const std::chrono::system_clock::time_point& session_start) override;

    std::map<std::string, int> getCheckpointFileSizes(const std::string& device_id,
                                                       const std::chrono::system_clock::time_point& session_start) override;

    std::map<std::string, int> getCheckpointFilesByFolder(const std::string& device_id,
                                                          const std::string& date_folder) override;

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

    bool replaceSessionFiles(
        const std::string& device_id,
        const std::chrono::system_clock::time_point& session_start,
        const std::vector<SessionFileRef>& files) override;

    std::vector<SessionFileRef> getSessionFilesForDateFolder(
        const std::string& device_id,
        const std::string& date_folder) override;

    bool saveSTRDailyRecords(const std::vector<STRDailyRecord>& records) override;
    std::optional<std::string> getLastSTRDate(const std::string& device_id) override;
    bool aggregateDailySummaryFromSessions(const std::string& device_id) override;

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
                             const cpapdash::parser::OximetrySession& session) override;

    bool oximetrySessionExists(const std::string& device_id,
                               const std::string& filename) override;

    bool saveLiveOximetrySample(const std::string& device_id, const std::string& date,
                                 int spo2, int hr, int motion) override;

    OxiSummary getOximetrySummary(const std::string& device_id,
                                   const std::string& date,
                                   const std::string& next_day) override;
    OxiRangeSummary getOximetryRangeSummary(const std::string& device_id,
                                              const std::string& start,
                                              const std::string& end) override;
    std::vector<OxiNightlyPoint> getOximetryNightlySpo2(const std::string& device_id,
                                                         const std::string& start,
                                                         const std::string& end) override;

    // -- Equipment profiles + supplies (SDD-004) -------------------------------

    std::vector<EquipmentType> listEquipmentTypes() override;
    std::optional<EquipmentType> resolveEquipmentType(const std::string& type_key) override;
    int  addEquipmentType(const EquipmentType& t) override;
    bool updateEquipmentType(int id, const EquipmentType& t) override;
    bool deleteEquipmentType(int id) override;

    std::vector<EquipmentProfile> listEquipmentProfiles(bool include_deleted) override;
    std::optional<EquipmentProfile> getEquipmentProfile(int id) override;
    int  upsertEquipmentProfile(const EquipmentProfile& p,
                                const std::string& updated_at_override) override;
    bool tombstoneEquipmentProfile(int id,
                                   const std::string& updated_at_override) override;
    int  ensureDefaultEquipmentProfile() override;

    std::vector<EquipmentItem> listEquipmentItems(bool include_history) override;
    std::optional<EquipmentItem> getEquipmentItem(int id) override;
    bool profileHasMachine(int profile_id, int exclude_item_id) override;
    int  upsertEquipmentItem(const EquipmentItem& item,
                             const std::string& updated_at_override) override;
    bool tombstoneEquipmentItem(int id,
                                const std::string& updated_at_override) override;

    // -- SDD-007: cleaning schedules ------------------------------------------
    std::vector<CleaningTaskType> listCleaningTaskTypes() override;
    std::vector<CleaningTask> listCleaningTasks(int profile_id) override;
    std::optional<CleaningTask> getCleaningTask(int id) override;
    int  upsertCleaningTask(const CleaningTask& t,
                            const std::string& updated_at_override) override;
    bool tombstoneCleaningTask(int id,
                               const std::string& updated_at_override) override;
    bool markCleaningTaskDone(int id, const std::string& done_at_override) override;

    // Sync folder ledger (SDD-008)
    std::vector<FolderLedger> listSyncFolders() override;
    std::optional<FolderLedger> getSyncFolder(const std::string& date_folder) override;
    bool upsertSyncFolder(const FolderLedger& f) override;


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

    /**
     * Run an arbitrary read query and return the rows as a JSON array.
     *
     * This is the ONLY thing the whole web/API layer reads through
     * (QueryService), so a backend that does not override it serves an empty
     * dashboard and empty session lists while the collector happily writes to
     * the same database. That is exactly what happened between 4.6.3 and 4.8.1:
     * the factory started building this class for PostgreSQL instead of
     * PostgresDatabase, and the inherited IDatabase stub answered every query
     * with `[]` and logged nothing (issue #18).
     *
     * Runs on its own libpq connection under its own mutex rather than the pqxx
     * handle above: Drogon serves requests from several threads, pqxx is not
     * thread-safe, and a web read must never join a transaction the collector
     * is in the middle of.
     */
    Json::Value executeQuery(const std::string& sql,
                             const std::vector<std::string>& params = {}) override;

private:
    std::string connection_string_;
    std::unique_ptr<pqxx::connection> conn_;
    mutable std::recursive_mutex mutex_;

    PGconn* query_conn_ = nullptr;      // separate libpq connection for web reads
    mutable std::mutex query_mutex_;

    /// Open (or reopen) query_conn_. Caller must hold query_mutex_.
    bool ensureQueryConn();

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

    void insertDesaturations(pqxx::work& work, int session_id,
                             const std::vector<DesatEvent>& desats);

    void insertBreaths(pqxx::work& work, int session_id,
                       const std::vector<Breath>& breaths);

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
     * Insert calculated respiratory metrics (calculated)
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
