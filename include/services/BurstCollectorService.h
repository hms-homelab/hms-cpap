#pragma once

// WiFiSwitchClient no longer needed - ez Share accessed via dedicated interface
#include "clients/IDataSource.h"
#include "clients/EzShareClient.h"
#ifndef _WIN32
#include "clients/FysetcTcpServer.h"
#endif
#ifndef _WIN32
#include "clients/FysetcDataSource.h"
#endif
#include "llm_client.h"
#include "parsers/CpapdashBridge.h"
#include "services/DataPublisherService.h"
#include "services/SupplyPublisher.h"
#include "services/CpapDashSyncService.h"
#include "services/MyAirService.h"
#include "services/OximetryService.h"
#include "services/PrismaIngestion.h"
#include "services/SessionDiscoveryService.h"
#include "mqtt_client.h"
#include "database/IDatabase.h"
#include "utils/AppConfig.h"
#include "utils/CardLayout.h"
#include "utils/FailureLogThrottle.h"
#include <memory>
#include <thread>
#include <atomic>
#include <chrono>
#include <mutex>

namespace hms_cpap {

/**
 * BurstCollectorService - Main service for CPAP data collection
 *
 * Implements periodic collection pattern:
 * 1. Wait for burst interval
 * 2. Access ez Share via EZSHARE_BASE_URL
 * 3. Download session EDF files
 * 4. Parse EDF files
 * 5. Publish to MQTT / save to database
 *
 * Runs continuously in background thread.
 * Note: No WiFi switching needed - ez Share is bridged to local network!
 */
class BurstCollectorService {
public:
    /**
     * Constructor
     *
     * @param burst_interval_seconds Interval between burst cycles (default: 300s = 5 min)
     */
    explicit BurstCollectorService(int burst_interval_seconds = 300);

    /**
     * SDD-011: is every one of this session's files already in the archive?
     *
     * Guards the "nothing changed, skip the download" shortcut. That shortcut
     * used to fire on "the files stopped changing" alone, which skipped the
     * download, which left downloaded_sessions empty, which returned before the
     * archive block ever ran. A session that went stable without being archived
     * could then NEVER be archived, because the only code that writes the
     * archive sits behind a branch that will never be taken for it again. The
     * night showed in the dashboard and the folder never appeared on disk
     * (Michael, ticket 67).
     *
     * Static and side-effect free: it asks the filesystem a question and
     * answers it. True when no archive is configured, because then there is
     * nothing to assert.
     */
    static bool sessionFilesArchived(const SessionFileSet& session,
                                     const std::string& archive_base_dir);

    /// Initialize all subsystems (call once after construction, before start())
    void initialize(AppConfig* cfg);

    /// Set live config pointer for hot-reload only (does NOT reinit subsystems)
    void setAppConfig(AppConfig* cfg) { app_config_ = cfg; }

    /// Signal that config changed (called from controller thread, safe)
    void markConfigDirty() { config_dirty_ = true; }

    /**
     * Destructor - cleanup and stop service
     */
    ~BurstCollectorService();

    // Disable copy
    BurstCollectorService(const BurstCollectorService&) = delete;
    BurstCollectorService& operator=(const BurstCollectorService&) = delete;

    /**
     * Start service (spawns worker thread)
     */
    void start();

    /**
     * Stop service (joins worker thread)
     */
    void stop();

    /**
     * SDD-004: inject the optional cloud-sync service. When left unset the burst
     * cycle simply never calls sweep(), so the equipment feature stays entirely
     * local — which is the default and the supported offline path.
     */
    void setCpapDashSync(std::shared_ptr<CpapDashSyncService> sync) {
        cpapdash_sync_ = std::move(sync);
    }

    /**
     * SDD-020: inject the optional myAir pull. Left unset, the burst cycle never
     * calls sweep() and nothing about myAir runs, which is the default. The
     * service throttles itself to myair.poll_minutes, so calling it every cycle
     * is deliberate and cheap.
     */
    void setMyAirService(std::shared_ptr<MyAirService> myair) {
        myair_ = std::move(myair);
    }

    /**
     * Check if service is running
     *
     * @return true if worker thread is active
     */
    bool isRunning() const;
    bool forceCompleteSession(const std::string& sleep_day);
    bool generateSummaryForDate(const std::string& sleep_day);

    /**
     * Get last burst execution time
     *
     * @return Timestamp of last burst cycle
     */
    std::chrono::system_clock::time_point getLastBurstTime() const;

