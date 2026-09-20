#pragma once
//
// SDD-040: the local card root, behind the same interface every other transport
// answers.
//
// IDataSource was already the seam (EzShareClient, FysetcDataSource), but
// nothing implemented it for a folder, so the burst grew a second branch that
// read the filesystem directly. Every local defect this month came out of that
// split: a night that never closed (SDD-037), history that was never scanned
// (SDD-038), a night_state with no ledger behind it. This is the half that lets
// the branch be deleted.
//
#include "clients/EzShareClient.h"   // EzShareFileEntry
#include "clients/IDataSource.h"

#include <string>
#include <vector>

namespace hms_cpap {

/**
 * A card root on disk: DATALOG/<YYYYMMDD>/ with the root files beside it.
 *
 * The "downloads" are reads, and that is the point of filesAreInPlace(): with a
 * local folder the files are ALREADY where the archive would put them, so the
 * cycle skips staging, archiving and the residual walk instead of copying a
 * user's card onto itself (Albin, SDD-040 D1: "local is already a copy", "would
 * not need to walk on residual since is there already").
 */
class LocalDataSource : public IDataSource {
public:
    explicit LocalDataSource(std::string card_root);

    /// A seek in a file, not a request over WiFi.
    bool supportsRange() const override { return true; }

    /// SDD-040: these files are already at the archive's own path, so the cycle
    /// must not stage, mirror or sweep them. Every other transport says false.
    bool filesAreInPlace() const override { return true; }

    /// The card root, so the cycle can parse in place rather than from a copy.
    std::string rootPath() const override { return card_root_; }

    std::string sourceName() const override { return "the local folder"; }

    std::vector<std::string> listDateFolders() override;
    std::vector<EzShareFileEntry> listFiles(const std::string& date_folder) override;
    std::vector<EzShareFileEntry> listDir(const std::string& card_path) override;

    bool downloadByPath(const std::string& card_rel_path,
                        const std::string& local_path) override;
    bool downloadFile(const std::string& date_folder,
                      const std::string& filename,
                      const std::string& local_path) override;
    bool downloadFileRange(const std::string& date_folder,
                           const std::string& filename,
                           const std::string& local_path,
                           size_t start_byte,
                           size_t& bytes_downloaded) override;
    bool downloadRootFile(const std::string& filename,
                          const std::string& local_path) override;

private:
    /// The DATALOG directory under the card root (SDD-010: the root is the card,
    /// the nights live under its DATALOG).
    std::string datalogDir() const;

    std::string card_root_;
};

}  // namespace hms_cpap
