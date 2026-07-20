#pragma once

#include "parsers/CpapdashBridge.h"
#include <json/json.h>
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

    virtual std::optional<std::chrono::system_clock::time_point>
        getSessionStartForSleepDay(const std::string& device_id,
                                   const std::string& sleep_day,
                                   bool open_only = false) = 0;

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

    // -- Checkpoint files by date folder (flat map across sessions in folder) -

    virtual std::map<std::string, int> getCheckpointFilesByFolder(
        const std::string& device_id,
        const std::string& date_folder) = 0;

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

    // -- Oximetry (O2 Ring) ---------------------------------------------------

    /**
     * Save a parsed oximetry session (header + samples) to the database.
     * @param device_id  e.g. "o2ring"
     * @param session    Parsed OximetrySession from VLDParser
     */
    virtual bool saveOximetrySession(const std::string& device_id,
                                     const cpapdash::parser::OximetrySession& session) = 0;

    /**
     * Check if an oximetry session (by filename) already exists.
     */
    virtual bool oximetrySessionExists(const std::string& device_id,
                                       const std::string& filename) = 0;

    /**
     * Append a live oximetry sample (from burst poll).
     * Creates a session for today if none exists. Source='live'.
     * Overwritten when full VLD file arrives.
     */
    virtual bool saveLiveOximetrySample(const std::string& device_id,
                                         const std::string& date,
                                         int spo2, int hr, int motion) = 0;

    struct OxiSummary {
        double avg_spo2 = 0, min_spo2 = 0, spo2_baseline = 0;
        double odi_3pct = 0, time_below_90 = 0, time_below_88 = 0;
        double avg_hr = 0;
        int min_hr = 0, max_hr = 0;
        int valid_samples = 0, duration_seconds = 0;
        bool found = false;
    };

    /**
     * Get oximetry summary for a date (checks both date and date+1 for midnight crossing).
     */
    virtual OxiSummary getOximetrySummary(const std::string& device_id,
                                           const std::string& date_yyyymmdd,
                                           const std::string& next_day_yyyymmdd) = 0;

    struct OxiRangeSummary {
        int nights = 0;
        double avg_spo2 = 0, min_spo2 = 0;
        double avg_odi = 0, avg_below_90 = 0;
        double avg_hr = 0;
        bool found = false;
    };

    virtual OxiRangeSummary getOximetryRangeSummary(const std::string& device_id,
                                                      const std::string& start_yyyymmdd,
                                                      const std::string& end_yyyymmdd) = 0;

    struct OxiNightlyPoint {
        std::string date;   // YYYYMMDD
        double avg_spo2 = 0;
        double min_spo2 = 0;
    };
    // Per-night SpO2 averages for charting (one row per cpap_session_date, longest session wins on tie).
    virtual std::vector<OxiNightlyPoint> getOximetryNightlySpo2(const std::string& device_id,
                                                                  const std::string& start_yyyymmdd,
                                                                  const std::string& end_yyyymmdd) = 0;

    // -- Raw connection -------------------------------------------------------

    /**
     * Access the underlying native connection handle.
     * Returns void*; callers cast to the backend-specific type
     * (e.g., pqxx::connection* for PostgreSQL).
     */
    // -- Equipment profiles + supplies (SDD-004) ------------------------------
    // Local-first mirror of the cloud model (hms-cpapdash-api SDD-035) WITHOUT
    // user_id: hms-cpap is single-household. A profile ("setup") owns exactly one
    // machine plus its accessories. Supply wear is COMPUTED by SupplyStatus on
    // read and is never stored here — this layer holds only the facts.
    //
    // Conventions shared by all three backends:
    //   * replace_after_days == -1 means "NULL / use the type default"
    //   * started_using_at / timestamps are ISO-8601 strings; "" means unset
    //   * client_uuid == "" means NULL (it exists only for optional cloud sync)
    //   * deleted rows are tombstones: hidden from lists, visible to sync

    struct EquipmentType {
        int         id{0};
        std::string type_key;
        std::string label;
        std::string category;                       // "machine" | "accessory"
        int         default_replace_after_days{-1}; // -1 == NULL (never tracked)
        bool        is_system{false};
        bool        active{true};
    };

    struct EquipmentProfile {
        int         id{0};
        std::string client_uuid;
        std::string name;
        bool        active{true};
        bool        deleted{false};
        std::string created_at;
        std::string updated_at;
    };

    struct EquipmentItem {
        int         id{0};
        int         profile_id{0};
        std::string client_uuid;
        std::string type_key;
        std::string category;                  // denormalized from the type
        std::string brand;
        std::string model;
        std::string variant;
        std::string started_using_at;          // ISO, "" when unset
        long long   started_epoch{0};          // 0 when unset; for SupplyStatus
        int         replace_after_days{-1};    // -1 == NULL (use type default)
        std::string notes;
        bool        active{true};
        bool        deleted{false};
        std::string created_at;
        std::string updated_at;
    };

    // Catalog. listEquipmentTypes returns active types (system + custom).
    virtual std::vector<EquipmentType> listEquipmentTypes() = 0;
    virtual std::optional<EquipmentType> resolveEquipmentType(const std::string& type_key) = 0;
    virtual int  addEquipmentType(const EquipmentType& t) = 0;      // -1 on failure/duplicate
    virtual bool updateEquipmentType(int id, const EquipmentType& t) = 0;
    virtual bool deleteEquipmentType(int id) = 0;                   // soft; never a system row

    // Profiles. listEquipmentProfiles hides tombstones unless include_deleted.
    virtual std::vector<EquipmentProfile> listEquipmentProfiles(bool include_deleted) = 0;
    virtual std::optional<EquipmentProfile> getEquipmentProfile(int id) = 0;
    virtual int  upsertEquipmentProfile(const EquipmentProfile& p) = 0;  // id<=0 inserts
    virtual bool tombstoneEquipmentProfile(int id) = 0;                  // cascades to items
    /// Returns an existing profile id, else creates "My CPAP" and returns it.
    virtual int  ensureDefaultEquipmentProfile() = 0;

    // Items. include_history adds retired (active=false) rows; tombstones stay hidden.
    virtual std::vector<EquipmentItem> listEquipmentItems(bool include_history) = 0;
    virtual std::optional<EquipmentItem> getEquipmentItem(int id) = 0;
    /// True when the profile already holds a live machine, ignoring exclude_item_id.
    virtual bool profileHasMachine(int profile_id, int exclude_item_id) = 0;
    virtual int  upsertEquipmentItem(const EquipmentItem& item) = 0;     // id<=0 inserts
    virtual bool tombstoneEquipmentItem(int id) = 0;

    virtual void* rawConnection() = 0;

    // -- Generic query --------------------------------------------------------

    /**
     * Execute a SELECT query and return results as a JSON array of objects.
     * Each row becomes a JSON object with column names as keys, values as strings
     * (or null for SQL NULLs). Parameters use positional placeholders: $1,$2,...
     * for PostgreSQL, ? for SQLite/MySQL.
     *
     * Default implementation returns an empty array (backends override).
     */
    virtual Json::Value executeQuery(const std::string& sql,
                                     const std::vector<std::string>& params = {}) {
        (void)sql; (void)params;
        return Json::Value(Json::arrayValue);
    }
};

} // namespace hms_cpap
