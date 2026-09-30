#include "services/FysetcSectorCollectorService.h"
#include "clients/FysetcDataSource.h"
#include "services/SessionDiscoveryService.h"
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

// Check if a file is a checkpoint type (BRP/PLD/SAD/SA2)
static bool isCheckpointFile(const std::string& name_lower) {
    return name_lower.find("_brp.edf") != std::string::npos ||
           name_lower.find("_pld.edf") != std::string::npos ||
           name_lower.find("_sad.edf") != std::string::npos ||
           name_lower.find("_sa2.edf") != std::string::npos;
}

static std::string lowered(const std::string& s) {
    std::string out = s;
    std::transform(out.begin(), out.end(), out.begin(), ::tolower);
    return out;
}

// SDD-047: this used to be a grouping of its own, with an older rule (a flat
// "> 60 minutes" from one file's modified time to the next file's name) and
// its own EVE/CSL matching (the last session within 12 hours of the file's
// name, which assumes a pair per mask-on). The collector now asks the one
// grouping and syncs the files discovery lists for each session, so a folder
// streamed over sectors groups exactly as it would from any other source.
std::vector<FysetcSectorCollectorService::FatSession>
FysetcSectorCollectorService::groupIntoSessions(const std::vector<Fat32DirEntry>& files,
                                                const std::string& date_folder) {
    std::vector<EzShareFileEntry> listing;
    std::map<std::string, const Fat32DirEntry*> by_name;
    for (const auto& f : files) {
        if (f.is_directory) continue;
        by_name[f.name] = &f;
        listing.push_back(FysetcDataSource::listingEntry(f));
    }

    std::vector<FatSession> sessions;
    std::set<std::string> placed;
    for (const auto& set : SessionDiscoveryService::groupFiles(listing, date_folder)) {
        FatSession s;
        s.session_start = set.session_start;
        for (const auto* names : {&set.brp_files, &set.pld_files, &set.sad_files,
                                  &set.tcv_files, &set.csl_files, &set.eve_files}) {
            for (const auto& name : *names) {
                const auto it = by_name.find(name);
                if (it == by_name.end()) continue;
                s.all_files.push_back(it->second);
                placed.insert(name);
                if (isCheckpointFile(lowered(name)))
                    s.checkpoint_sizes_kb[name] =
                        static_cast<int>((it->second->size + 1023) / 1024);
            }
        }
        sessions.push_back(std::move(s));
    }

    // An .edf under a short 8.3 name carries no session prefix, so no grouping
    // can place it. It is still the card's, so it rides with the last session,
    // or on its own when the folder has no session at all.
    std::vector<const Fat32DirEntry*> unnamed;
    for (const auto& f : files) {
        if (f.is_directory || placed.count(f.name) || f.name.size() >= 15) continue;
        const std::string nl = lowered(f.name);
        if (nl.size() >= 4 && nl.substr(nl.size() - 4) == ".edf") unnamed.push_back(&f);
    }
    if (!unnamed.empty()) {
        if (sessions.empty()) sessions.push_back(FatSession{});
        auto& last = sessions.back().all_files;
        last.insert(last.end(), unnamed.begin(), unnamed.end());
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

        // The one grouping (SessionDiscoveryService::groupFiles).
        auto sessions = groupIntoSessions(files, date_entry.name);

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
