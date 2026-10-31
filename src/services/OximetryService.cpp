#include "services/OximetryService.h"
#include "services/RemovedNights.h"
#include "utils/OximetryDevice.h"
#include <cpapdash/parser/O2RingSParser.h>
#include <cpapdash/parser/VLDParser.h>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iomanip>
#include <iterator>
#include <sstream>

namespace hms_cpap {

using VLDParser = cpapdash::parser::VLDParser;
using cpapdash::parser::parseOximetryFile;

OximetryService::OximetryService(std::shared_ptr<IO2RingClient> client,
                                 std::shared_ptr<IDatabase> db,
                                 std::string cpap_device_id)
    : client_(std::move(client)),
      db_(std::move(db)),
      cpap_device_id_(std::move(cpap_device_id)) {}

bool OximetryService::retainRawFile(const std::string& archive_dir, const std::string& filename,
                                    const std::vector<uint8_t>& bytes) {
    namespace fs = std::filesystem;
    // Only the final component: a name from the device must not leave DATALOG.
    const std::string name = fs::path(filename).filename().string();
    if (archive_dir.empty() || name.empty() || name == "." || name == ".." || bytes.empty())
        return false;
    std::error_code ec;
    const fs::path dir = fs::path(archive_dir) / "DATALOG";
    fs::create_directories(dir, ec);
    if (ec) return false;
    const fs::path dest = dir / name;
    if (fs::is_regular_file(dest, ec) && fs::file_size(dest, ec) == bytes.size()) {
        std::ifstream cur(dest, std::ios::binary);
        std::vector<uint8_t> have((std::istreambuf_iterator<char>(cur)),
                                  std::istreambuf_iterator<char>());
        if (have == bytes) return true;   // identical, nothing to write
    }
    const fs::path tmp = dir / (name + ".part");
    {
        std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
        out.write(reinterpret_cast<const char*>(bytes.data()),
                  static_cast<std::streamsize>(bytes.size()));
        if (!out) { out.close(); fs::remove(tmp, ec); return false; }
    }
    fs::rename(tmp, dest, ec);
    if (ec) { fs::remove(tmp, ec); return false; }
    return true;
}

bool OximetryService::collectAndPublish() {
    // Step 1: List available files
    auto files = client_->listFiles();
    if (files.empty()) {
        std::cout << "O2Ring: No files available" << std::endl;
        return false;
    }

    std::cout << "O2Ring: " << files.size() << " file(s) on device"
              << " (battery: " << client_->getBattery() << "%)" << std::endl;

    // SDD-029: the nights an operator removed, once per poll.
    const auto removed = cpap_device_id_.empty()
        ? std::set<std::string>{}
        : removedNightSet(*db_, cpap_device_id_);

    // Step 2: Filter out already-processed files
    bool any_new = false;
    for (const auto& filename : files) {
        // Skip if already processed in this session
        if (processed_files_.count(filename)) {
            continue;
        }

        // Known to be on a removed night: no download while it stays removed.
        if (auto it = removed_files_.find(filename); it != removed_files_.end()) {
            if (removed.count(it->second)) continue;
            removed_files_.erase(it);  // restored since; fetch it again
        }

        // Skip if already in DB
        // Use a fixed device_id for the O2 Ring
        std::string device_id = kOximetryDeviceId;
        if (db_->oximetrySessionExists(device_id, filename)) {
            processed_files_.insert(filename);
            continue;
        }

        // Step 3: Download and parse
        std::cout << "O2Ring: Processing new file: " << filename << std::endl;

        auto data = client_->downloadFile(filename);
        if (data.empty()) {
            std::cerr << "O2Ring: Failed to download " << filename << std::endl;
            continue;
        }

        // Either ring's format, chosen by the bytes: the original O2Ring's .vld
        // or the O2Ring-S's own, whose start time is its file name.
        auto session = parseOximetryFile(data.data(), data.size(), filename);
        if (!session) {
            std::cerr << "O2Ring: Failed to parse " << filename << std::endl;
            continue;
        }

        if (isRemovedOximetryNight(removed, session->start_time)) {
            const auto night = oximetryNightOf(session->start_time);
            removed_files_[filename] = night;
            std::cout << "O2Ring: " << filename << " is on removed night " << night
                      << ", not stored" << std::endl;
            continue;
        }

        std::cout << "O2Ring: Parsed " << filename
                  << " - " << session->samples.size() << " samples"
                  << ", " << session->duration_seconds << "s"
                  << ", avg SpO2 " << std::fixed << std::setprecision(1)
                  << session->metrics.avg_spo2 << "%"
                  << ", avg HR " << session->metrics.avg_hr << " bpm"
                  << std::endl;

        // Step 4: Save to database
        if (db_->saveOximetrySession(device_id, session.value())) {
            processed_files_.insert(filename);
            any_new = true;
            std::cout << "O2Ring: Saved " << filename << " to database" << std::endl;
            if (archive_dir_) {
                const std::string dir = archive_dir_();
                if (!dir.empty()) {
                    if (retainRawFile(dir, filename, data))
                        std::cout << "O2Ring: Kept raw " << filename << " in " << dir
                                  << "/DATALOG" << std::endl;
                    else
                        std::cerr << "O2Ring: Could not keep raw " << filename << " in "
                                  << dir << "/DATALOG (non-fatal)" << std::endl;
                }
            }
        } else {
            std::cerr << "O2Ring: Failed to save " << filename << " to database" << std::endl;
        }
    }

    return any_new;
}

IO2RingClient::LiveReading OximetryService::pollLive() {
    last_live_ = client_->getLive();
    std::cout << "O2Ring: active=" << (last_live_.active ? "ON" : "OFF")
              << " SpO2=" << last_live_.spo2
              << " HR=" << last_live_.hr
              << " Motion=" << last_live_.motion << std::endl;
    return last_live_;
}

} // namespace hms_cpap
