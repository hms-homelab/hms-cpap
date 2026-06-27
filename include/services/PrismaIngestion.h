#pragma once

#include "parsers/CpapdashBridge.h"
#include <string>
#include <vector>
#include <optional>
#include <chrono>

namespace hms_cpap {

struct PrismaSessionFile {
    std::string date_folder;
    int sequence_number = 0;
    std::string event_path;
    std::string signal_path;
    std::chrono::system_clock::time_point session_start;
};

class PrismaIngestion {
public:
    // Extract a zip archive into dest_dir (preserving internal paths). Reused
    // by the CPAP zip upload endpoint.
    static bool extractZip(const std::string& zip_path, const std::string& dest_dir);

    explicit PrismaIngestion(const std::string& data_dir);

    bool initialize();

    std::vector<PrismaSessionFile> discoverSessions(
        std::optional<std::chrono::system_clock::time_point> last_session_start);

    std::string stageSession(const PrismaSessionFile& session);

    std::string deviceXmlPath() const;

private:
    std::string data_dir_;
    std::string therapy_dir_;
    std::string config_dir_;
    bool is_zip_mode_ = false;
    bool initialized_ = false;

    // Newer Prisma firmware (e.g. SMART max fw 3.17) drops the split events/ +
    // signals/ trees and instead nests everything under
    //   <serial>/<YYYYMMDD>/<NNNN>/{event_*.xml, signal_*.wmedf, trendCurves.tc}
    // combined_root_ points at the dir that directly holds the date folders.
    bool combined_layout_ = false;
    std::string combined_root_;

    bool detectAndExtractZips();
    bool findTherapyRoot(const std::string& base);
    bool findCombinedRoot(const std::string& base);
    std::vector<PrismaSessionFile> discoverCombined(
        const std::string& last_date,
        std::optional<std::chrono::system_clock::time_point> last_session_start);
};

} // namespace hms_cpap