    /// Lifecycle decision for the Fysetc TCP listener on a source change.
    enum class FysetcLifecycleAction { None, Start, Stop };

    /// Pure decision: what to do with the fysetc TCP server when source changes.
    /// Start when switching into fysetc mode without a live server.
    /// Stop when switching out of fysetc mode with a live server.
    static FysetcLifecycleAction decideFysetcLifecycle(
        const std::string& old_source,
        const std::string& new_source,
        bool server_exists);

    // ── SDD-005: sync now ────────────────────────────────────────────────
    /// What a "sync now" request did.
    enum class SyncNowOutcome {
        Requested,         ///< worker will cut its sleep short and cycle
        AlreadyRunning,    ///< a cycle is mid-flight; not queued, not doubled
        AlreadyRequested,  ///< a request is already pending; collapsed into it
        NotRunning         ///< worker is stopped; nothing to wake
    };

    /// Pure decision, deliberately shaped like decideFysetcLifecycle above.
    /// The test binary excludes controllers/ and web/ (tests/CMakeLists.txt),
    /// so the only way sync-now is unit-testable at all is if the reasoning
    /// lives here, in a function that touches no thread and no socket.
    static SyncNowOutcome decideSyncNow(bool service_running,
                                        bool cycle_in_flight,
                                        bool request_pending);

    /// Ask the worker to run a burst cycle now rather than waiting out the
    /// rest of its interval. Never starts a second concurrent cycle, and
    /// repeated calls collapse into the one pending request.
    SyncNowOutcome requestSyncNow();

    /// Stable string for the API response body.
    static const char* syncNowOutcomeString(SyncNowOutcome outcome);

    /// Which sleep day a generated summary is filed under.
    ///
    /// `requested` wins whenever the caller named a night. Empty means the
    /// automatic end-of-night path, where "the night that just ended" is
    /// `now - 12h`: a session that ended this morning started last night.
    ///
    /// Pure and static for the same reason decideSyncNow above is: the test
    /// binary cannot reach the LLM path (llm_enabled_ has no setter), so the
    /// only way this decision is testable at all is if it lives somewhere that
    /// touches no LLM and no database. It was wrong for as long as it was
    /// because it was buried mid-function: a summary generated for 2026-08-12
    /// on the 13th was stamped with the 13th, overwriting that day's row and
    /// leaving the requested night with none. Issue 24.
    static std::string resolveSleepDay(const std::string& requested,
                                       std::chrono::system_clock::time_point now);

    // ── Test seam (unit tests only) ──────────────────────────────────────
    // Inject collaborators directly, bypassing initialize() (which opens real
    // DB/MQTT/network connections). Lets the burst cycle, completion, archive
    // and summary logic be unit-tested with a MockDatabase, a fake IDataSource,
    // and a DataPublisherService built with a null MqttClient (publishes no-op).
    // Production code never calls this.
    void injectDependenciesForTest(
        std::shared_ptr<IDatabase> db,
        std::unique_ptr<IDataSource> source,
        std::unique_ptr<DataPublisherService> publisher,
        std::unique_ptr<SessionDiscoveryService> discovery = nullptr,
        std::unique_ptr<PrismaIngestion> prisma = nullptr);

    /// Run a single burst cycle synchronously (no worker thread). Test-only.
    bool runBurstCycleForTest() { return executeBurstCycle(); }

    /// Apply a pending config change synchronously. Test-only — drives the same
    /// reloadConfig() path the worker thread runs when markConfigDirty() fires.
    void reloadConfigForTest() { reloadConfig(); }

    /// Test-only thin wrappers over the private LLM-prompt formatters so their
    /// many optional-field branches can be unit-tested without a live LLM/MQTT.
    std::string buildMetricsStringForTest(const SessionMetrics& metrics,
                                          const STRDailyRecord* str_record = nullptr) const {
        return buildMetricsString(metrics, str_record);
    }
    std::string buildRangeMetricsStringForTest(const std::vector<SessionMetrics>& nights,
                                               SummaryPeriod period) const {
        return buildRangeMetricsString(nights, period);
    }

private:
    // Configuration
    int burst_interval_seconds_;
    /// SleepHQ fallback: queue archive date folders that hold files but produced
    /// no session (our parser rejected them). SleepHQ parses independently, so
    /// dropping these loses real nights. Ported from hms-cpapdash-api 6bc1a2a.
    void markUnparsedNightsForExport();

