#include "services/SleepHqExportService.h"
#include "services/SleepHqClient.h"
#include "utils/AppConfig.h"
#include "utils/ConfigManager.h"
#include "utils/CardResidue.h"
#include "database/IDatabase.h"
#include "database/SqlDialect.h"
#include "services/O2RingCsvWriter.h"
#include "services/O2RingCsvParser.h"
#include "utils/OximetryDevice.h"
#include "utils/TimeCompat.h"

#include <algorithm>
#include <regex>
#include <set>
#include <cstdio>
#include <fstream>
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
                                         const std::string& root_dir,
                                         const std::vector<SessionFileRef>& recorded) {
    namespace fs = std::filesystem;
    std::vector<ExportFile> out;
    std::error_code ec;

    // The night's own files, under their card-accurate DATALOG/<date> path.
    //
    // cpap_session_files decides this when it has anything to say: a merged
    // night is several mask-on blocks and the database is what knows which
    // files belong to it. The directory walk stays as the fallback for nights
    // that were never recorded (see the header).
    if (!recorded.empty()) {
        std::set<std::string> taken;
        for (const auto& r : recorded) {
            fs::path rel(r.rel_path);
            fs::path local = fs::path(root_dir) / rel;
            if (!fs::exists(local, ec)) continue;
            // rel_path is card-accurate ("DATALOG/<date>/<name>"), so its parent
            // IS the import path and nothing has to be reconstructed.
            out.push_back({rel.filename().string(),
                           rel.parent_path().generic_string(),
                           local.string()});
            taken.insert(rel.filename().string());
        }

        // ...plus whatever else is in the date folder that the table does not
        // track. cpap_session_files records the five analytical kinds; the
        // night's .crc sidecars and anything else downloadDatalogResidue()
        // pulled in have NO other source, and SleepHQ wants the card as it is.
        // Without this the table branch would ship a night missing its
        // checksums, which the old directory walk always included.
        if (fs::exists(datalog_dir, ec)) {
            for (const auto& e : fs::directory_iterator(datalog_dir, ec)) {
                if (!e.is_regular_file(ec)) continue;
                const std::string name = e.path().filename().string();
                if (taken.count(name)) continue;
                auto sz = fs::file_size(e.path(), ec);
                if (ec) { ec.clear(); continue; }
                if (residualSkip(name, static_cast<uint64_t>(sz))) continue;
                out.push_back({name, "DATALOG/" + date_folder, e.path().string()});
            }
        }
    } else if (fs::exists(datalog_dir, ec)) {
        for (const auto& e : fs::directory_iterator(datalog_dir, ec)) {
            if (!e.is_regular_file()) continue;
            out.push_back({e.path().filename().string(),
                           "DATALOG/" + date_folder,
                           e.path().string()});
        }
    }

    // Everything else on the card, at its own relative path, so SleepHQ sees the
    // layout a real SD card has. Without STR.edf the import has no therapy
    // summary, and without Identification.* it has no machine — SleepHQ then
    // processes it into nothing visible. root_dir MUST therefore be the card
    // root, not the DATALOG directory.
    //
    // This used to be four hardcoded names, which dropped Journal.dat, SETTINGS/
    // and the .crc sidecars. It is now a walk under the same denylist the card
    // mirror uses -- and this is the one path that sends card contents to a third
    // party, so ezshare.cfg being on that denylist is load-bearing.
    if (fs::exists(root_dir, ec)) {
        for (auto it = fs::recursive_directory_iterator(root_dir, ec);
             it != fs::recursive_directory_iterator(); it.increment(ec)) {
            if (ec) break;
            if (it->is_directory(ec)) {
                // DATALOG is the session files' business, handled above.
                if (it->path().filename() == "DATALOG") it.disable_recursion_pending();
                continue;
            }
            if (!it->is_regular_file(ec)) continue;

            const std::string name = it->path().filename().string();

            // Session EDFs belong to the DATALOG branch above, and are named
            // YYYYMMDD_HHMMSS_XXX.edf. Skipping them by name rather than by
            // location is what keeps the misconfiguration visible: when root_dir
            // is wrongly pointed at DATALOG (or at a date folder inside it) the
            // walk finds no root files at all, which is the loud, testable
            // failure. Matching on location would silently re-export them as
            // root files and SleepHQ would accept a machine-less import.
            static const std::regex session_edf(
                R"(^\d{8}_\d{6}_[A-Za-z0-9]+\.edf$)", std::regex::icase);
            if (std::regex_match(name, session_edf)) continue;

            auto size = fs::file_size(it->path(), ec);
            if (ec) { ec.clear(); continue; }
            if (residualSkip(name, static_cast<uint64_t>(size))) continue;

            const fs::path rel = fs::relative(it->path(), root_dir, ec);
            if (ec) { ec.clear(); continue; }
            out.push_back({name, rel.parent_path().generic_string(), it->path().string()});
        }
    }
    return out;
}

