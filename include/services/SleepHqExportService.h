#pragma once
//
// SleepHqExportService — pushes a completed session's raw SD files to SleepHQ
// (see hms-cpapdash-api SDD-009). hms-cpap already archives the full SD tree, so
// this just enumerates the date's files and uploads them.
//
// Live-collector exports are debounced (SDD-003): completions mark a date
// folder dirty and sweep() exports it only once the archive has been quiet
// for sleephq.quiet_minutes, with retry/backoff on failure.
//
#include <chrono>
#include <cstdint>
#include <functional>
#include <map>
#include <mutex>
#include <string>

namespace hms_cpap {

class AppConfig;

class SleepHqExportService {
public:
    static SleepHqExportService& getInstance();

    void initialize(AppConfig* cfg) { config_ = cfg; }

    // Mark a date folder (YYYYMMDD) as needing export once the night settles.
    // Called by the live collector on session completion. No-op if disabled.
    void markDirty(const std::string& date_folder);

    /// Queue a night ONLY if it has not already been exported unchanged. Used by
    /// the sessionless disk-walk fallback, which re-offers the same folders every
    /// burst cycle. Returns true if it was queued. markDirty() keeps its original
    /// unconditional semantics.
    bool markDirtyIfNotExported(const std::string& date_folder);

    // Scan dirty folders and export the first one that has been quiet long
    // enough. Called once per burst cycle. Real exports run on a detached
    // thread, one at a time; `now` is injectable for tests.
    void sweep(std::chrono::steady_clock::time_point now =
                   std::chrono::steady_clock::now());

    // Immediate export of one date folder (YYYYMMDD) from the ARCHIVE
    // (CPAP_ARCHIVE_DIR). Used by sweep(); the async form serves the manual
    // "Upload to SleepHQ" endpoint, which must not wait for a quiet window.
    void exportDateAsync(const std::string& date_folder);
    bool exportDate(const std::string& date_folder);

    // Export from EXPLICIT directories — used by local-mode/backfill, where files
    // live under local_dir, not the archive. datalog_dir holds the date's EDFs
    // (uploaded as DATALOG/<date>/*); root_dir is searched for STR.edf +
    // Identification.* (uploaded at the import root).
    void exportFolderAsync(const std::string& date_folder,
                           const std::string& datalog_dir, const std::string& root_dir);
    /// One file destined for a SleepHQ import.
    /// import_path "" means the import ROOT (card root files); otherwise it is the
    /// card-accurate relative path, e.g. "DATALOG/20260706".
    struct ExportFile {
        std::string name;
        std::string import_path;
        std::string local_path;
    };

    /// Enumerate exactly what exportFolder() would upload. Split out so the file
    /// SET is testable without a network: a regression here (notably root_dir
    /// pointing at DATALOG instead of the card root) silently drops STR.edf and
    /// Identification.*, which SleepHQ accepts and then processes into nothing.
    static std::vector<ExportFile> collectExportFiles(const std::string& date_folder,
                                                     const std::string& datalog_dir,
                                                     const std::string& root_dir);

    bool exportFolder(const std::string& date_folder,
                      const std::string& datalog_dir, const std::string& root_dir);

    // Test hooks: replace the real upload (hook runs inline on the sweep
    // caller's thread) and reset all debounce state.
    void setExportHookForTest(std::function<bool(const std::string&)> hook);
    void resetForTest();
    bool isDirtyForTest(const std::string& date_folder);

private:
    SleepHqExportService() = default;

    struct FolderState {
        std::map<std::string, std::uintmax_t> snapshot;   // filename -> bytes
        std::chrono::steady_clock::time_point last_change{};
        std::chrono::steady_clock::time_point next_attempt{};
        int failures = 0;
    };

    // Scan the archive DATALOG dir for `folder`; empty map if missing.
    static std::map<std::string, std::uintmax_t> scanFolder(
        const std::string& archive_base, const std::string& folder);

    // Record an export outcome; keeps the folder dirty on failure (with
    // backoff) or when its contents changed during the upload.
    void finishExport(const std::string& folder, bool ok,
                      std::map<std::string, std::uintmax_t> pre_snapshot,
                      std::chrono::steady_clock::time_point now);

    AppConfig* config_ = nullptr;
    std::mutex mu_;
    std::map<std::string, FolderState> dirty_;
    /// folder -> the file snapshot at its last SUCCESSFUL export. Guards against
    /// shipping the same unchanged night twice now that two independent triggers
    /// (parsed session, and the sessionless disk-walk fallback) can queue it.
    /// In-memory only: a restart forgets, which re-exports at worst once.
    std::map<std::string, std::map<std::string, std::uintmax_t>> exported_;
    bool export_in_flight_ = false;  // guarded by mu_
    std::function<bool(const std::string&)> export_hook_;  // tests only
};

} // namespace hms_cpap
