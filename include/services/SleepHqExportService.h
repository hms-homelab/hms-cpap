#pragma once
//
// SleepHqExportService — pushes a completed session's raw SD files to SleepHQ
// (see hms-cpapdash-api SDD-009). hms-cpap already archives the full SD tree, so
// this just enumerates the date's files and uploads them.
//
#include <string>

namespace hms_cpap {

class AppConfig;

class SleepHqExportService {
public:
    static SleepHqExportService& getInstance();

    void initialize(AppConfig* cfg) { config_ = cfg; }

    // Fire-and-forget export of one date folder (YYYYMMDD) from the ARCHIVE
    // (CPAP_ARCHIVE_DIR) — used by the live collector. No-op if disabled.
    void exportDateAsync(const std::string& date_folder);
    bool exportDate(const std::string& date_folder);

    // Export from EXPLICIT directories — used by local-mode/backfill, where files
    // live under local_dir, not the archive. datalog_dir holds the date's EDFs
    // (uploaded as DATALOG/<date>/*); root_dir is searched for STR.edf +
    // Identification.* (uploaded at the import root).
    void exportFolderAsync(const std::string& date_folder,
                           const std::string& datalog_dir, const std::string& root_dir);
    bool exportFolder(const std::string& date_folder,
                      const std::string& datalog_dir, const std::string& root_dir);

private:
    SleepHqExportService() = default;
    AppConfig* config_ = nullptr;
};

} // namespace hms_cpap
