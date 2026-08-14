#include "services/SessionDiscoveryService.h"
#include "utils/ConfigManager.h"
#include "utils/FileUtils.h"
#include <algorithm>
#include <regex>
#include <map>
#include <iostream>
#include <filesystem>

namespace hms_cpap {

SessionDiscoveryService::SessionDiscoveryService(IDataSource& data_source)
    : data_source_(data_source) {}

std::string SessionDiscoveryService::extractSessionPrefix(const std::string& filename) {
    // Extract YYYYMMDD_HHMMSS from "20260204_001809_CSL.edf"
    std::regex prefix_regex(R"(^(\d{8}_\d{6})_)");
    std::smatch match;
    if (std::regex_search(filename, match, prefix_regex)) {
        return match[1].str();
    }
    return "";
}

std::chrono::system_clock::time_point
SessionDiscoveryService::parseSessionTime(const std::string& prefix) {
    // Parse "20260204_001809" → 2026-02-04 00:18:09
    if (prefix.size() != 15) {
        return {};  // Invalid format
    }

    std::tm tm = {};
    tm.tm_year = std::stoi(prefix.substr(0, 4)) - 1900;
    tm.tm_mon  = std::stoi(prefix.substr(4, 2)) - 1;
    tm.tm_mday = std::stoi(prefix.substr(6, 2));
    tm.tm_hour = std::stoi(prefix.substr(9, 2));
    tm.tm_min  = std::stoi(prefix.substr(11, 2));
    tm.tm_sec  = std::stoi(prefix.substr(13, 2));
    tm.tm_isdst = -1;

    return std::chrono::system_clock::from_time_t(std::mktime(&tm));
}

// Estimate when a checkpoint file STOPPED being written. Session gaps must be
// measured end-to-start (ResMed closes a session ~1h after the last file is
// CLOSED): the filename prefix only gives the block's START, so measuring
// start-to-start makes any recording block longer than the threshold look like
// a gap and phantom-splits the night (tickets 39/41: a 4h07m evening block
// followed by a 9-minute mask-off break split one night into two "sessions",
// halving durations and doubling apparent AHI).
// Two estimators: (1) BRP size — ResMed flow data runs ~6 KB/min (observed
// 5.9-6.0 across AirSense nights), so start + size/6 min approximates the end;
// (2) the file's modified time (ezShare listing FAT time / fs mtime), trusted
// only when it falls within (start, start+24h] — a copied or freshly-touched
// file whose mtime has nothing to do with therapy fails the gate and falls
// back to the size estimate.
static std::chrono::system_clock::time_point estimateCheckpointEnd(
    std::chrono::system_clock::time_point start, bool is_brp, int size_kb,
    std::chrono::system_clock::time_point mtime) {
    auto end = start;
    if (is_brp && size_kb > 0)
        end = start + std::chrono::minutes(size_kb / 6);
    if (mtime > start && mtime <= start + std::chrono::hours(24))
        end = std::max(end, mtime);
    return end;
}

std::string SessionDiscoveryService::findLargestFile(
    const std::vector<EzShareFileEntry>& files,
    const std::string& prefix,
    const std::string& suffix) {

    std::string largest_filename;
    int largest_size = 0;

    for (const auto& file : files) {
        std::string file_prefix = extractSessionPrefix(file.name);
        if (file_prefix != prefix) continue;

        // Case-insensitive suffix match
        std::string name_lower = file.name;
        std::transform(name_lower.begin(), name_lower.end(),
                      name_lower.begin(), ::tolower);
        std::string suffix_lower = suffix;
        std::transform(suffix_lower.begin(), suffix_lower.end(),
                      suffix_lower.begin(), ::tolower);

        if (name_lower.find(suffix_lower) != std::string::npos) {
            if (file.size_kb > largest_size) {
                largest_size = file.size_kb;
                largest_filename = file.name;
            }
        }
    }

    return largest_filename;
}

std::vector<SessionFileSet>
SessionDiscoveryService::groupSessionsInFolder(const std::string& date_folder) {
    auto files = data_source_.listFiles(date_folder);

    if (files.empty()) {
        return {};
    }

    // SESSION SPLITTING FIX: Split checkpoint files by time gaps
    // ResMed considers a session over 1 hour after the last file is closed.
    // Configurable via SESSION_GAP_MINUTES (default: 60)

    const std::chrono::minutes SESSION_GAP_THRESHOLD(
        ConfigManager::getInt("SESSION_GAP_MINUTES", 60));

    // Step 1: Collect and sort ALL checkpoint files by timestamp
    struct CheckpointFile {
        std::string name;
        std::string prefix;
        std::chrono::system_clock::time_point timestamp;
        std::chrono::system_clock::time_point end;   // estimated write-close time
        int size_kb;
        bool is_brp;
        bool is_pld;
        bool is_sad;
    };

    std::vector<CheckpointFile> checkpoints;
    std::map<std::string, EzShareFileEntry> csl_files;
    std::map<std::string, EzShareFileEntry> eve_files;

    for (const auto& file : files) {
        std::string name_lower = file.name;
        std::transform(name_lower.begin(), name_lower.end(),
                      name_lower.begin(), ::tolower);

        std::string prefix = extractSessionPrefix(file.name);
        if (prefix.empty()) continue;

        // Collect CSL/EVE files separately
        if (name_lower.find("_csl.edf") != std::string::npos) {
            csl_files[prefix] = file;
            continue;
        } else if (name_lower.find("_eve.edf") != std::string::npos) {
            eve_files[prefix] = file;
            continue;
        }

        // Collect checkpoint files
        bool is_brp = name_lower.find("_brp.edf") != std::string::npos;
        bool is_pld = name_lower.find("_pld.edf") != std::string::npos;
        bool is_sad = isOximetryFile(file.name);

        if (is_brp || is_pld || is_sad) {
            CheckpointFile cp;
            cp.name = file.name;
            cp.prefix = prefix;
            cp.timestamp = parseSessionTime(prefix);
            cp.end = estimateCheckpointEnd(cp.timestamp, is_brp, file.size_kb,
                                           file.getModTime());
            cp.size_kb = file.size_kb;
            cp.is_brp = is_brp;
            cp.is_pld = is_pld;
            cp.is_sad = is_sad;
            checkpoints.push_back(cp);
        }
    }

    if (checkpoints.empty()) {
        return {};  // No checkpoint files found
    }

    // Sort checkpoints by timestamp
    std::sort(checkpoints.begin(), checkpoints.end(),
              [](const CheckpointFile& a, const CheckpointFile& b) {
                  return a.timestamp < b.timestamp;
              });

    // Step 2: Split checkpoints into session groups based on session time gaps.
    // END-to-start: the mask-off gap is next block's start minus when the
    // current group STOPPED writing (see estimateCheckpointEnd), never
    // start-to-start, which counted a long block's own duration as "gap".
    std::vector<std::vector<CheckpointFile>> session_groups;
    std::vector<CheckpointFile> current_group;

    current_group.push_back(checkpoints[0]);
    auto group_end = checkpoints[0].end;

    for (size_t i = 1; i < checkpoints.size(); ++i) {
        auto gap = std::chrono::duration_cast<std::chrono::minutes>(
            checkpoints[i].timestamp - group_end
        );

        if (gap >= SESSION_GAP_THRESHOLD) {
            // Gap detected - start new session
            session_groups.push_back(current_group);
            current_group.clear();
            group_end = checkpoints[i].end;
            std::cout << "  ⏱️  Detected " << gap.count() << "-minute gap - splitting into new session" << std::endl;
        } else {
            group_end = std::max(group_end, checkpoints[i].end);
        }

        current_group.push_back(checkpoints[i]);
    }

    // Add the last group
    if (!current_group.empty()) {
        session_groups.push_back(current_group);
    }

    std::cout << "  📊 Split " << checkpoints.size() << " checkpoint files into "
              << session_groups.size() << " session(s)" << std::endl;

    // Step 3: Create SessionFileSet for each group
    std::vector<SessionFileSet> sessions;

    for (size_t group_idx = 0; group_idx < session_groups.size(); ++group_idx) {
        const auto& group = session_groups[group_idx];

        // Use the timestamp of the FIRST checkpoint in this group as session start
        std::string session_prefix = group[0].prefix;
        auto session_start = group[0].timestamp;

        SessionFileSet session;
        session.date_folder = date_folder;
        session.session_prefix = session_prefix;
        session.session_start = session_start;

        std::cout << "  📋 Session " << (group_idx + 1) << "/" << session_groups.size()
                  << ": " << session_prefix << std::endl;

        // Add all checkpoint files from this group
        for (const auto& cp : group) {
            session.total_size_kb += cp.size_kb;
            session.file_sizes_kb[cp.name] = cp.size_kb;  // Store individual size

            if (cp.is_brp) {
                session.brp_files.push_back(cp.name);
                std::cout << "    BRP: " << cp.name << " (" << cp.size_kb << " KB)" << std::endl;
            } else if (cp.is_pld) {
                session.pld_files.push_back(cp.name);
                std::cout << "    PLD: " << cp.name << " (" << cp.size_kb << " KB)" << std::endl;
            } else if (cp.is_sad) {
                session.sad_files.push_back(cp.name);
                std::cout << "    SAD: " << cp.name << " (" << cp.size_kb << " KB)" << std::endl;
            }
        }

        // Step 4: Match CSL/EVE files to this session
        // CSL/EVE are written when user presses STOP button
        // Match them to the LAST session group (most recent)
        // OR match by timestamp if CSL prefix falls within this session's time range
        //
        // EVERY sidecar inside the session's own span is taken, not just the
        // first. A merged session covers several mask-on blocks and each wrote
        // its own pair; keeping only the first left the night's real
        // annotations on the card. See SDD-014 and issue #22.
        //
        // Matching on the SPAN rather than a flat 12-hour window matters on a
        // date folder with several sessions: a window that wide would give the
        // first group every sidecar on the card and leave the rest with none.

        bool is_last_session = (group_idx == session_groups.size() - 1);

        auto session_end = session_start;
        for (const auto& cp : group) session_end = std::max(session_end, cp.end);

        const auto slack = std::chrono::minutes(1);
        auto belongsHere = [&](std::chrono::system_clock::time_point t) {
            return t >= session_start - slack && t <= session_end + slack;
        };

        auto claim = [&](std::map<std::string, EzShareFileEntry>& pool,
                         std::vector<std::string>& into, const char* label) {
            for (auto it = pool.begin(); it != pool.end(); ) {
                if (is_last_session || belongsHere(parseSessionTime(it->first))) {
                    into.push_back(it->second.name);
                    session.total_size_kb += it->second.size_kb;
                    session.file_sizes_kb[it->second.name] = it->second.size_kb;
                    std::cout << "    " << label << ": " << it->second.name << std::endl;
                    it = pool.erase(it);
                } else {
                    ++it;
                }
            }
        };
        claim(csl_files, session.csl_files, "CSL");
        claim(eve_files, session.eve_files, "EVE");

        sessions.push_back(session);
    }

    // Print summary for each session
    for (const auto& session : sessions) {
        std::cout << "  ✅ Session " << session.session_prefix << " summary:" << std::endl;
        std::cout << "    CSL: " << (session.csl_files.empty()
                                         ? "MISSING (in progress)"
                                         : std::to_string(session.csl_files.size())) << std::endl;
        std::cout << "    EVE: " << (session.eve_files.empty()
                                         ? "MISSING (in progress)"
                                         : std::to_string(session.eve_files.size())) << std::endl;
        std::cout << "    BRP checkpoints: " << session.brp_files.size() << std::endl;
        std::cout << "    PLD checkpoints: " << session.pld_files.size() << std::endl;
        std::cout << "    SAD checkpoints: " << session.sad_files.size() << std::endl;
        std::cout << "    Total size: " << session.total_size_kb << " KB" << std::endl;
    }

    return sessions;
}

std::vector<SessionFileSet>
SessionDiscoveryService::discoverNewSessions(
    std::optional<std::chrono::system_clock::time_point> last_session_start,
    std::optional<std::chrono::system_clock::time_point> retain_from) {

    std::cout << "CPAP: Discovering sessions on ez Share..." << std::endl;

    // SDD-010: the folder-level cut must not exclude a night the per-session
    // retention rule below is about to ask for, so it uses the EARLIER of the
    // two anchors. last_session_start itself is left alone: it still means
    // "the newest thing we have stored", which is what is_new is asking about.
    const auto folder_anchor =
        (retain_from.has_value() &&
         (!last_session_start.has_value() || *retain_from < *last_session_start))
            ? retain_from
            : last_session_start;

    // List all date folders on card
    auto date_folders = data_source_.listDateFolders();

    if (date_folders.empty()) {
        std::cout << "CPAP: No date folders found on ez Share" << std::endl;
        return {};
    }

    std::cout << "CPAP: Found " << date_folders.size() << " date folders on ez Share" << std::endl;

    // Filter folders by date if we have a last session timestamp
    std::vector<std::string> relevant_folders;

    if (folder_anchor.has_value()) {
        auto last_tp = folder_anchor.value();
        std::time_t last_time = std::chrono::system_clock::to_time_t(last_tp);
        std::tm* last_tm = std::localtime(&last_time);

        // Format as YYYYMMDD
        char last_date_str[9];
        std::strftime(last_date_str, sizeof(last_date_str), "%Y%m%d", last_tm);
        std::string last_date(last_date_str);

        std::cout << "CPAP: Scanning folders from: " << last_date << std::endl;

        // Include folders >= last date (to catch sessions later on the same day)
        // ALSO include previous day's folder because ResMed stores early AM sessions there
        for (const auto& folder : date_folders) {
            if (folder >= last_date) {
                relevant_folders.push_back(folder);
            }
        }

        // Add previous day folder if it exists (for early morning sessions)
        std::tm prev_tm = *last_tm;
        prev_tm.tm_mday -= 1;
        std::mktime(&prev_tm);  // Normalize
        char prev_date_str[9];
        std::strftime(prev_date_str, sizeof(prev_date_str), "%Y%m%d", &prev_tm);
        std::string prev_date(prev_date_str);

        if (std::find(date_folders.begin(), date_folders.end(), prev_date) != date_folders.end() &&
            std::find(relevant_folders.begin(), relevant_folders.end(), prev_date) == relevant_folders.end()) {
            relevant_folders.push_back(prev_date);
            std::cout << "CPAP: Also checking prev day folder " << prev_date
                      << " (early AM sessions stored there)" << std::endl;
        }

        std::cout << "CPAP: " << relevant_folders.size()
                  << " folders with potentially new data" << std::endl;
    } else {
        // First run - get everything
        std::cout << "CPAP: No previous sessions in DB, will scan all folders" << std::endl;
        relevant_folders = date_folders;
    }

    if (relevant_folders.empty()) {
        std::cout << "CPAP: No relevant folders to scan" << std::endl;
        return {};
    }

    // Discover sessions in each folder
    std::vector<SessionFileSet> all_sessions;

    for (const auto& folder : relevant_folders) {
        std::cout << "CPAP: Scanning folder " << folder << "..." << std::endl;

        auto folder_sessions = groupSessionsInFolder(folder);

        std::cout << "CPAP: Found " << folder_sessions.size()
                  << " sessions in " << folder << std::endl;

        // Filter sessions: include NEW sessions OR sessions from TODAY (growing files)
        // Use both localtime and UTC to handle Docker containers that default to UTC
        auto now = std::chrono::system_clock::now();
        std::time_t now_time = std::chrono::system_clock::to_time_t(now);

        char today_local[9], today_utc[9];
        std::tm* local_tm = std::localtime(&now_time);
        std::strftime(today_local, sizeof(today_local), "%Y%m%d", local_tm);
        std::tm* utc_tm = std::gmtime(&now_time);
        std::strftime(today_utc, sizeof(today_utc), "%Y%m%d", utc_tm);

        // Calculate 48 hours ago (to catch late EVE files that can be written hours later)
        auto forty_eight_hours_ago = now - std::chrono::hours(48);

        for (const auto& session : folder_sessions) {
            bool is_today = (folder == today_local || folder == today_utc);
            bool is_new = (!last_session_start.has_value() ||
                          session.session_start > last_session_start.value());
            bool is_recent = (session.session_start > forty_eight_hours_ago);
            // SDD-010: always re-check the newest stored nights, whatever the
            // calendar says. is_today and is_recent are both anchored on the
            // wall clock, so on a card that stopped being written to, or an
            // archive copied once from an old SD card, NOTHING matches and a
            // folder is never observed twice. Settling needs two observations
            // of the same signature, so those nights sit at Live forever and
            // never resolve to Complete or Partial. >= not >, so the anchor
            // session itself is retained and not just the one after it.
            bool is_retained = (retain_from.has_value() &&
                                session.session_start >= retain_from.value());

            // Re-download if: new session, today's session, within last 48h
            // (catch late EVE files), or among the newest stored nights.
            if (is_new || is_today || is_recent || is_retained) {
                all_sessions.push_back(session);

                std::string reason = is_new ? "New session" :
                                    is_today ? "Checking today's session" :
                                    is_recent ? "Checking recent session (catch late EVE files)" :
                                    "Re-checking newest stored session (settling)";

                std::cout << "  - " << reason << ": " << session.session_prefix
                          << " (" << session.total_size_kb << " KB)"
                          << std::endl;
            } else {
                std::cout << "  - Skipping already-stored session: "
                          << session.session_prefix << std::endl;
            }
        }
    }

    std::cout << "CPAP: Discovered " << all_sessions.size()
              << " new sessions to download" << std::endl;

    return all_sessions;
}

std::vector<SessionFileSet>
SessionDiscoveryService::discoverLocalSessions(
    const std::string& local_datalog_dir,
    std::optional<std::chrono::system_clock::time_point> last_session_start,
    std::optional<std::chrono::system_clock::time_point> retain_from) {

    std::cout << "CPAP: Discovering sessions from local directory: " << local_datalog_dir << std::endl;

    // SDD-010: see discoverNewSessions. The folder-level cut uses the EARLIER
    // anchor so a retained night cannot be filtered out before the per-session
    // rule gets to ask for it.
    const auto folder_anchor =
        (retain_from.has_value() &&
         (!last_session_start.has_value() || *retain_from < *last_session_start))
            ? retain_from
            : last_session_start;

    if (!std::filesystem::exists(local_datalog_dir)) {
        std::cerr << "CPAP: Local directory not found: " << local_datalog_dir << std::endl;
        return {};
    }

    // List all YYYYMMDD date folders
    std::vector<std::string> date_folders;
    std::regex date_regex(R"(^\d{8}$)");

    // Iterate with error_code: an unreadable DATALOG dir must degrade to an
    // empty scan, not an uncaught filesystem_error that kills the burst
    // worker (incident 2026-07-17: root-owned 0750 upload crash-looped the
    // service).
    std::error_code dir_ec;
    std::filesystem::directory_iterator root_it(local_datalog_dir, dir_ec);
    if (dir_ec) {
        std::cerr << "CPAP: ⚠️  Cannot read local directory " << local_datalog_dir
                  << " (" << dir_ec.message()
                  << ") — fix ownership/permissions so the service user can read it"
                  << std::endl;
        return {};
    }
    for (; root_it != std::filesystem::directory_iterator();
         root_it.increment(dir_ec)) {
        const auto& entry = *root_it;
        std::error_code entry_ec;
        if (!entry.is_directory(entry_ec) || entry_ec) continue;
        std::string name = entry.path().filename().string();
        if (std::regex_match(name, date_regex)) {
            date_folders.push_back(name);
        }
    }
    if (dir_ec) {
        std::cerr << "CPAP: ⚠️  Listing of " << local_datalog_dir
                  << " ended early (" << dir_ec.message() << ")" << std::endl;
    }

    std::sort(date_folders.begin(), date_folders.end());

    if (date_folders.empty()) {
        std::cout << "CPAP: No date folders found in " << local_datalog_dir << std::endl;
        return {};
    }

    std::cout << "CPAP: Found " << date_folders.size() << " date folders" << std::endl;

    // Filter folders by last session date (same logic as discoverNewSessions)
    std::vector<std::string> relevant_folders;

    if (folder_anchor.has_value()) {
        auto last_tp = folder_anchor.value();
        std::time_t last_time = std::chrono::system_clock::to_time_t(last_tp);
        std::tm* last_tm = std::localtime(&last_time);

        char last_date_str[9];
        std::strftime(last_date_str, sizeof(last_date_str), "%Y%m%d", last_tm);
        std::string last_date(last_date_str);

        std::cout << "CPAP: Scanning folders from: " << last_date << std::endl;

        for (const auto& folder : date_folders) {
            if (folder >= last_date) {
                relevant_folders.push_back(folder);
            }
        }

        // Add previous day folder (for early AM sessions)
        std::tm prev_tm = *last_tm;
        prev_tm.tm_mday -= 1;
        std::mktime(&prev_tm);
        char prev_date_str[9];
        std::strftime(prev_date_str, sizeof(prev_date_str), "%Y%m%d", &prev_tm);
        std::string prev_date(prev_date_str);

        if (std::find(date_folders.begin(), date_folders.end(), prev_date) != date_folders.end() &&
            std::find(relevant_folders.begin(), relevant_folders.end(), prev_date) == relevant_folders.end()) {
            relevant_folders.push_back(prev_date);
        }

        std::cout << "CPAP: " << relevant_folders.size()
                  << " folders with potentially new data" << std::endl;
    } else {
        std::cout << "CPAP: No previous sessions in DB, will scan all folders" << std::endl;
        relevant_folders = date_folders;
    }

    if (relevant_folders.empty()) {
        return {};
    }

    // Discover sessions in each folder
    std::vector<SessionFileSet> all_sessions;

    for (const auto& folder : relevant_folders) {
        std::string folder_path = local_datalog_dir + "/" + folder;
        std::cout << "CPAP: Scanning local folder " << folder << "..." << std::endl;

        // Belt-and-braces: groupLocalFolder degrades gracefully itself, but a
        // scan surprise in one folder must never abort discovery of the rest
        // (an escape here reaches the burst worker thread and terminates the
        // whole process).
        std::vector<SessionFileSet> folder_sessions;
        try {
            folder_sessions = groupLocalFolder(folder_path, folder);
        } catch (const std::exception& e) {
            std::cerr << "CPAP: ⚠️  Skipping folder " << folder
                      << " after scan error: " << e.what() << std::endl;
            continue;
        }
        std::cout << "CPAP: Found " << folder_sessions.size()
                  << " sessions in " << folder << std::endl;

        // Same filtering as discoverNewSessions: new or recent sessions
        // Use both localtime and UTC to handle Docker containers that default to UTC
        auto now = std::chrono::system_clock::now();
        auto forty_eight_hours_ago = now - std::chrono::hours(48);

        std::time_t now_time = std::chrono::system_clock::to_time_t(now);
        char today_local[9], today_utc[9];
        std::tm* local_tm = std::localtime(&now_time);
        std::strftime(today_local, sizeof(today_local), "%Y%m%d", local_tm);
        std::tm* utc_tm = std::gmtime(&now_time);
        std::strftime(today_utc, sizeof(today_utc), "%Y%m%d", utc_tm);

        for (const auto& session : folder_sessions) {
            bool is_today = (folder == today_local || folder == today_utc);
            bool is_new = (!last_session_start.has_value() ||
                          session.session_start > last_session_start.value());
            bool is_recent = (session.session_start > forty_eight_hours_ago);
            // SDD-010: the newest stored nights are re-checked whatever the
            // calendar says, so a folder can always take the second observation
            // that settling needs. Without this, an archive copied once from an
            // old SD card never settles: every other test here is anchored on
            // the current date, and none of them can ever match again.
            bool is_retained = (retain_from.has_value() &&
                                session.session_start >= retain_from.value());

            if (is_new || is_today || is_recent || is_retained) {
                all_sessions.push_back(session);
            }
        }
    }

    std::cout << "CPAP: Discovered " << all_sessions.size()
              << " session(s) to process" << std::endl;

    return all_sessions;
}

std::vector<SessionFileSet>
SessionDiscoveryService::groupLocalFolder(
    const std::string& dir_path,
    const std::string& date_folder) {

    if (!std::filesystem::exists(dir_path) || !std::filesystem::is_directory(dir_path)) {
        return {};
    }

    const std::chrono::minutes SESSION_GAP_THRESHOLD(
        ConfigManager::getInt("SESSION_GAP_MINUTES", 60));

    struct CheckpointFile {
        std::string name;
        std::string prefix;
        std::chrono::system_clock::time_point timestamp;
        std::chrono::system_clock::time_point end;   // estimated write-close time
        int size_kb;
        bool is_brp;
        bool is_pld;
        bool is_sad;
    };

    std::vector<CheckpointFile> checkpoints;
    std::map<std::string, std::pair<std::string, int>> csl_files;  // prefix -> {name, size_kb}
    std::map<std::string, std::pair<std::string, int>> eve_files;

    // Helper to extract prefix (same regex as instance method)
    auto extractPrefix = [](const std::string& filename) -> std::string {
        std::regex prefix_regex(R"(^(\d{8}_\d{6})_)");
        std::smatch match;
        if (std::regex_search(filename, match, prefix_regex)) {
            return match[1].str();
        }
        return "";
    };

    // Helper to parse timestamp from prefix (same logic as instance method)
    auto parseTime = [](const std::string& prefix) -> std::chrono::system_clock::time_point {
        if (prefix.size() != 15) return {};
        std::tm tm = {};
        tm.tm_year = std::stoi(prefix.substr(0, 4)) - 1900;
        tm.tm_mon  = std::stoi(prefix.substr(4, 2)) - 1;
        tm.tm_mday = std::stoi(prefix.substr(6, 2));
        tm.tm_hour = std::stoi(prefix.substr(9, 2));
        tm.tm_min  = std::stoi(prefix.substr(11, 2));
        tm.tm_sec  = std::stoi(prefix.substr(13, 2));
        tm.tm_isdst = -1;
        return std::chrono::system_clock::from_time_t(std::mktime(&tm));
    };

    // Iterate with error_code (mirrors the SleepHq/Prisma scans): a date
    // folder the service user cannot open — e.g. uploaded root-owned 0750 —
    // is skipped with a warning instead of throwing an uncaught
    // filesystem_error that terminates the service (incident 2026-07-17).
    std::error_code dir_ec;
    std::filesystem::directory_iterator dir_it(dir_path, dir_ec);
    if (dir_ec) {
        std::cerr << "CPAP: ⚠️  Skipping unreadable folder " << dir_path
                  << " (" << dir_ec.message()
                  << ") — fix ownership/permissions so the service user can read it"
                  << std::endl;
        return {};
    }

    for (; dir_it != std::filesystem::directory_iterator();
         dir_it.increment(dir_ec)) {
        const auto& entry = *dir_it;
        std::error_code entry_ec;
        if (!entry.is_regular_file(entry_ec) || entry_ec) continue;

        std::string filename = entry.path().filename().string();
        std::string name_lower = filename;
        std::transform(name_lower.begin(), name_lower.end(), name_lower.begin(), ::tolower);

        std::string prefix = extractPrefix(filename);
        if (prefix.empty()) continue;

        auto file_bytes = std::filesystem::file_size(entry.path(), entry_ec);
        if (entry_ec) continue;
        int size_kb = static_cast<int>(file_bytes / 1024);

        if (name_lower.find("_csl.edf") != std::string::npos) {
            csl_files[prefix] = {filename, size_kb};
            continue;
        } else if (name_lower.find("_eve.edf") != std::string::npos) {
            eve_files[prefix] = {filename, size_kb};
            continue;
        }

        bool is_brp = name_lower.find("_brp.edf") != std::string::npos;
        bool is_pld = name_lower.find("_pld.edf") != std::string::npos;
        bool is_sad = isOximetryFile(filename);

        if (is_brp || is_pld || is_sad) {
            CheckpointFile cp;
            cp.name = filename;
            cp.prefix = prefix;
            cp.timestamp = parseTime(prefix);
            // fs mtime -> system_clock (C++17: bridge via the two clocks' nows).
            // A copied file's mtime is the copy time, not therapy time -- the
            // plausibility gate in estimateCheckpointEnd discards it then.
            auto ftime = entry.last_write_time(entry_ec);
            if (entry_ec) continue;
            auto mtime = std::chrono::time_point_cast<std::chrono::system_clock::duration>(
                ftime - std::filesystem::file_time_type::clock::now()
                + std::chrono::system_clock::now());
            cp.end = estimateCheckpointEnd(cp.timestamp, is_brp, size_kb, mtime);
            cp.size_kb = size_kb;
            cp.is_brp = is_brp;
            cp.is_pld = is_pld;
            cp.is_sad = is_sad;
            checkpoints.push_back(cp);
        }
    }

    if (dir_ec) {
        std::cerr << "CPAP: ⚠️  Scan of " << dir_path << " ended early ("
                  << dir_ec.message() << ")" << std::endl;
    }

    if (checkpoints.empty()) return {};

    std::sort(checkpoints.begin(), checkpoints.end(),
              [](const CheckpointFile& a, const CheckpointFile& b) {
                  return a.timestamp < b.timestamp;
              });

    // Split into session groups by session time gaps (END-to-start: a long
    // block's own duration is not a gap -- see estimateCheckpointEnd).
    std::vector<std::vector<CheckpointFile>> session_groups;
    std::vector<CheckpointFile> current_group;
    current_group.push_back(checkpoints[0]);
    auto group_end = checkpoints[0].end;

    for (size_t i = 1; i < checkpoints.size(); ++i) {
        auto gap = std::chrono::duration_cast<std::chrono::minutes>(
            checkpoints[i].timestamp - group_end);
        if (gap >= SESSION_GAP_THRESHOLD) {
            session_groups.push_back(current_group);
            current_group.clear();
            group_end = checkpoints[i].end;
        } else {
            group_end = std::max(group_end, checkpoints[i].end);
        }
        current_group.push_back(checkpoints[i]);
    }
    if (!current_group.empty()) {
        session_groups.push_back(current_group);
    }

    std::cout << "  Split " << checkpoints.size() << " checkpoint files into "
              << session_groups.size() << " session(s)" << std::endl;

    // Build SessionFileSet for each group
    std::vector<SessionFileSet> sessions;

    for (size_t group_idx = 0; group_idx < session_groups.size(); ++group_idx) {
        const auto& group = session_groups[group_idx];
        std::string session_prefix = group[0].prefix;
        auto session_start = group[0].timestamp;

        SessionFileSet session;
        session.date_folder = date_folder;
        session.session_prefix = session_prefix;
        session.session_start = session_start;

        for (const auto& cp : group) {
            session.total_size_kb += cp.size_kb;
            session.file_sizes_kb[cp.name] = cp.size_kb;
            if (cp.is_brp) session.brp_files.push_back(cp.name);
            else if (cp.is_pld) session.pld_files.push_back(cp.name);
            else if (cp.is_sad) session.sad_files.push_back(cp.name);
        }

        // Match CSL/EVE to this session. EVERY sidecar inside the session's own
        // span, not just the first: a merged session covers several mask-on
        // blocks and each block wrote its own pair. Taking only the first kept
        // the earliest, which is usually the seconds-long mask-fit check with
        // its empty 832-byte EVE stub, and dropped every real annotation of the
        // night. See SDD-014 and issue #22.
        //
        // The span, NOT a flat 12-hour window: on a date folder holding several
        // sessions, a window that wide would hand the first group every sidecar
        // on the card and leave the later groups with none.
        bool is_last_session = (group_idx == session_groups.size() - 1);

        auto session_end = session_start;
        for (const auto& cp : group) session_end = std::max(session_end, cp.end);

        // A sidecar is written at the block boundary, so allow a minute either
        // side of the span rather than demanding it land inside to the second.
        const auto slack = std::chrono::minutes(1);
        auto belongsHere = [&](std::chrono::system_clock::time_point t) {
            return t >= session_start - slack && t <= session_end + slack;
        };

        auto claim = [&](std::map<std::string, std::pair<std::string, int>>& pool,
                         std::vector<std::string>& into) {
            for (auto it = pool.begin(); it != pool.end(); ) {
                if (is_last_session || belongsHere(parseTime(it->first))) {
                    into.push_back(it->second.first);
                    session.total_size_kb += it->second.second;
                    session.file_sizes_kb[it->second.first] = it->second.second;
                    it = pool.erase(it);
                } else {
                    ++it;
                }
            }
        };
        claim(csl_files, session.csl_files);
        claim(eve_files, session.eve_files);

        sessions.push_back(session);
    }

    // Print summary
    for (const auto& session : sessions) {
        std::cout << "  Session " << session.session_prefix
                  << ": BRP=" << session.brp_files.size()
                  << " PLD=" << session.pld_files.size()
                  << " SAD=" << session.sad_files.size()
                  << " CSL=" << session.csl_files.size()
                  << " EVE=" << session.eve_files.size()
                  << " (" << session.total_size_kb << " KB)" << std::endl;
    }

    return sessions;
}

} // namespace hms_cpap
