#pragma once

#include <string>
#include <vector>

namespace hms_cpap {

struct EzShareFileEntry;

class IDataSource {
public:
    virtual ~IDataSource() = default;

    virtual bool supportsRange() const { return true; }

    /**
     * SDD-040: are this source's files ALREADY where the archive would put
     * them?
     *
     * True only for a local card root. It is what lets one cycle serve every
     * transport without copying a user's own folder onto itself: staging, the
     * archive mirror and the residual walk all exist to bring a card's bytes
     * ACROSS a transport, and there is no across here (Albin, SDD-040 D1).
     * A source that answers true must also answer rootPath().
     */
    virtual bool filesAreInPlace() const { return false; }

    /// The card root on disk when filesAreInPlace(), else empty.
    virtual std::string rootPath() const { return {}; }

    /// What to call this source in a log line, e.g. "ez Share", "the local
    /// folder". One cycle now serves every transport (SDD-040), so a shared
    /// line that named one of them was wrong for the others: a local folder
    /// announced "Discovering sessions on ez Share..." and sent a user looking
    /// for HTTP calls that were never made (ticket 129).
    virtual std::string sourceName() const { return "the card"; }

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
