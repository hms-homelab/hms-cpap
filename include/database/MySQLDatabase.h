#pragma once

#ifdef WITH_MYSQL

#include "database/IDatabase.h"
// Header layout differs per client and per platform. pkg-config points -I
// straight at the directory holding mysql.h (Homebrew mysql-client gives
// include/mysql, mariadb-connector-c gives include/mariadb, Debian's
// libmariadb-dev gives /usr/include/mariadb), so the bare name is what
// resolves once MYSQLCLIENT_INCLUDE_DIRS is on the search path. The prefixed
// forms stay as fallbacks for builds that only have /usr/include.
#if __has_include(<mysql.h>)
#include <mysql.h>
#elif __has_include(<mysql/mysql.h>)
#include <mysql/mysql.h>
#else
#include <mariadb/mysql.h>
#endif
#include <mutex>
#include <string>
#include <iostream>

// Oracle dropped the my_bool typedef in MySQL 8.0 in favour of plain bool.
// MariaDB (what Linux CI and the Synology NAS both use) still defines it, so
// restore it only for an Oracle client that lacks it.
#if defined(MYSQL_VERSION_ID) && MYSQL_VERSION_ID >= 80000 && \
    !defined(MARIADB_BASE_VERSION) && !defined(MARIADB_VERSION_ID)
using my_bool = bool;
#endif

namespace hms_cpap {

/**
 * MySQLDatabase - MySQL/MariaDB backend implementing IDatabase.
 *
 * Uses libmysqlclient C API with prepared statements.
 * Thread-safe via recursive_mutex (same pattern as SQLiteDatabase).
 */
class MySQLDatabase : public IDatabase {
public:
    /**
     * @param host     MySQL server hostname or IP
     * @param port     MySQL server port (default 3306)
     * @param user     MySQL username
     * @param password MySQL password
     * @param database MySQL database name
     */
    MySQLDatabase(const std::string& host,
                  unsigned int port,
                  const std::string& user,
                  const std::string& password,
                  const std::string& database);
    ~MySQLDatabase() override;

    // Disable copy
    MySQLDatabase(const MySQLDatabase&) = delete;
    MySQLDatabase& operator=(const MySQLDatabase&) = delete;

    // -- IDatabase interface ---------------------------------------------------

    DbType dbType() const override { return DbType::MYSQL; }

    bool connect() override;
    void disconnect() override;
    bool isConnected() const override;

    bool saveSession(const CPAPSession& session) override;

    bool sessionExists(const std::string& device_id,
                       const std::chrono::system_clock::time_point& session_start) override;

    std::optional<std::chrono::system_clock::time_point>
        getLastSessionStart(const std::string& device_id) override;

    std::optional<std::chrono::system_clock::time_point>
        getSessionStartForSleepDay(const std::string& device_id,
                                   const std::string& sleep_day,
                                   bool open_only = false) override;

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

    std::map<std::string, int> getCheckpointFilesByFolder(
        const std::string& device_id,
        const std::string& date_folder) override;

    bool updateCheckpointFileSizes(
        const std::string& device_id,
        const std::chrono::system_clock::time_point& session_start,
        const std::map<std::string, int>& file_sizes) override;

    bool updateDeviceLastSeen(const std::string& device_id) override;

    bool saveSTRDailyRecords(const std::vector<STRDailyRecord>& records) override;

    std::optional<std::string> getLastSTRDate(const std::string& device_id) override;

    bool aggregateDailySummaryFromSessions(const std::string& device_id) override;

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

    OxiSummary getOximetrySummary(const std::string& device_id,
                                   const std::string& date_yyyymmdd,
                                   const std::string& next_day_yyyymmdd) override;
    OxiRangeSummary getOximetryRangeSummary(const std::string& device_id,
                                              const std::string& start_yyyymmdd,
                                              const std::string& end_yyyymmdd) override;
    std::vector<OxiNightlyPoint> getOximetryNightlySpo2(const std::string& device_id,
                                                         const std::string& start_yyyymmdd,
                                                         const std::string& end_yyyymmdd) override;

    // -- Equipment profiles + supplies (SDD-004) ------------------------------
    // Conventions (see IDatabase.h): replace_after_days == -1 <-> SQL NULL,
    // client_uuid == "" <-> SQL NULL, started_using_at == "" <-> SQL NULL.
    // MySQL has NO partial indexes, so the one-live-machine-per-profile rule is
    // enforced by profileHasMachine() in the controller, not by the schema.

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

    void* rawConnection() override;

    // -- Generic query --------------------------------------------------------

    Json::Value executeQuery(const std::string& sql,
                             const std::vector<std::string>& params = {}) override;

    // -- Schema introspection + migration -------------------------------------
    //
    // Public because MySQL, unlike the other two engines, has no declarative way
    // to reach these: it has neither ADD COLUMN IF NOT EXISTS (Oracle) nor a
    // migration story of its own, so the drift guard in
    // tests/database/test_MySQLMigration.cpp has to drive them directly.

    bool tableExists(const std::string& table);
    bool columnExists(const std::string& table, const std::string& column);

    /// Returns 1 if a column was added, 0 if it already existed or the table is
    /// absent (in which case CREATE TABLE will build it with the column present).
    int addColumnIfMissing(const std::string& table, const std::string& column,
                           const std::string& definition);

    /// Bring an existing database forward to the shape createSchema() declares.
    /// CREATE TABLE IF NOT EXISTS is a no-op on a table that already exists, so
    /// without this an install made by an older build never gains new columns.
    void migrateSchema();

private:
    std::string host_;
    unsigned int port_;
    std::string user_;
    std::string password_;
    std::string database_;
    MYSQL* conn_ = nullptr;
    mutable std::recursive_mutex mutex_;

    /// Create all tables (called from connect())
    void createSchema();

    /// Execute SQL with no result rows
    bool exec(const std::string& sql);

    /// Format a time_point as "YYYY-MM-DD HH:MM:SS"
    static std::string fmtTimestamp(const std::chrono::system_clock::time_point& tp);

    /// Oximetry-only renderer. The oximetry parsers read the ring's printed time
    /// as if it were UTC, so only a gmtime render gives that wall clock back;
    /// fmtTimestamp would shift it by the host's UTC offset. See the definition.
    static std::string fmtOximetryTimestamp(
        const std::chrono::system_clock::time_point& tp);

    /// Upsert device during saveSession
    void upsertDevice(const CPAPSession& session);

    /// Insert/upsert session record, return row id
    int64_t insertSession(const CPAPSession& session);

    /// Insert breathing summaries (batch)
    void insertBreathingSummaries(int64_t session_id,
                                  const std::vector<BreathingSummary>& summaries);

    /// Insert events
    void insertEvents(int64_t session_id, const std::vector<CPAPEvent>& events);

    /// Insert SpO2 desaturations (as cpap_events of type "Desaturation")
    void insertDesaturations(int64_t session_id, const std::vector<DesatEvent>& desats);

    /// Insert breath-by-breath detail (batch)
    void insertBreaths(int64_t session_id, const std::vector<Breath>& breaths);

    /// Insert vitals (batch)
    void insertVitals(int64_t session_id, const std::vector<CPAPVitals>& vitals);

    /// Insert/upsert session metrics
    void insertSessionMetrics(int64_t session_id, const SessionMetrics& metrics);

    /// Insert calculated respiratory metrics (batch)
    void insertCalculatedMetrics(int64_t session_id,
                                  const std::vector<BreathingSummary>& summaries);
};

} // namespace hms_cpap

#endif // WITH_MYSQL
