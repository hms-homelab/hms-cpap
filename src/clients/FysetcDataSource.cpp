#include "clients/FysetcDataSource.h"
#include <filesystem>
#include <fstream>
#include <iostream>
#include <algorithm>

namespace hms_cpap {

FysetcDataSource::FysetcDataSource(FysetcTcpServer& tcp)
    : tcp_(tcp) {}

Fat32Parser::SectorReader FysetcDataSource::makeSectorReader() {
    return [this](uint32_t lba, uint32_t count,
                  std::vector<uint8_t>& out) -> bool {
        std::vector<fysetc::SectorRange> ranges = {{lba, static_cast<uint16_t>(count)}};
        std::vector<std::pair<uint32_t, uint16_t>> delivered;
        if (!tcp_.readSectors(ranges, out, delivered)) return false;
        if (delivered.empty()) return false;
        return delivered[0].second == count;
    };
}

bool FysetcDataSource::ensureFat() {
    if (fat_) {
        fat_->clearFatCache();
        return true;
    }

    if (!tcp_.isConnected()) return false;

    fat_ = std::make_unique<Fat32Parser>(makeSectorReader());
    if (!fat_->init()) {
        std::cerr << "FysetcDataSource: FAT32 init failed" << std::endl;
        fat_.reset();
        return false;
    }

    // Find DATALOG cluster
    auto root = fat_->listDir(fat_->bpb().root_cluster);
    for (auto& e : root) {
        std::string name_lower = e.name;
        std::transform(name_lower.begin(), name_lower.end(), name_lower.begin(), ::tolower);
        if (name_lower == "datalog" && e.is_directory) {
            datalog_cluster_ = e.first_cluster;
            break;
        }
    }

    std::cout << "FysetcDataSource: FAT32 initialized ("
              << fat_->bpb().sectors_per_cluster << " sectors/cluster)" << std::endl;
    return true;
}

uint32_t FysetcDataSource::findClusterForFolder(const std::string& folder_name) {
    if (!datalog_cluster_) return 0;

    auto entries = fat_->listDir(datalog_cluster_);
    for (auto& e : entries) {
        std::string name_lower = e.name;
        std::string target_lower = folder_name;
        std::transform(name_lower.begin(), name_lower.end(), name_lower.begin(), ::tolower);
        std::transform(target_lower.begin(), target_lower.end(), target_lower.begin(), ::tolower);
        if (name_lower == target_lower && e.is_directory) {
            return e.first_cluster;
        }
    }
    return 0;
}

std::vector<std::string> FysetcDataSource::listDateFolders() {
    if (!ensureFat() || !datalog_cluster_) return {};

    auto entries = fat_->listDir(datalog_cluster_);
    std::vector<std::string> folders;

    for (auto& e : entries) {
        if (e.is_directory && e.name.size() == 8) {
            folders.push_back(e.name);
        }
    }

    std::sort(folders.begin(), folders.end());
    return folders;
}

std::vector<EzShareFileEntry> FysetcDataSource::listFiles(const std::string& date_folder) {
    if (!ensureFat()) return {};

    uint32_t cluster = findClusterForFolder(date_folder);
    if (!cluster) return {};

    auto entries = fat_->listDir(cluster);
    std::vector<EzShareFileEntry> result;

    for (auto& e : entries) {
        if (e.is_directory) continue;

        EzShareFileEntry fe;
        fe.name = e.name;
        fe.size_kb = static_cast<int>((e.size + 1023) / 1024);
        fe.is_dir = false;

        // Parse FAT modify date/time into EzShareFileEntry fields
        fe.year  = ((e.modify_date >> 9) & 0x7F) + 1980;
        fe.month = (e.modify_date >> 5) & 0x0F;
        fe.day   = e.modify_date & 0x1F;
        fe.hour  = (e.modify_time >> 11) & 0x1F;
        fe.minute = (e.modify_time >> 5) & 0x3F;
        fe.second = (e.modify_time & 0x1F) * 2;

        result.push_back(std::move(fe));
    }

    return result;
}

bool FysetcDataSource::readFileToLocal(uint32_t first_cluster, uint32_t file_size,
                                        const std::string& local_path,
                                        size_t start_byte, size_t* bytes_out) {
    auto ranges = fat_->fileSectorRanges(first_cluster, file_size, start_byte);
    if (ranges.empty()) {
        if (bytes_out) *bytes_out = 0;
        return true;
    }

    uint32_t bytes_needed = file_size - start_byte;

    // Flatten into <=64-sector chunks, batch by 16
    std::vector<fysetc::SectorRange> all_chunks;
    for (auto& r : ranges) {
        uint32_t lba = r.lba;
        uint32_t remaining = r.count;
        while (remaining > 0) {
            uint16_t chunk = static_cast<uint16_t>(std::min(remaining, 64u));
            all_chunks.push_back({lba, chunk});
            lba += chunk;
            remaining -= chunk;
        }
    }

    std::vector<uint8_t> file_data;
    for (size_t i = 0; i < all_chunks.size(); ) {
        size_t batch_end = std::min(i + 16, all_chunks.size());
        std::vector<fysetc::SectorRange> batch(all_chunks.begin() + i,
                                                all_chunks.begin() + batch_end);
        std::vector<uint8_t> sector_data;
        std::vector<std::pair<uint32_t, uint16_t>> delivered;

        if (!tcp_.readSectors(batch, sector_data, delivered)) {
            return false;
        }

        file_data.insert(file_data.end(), sector_data.begin(), sector_data.end());
        i = batch_end;
    }

    if (file_data.size() > bytes_needed) {
        file_data.resize(bytes_needed);
    }

    // Write to local file
    std::filesystem::create_directories(
        std::filesystem::path(local_path).parent_path());

    std::ios_base::openmode mode = std::ios::binary;
    if (start_byte > 0) {
        mode |= std::ios::in | std::ios::out;
    } else {
        mode |= std::ios::out;
    }

    std::fstream f(local_path, mode);
    if (!f.is_open() && start_byte > 0) {
        f.open(local_path, std::ios::binary | std::ios::out);
    }
    if (!f.is_open()) return false;

    f.seekp(start_byte);
    f.write(reinterpret_cast<const char*>(file_data.data()), file_data.size());
    f.flush();

    if (bytes_out) *bytes_out = file_data.size();
    return f.good();
}

bool FysetcDataSource::downloadFile(const std::string& date_folder,
                                     const std::string& filename,
                                     const std::string& local_path) {
    if (!ensureFat()) return false;

    uint32_t cluster = findClusterForFolder(date_folder);
    if (!cluster) return false;

    auto entries = fat_->listDir(cluster);
    for (auto& e : entries) {
        if (e.name == filename) {
            return readFileToLocal(e.first_cluster, e.size, local_path, 0, nullptr);
        }
    }

    std::cerr << "FysetcDataSource: File not found: " << date_folder << "/" << filename << std::endl;
    return false;
}

bool FysetcDataSource::downloadFileRange(const std::string& date_folder,
                                          const std::string& filename,
                                          const std::string& local_path,
                                          size_t start_byte,
                                          size_t& bytes_downloaded) {
    if (!ensureFat()) return false;

    uint32_t cluster = findClusterForFolder(date_folder);
    if (!cluster) return false;

    auto entries = fat_->listDir(cluster);
    for (auto& e : entries) {
        if (e.name == filename) {
            if (start_byte >= e.size) {
                bytes_downloaded = 0;
                return true;
            }
            return readFileToLocal(e.first_cluster, e.size, local_path,
                                   start_byte, &bytes_downloaded);
        }
    }

    std::cerr << "FysetcDataSource: File not found for range: " << date_folder << "/" << filename << std::endl;
    return false;
}

bool FysetcDataSource::downloadRootFile(const std::string& filename,
                                         const std::string& local_path) {
    if (!ensureFat()) return false;

    auto root = fat_->listDir(fat_->bpb().root_cluster);
    for (auto& e : root) {
        std::string name_lower = e.name;
        std::string target_lower = filename;
        std::transform(name_lower.begin(), name_lower.end(), name_lower.begin(), ::tolower);
        std::transform(target_lower.begin(), target_lower.end(), target_lower.begin(), ::tolower);
        if (name_lower == target_lower) {
            return readFileToLocal(e.first_cluster, e.size, local_path, 0, nullptr);
        }
    }

    return false;
}

}  // namespace hms_cpap
