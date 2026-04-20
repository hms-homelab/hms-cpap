#include "services/FysetcSectorCollectorService.h"
#include <filesystem>
#include <fstream>
#include <iostream>
#include <algorithm>
#include <set>

namespace hms_cpap {

FysetcSectorCollectorService::FysetcSectorCollectorService(
    FysetcTcpServer& tcp_server, const std::string& archive_dir)
    : tcp_(tcp_server), archive_dir_(archive_dir) {}

Fat32Parser::SectorReader FysetcSectorCollectorService::makeSectorReader() {
    return [this](uint32_t lba, uint32_t count,
                  std::vector<uint8_t>& out) -> bool {
        std::vector<fysetc::SectorRange> ranges = {{lba, static_cast<uint16_t>(count)}};
        std::vector<std::pair<uint32_t, uint16_t>> delivered;
        if (!tcp_.readSectors(ranges, out, delivered)) return false;
        if (delivered.empty()) return false;
        return delivered[0].second == count;
    };
}

bool FysetcSectorCollectorService::initFat() {
    if (!tcp_.isConnected()) return false;

    fat_ = std::make_unique<Fat32Parser>(makeSectorReader());
    if (!fat_->init()) {
        std::cerr << "FysetcCollector: FAT32 init failed" << std::endl;
        fat_.reset();
        return false;
    }

    std::cout << "FysetcCollector: FAT32 initialized ("
              << fat_->bpb().sectors_per_cluster << " sectors/cluster)" << std::endl;
    return true;
}

bool FysetcSectorCollectorService::refreshFatLayout() {
    if (!fat_) {
        if (!initFat()) return false;
    }

    root_entries_ = fat_->listDir(fat_->bpb().root_cluster);
    datalog_cluster_ = 0;

    for (auto& e : root_entries_) {
        std::string name_lower = e.name;
        std::transform(name_lower.begin(), name_lower.end(), name_lower.begin(), ::tolower);
        if (name_lower == "datalog" && e.is_directory) {
            datalog_cluster_ = e.first_cluster;
            break;
        }
    }

    if (datalog_cluster_ == 0) {
        std::cerr << "FysetcCollector: DATALOG directory not found" << std::endl;
        return false;
    }

    datalog_entries_ = fat_->listDir(datalog_cluster_);
    return true;
}

bool FysetcSectorCollectorService::scanDatalogDir() {
    if (!refreshFatLayout()) return false;

    // Sort date folders by name (YYYYMMDD) descending — most recent first
    std::sort(datalog_entries_.begin(), datalog_entries_.end(),
              [](const Fat32DirEntry& a, const Fat32DirEntry& b) {
                  return a.name > b.name;
              });

    return true;
}

bool FysetcSectorCollectorService::syncFile(const std::string& date_folder,
                                             const Fat32DirEntry& entry) {
    std::string rel_path = "DATALOG/" + date_folder + "/" + entry.name;
    auto& tracked = tracked_files_[rel_path];

    bool is_new = (tracked.first_cluster == 0);
    bool has_grown = (entry.size > tracked.confirmed_bytes);
    bool cluster_changed = (entry.first_cluster != tracked.first_cluster && !is_new);

    if (!is_new && !has_grown && !cluster_changed) return true;

    tracked.path = rel_path;
    tracked.first_cluster = entry.first_cluster;
    tracked.size = entry.size;
    tracked.modify_date = entry.modify_date;
    tracked.modify_time = entry.modify_time;

    uint32_t offset = is_new ? 0 : tracked.confirmed_bytes;
    uint32_t bytes_needed = entry.size - offset;

    if (bytes_needed == 0) return true;

    auto ranges = fat_->fileSectorRanges(entry.first_cluster, entry.size, offset);
    if (ranges.empty()) return true;

    // Read sectors in batches of <=16 ranges per protocol spec
    std::vector<uint8_t> file_data;

    // First, flatten all ranges into <=64-sector chunks
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

    // Send in batches of 16
    for (size_t i = 0; i < all_chunks.size(); ) {
        size_t batch_end = std::min(i + 16, all_chunks.size());
        std::vector<fysetc::SectorRange> batch(all_chunks.begin() + i,
                                                all_chunks.begin() + batch_end);

        std::vector<uint8_t> sector_data;
        std::vector<std::pair<uint32_t, uint16_t>> delivered;

        if (!tcp_.readSectors(batch, sector_data, delivered)) {
            std::cerr << "FysetcCollector: Sector read failed for " << rel_path << std::endl;
            return false;
        }

        file_data.insert(file_data.end(), sector_data.begin(), sector_data.end());
        i = batch_end;
    }

    // Trim to exact file size (last sector may have padding)
    if (file_data.size() > bytes_needed) {
        file_data.resize(bytes_needed);
    }

    if (!writeFileData(rel_path, file_data, offset)) return false;

    tracked.confirmed_bytes = entry.size;
    return true;
}

bool FysetcSectorCollectorService::writeFileData(const std::string& rel_path,
                                                   const std::vector<uint8_t>& data,
                                                   uint32_t offset) {
    std::string full_path = archive_dir_ + "/" + rel_path;
    std::filesystem::create_directories(
        std::filesystem::path(full_path).parent_path());

    std::ios_base::openmode mode = std::ios::binary;
    if (offset > 0) {
        mode |= std::ios::in | std::ios::out;
    } else {
        mode |= std::ios::out;
    }

    std::fstream f(full_path, mode);
    if (!f.is_open() && offset > 0) {
        // File doesn't exist yet but offset > 0 — create it
        f.open(full_path, std::ios::binary | std::ios::out);
        if (!f.is_open()) return false;
        // Pad to offset
        std::vector<uint8_t> pad(offset, 0);
        f.write(reinterpret_cast<const char*>(pad.data()), pad.size());
    }
    if (!f.is_open()) return false;

    f.seekp(offset);
    f.write(reinterpret_cast<const char*>(data.data()), data.size());
    f.flush();

    return f.good();
}

FysetcSectorCollectorService::CollectResult
FysetcSectorCollectorService::collect() {
    CollectResult result;

    if (!tcp_.isConnected()) {
        std::cerr << "FysetcCollector: Not connected" << std::endl;
        return result;
    }

    if (tcp_.deviceState().needs_full_sync || needs_full_sync_) {
        fat_.reset();
        tracked_files_.clear();
        needs_full_sync_ = false;
        tcp_.clearFullSyncFlag();
    }

    // Clear FAT cache each cycle so grown files get fresh cluster chains
    if (fat_) fat_->clearFatCache();

    if (!scanDatalogDir()) return result;

    // Process the most recent date folders (last 3)
    int folders_to_scan = std::min(static_cast<int>(datalog_entries_.size()), 3);
    std::set<std::string> updated_folders;

    for (int i = 0; i < folders_to_scan; ++i) {
        auto& date_entry = datalog_entries_[i];
        if (!date_entry.is_directory) continue;

        auto files = fat_->listDir(date_entry.first_cluster);

        for (auto& file : files) {
            if (file.is_directory) continue;

            std::string name_lower = file.name;
            std::transform(name_lower.begin(), name_lower.end(),
                          name_lower.begin(), ::tolower);

            // Only sync EDF files
            if (name_lower.size() < 4 ||
                name_lower.substr(name_lower.size() - 4) != ".edf") continue;

            std::string rel_path = "DATALOG/" + date_entry.name + "/" + file.name;
            bool was_new = (tracked_files_.find(rel_path) == tracked_files_.end());
            bool was_smaller = (!was_new &&
                                tracked_files_[rel_path].confirmed_bytes < file.size);

            if (syncFile(date_entry.name, file)) {
                if (was_new) {
                    result.new_files++;
                    updated_folders.insert(date_entry.name);
                } else if (was_smaller) {
                    result.updated_files++;
                    updated_folders.insert(date_entry.name);
                }
                result.bytes_received += file.size -
                    (was_new ? 0 : tracked_files_[rel_path].confirmed_bytes);
            }
        }
    }

    // Also sync STR.edf from root (reuse root listing from refreshFatLayout)
    auto root = root_entries_;
    for (auto& e : root) {
        std::string name_lower = e.name;
        std::transform(name_lower.begin(), name_lower.end(),
                      name_lower.begin(), ::tolower);
        if (name_lower == "str.edf") {
            Fat32DirEntry str_entry = e;
            str_entry.name = e.name;  // preserve original case
            // Treat STR.edf as root-level file with empty date folder
            std::string rel_path = e.name;
            auto& tracked = tracked_files_[rel_path];
            if (tracked.first_cluster == 0 || e.size > tracked.confirmed_bytes) {
                uint32_t offset = tracked.confirmed_bytes;
                auto ranges = fat_->fileSectorRanges(e.first_cluster, e.size, offset);
                if (!ranges.empty()) {
                    // Flatten + cap at 16 per batch (same as syncFile)
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
                    std::vector<uint8_t> data;
                    bool read_ok = true;
                    for (size_t ci = 0; ci < all_chunks.size(); ) {
                        size_t ce = std::min(ci + 16, all_chunks.size());
                        std::vector<fysetc::SectorRange> batch(all_chunks.begin() + ci,
                                                                all_chunks.begin() + ce);
                        std::vector<uint8_t> chunk_data;
                        std::vector<std::pair<uint32_t, uint16_t>> delivered;
                        if (!tcp_.readSectors(batch, chunk_data, delivered)) {
                            read_ok = false;
                            break;
                        }
                        data.insert(data.end(), chunk_data.begin(), chunk_data.end());
                        ci = ce;
                    }
                    if (read_ok) {
                        uint32_t bytes_needed = e.size - offset;
                        if (data.size() > bytes_needed) data.resize(bytes_needed);
                        writeFileData(rel_path, data, offset);
                        tracked.path = rel_path;
                        tracked.first_cluster = e.first_cluster;
                        tracked.size = e.size;
                        tracked.confirmed_bytes = e.size;
                        result.updated_files++;
                    }
                }
            }
            break;
        }
    }

    result.success = true;
    result.updated_date_folders.assign(updated_folders.begin(), updated_folders.end());

    if (archive_callback_ && !updated_folders.empty()) {
        for (auto& folder : updated_folders) {
            archive_callback_(folder);
        }
    }

    return result;
}

}  // namespace hms_cpap