namespace {

// "2026-06-19 23:20:29" as the engines return it, back to an instant.
//
// UTC, matching O2RingCsvParser: what is in the table was put there by that
// parser or by the ring intake, both of which work in UTC. Getting this wrong
// moves the whole night, which is exactly the bug the writer's round-trip test
// caught during development.
// executeQuery hands back whatever the engine gave it, and the engines do not
// agree: SQLite returns numeric columns as strings, Postgres and MySQL vary by
// column and cast. Reading them with asInt() throws "Value is not convertible
// to Int" on the string form, which is a runtime surprise rather than a compile
// one -- so every numeric read here goes through this.
int looseInt(const Json::Value& v, int fallback = 0) {
    if (v.isNull()) return fallback;
    if (v.isIntegral()) return v.asInt();
    if (v.isDouble()) return static_cast<int>(v.asDouble());
    if (v.isString()) {
        try { return std::stoi(v.asString()); } catch (...) { return fallback; }
    }
    return fallback;
}

// "20260619" -> "2026-06-19", which is what the sleep-day comparison wants.
std::string dashDate(const std::string& folder) {
    if (folder.size() != 8) return folder;
    return folder.substr(0, 4) + "-" + folder.substr(4, 2) + "-" + folder.substr(6, 2);
}

bool parseDbTimestamp(const std::string& s, std::chrono::system_clock::time_point& out) {
    int y = 0, mo = 0, d = 0, h = 0, mi = 0, sec = 0;
    if (std::sscanf(s.c_str(), "%d-%d-%d %d:%d:%d", &y, &mo, &d, &h, &mi, &sec) < 6) {
        // Postgres hands back an ISO 'T' separator depending on the cast.
        if (std::sscanf(s.c_str(), "%d-%d-%dT%d:%d:%d", &y, &mo, &d, &h, &mi, &sec) < 6)
            return false;
    }
    std::tm tm{};
    tm.tm_year = y - 1900; tm.tm_mon = mo - 1; tm.tm_mday = d;
    tm.tm_hour = h; tm.tm_min = mi; tm.tm_sec = sec;
    out = std::chrono::system_clock::from_time_t(timegm_utc(&tm));
    return true;
}

}  // namespace

cpapdash::parser::OximetrySession
SleepHqExportService::oximetrySessionFor(const std::string& date_folder) {
    cpapdash::parser::OximetrySession session;
    if (!db_) return session;

    // Every sample for the night, INCLUDING the unreadable ones. The chart
    // query filters `valid` and is right to; here a dropped row would heal the
    // timeline over and hand the interval detector the wrong cadence (SDD-015).
    const DbType dt = db_->dbType();
    const std::string q =
        "SELECT s.timestamp" + std::string(dt == DbType::POSTGRESQL ? "::text" : "") + " AS ts,"
        " s.spo2, s.heart_rate, s.motion"
        " FROM oximetry_sessions os"
        " JOIN oximetry_samples s ON s.oximetry_session_id = os.id"
        " WHERE os.device_id = " + sql::param(1, dt) +
        " AND " + sql::sleepDay("os.start_time", dt) + " = " + sql::castDate(2, dt) +
        " ORDER BY s.timestamp";

    Json::Value rows;
    try {
        rows = db_->executeQuery(q, {kOximetryDeviceId, dashDate(date_folder)});
    } catch (const std::exception& e) {
        std::cerr << "[sleephq] oximetry query failed for " << date_folder
                  << ": " << e.what() << std::endl;
        return session;
    }
    if (!rows.isArray() || rows.empty()) return session;   // no ring that night

    for (const auto& r : rows) {
        cpapdash::parser::OximetrySample smp{};
        if (!parseDbTimestamp(r.get("ts", "").asString(), smp.timestamp)) continue;
        const int sp = looseInt(r["spo2"]);
        const int hr = looseInt(r["heart_rate"]);
        smp.spo2         = (sp > 0 && sp <= 100) ? (uint8_t)sp : 0xFF;
        smp.heart_rate   = (hr > 0 && hr < 255)  ? (uint8_t)hr : 0xFF;
        smp.invalid_flag = (smp.spo2 == 0xFF) ? 1 : 0;
        smp.motion       = (uint8_t)std::min(255, std::max(0, looseInt(r["motion"])));
        session.samples.push_back(smp);
    }
    if (session.samples.empty()) return session;
    session.start_time = session.samples.front().timestamp;
    session.end_time   = session.samples.back().timestamp;
    return session;
}