    std::string device_id_;
    /// SDD-004 optional cloud mirror; null when the feature is off.
    std::shared_ptr<CpapDashSyncService> cpapdash_sync_;
    std::shared_ptr<MyAirService> myair_;
    std::string device_name_;
    /// SDD-010: the card ROOT when CPAP_SOURCE=local. STR.edf and DATALOG are
    /// siblings INSIDE it. Never the DATALOG folder itself: that was the old
    /// contract, and reaching up out of DATALOG to find STR is the bug SDD-010
    /// removes. Use datalogDirFor() to get the session folders beneath it.
    std::string local_source_dir_;
    std::string cpap_source_;       // "ezshare", "local", "fysetc", or "lowenstein"

    /// SDD-010: what local_source_dir_ turned out to be. Anything but Root means
    /// the configuration is wrong and NOTHING may be ingested: the dashboard
    /// keeps serving what is already stored, and the UI carries a banner naming
    /// the fix. Re-evaluated every burst so a share that mounts late recovers on
    /// its own, without a restart.
    LocalDirLayout local_layout_ = LocalDirLayout::Unusable;

    /// One log line when the configuration goes wrong, one on recovery, and a
    /// periodic reminder in between. A 65-second burst loop shouting the same
    /// line forever is how real signal gets buried (see FailureLogThrottle).
    FailureLogThrottle local_layout_log_;

    // Data source (ezShare HTTP or Fysetc TCP — both implement IDataSource)
    std::unique_ptr<IDataSource> data_source_;
    std::unique_ptr<PrismaIngestion> prisma_ingestion_;
    #ifndef _WIN32
    std::unique_ptr<FysetcTcpServer> fysetc_server_;
#endif

    // Services
    std::unique_ptr<SessionDiscoveryService> discovery_service_;

    // Publishers
    std::shared_ptr<hms::MqttClient> mqtt_client_;
    std::shared_ptr<IDatabase> db_service_;
    std::unique_ptr<DataPublisherService> data_publisher_;

    // O2 Ring oximetry
public:
    OximetryService* getOximetryService() const { return oximetry_service_.get(); }
private:
    std::unique_ptr<OximetryService> oximetry_service_;

    // Worker thread
    std::thread worker_thread_;
    std::atomic<bool> running_;
    std::mutex mutex_;

    /// SDD-005 sync-now. `cycle_in_flight_` brackets executeBurstCycle() so a
    /// request landing mid-cycle is REPORTED as such instead of silently
    /// starting a second one; `sync_now_requested_` is what cuts the
    /// inter-cycle sleep short. Both atomic: requestSyncNow() is called from
    /// the web thread, these are read and written by the worker.
    std::atomic<bool> cycle_in_flight_{false};
    std::atomic<bool> sync_now_requested_{false};

    // State
    std::chrono::system_clock::time_point last_burst_time_;

    // LLM summary
    std::unique_ptr<hms::LLMClient> llm_client_;
    bool llm_enabled_ = false;
    std::string llm_prompt_template_;

    // Cached STR records for on-demand insights regeneration
    std::vector<STRDailyRecord> last_str_records_;

    /// Whether the most recent STR attempt actually populated cpap_daily_summary.
    /// False means the dashboard is being fed by the session-derived fallback, so
    /// it has to be recomputed after sessions are saved — STR is attempted early
    /// in the cycle, before anything is parsed (issue #16).
    bool str_summary_ok_ = false;

    // Pending range summary requests (set by MQTT callback, executed by worker thread)
    std::atomic<int> pending_weekly_days_{0};   // >0 = generate weekly with N days
    std::atomic<int> pending_monthly_days_{0};  // >0 = generate monthly with N days

    // Connection recovery tracking
    int consecutive_failures_ = 0;
    static constexpr int MAX_FAILURES_BEFORE_RESET = 3;  // Log recovery after 3 consecutive failures
    bool recovery_logged_ = false;  // Prevent repeated recovery log messages

    /**
     * Worker thread main loop
     */
    void runLoop();

    /**
     * Execute single burst cycle
     *
     * @return true if cycle completed successfully
     */
    bool executeBurstCycle();

    /**
     * Download a single session file set
     *
     * @param session Session file set to download
     * @param local_base_dir Local base directory (/tmp/cpap_data)
     * @return true if at least required files downloaded
     */
    bool downloadSessionFiles(const SessionFileSet& session,
                             const std::string& local_base_dir);

