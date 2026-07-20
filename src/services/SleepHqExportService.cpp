#include "services/SleepHqExportService.h"
#include "services/SleepHqClient.h"
#include "utils/AppConfig.h"
#include "utils/ConfigManager.h"

#include <algorithm>
#include <filesystem>
#include <thread>
#include <iostream>

namespace hms_cpap {

namespace {
constexpr int kMaxAttempts = 8;

std::chrono::minutes backoffAfter(int failures) {
    // 5, 10, 20, 40, 60, 60, ... minutes
    int mins = 5 * (1 << std::min(failures - 1, 4));
    return std::chrono::minutes(std::min(mins, 60));
}
} // namespace

SleepHqExportService& SleepHqExportService::getInstance() {
    static SleepHqExportService instance;
    return instance;
}

static bool sleephqReady(const AppConfig* cfg) {
    return cfg && cfg->sleephq.enabled && !cfg->sleephq.client_id.empty();
}

std::map<std::string, std::uintmax_t> SleepHqExportService::scanFolder(
    const std::string& archive_base, const std::string& folder) {
    namespace fs = std::filesystem;
    std::map<std::string, std::uintmax_t> snap;
    std::error_code ec;
    fs::path dir = fs::path(archive_base) / "DATALOG" / folder;
    if (!fs::exists(dir, ec)) return snap;
    for (const auto& e : fs::directory_iterator(dir, ec)) {
        if (!e.is_regular_file(ec)) continue;
        snap[e.path().filename().string()] = e.file_size(ec);
    }
    return snap;
}

void SleepHqExportService::markDirty(const std::string& date_folder) {
    if (!sleephqReady(config_) || date_folder.empty()) return;
    std::lock_guard<std::mutex> lock(mu_);
    auto& st = dirty_[date_folder];
    st.last_change = std::chrono::steady_clock::now();
}

bool SleepHqExportService::markDirtyIfNotExported(const std::string& date_folder) {
    if (!sleephqReady(config_) || date_folder.empty()) return false;

    // Deliberately NOT folded into markDirty(): an explicit markDirty() means a
    // caller decided this night must ship, and re-exporting on demand is
    // established behaviour (SleepHqExportServiceTest.RedirtyAfterSuccessReexports
    // pins it). This variant is for the sessionless disk-walk, which re-offers the
    // same folders every burst cycle and would otherwise re-upload an unchanged
    // night forever. Content that genuinely changed still queues.
    const std::string archive_base = ConfigManager::get("CPAP_ARCHIVE_DIR", "");
    if (!archive_base.empty()) {
        auto current = scanFolder(archive_base, date_folder);
        std::lock_guard<std::mutex> lock(mu_);
        auto done = exported_.find(date_folder);
        if (done != exported_.end() && done->second == current) return false;
        if (dirty_.count(date_folder)) return false;   // already queued
    }

    std::lock_guard<std::mutex> lock(mu_);
    auto& st = dirty_[date_folder];
    st.last_change = std::chrono::steady_clock::now();
    return true;
}

void SleepHqExportService::sweep(std::chrono::steady_clock::time_point now) {
    if (!sleephqReady(config_)) return;

    std::string archive_base = ConfigManager::get("CPAP_ARCHIVE_DIR", "");
    if (archive_base.empty()) {
        std::lock_guard<std::mutex> lock(mu_);
        if (!dirty_.empty())
            std::cerr << "[sleephq] CPAP_ARCHIVE_DIR not set; "
                      << dirty_.size() << " pending export(s) blocked" << std::endl;
        return;
    }

    int quiet_minutes = std::max(1, config_->sleephq.quiet_minutes);
    auto quiet = std::chrono::minutes(quiet_minutes);

    std::string candidate;
    std::map<std::string, std::uintmax_t> pre_snapshot;
    {
        std::lock_guard<std::mutex> lock(mu_);
        for (auto& [folder, st] : dirty_) {
            auto snap = scanFolder(archive_base, folder);
            if (snap != st.snapshot) {
                // Still settling (growing session, late EVE/CSL/STR, archive
                // catch-up) — restart the quiet window.
                st.snapshot = std::move(snap);
                st.last_change = now;
                continue;
            }
            if (export_in_flight_) continue;          // one upload at a time
            if (now < st.next_attempt) continue;      // failure backoff
            if (now - st.last_change < quiet) continue;
            candidate = folder;
            pre_snapshot = st.snapshot;
            export_in_flight_ = true;
            break;
        }
    }
    if (candidate.empty()) return;

    std::cout << "[sleephq] " << candidate << " quiet for " << quiet_minutes
              << "m — exporting" << std::endl;

    if (export_hook_) {
        bool ok = export_hook_(candidate);
        finishExport(candidate, ok, std::move(pre_snapshot), now);
        return;
    }
    std::thread([this, candidate, pre = std::move(pre_snapshot)]() mutable {
        bool ok = exportDate(candidate);
        finishExport(candidate, ok, std::move(pre),
                     std::chrono::steady_clock::now());
    }).detach();
}

void SleepHqExportService::finishExport(
    const std::string& folder, bool ok,
    std::map<std::string, std::uintmax_t> pre_snapshot,
    std::chrono::steady_clock::time_point now) {
    std::string archive_base = ConfigManager::get("CPAP_ARCHIVE_DIR", "");
    auto current = scanFolder(archive_base, folder);

    std::lock_guard<std::mutex> lock(mu_);
    export_in_flight_ = false;
    auto it = dirty_.find(folder);
    if (it == dirty_.end()) return;

    if (ok) {
        if (current == pre_snapshot) {
            // Remember exactly what we shipped. markDirty() consults this so a
            // night already uploaded is not uploaded again just because a second
            // trigger fired for it — which is now possible, since a folder can be
            // marked both by the parsed-session path and by the sessionless
            // disk-walk fallback. Content that genuinely changes still re-exports.
            exported_[folder] = current;
            dirty_.erase(it);
            return;
        }
        // Files landed while uploading — keep dirty so the complete folder
        // re-exports after the next quiet window.
        it->second.snapshot = std::move(current);
        it->second.last_change = now;
        it->second.failures = 0;
        it->second.next_attempt = {};
        std::cout << "[sleephq] " << folder
                  << " changed during upload; will re-export" << std::endl;
        return;
    }

    it->second.failures++;
    if (it->second.failures >= kMaxAttempts) {
        std::cerr << "[sleephq] giving up on " << folder << " after "
                  << it->second.failures << " failed attempts" << std::endl;
        dirty_.erase(it);
        return;
    }
    it->second.next_attempt = now + backoffAfter(it->second.failures);
    std::cerr << "[sleephq] export of " << folder << " failed (attempt "
              << it->second.failures << "); will retry" << std::endl;
}

void SleepHqExportService::setExportHookForTest(
    std::function<bool(const std::string&)> hook) {
    export_hook_ = std::move(hook);
}

void SleepHqExportService::resetForTest() {
    std::lock_guard<std::mutex> lock(mu_);
    dirty_.clear();
    export_in_flight_ = false;
    export_hook_ = nullptr;
}

bool SleepHqExportService::isDirtyForTest(const std::string& date_folder) {
    std::lock_guard<std::mutex> lock(mu_);
    return dirty_.count(date_folder) > 0;
}

void SleepHqExportService::exportDateAsync(const std::string& date_folder) {
    if (!sleephqReady(config_) || date_folder.empty()) return;
    std::string d = date_folder;
    std::thread([this, d]() { exportDate(d); }).detach();
}

bool SleepHqExportService::exportDate(const std::string& date_folder) {
    std::string archive_base = ConfigManager::get("CPAP_ARCHIVE_DIR", "");
    if (archive_base.empty()) {
        std::cerr << "[sleephq] CPAP_ARCHIVE_DIR not set; cannot export " << date_folder << std::endl;
        return false;
    }
    return exportFolder(date_folder, archive_base + "/DATALOG/" + date_folder, archive_base);
}

void SleepHqExportService::exportFolderAsync(const std::string& date_folder,
                                             const std::string& datalog_dir,
                                             const std::string& root_dir) {
    if (!sleephqReady(config_) || date_folder.empty()) return;
    std::string d = date_folder, dl = datalog_dir, rd = root_dir;
    std::thread([this, d, dl, rd]() { exportFolder(d, dl, rd); }).detach();
}

std::vector<SleepHqExportService::ExportFile>
SleepHqExportService::collectExportFiles(const std::string& date_folder,
                                         const std::string& datalog_dir,
                                         const std::string& root_dir) {
    namespace fs = std::filesystem;
    std::vector<ExportFile> out;
    std::error_code ec;

    // The night's own files, under their card-accurate DATALOG/<date> path.
    if (fs::exists(datalog_dir, ec)) {
        for (const auto& e : fs::directory_iterator(datalog_dir, ec)) {
            if (!e.is_regular_file()) continue;
            out.push_back({e.path().filename().string(),
                           "DATALOG/" + date_folder,
                           e.path().string()});
        }
    }

    // Card root files, uploaded to the import ROOT (import_path ""), so SleepHQ
    // sees the same layout a real SD card has. Without STR.edf the import has no
    // therapy summary, and without Identification.* it has no machine — SleepHQ
    // then processes it into nothing visible. root_dir MUST therefore be the card
    // root, not the DATALOG directory.
    for (const char* name : {"STR.edf", "Identification.tgt",
                             "Identification.json", "Identification.crc"}) {
        std::string p = root_dir + "/" + name;
        if (!fs::exists(p, ec)) continue;
        out.push_back({name, "", p});
    }
    return out;
}

bool SleepHqExportService::exportFolder(const std::string& date_folder,
                                        const std::string& datalog_dir,
                                        const std::string& root_dir) {
    namespace fs = std::filesystem;
    if (!sleephqReady(config_)) return true;

    SleepHqClient client(config_->sleephq.client_id, config_->sleephq.client_secret);
    std::string err;
    if (!client.connect(err)) { std::cerr << "[sleephq] connect failed: " << err << std::endl; return false; }

    std::string import_id = client.createImport(err);
    if (import_id.empty()) { std::cerr << "[sleephq] createImport failed: " << err << std::endl; return false; }

    int count = 0;
    for (const auto& f : collectExportFiles(date_folder, datalog_dir, root_dir)) {
        if (!client.uploadFile(import_id, f.name, f.import_path, f.local_path, err)) {
            std::cerr << "[sleephq] " << err << std::endl; return false;
        }
        ++count;
    }

    if (count == 0) { std::cerr << "[sleephq] no files to export for " << date_folder << std::endl; return false; }
    if (!client.processFiles(import_id, err)) { std::cerr << "[sleephq] " << err << std::endl; return false; }
    std::cout << "[sleephq] exported " << count << " files for " << date_folder
              << " (import " << import_id << ")" << std::endl;
    return true;
}

} // namespace hms_cpap
