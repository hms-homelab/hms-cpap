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

/// Size and modified time, as the listing shows them.
VldFileSig sigOf(const fs::path& p) {
    VldFileSig s;
    std::error_code ec;
    const auto size = fs::file_size(p, ec);
    if (!ec) s.size = size;
    const auto t = fs::last_write_time(p, ec);
    if (!ec) s.mtime = static_cast<long long>(t.time_since_epoch().count());
    return s;
}

/// Folders directly inside [dir], sorted.
std::vector<fs::path> subfoldersIn(const fs::path& dir) {
    std::vector<fs::path> out;
    std::error_code ec;
    for (fs::directory_iterator it(dir, ec), end; !ec && it != end; it.increment(ec)) {
        std::error_code de;
        if (it->is_directory(de)) out.push_back(it->path());
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
                              VldScanState& state,
                              const std::set<std::string>& removed_nights) {
    VldFolderScan scan;
    if (card_root.empty()) return scan;
    const fs::path root(card_root);
    const std::string where = "O2Ring: card folder " + card_root + ": ";
    std::error_code ec;

    if (!fs::is_directory(root, ec)) {
        scan.summary = where + "not a folder, nothing scanned";
    } else {
        // D1 (amended, SDD-028 §7): the root, each folder directly under it
        // except the card's own, and each folder inside those (a tool that
        // files every night in its own folder, OXYMETRY/20260913/). No deeper.
        std::vector<fs::path> files = vldFilesIn(root);
        std::vector<fs::path> dirs;
        std::error_code le;
        for (fs::directory_iterator it(root, le), end; !le && it != end; it.increment(le)) {
            std::error_code de;
            if (it->is_directory(de) && !isCardFolder(it->path().filename().string()))
                dirs.push_back(it->path());
        }
        std::sort(dirs.begin(), dirs.end());
        std::vector<int> inner_count(dirs.size(), 0);   // folders searched inside each
        std::vector<int> deeper_count(dirs.size(), 0);  // folders below those, not searched
        for (size_t i = 0; i < dirs.size(); ++i) {
            auto more = vldFilesIn(dirs[i]);
            files.insert(files.end(), more.begin(), more.end());
            const auto inner = subfoldersIn(dirs[i]);
            inner_count[i] = static_cast<int>(inner.size());
            for (const auto& sub : inner) {
                auto deeper = vldFilesIn(sub);
                files.insert(files.end(), deeper.begin(), deeper.end());
                deeper_count[i] += static_cast<int>(subfoldersIn(sub).size());
            }
        }

        int unreadable = 0;
        std::set<std::string> seen;
        for (const auto& p : files) {
            ++scan.found;
            const std::string key  = p.string();
            const std::string name = p.filename().string();
            const VldFileSig sig = sigOf(p);
            seen.insert(key);

            // Refused before: read it again only once it has changed.
            if (auto r = state.refused.find(key); r != state.refused.end()) {
                if (r->second == sig) { ++scan.skipped; ++unreadable; continue; }
                state.refused.erase(r);
            }

            // Stored already. Seen for the first time in this run: trusted as
            // it is. Seen before: read again if it changed since, which is the
            // file the other tool was still writing when a pass stored it.
            bool update = false;
            if (db.oximetrySessionExists(kOximetryDeviceId, name)) {
                auto known = state.stored.find(key);
                if (known == state.stored.end()) {
                    state.stored[key] = sig;
                    ++scan.skipped;
                    continue;
                }
                if (known->second == sig) { ++scan.skipped; continue; }
                update = true;
            }

            std::ifstream in(p, std::ios::binary);
            const std::string bytes((std::istreambuf_iterator<char>(in)),
                                    std::istreambuf_iterator<char>());
            const auto r = importVldFile(db, bytes, name, removed_nights);
            if (r.ok) {
                state.stored[key] = sig;
                if (update) ++scan.reimported; else ++scan.imported;
                std::cout << "O2Ring: " << (update ? "updated " : "imported ") << key
                          << (update ? " (it changed since it was stored, " : " (")
                          << r.samples << " samples, avg SpO2 " << r.avg_spo2 << "%)"
                          << std::endl;
            } else if (r.removed_night) {
                ++scan.skipped;  // quiet, and not remembered: a Reparse may restore it
            } else {
                state.refused[key] = sig;
                ++scan.refused;
                ++unreadable;
                std::cerr << "O2Ring: " << r.error << " (" << key
                          << "), read again when the file changes" << std::endl;
            }
        }

        // Forget files that are gone, so the maps do not grow without bound.
        for (auto* m : {&state.stored, &state.refused})
            for (auto it = m->begin(); it != m->end();)
                it = seen.count(it->first) ? std::next(it) : m->erase(it);

        // The one line that answers "did it look where my files are?".
        // A folder's own sub-folders are counted, not listed, so a tool that
        // adds one every night keeps this one short line.
        std::string places = "the root";
        for (size_t i = 0; i < dirs.size(); ++i) {
            places += ", " + dirs[i].filename().string() + "/";
            if (inner_count[i] > 0)
                places += " and its " + std::to_string(inner_count[i]) + " folder(s)";
        }
        scan.summary = where;
        if (le) scan.summary += "could not list it fully (" + le.message() + "); ";
        if (scan.found == 0)
            scan.summary += "no .vld files in " + places;
        else
            scan.summary += std::to_string(scan.found) + " .vld file(s) in " + places;
        scan.summary += " (DATALOG and SETTINGS are not searched)";
        for (size_t i = 0; i < dirs.size(); ++i)
            if (deeper_count[i] > 0)
                scan.summary += "; " + std::to_string(deeper_count[i]) + " folder(s) further down in " +
                                dirs[i].filename().string() + "/ are not searched";
        if (unreadable > 0)
            scan.summary += "; " + std::to_string(unreadable) +
                            " unreadable, read again when they change";
    }

    if (scan.summary != state.last_summary) {
        std::cout << scan.summary << std::endl;
        state.last_summary = scan.summary;
        scan.summary_logged = true;
    }
    return scan;
}

}  // namespace hms_cpap
