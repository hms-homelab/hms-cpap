#include "services/OximetryImport.h"

#include "database/IDatabase.h"
#include "services/RemovedNights.h"
#include "utils/OximetryDevice.h"

#include <cpapdash/parser/VLDParser.h>

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <system_error>
#include <vector>

namespace hms_cpap {

namespace fs = std::filesystem;

namespace {

std::string lower(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return s;
}

/// The card's own folders. Its ring files are never in them, and DATALOG holds
/// thousands of EDFs a pass should not stat every burst.
bool isCardFolder(const std::string& name) {
    const auto n = lower(name);
    return n == "datalog" || n == "settings";
}

/// The .vld files directly inside [dir], one level, sorted so a pass imports
/// in a stable order.
std::vector<fs::path> vldFilesIn(const fs::path& dir) {
    std::vector<fs::path> out;
    std::error_code ec;
    for (fs::directory_iterator it(dir, ec), end; !ec && it != end; it.increment(ec)) {
        std::error_code fe;
        if (it->is_regular_file(fe) && isVldFilename(it->path().filename().string()))
            out.push_back(it->path());
    }
    std::sort(out.begin(), out.end());
    return out;
}

}  // namespace

bool isVldFilename(const std::string& name) {
    const auto n = lower(name);
    return n.size() > 4 && n.compare(n.size() - 4, 4, ".vld") == 0;
}

VldImportResult importVldFile(IDatabase& db, const std::string& bytes,
                              const std::string& filename,
                              const std::set<std::string>& removed_nights) {
    VldImportResult r;
    auto session = cpapdash::parser::VLDParser::parse(
        reinterpret_cast<const uint8_t*>(bytes.data()), bytes.size(), filename);
    if (!session || session->samples.empty()) {
        r.error = "Not a readable O2 Ring .vld file: " + filename;
        return r;
    }
    if (isRemovedOximetryNight(removed_nights, session->start_time)) {
        r.removed_night = true;
        r.error = "On a removed night: " + filename;
        return r;
    }
    // The name the live path would have stored it under, so the same file
    // pulled off the ring later is the same row, not a second night.
    session->filename = filename;
    if (!db.saveOximetrySession(kOximetryDeviceId, *session)) {
        r.error = "Failed to save oximetry session " + filename;
        return r;
    }
    r.ok              = true;
    r.samples         = static_cast<int>(session->samples.size());
    r.valid_samples   = session->metrics.valid_samples;
    r.avg_spo2        = session->metrics.avg_spo2;
    r.min_spo2        = session->metrics.min_spo2;
    r.sample_interval = session->sample_interval;
    r.duration_seconds = static_cast<int>(session->duration_seconds);
    return r;
}

VldFolderScan importVldFolder(IDatabase& db, const std::string& card_root,
                              std::set<std::string>& refused,
                              const std::set<std::string>& removed_nights) {
    VldFolderScan scan;
    if (card_root.empty()) return scan;
    const fs::path root(card_root);
    std::error_code ec;
    if (!fs::is_directory(root, ec)) return scan;

    // D1: the root, then each folder directly under it except the card's own.
    std::vector<fs::path> files = vldFilesIn(root);
    std::vector<fs::path> dirs;
    for (fs::directory_iterator it(root, ec), end; !ec && it != end; it.increment(ec)) {
        std::error_code de;
        if (it->is_directory(de) && !isCardFolder(it->path().filename().string()))
            dirs.push_back(it->path());
    }
    std::sort(dirs.begin(), dirs.end());
    for (const auto& d : dirs) {
        auto more = vldFilesIn(d);
        files.insert(files.end(), more.begin(), more.end());
    }

    for (const auto& p : files) {
        const std::string name = p.filename().string();
        if (refused.count(p.string()) || db.oximetrySessionExists(kOximetryDeviceId, name)) {
            ++scan.skipped;
            continue;
        }
        std::ifstream in(p, std::ios::binary);
        const std::string bytes((std::istreambuf_iterator<char>(in)),
                                std::istreambuf_iterator<char>());
        const auto r = importVldFile(db, bytes, name, removed_nights);
        if (r.ok) {
            ++scan.imported;
            std::cout << "O2Ring: imported " << p.string() << " (" << r.samples
                      << " samples, avg SpO2 " << r.avg_spo2 << "%)" << std::endl;
        } else if (r.removed_night) {
            ++scan.skipped;  // quiet, and not remembered: a Reparse may restore it
        } else {
            ++scan.refused;
            refused.insert(p.string());
            std::cerr << "O2Ring: " << r.error << " (" << p.string()
                      << "), not retried until restart" << std::endl;
        }
    }
    return scan;
}

}  // namespace hms_cpap
