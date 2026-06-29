#pragma once

#include <string>
#include <vector>

namespace hms_cpap {

struct EzShareFileEntry;

class IDataSource {
public:
    virtual ~IDataSource() = default;

    virtual bool supportsRange() const { return true; }

    virtual std::vector<std::string> listDateFolders() = 0;

    virtual std::vector<EzShareFileEntry> listFiles(const std::string& date_folder) = 0;

    // SDD-002 full-card residue sweep. listDir() returns ALL entries (files AND
    // dirs) of an arbitrary card directory — "" is the card root, otherwise a
    // backslash-separated card-relative path (e.g. "SETTINGS"). downloadByPath()
    // fetches one file by its card-relative path into local_path. Both are
    // backup-only helpers; transports that don't support them keep the no-op
    // defaults (the sweep simply captures nothing for that source).
    virtual std::vector<EzShareFileEntry> listDir(const std::string& /*card_path*/) {
        return {};
    }

    virtual bool downloadByPath(const std::string& /*card_rel_path*/,
                                const std::string& /*local_path*/) {
        return false;
    }

    virtual bool downloadFile(const std::string& date_folder,
                              const std::string& filename,
                              const std::string& local_path) = 0;

    virtual bool downloadFileRange(const std::string& date_folder,
                                   const std::string& filename,
                                   const std::string& local_path,
                                   size_t start_byte,
                                   size_t& bytes_downloaded) = 0;

    virtual bool downloadRootFile(const std::string& filename,
                                  const std::string& local_path) = 0;
};

}  // namespace hms_cpap
