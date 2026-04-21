#include "services/FysetcSectorCollectorService.h"
#include <filesystem>
#include <fstream>
#include <iostream>
#include <algorithm>
#include <set>
#include <cstdio>
#include <cstdlib>
#include <ctime>

namespace hms_cpap {

FysetcSectorCollectorService::FysetcSectorCollectorService(
    FysetcTcpServer& tcp_server, const std::string& archive_dir,
    const std::string& device_id)
    : tcp_(tcp_server), archive_dir_(archive_dir), device_id_(device_id) {}

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

    fat_->clearFatCache();

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

    std::sort(datalog_entries_.begin(), datalog_entries_.end(),
              [](const Fat32DirEntry& a, const Fat32DirEntry& b) {
                  return a.name > b.name;
              });

    return true;
}

// Parse "YYYYMMDD_HHMMSS" from filename into time_point
static std::chrono::system_clock::time_point parseTimestamp(const std::string& name) {
    std::tm tm = {};
    if (name.size() >= 15) {
        sscanf(name.c_str(), "%4d%2d%2d_%2d%2d%2d",
               &tm.tm_year, &tm.tm_mon, &tm.tm_mday,
               &tm.tm_hour, &tm.tm_min, &tm.tm_sec);
        tm.tm_year -= 1900;
        tm.tm_mon -= 1;
        tm.tm_isdst = -1;
    }
    return std::chrono::system_clock::from_time_t(std::mktime(&tm));
}

// Check if a file is a checkpoint type (BRP/PLD/SAD/SA2)
static bool isCheckpointFile(const std::string& name_lower) {
    return name_lower.find("_brp.edf") != std::string::npos ||
           name_lower.find("_pld.edf") != std::string::npos ||
           name_lower.find("_sad.edf") != std::string::npos ||
           name_lower.find("_sa2.edf") != std::string::npos;
}

// Group files into sessions using the same gap logic as SessionDiscoveryService:
// Sort checkpoint files by creation timestamp. Gap between file N's mtime and
// file N+1's creation timestamp > 1 hour = new session.
struct FatSession {
    std::chrono::system_clock::time_point session_start;
    std::vector<Fat32DirEntry*> all_files;
    std::map<std::string, int> checkpoint_sizes_kb;  // BRP/PLD/SAD only
};

static std::vector<FatSession> groupIntoSessions(std::vector<Fat32DirEntry>& files) {
    // Separate checkpoint files (BRP/PLD/SAD) from summary files (CSL/EVE)
    struct CheckpointFile {
        Fat32DirEntry* entry;
        std::chrono::system_clock::time_point created;  // from filename
        std::chrono::system_clock::time_point modified;  // from FAT mtime
    };

    std::vector<CheckpointFile> checkpoints;
    std::vector<Fat32DirEntry*> summary_files;  // CSL/EVE
    std::vector<Fat32DirEntry*> ungrouped;       // short names, non-standard

    for (auto& file : files) {
        if (file.is_directory) continue;

        if (file.name.size() < 15) {
            std::string nl = file.name;
            std::transform(nl.begin(), nl.end(), nl.begin(), ::tolower);
            if (nl.size() >= 4 && nl.substr(nl.size() - 4) == ".edf")
                ungrouped.push_back(&file);
            continue;
        }

        std::string name_lower = file.name;
        std::transform(name_lower.begin(), name_lower.end(),
                      name_lower.begin(), ::tolower);

        if (name_lower.size() < 4 ||
            name_lower.substr(name_lower.size() - 4) != ".edf") continue;

        if (isCheckpointFile(name_lower)) {
            checkpoints.push_back({
                &file,
                parseTimestamp(file.name),
                file.modTime()
            });
        } else {
            summary_files.push_back(&file);
        }
    }

    if (checkpoints.empty()) {
        // No checkpoint files but may have ungrouped files
        if (!ungrouped.empty()) {
            FatSession s;
            s.session_start = std::chrono::system_clock::time_point{};
            s.all_files = ungrouped;
            return {s};
        }
        return {};
    }

    // Sort by creation time
    std::sort(checkpoints.begin(), checkpoints.end(),
              [](const CheckpointFile& a, const CheckpointFile& b) {
                  return a.created < b.created;
              });

    // Group by time gaps: gap between prev file's mtime and next file's created time
    std::vector<FatSession> sessions;
    FatSession current;
    current.session_start = checkpoints[0].created;
    current.all_files.push_back(checkpoints[0].entry);
    int size_kb = static_cast<int>((checkpoints[0].entry->size + 1023) / 1024);
    current.checkpoint_sizes_kb[checkpoints[0].entry->name] = size_kb;

    for (size_t i = 1; i < checkpoints.size(); ++i) {
        auto gap = checkpoints[i].created - checkpoints[i-1].modified;
        auto gap_minutes = std::chrono::duration_cast<std::chrono::minutes>(gap).count();

        if (gap_minutes > 60) {
            // New session — push previous
            sessions.push_back(std::move(current));
            current = FatSession{};
            current.session_start = checkpoints[i].created;
        }

        current.all_files.push_back(checkpoints[i].entry);
        int kb = static_cast<int>((checkpoints[i].entry->size + 1023) / 1024);
        current.checkpoint_sizes_kb[checkpoints[i].entry->name] = kb;
    }
    // Add ungrouped files to last session
    for (auto* uf : ungrouped) {
        current.all_files.push_back(uf);
    }

    sessions.push_back(std::move(current));

    // Match CSL/EVE to sessions (same logic as SessionDiscoveryService:
    // match to last session, or by timestamp within 12 hours)
    for (auto* sf : summary_files) {
        auto sf_time = parseTimestamp(sf->name);

        // Try last session first
        if (!sessions.empty()) {
            auto& last = sessions.back();
            auto diff = std::chrono::abs(sf_time - last.session_start);
            if (diff < std::chrono::hours(12)) {
                last.all_files.push_back(sf);
                continue;
            }
        }

        // Try each session by time proximity
        for (auto& sess : sessions) {
            auto diff = std::chrono::abs(sf_time - sess.session_start);
            if (diff < std::chrono::hours(12)) {
                sess.all_files.push_back(sf);
                break;
            }
        }
    }

    return sessions;
}

