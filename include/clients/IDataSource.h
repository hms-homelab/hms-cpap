#pragma once

#include <string>
#include <vector>

namespace hms_cpap {

struct EzShareFileEntry;

class IDataSource {
public:
    virtual ~IDataSource() = default;

    virtual std::vector<std::string> listDateFolders() = 0;

    virtual std::vector<EzShareFileEntry> listFiles(const std::string& date_folder) = 0;

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