    /**
     * Archive downloaded files to permanent storage
     *
     * Copies files from /tmp/cpap_data/ to permanent DATALOG archive,
     * preserving original SD card structure.
     *
     * @param date_folder Date folder (YYYYMMDD format)
     * @param temp_base_dir Temporary download location (default: /tmp/cpap_data)
     * @param archive_base_dir Permanent archive location
     * @return true if files archived successfully
     */
    bool archiveSessionFiles(const std::string& date_folder,
                            const std::string& temp_base_dir,
                            const std::string& archive_base_dir);

    /**
     * SDD-002: download the non-EDF / non-junk residue (the per-night .crc and any
     * brand-agnostic metadata) for a DATALOG date folder into the temp dir, so
     * archiveSessionFiles() copies it into the OSCAR layout. EDFs are pulled by
     * downloadSessionFiles(); this captures only what that path skips.
     */
    void downloadDatalogResidue(const std::string& date_folder,
                                const std::string& local_dir);

    /**
     * SDD-002: recursive sweep of the card root + every non-DATALOG directory
     * (Identification.*, SETTINGS/, JOURNAL, unknown vendor layouts), writing each
     * non-junk file straight into the OSCAR archive root. Backup-only (never parsed).
     * ezShare only — transports without listDir()/downloadByPath() no-op here.
     */
    void captureCardResidue(const std::string& archive_root);

    /**
     * SDD-008: fold this burst's view of the card into the per-folder ledger.
     *
     * One observation per date folder (file count, total KB, whether every listed
     * file is stored locally), fed through advanceFolder and persisted. Acts on
     * the close edge: arms the STR and sidecar debts, and refetches the sidecars.
     *
     * Called with whatever the burst listed, so a folder the card did not report
     * this cycle simply does not advance. That is the honest answer, and it is
     * why there are no grace windows here.
     */
    void updateFolderLedgers(const std::vector<SessionFileSet>& sessions,
                             const std::string& local_base_dir);

    /**
     * SDD-008: re-download a folder's EVE and CSL files in full.
     *
     * MUST fetch from offset 0. Not an optimization gap: ezShare listings are
     * KB-rounded, so nothing can prove where the file really ends, and a ranged
     * request at or past the card's real EOF HANGS the ezShare. The regular
     * download path already does this (downloadFile, not the range-append
     * smartDownload); this exists because the close path never re-downloads at
     * all, which is how sub-KB sidecar growth was being lost.
     *
     * @return true when every sidecar present in the set was refetched.
     */
    bool refetchSidecars(const std::vector<const SessionFileSet*>& sets,
                         const std::string& local_base_dir);

    /**
     * SDD-008: is this night short of its STR?
     *
     * DERIVED, never stored. The folder is the local calendar date the session
     * started; its ledger row says whether the transfer settled and whether the
     * machine's own daily record ever arrived. A stored flag would be a second
     * source of truth that goes stale every time a night recovers.
     *
     * Returns false when there is no ledger row at all, which is the honest
     * answer for the local-directory and Prisma sources: their files are on a
     * filesystem, so there is no transfer that can stall.
     */
    bool isNightPartial(const std::chrono::system_clock::time_point& session_start) const;

    /**
     * SDD-008: publish a finished night's outcome.
     *
     * Full metrics plus the LLM summary when the transfer actually finished;
     * ONLY the partial fact when it did not. Suppression is the point: a
     * truncated night's AHI and usage hours are WRONG rather than uncertain,
     * MQTT values are retained so Home Assistant keeps them in history, and a
     * stored LLM summary narrates a night that did not happen that way. Both
     * are hard to retract, which is why the decision lives in one place instead
     * of being repeated at each publish site.
     *
     * @return true when the full metrics were published, false when the night
     *         was partial and only the fact was. Callers use this to gate
     *         follow-on aggregates, which must not be recomputed off a night
     *         whose usage hours are known to be short.
     */
    bool publishNightOutcome(const std::chrono::system_clock::time_point& session_start);

    /**
     * SDD-008: clear the STR debt for every folder whose day now has a parsed
     * STR record. Called after processSTRFile, so the debt clears ON RECOVERY
     * and never on a timer: a dead card is not hammered, and "not here yet"
     * stays distinguishable from "never coming".
     */
    void clearStrDebtForParsedDays(const std::vector<std::string>& record_dates);

