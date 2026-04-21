#pragma once

#include "clients/FysetcTcpServer.h"
#include "parsers/Fat32Parser.h"
#include "database/IDatabase.h"
#include <string>
#include <vector>
#include <map>
#include <memory>
#include <functional>

namespace hms_cpap {

class FysetcSectorCollectorService {
public:
    using ArchiveCallback = std::function<void(const std::string& date_folder)>;

    FysetcSectorCollectorService(FysetcTcpServer& tcp_server,
                                  const std::string& archive_dir,
                                  const std::string& device_id);

    void setDatabase(std::shared_ptr<IDatabase> db) { db_ = db; }

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

private:
    bool refreshFatLayout();
    bool scanDatalogDir();
    bool syncFile(const std::string& date_folder, const Fat32DirEntry& entry);
    bool writeFileData(const std::string& path, const std::vector<uint8_t>& data,
                       uint32_t offset);

    Fat32Parser::SectorReader makeSectorReader();


    FysetcTcpServer& tcp_;
    std::string archive_dir_;
    std::string device_id_;
    std::shared_ptr<IDatabase> db_;
    std::unique_ptr<Fat32Parser> fat_;

    // Cached FAT layout
    uint32_t datalog_cluster_ = 0;
    std::vector<Fat32DirEntry> datalog_entries_;
    std::vector<Fat32DirEntry> root_entries_;

    ArchiveCallback archive_callback_;
};

}  // namespace hms_cpap
