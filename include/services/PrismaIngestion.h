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

    bool detectAndExtractZips();
    bool findTherapyRoot(const std::string& base);

    static bool extractZip(const std::string& zip_path, const std::string& dest_dir);
};

} // namespace hms_cpap
