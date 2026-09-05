#pragma once

#include "parsers/CpapdashBridge.h"
#include <string>
#include <vector>
#include <optional>
#include <chrono>

namespace hms_cpap {

// Sefam S.Box card ingestion.
//
// Sibling of PrismaIngestion, and simpler in one way and harder in another. It
// needs no staging -- the parser reads a session in place -- but a Sefam card
// has two layouts and they disagree about what a folder holds:
//
//   <model><serial>/DATA_<n>/DATA_<n>.INI      one session per folder
//                                              (S.Box AUTO, 1263R)
//   <model><serial>/<YYMMDD>/<HHMMSS>.ini      one folder per DAY, holding every
//                                              recording that started that day
//                                              (SleepBox_AUTO, 1200R)
//
// So a session is identified by a folder AND a manifest name, not by a folder
// alone, and discovery walks for manifests rather than for session directories.

struct SefamSessionFile {
    std::string dir;    // the folder holding the manifest
    std::string stem;   // the manifest's name, without ".INI"
    std::chrono::system_clock::time_point session_start;
};

class SefamIngestion {
public:
    explicit SefamIngestion(const std::string& data_dir);

    // Walk the card for session manifests. False when there are none, which is
    // the honest answer for a folder that is not a Sefam card.
    bool initialize();

    // Sessions newer than last_session_start, oldest first.
    //
    // Reading a session's INI is cheap -- a couple of kilobytes of text -- so
    // discovery reads every manifest for its start time and never touches the
    // channel data. A card holding ten months of nights is walked in a moment.
    std::vector<SefamSessionFile> discoverSessions(
        std::optional<std::chrono::system_clock::time_point> last_session_start);

    // The device identity the card's manifests agree on, as the device writes
    // it: model code and serial run together, e.g. "1263R24337476".
    const std::string& deviceSerial() const { return serial_; }

    // The model string from [Create Info], e.g. "S.Box_AUTO".
    const std::string& deviceModel() const { return model_; }

    size_t sessionCount() const { return sessions_.size(); }

private:
    void walk(const std::string& dir, int depth);

    std::string data_dir_;
    std::string serial_;
    std::string model_;
    std::vector<SefamSessionFile> sessions_;
    bool initialized_ = false;
};

} // namespace hms_cpap