    /**
     * Process manufacturer-specific summary data on session completion.
     * ResMed: download + parse STR.edf, save to DB, publish to MQTT.
     * Lowenstein: parse statistics_year.bin (future).
     * Non-fatal: failure does not affect the session cycle.
     */
    void processSessionSummary();

    /**
     * @return true only if STR.edf was found, parsed, and its daily records were
     *         written to cpap_daily_summary. False on every other path (card
     *         unreachable, no STR on the card, unparseable, zero therapy days).
     *         The caller uses this to decide whether the dashboard still needs a
     *         summary derived from the sessions themselves — see issue #16.
     */
    bool processSTRFile();

    /**
     * Generate LLM summary of session metrics and publish to MQTT.
     * Non-fatal: failure does not affect the session cycle.
     *
     * @param metrics Aggregated session metrics
     * @param str_record Optional STR daily record for additional context
     * @param sleep_day  The night this summary is FOR, "YYYY-MM-DD". Empty
     *        means "the night that just ended", which is the automatic
     *        end-of-night case and keeps the now()-12h fallback.
     *
     * The parameter exists because the row used to be stamped with now()-12h
     * unconditionally, so generating a summary for 2026-08-12 on the 13th
     * wrote range_start=range_end=2026-08-13 and the night's own summary was
     * filed under the wrong day. Issue 24.
     */
    void generateAndPublishSummary(const SessionMetrics& metrics,
                                   const STRDailyRecord* str_record = nullptr,
                                   const std::string& sleep_day = "");

    /**
     * Build metrics string from session data for LLM prompt substitution.
     */
    std::string buildMetricsString(const SessionMetrics& metrics,
                                   const STRDailyRecord* str_record = nullptr) const;

    /**
     * Generate and publish a weekly or monthly LLM summary.
     *
     * Queries the DB for per-night metrics over the date range, formats them
     * into a multi-night report, sends to the LLM, and publishes to MQTT.
     * Called automatically on Sundays (weekly) and 1st of month (monthly),
     * or on-demand via MQTT command.
     *
     * @param period WEEKLY (7 days) or MONTHLY (30 days)
     * @param days_override Override the default range (0 = use default for period)
     */
    void generateRangeSummary(SummaryPeriod period, int days_override = 0);

    /**
     * Build a multi-night metrics string for the LLM (weekly/monthly).
     * Lists each night with key metrics, then appends period averages.
     */
    std::string buildRangeMetricsString(const std::vector<SessionMetrics>& nights,
                                        SummaryPeriod period) const;

    /**
     * Load prompt template from file.
     *
     * @param filepath Path to prompt template file
     * @return Prompt template string, or empty on failure
     */
    static std::string loadPromptFile(const std::string& filepath);

    /**
     * Get current date string (YYYYMMDD format)
     *
     * @return Date string for today
     */
    std::string getCurrentDateString() const;

    // ── Hot-reload ──────────────────────────────────────────────────────
    AppConfig* app_config_ = nullptr;
    std::atomic<bool> config_dirty_{false};

    struct ConfigSnapshot {
        std::string source, ezshare_url, local_dir;
        std::string db_type, db_host, db_name, db_user, db_password, sqlite_path;
        int db_port = 0;
        bool mqtt_enabled = false;
        std::string mqtt_broker, mqtt_user, mqtt_password, mqtt_client_id;
        int mqtt_port = 1883;
        bool llm_enabled = false;
        std::string llm_provider, llm_endpoint, llm_model, llm_api_key;
        std::string device_id, device_name;
        int burst_interval = 0;
        bool o2ring_enabled = false;
        std::string o2ring_mode, o2ring_mule_url;
    };
    ConfigSnapshot last_config_;

    /// Check dirty flag and reinitialize changed clients
    void reloadConfig();
    /// Copy live AppConfig into snapshot
    void snapshotConfig(ConfigSnapshot& snap);
    /// (Re)create MQTT subscriptions for commands
    void setupMqttSubscriptions();
    /// Create and start fysetc_server_ (idempotent; reads port/bind from env)
    #ifndef _WIN32
    void startFysetcServer();
    #endif
    /// Stop and destroy fysetc_server_ (safe to call when null)
    void stopFysetcServer();

    /// Subsystem init helpers — called from initialize()
    void initDataSource();
    void initDatabase();
    void initMqtt();
    void initDataPublisher();
    void initLlm();
    void initO2Ring();
};

} // namespace hms_cpap