bool FysetcSectorCollectorService::syncFile(const std::string& date_folder,
                                             const Fat32DirEntry& entry) {
    std::string rel_path = "DATALOG/" + date_folder + "/" + entry.name;

    // Use archive file size for byte-level resume offset
    uint32_t archived_size = 0;
    std::string full_path = archive_dir_ + "/" + rel_path;
    try {
        if (std::filesystem::exists(full_path)) {
            archived_size = static_cast<uint32_t>(std::filesystem::file_size(full_path));
        }
    } catch (...) {}

    uint32_t fat_size = entry.size;

    if (archived_size >= fat_size) return true;

    uint32_t offset = archived_size;
    uint32_t bytes_needed = fat_size - offset;

    if (bytes_needed == 0) return true;

    auto ranges = fat_->fileSectorRanges(entry.first_cluster, fat_size, offset);
    if (ranges.empty()) return true;

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
            std::cerr << "FysetcCollector: Sector read failed for " << rel_path << std::endl;
            return false;
        }

        file_data.insert(file_data.end(), sector_data.begin(), sector_data.end());
        i = batch_end;
    }

    if (file_data.size() > bytes_needed) {
        file_data.resize(bytes_needed);
    }

    if (!writeFileData(rel_path, file_data, offset)) return false;

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
        f.open(full_path, std::ios::binary | std::ios::out);
        if (!f.is_open()) return false;
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

    if (!scanDatalogDir()) return result;

    std::set<std::string> updated_folders;

    for (size_t i = 0; i < datalog_entries_.size(); ++i) {
        auto& date_entry = datalog_entries_[i];
        if (!date_entry.is_directory) continue;

        auto files = fat_->listDir(date_entry.first_cluster);

        // Group files into sessions using mtime-based gap detection
        auto sessions = groupIntoSessions(files);

        for (auto& session : sessions) {
            // Step 1: Check if session exists in DB
            bool exists = db_ && db_->sessionExists(device_id_, session.session_start);

            if (!exists) {
                // New session — download all files
                for (auto* file : session.all_files) {
                    if (syncFile(date_entry.name, *file)) {
                        result.new_files++;
                        result.bytes_received += file->size;
                        updated_folders.insert(date_entry.name);
                    }
                }
                continue;
            }

            // Step 2: Compare BRP/PLD/SAD sizes against DB checkpoints
            auto db_sizes = db_->getCheckpointFileSizes(device_id_, session.session_start);

            bool any_changed = false;

            for (auto& [name, current_kb] : session.checkpoint_sizes_kb) {
                auto it = db_sizes.find(name);
                if (it == db_sizes.end()) {
                    // New checkpoint file — session is active
                    any_changed = true;
                    break;
                }
                if (std::abs(it->second - current_kb) > 1) {
                    // Size changed beyond rounding tolerance — session is active
                    any_changed = true;
                    break;
                }
            }

            // Check for new checkpoint files not yet in DB
            if (!any_changed && session.checkpoint_sizes_kb.size() > db_sizes.size()) {
                any_changed = true;
            }

            if (!any_changed) continue;  // session completed, skip everything

            // Step 3: Session is active — download changed files
            for (auto* file : session.all_files) {
                std::string name_lower = file->name;
                std::transform(name_lower.begin(), name_lower.end(),
                              name_lower.begin(), ::tolower);

                if (isCheckpointFile(name_lower)) {
                    // BRP/PLD/SAD: check if this specific file changed
                    auto it = db_sizes.find(file->name);
                    int file_kb = static_cast<int>((file->size + 1023) / 1024);

                    if (it != db_sizes.end() && std::abs(it->second - file_kb) <= 1) {
                        continue;  // this checkpoint unchanged, skip
                    }

                    // Changed or new — download delta (syncFile uses archive offset)
                    if (syncFile(date_entry.name, *file)) {
                        result.updated_files++;
                        result.bytes_received += file->size;
                        updated_folders.insert(date_entry.name);
                    }
                } else {
                    // CSL/EVE — session is active, re-download in full
                    if (syncFile(date_entry.name, *file)) {
                        result.updated_files++;
                        result.bytes_received += file->size;
                        updated_folders.insert(date_entry.name);
                    }
                }
            }
        }
    }

    // Sync STR.edf from root
    for (auto& e : root_entries_) {
        std::string name_lower = e.name;
        std::transform(name_lower.begin(), name_lower.end(),
                      name_lower.begin(), ::tolower);
        if (name_lower != "str.edf") continue;

        uint32_t archived_size = 0;
        {
            std::string str_path = archive_dir_ + "/" + e.name;
            try {
                if (std::filesystem::exists(str_path))
                    archived_size = static_cast<uint32_t>(std::filesystem::file_size(str_path));
            } catch (...) {}
        }
        if (e.size <= archived_size) break;

        uint32_t offset = archived_size;
        auto ranges = fat_->fileSectorRanges(e.first_cluster, e.size, offset);
        if (ranges.empty()) break;

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
            writeFileData(e.name, data, offset);
            result.updated_files++;
        }

        break;
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
