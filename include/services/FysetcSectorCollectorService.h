#pragma once

#include "clients/FysetcTcpServer.h"
#include "parsers/Fat32Parser.h"
#include <string>
#include <vector>
#include <map>
#include <mutex>
#include <functional>

namespace hms_cpap {

struct TrackedFile {
    std::string path;
    uint32_t first_cluster = 0;
    uint32_t size = 0;
    uint32_t confirmed_bytes = 0;
    uint16_t modify_date = 0;
    uint16_t modify_time = 0;
};

class FysetcSectorCollectorService {
public:
    using ArchiveCallback = std::function<void(const std::string& date_folder)>;

    FysetcSectorCollectorService(FysetcTcpServer& tcp_server,
                                  const std::string& archive_dir);

    bool initFat();

    struct CollectResult {
        int new_files = 0;
        int updated_files = 0;
        int bytes_received = 0;
        bool success = false;
        std::vector<std::string> updated_date_folders;
    };

    CollectResult collect();

    void setArchiveCallback(ArchiveCallback cb) { archive_callback_ = std::move(cb); }

    const std::map<std::string, TrackedFile>& trackedFiles() const { return tracked_files_; }

    bool needsFullSync() const { return needs_full_sync_; }

private:
    bool refreshFatLayout();
    bool scanDatalogDir();
    bool syncFile(const std::string& date_folder, const Fat32DirEntry& entry);
    bool writeFileData(const std::string& path, const std::vector<uint8_t>& data,
                       uint32_t offset);

    Fat32Parser::SectorReader makeSectorReader();

    FysetcTcpServer& tcp_;
    std::string archive_dir_;
    std::unique_ptr<Fat32Parser> fat_;
    bool needs_full_sync_ = true;

    // Cached FAT layout
    uint32_t datalog_cluster_ = 0;
    std::vector<Fat32DirEntry> datalog_entries_;
    std::vector<Fat32DirEntry> root_entries_;

    // Per-file tracking
    std::map<std::string, TrackedFile> tracked_files_;

    ArchiveCallback archive_callback_;
};

}  // namespace hms_cpap