bool SleepHqExportService::exportOximetry(const std::string& date_folder) {
    namespace fs = std::filesystem;

    const auto session = oximetrySessionFor(date_folder);
    if (session.samples.empty()) return true;    // no ring that night

    const std::string name = O2RingCsvWriter::filenameFor(session);
    const fs::path tmp = fs::temp_directory_path() / ("hms_oxi_" + date_folder + "_" + name);
    {
        // Derived data, so it lives in temp. Writing it into the card archive
        // would put a file on the card that was never on the card.
        std::ofstream f(tmp, std::ios::binary);
        if (!f) { std::cerr << "[sleephq] cannot write " << tmp << std::endl; return false; }
        f << O2RingCsvWriter::write(session);
    }

    std::string err;
    SleepHqClient client(config_->sleephq.client_id, config_->sleephq.client_secret);
    bool ok = false;
    if (!client.connect(err)) {
        std::cerr << "[sleephq] oximetry connect failed: " << err << std::endl;
    } else {
        const std::string import_id = client.createImport(err);
        if (import_id.empty()) {
            std::cerr << "[sleephq] oximetry createImport failed: " << err << std::endl;
        } else if (!client.uploadFile(import_id, name, "", tmp.string(), err)) {
            std::cerr << "[sleephq] oximetry upload failed: " << err << std::endl;
        } else if (!client.processFiles(import_id, err)) {
            std::cerr << "[sleephq] oximetry process failed: " << err << std::endl;
        } else {
            std::cout << "[sleephq] exported " << session.samples.size()
                      << " oximetry samples for " << date_folder
                      << " as " << name << " (import " << import_id << ")" << std::endl;
            ok = true;
        }
    }

    std::error_code ec;
    fs::remove(tmp, ec);
    return ok;
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

    // What the database says this night is made of. Empty is fine and means the
    // walk decides instead (see collectExportFiles).
    std::vector<SessionFileRef> recorded;
    if (db_ && config_ && !config_->device_id.empty()) {
        recorded = db_->getSessionFilesForDateFolder(config_->device_id, date_folder);
    }

    int count = 0;
    for (const auto& f : collectExportFiles(date_folder, datalog_dir, root_dir, recorded)) {
        if (!client.uploadFile(import_id, f.name, f.import_path, f.local_path, err)) {
            std::cerr << "[sleephq] " << err << std::endl; return false;
        }
        ++count;
    }

    if (count == 0) { std::cerr << "[sleephq] no files to export for " << date_folder << std::endl; return false; }
    if (!client.processFiles(import_id, err)) { std::cerr << "[sleephq] " << err << std::endl; return false; }

    // The ring's night, if there is one, goes up as its own import (SDD-015).
    // Its result deliberately does NOT decide this function's: the therapy data
    // is already in SleepHQ by this point, and reporting the night as failed
    // would re-upload all of it on the next sweep to retry one CSV.
    exportOximetry(date_folder);
    std::cout << "[sleephq] exported " << count << " files for " << date_folder
              << " (import " << import_id << ")" << std::endl;
    return true;
}

} // namespace hms_cpap
