#pragma once

#ifndef _WIN32

#include "clients/IDataSource.h"
#include "clients/EzShareClient.h"
#include "clients/FysetcTcpServer.h"
#include "parsers/Fat32Parser.h"
#include <memory>
#include <string>

namespace hms_cpap {

class FysetcDataSource : public IDataSource {
public:
    FysetcDataSource(FysetcTcpServer& tcp);

    std::vector<std::string> listDateFolders() override;
    std::vector<EzShareFileEntry> listFiles(const std::string& date_folder) override;

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
    bool ensureFat();
    Fat32Parser::SectorReader makeSectorReader();

    bool readFileToLocal(uint32_t first_cluster, uint32_t file_size,
                         const std::string& local_path,
                         size_t start_byte, size_t* bytes_out);

    uint32_t findClusterForFolder(const std::string& folder_name);

    FysetcTcpServer& tcp_;
    /// Locate DATALOG under the card root and cache its cluster.
    /// Sets datalog_cluster_ to 0 when it is not there.
    void findDatalogCluster();

    std::unique_ptr<Fat32Parser> fat_;
    uint32_t datalog_cluster_ = 0;
    /// Which device connection fat_ was built against. The Fysetc has no USB,
    /// so it gets power-cycled by hand routinely and can reconnect underneath
    /// us with a different card state; a parser cached across that is stale.
    uint32_t fat_session_ = 0;
};

}  // namespace hms_cpap

#endif // _WIN32
