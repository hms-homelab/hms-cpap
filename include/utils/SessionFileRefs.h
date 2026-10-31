#pragma once

#include "parsers/CpapdashBridge.h"
#include "utils/FileUtils.h"

#include <algorithm>
#include <filesystem>
#include <string>
#include <vector>

namespace hms_cpap {

/**
 * The files a parsed session is made of, as the database records them
 * (SDD-014): one cpap_session_files row per EDF in [session_dir], and the
 * first file of each kind on the session's own path columns.
 *
 * [rel_base] is what the rows are relative to, with its trailing slash:
 * "DATALOG/<date>/" for a card or the archive, "as11/<serial>/DATALOG/<date>/"
 * for a night hms-cpap wrote from the machine's Bluetooth stream (SDD-057).
 * The names are sorted first: directory_iterator order is unspecified, so
 * "first of each kind" would otherwise not be the same file twice.
 */
inline std::vector<SessionFileRef> recordSessionFiles(const std::string& session_dir,
                                                      const std::string& rel_base,
                                                      CPAPSession& parsed) {
    std::vector<SessionFileRef> refs;
    std::error_code ec;
    if (!std::filesystem::exists(session_dir, ec)) return refs;

    std::vector<std::string> names;
    for (const auto& entry : std::filesystem::directory_iterator(session_dir, ec))
        names.push_back(entry.path().filename().string());
    std::sort(names.begin(), names.end());

    for (const auto& filename : names) {
        const char* kind = nullptr;
        if (filename.find("_BRP.edf") != std::string::npos)      kind = "brp";
        else if (filename.find("_EVE.edf") != std::string::npos) kind = "eve";
        else if (isOximetryFile(filename))                        kind = "sad";
        else if (filename.find("_PLD.edf") != std::string::npos) kind = "pld";
        else if (filename.find("_CSL.edf") != std::string::npos) kind = "csl";
        if (!kind) continue;

        const std::string rel = rel_base + filename;
        refs.push_back({kind, rel});

        const std::string k(kind);
        if (k == "brp" && !parsed.brp_file_path.has_value()) parsed.brp_file_path = rel;
        else if (k == "eve" && !parsed.eve_file_path.has_value()) parsed.eve_file_path = rel;
        else if (k == "sad" && !parsed.sad_file_path.has_value()) parsed.sad_file_path = rel;
        else if (k == "pld" && !parsed.pld_file_path.has_value()) parsed.pld_file_path = rel;
        else if (k == "csl" && !parsed.csl_file_path.has_value()) parsed.csl_file_path = rel;
    }
    return refs;
}

}  // namespace hms_cpap
