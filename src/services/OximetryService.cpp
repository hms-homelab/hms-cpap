#include "services/OximetryService.h"
#include <cpapdash/parser/VLDParser.h>
#include <iostream>
#include <iomanip>
#include <sstream>

namespace hms_cpap {

using VLDParser = cpapdash::parser::VLDParser;

OximetryService::OximetryService(std::shared_ptr<IO2RingClient> client,
                                 std::shared_ptr<IDatabase> db)
    : client_(std::move(client)),
      db_(std::move(db)) {}

bool OximetryService::collectAndPublish() {
    // Step 1: List available files
    auto files = client_->listFiles();
    if (files.empty()) {
        std::cout << "O2Ring: No files available" << std::endl;
        return false;
    }

    std::cout << "O2Ring: " << files.size() << " file(s) on device"
              << " (battery: " << client_->getBattery() << "%)" << std::endl;

    // Step 2: Filter out already-processed files
    bool any_new = false;
    for (const auto& filename : files) {
        // Skip if already processed in this session
        if (processed_files_.count(filename)) {
            continue;
        }

        // Skip if already in DB
        // Use a fixed device_id for the O2 Ring
        std::string device_id = "o2ring";
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

        auto session = VLDParser::parse(data.data(), data.size(), filename);
        if (!session) {
            std::cerr << "O2Ring: Failed to parse " << filename << std::endl;
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
