#include "services/SessionDiscoveryService.h"
#include <algorithm>
#include <regex>
#include <map>
#include <iostream>
#include <filesystem>

namespace hms_cpap {

SessionDiscoveryService::SessionDiscoveryService(EzShareClient& ezshare_client)
    : ezshare_client_(ezshare_client) {}

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
    auto files = ezshare_client_.listFiles(date_folder);

    if (files.empty()) {
        return {};
    }

    // SESSION SPLITTING FIX: Split checkpoint files by 2-hour gaps
    // ResMed machines write checkpoint files during sessions - if there's a 2+ hour gap
    // between consecutive BRP files, they're from different sleep sessions

    const std::chrono::hours SESSION_GAP_THRESHOLD(2);  // 2 hours = new session

    // Step 1: Collect and sort ALL checkpoint files by timestamp
    struct CheckpointFile {
        std::string name;
        std::string prefix;
        std::chrono::system_clock::time_point timestamp;
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
        bool is_sad = name_lower.find("_sad.edf") != std::string::npos;

        if (is_brp || is_pld || is_sad) {
            CheckpointFile cp;
            cp.name = file.name;
            cp.prefix = prefix;
            cp.timestamp = parseSessionTime(prefix);
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

    // Step 2: Split checkpoints into session groups based on 2-hour gaps
    std::vector<std::vector<CheckpointFile>> session_groups;
    std::vector<CheckpointFile> current_group;

    current_group.push_back(checkpoints[0]);

    for (size_t i = 1; i < checkpoints.size(); ++i) {
        auto gap = std::chrono::duration_cast<std::chrono::hours>(
            checkpoints[i].timestamp - checkpoints[i-1].timestamp
        );

        if (gap >= SESSION_GAP_THRESHOLD) {
            // Gap detected - start new session
            session_groups.push_back(current_group);
            current_group.clear();
            std::cout << "  ⏱️  Detected " << gap.count() << "-hour gap - splitting into new session" << std::endl;
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

        bool is_last_session = (group_idx == session_groups.size() - 1);

        for (const auto& [csl_prefix, csl_file] : csl_files) {
            auto csl_time = parseSessionTime(csl_prefix);

            // Match CSL to session if:
            // 1. This is the last session group (CSL written at end)
            // 2. CSL timestamp is close to this session (within 12 hours)
            auto time_diff = std::chrono::abs(csl_time - session_start);
            bool time_match = time_diff < std::chrono::hours(12);

            if (is_last_session || time_match) {
                session.csl_file = csl_file.name;
                session.total_size_kb += csl_file.size_kb;
                session.file_sizes_kb[csl_file.name] = csl_file.size_kb;  // Store individual size
                std::cout << "    CSL: " << csl_file.name << std::endl;
                break;  // Only one CSL per session
            }
        }

        for (const auto& [eve_prefix, eve_file] : eve_files) {
            auto eve_time = parseSessionTime(eve_prefix);

            auto time_diff = std::chrono::abs(eve_time - session_start);
            bool time_match = time_diff < std::chrono::hours(12);

            if (is_last_session || time_match) {
                session.eve_file = eve_file.name;
                session.total_size_kb += eve_file.size_kb;
                session.file_sizes_kb[eve_file.name] = eve_file.size_kb;  // Store individual size
                std::cout << "    EVE: " << eve_file.name << std::endl;
                break;  // Only one EVE per session
            }
        }

        sessions.push_back(session);
    }

    // Print summary for each session
    for (const auto& session : sessions) {
        std::cout << "  ✅ Session " << session.session_prefix << " summary:" << std::endl;
        std::cout << "    CSL: " << (session.csl_file.empty() ? "MISSING (in progress)" : "✓") << std::endl;
        std::cout << "    EVE: " << (session.eve_file.empty() ? "MISSING (in progress)" : "✓") << std::endl;
        std::cout << "    BRP checkpoints: " << session.brp_files.size() << std::endl;
        std::cout << "    PLD checkpoints: " << session.pld_files.size() << std::endl;
        std::cout << "    SAD checkpoints: " << session.sad_files.size() << std::endl;
        std::cout << "    Total size: " << session.total_size_kb << " KB" << std::endl;
    }

    return sessions;
}

std::vector<SessionFileSet>
SessionDiscoveryService::discoverNewSessions(
    std::optional<std::chrono::system_clock::time_point> last_session_start) {

    std::cout << "CPAP: Discovering sessions on ez Share..." << std::endl;

    // List all date folders on card
    auto date_folders = ezshare_client_.listDateFolders();

    if (date_folders.empty()) {
        std::cout << "CPAP: No date folders found on ez Share" << std::endl;
        return {};
    }

    std::cout << "CPAP: Found " << date_folders.size() << " date folders on ez Share" << std::endl;

    // Filter folders by date if we have a last session timestamp
    std::vector<std::string> relevant_folders;

    if (last_session_start.has_value()) {
        auto last_tp = last_session_start.value();
        std::time_t last_time = std::chrono::system_clock::to_time_t(last_tp);
        std::tm* last_tm = std::localtime(&last_time);

        // Format as YYYYMMDD
        char last_date_str[9];
        std::strftime(last_date_str, sizeof(last_date_str), "%Y%m%d", last_tm);
        std::string last_date(last_date_str);

        std::cout << "CPAP: Last stored session date: " << last_date << std::endl;

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
        auto now = std::chrono::system_clock::now();
        std::time_t now_time = std::chrono::system_clock::to_time_t(now);
        std::tm* now_tm = std::localtime(&now_time);

        char today_str[9];
        std::strftime(today_str, sizeof(today_str), "%Y%m%d", now_tm);
        std::string today(today_str);

        // Calculate 48 hours ago (to catch late EVE files that can be written hours later)
        auto forty_eight_hours_ago = now - std::chrono::hours(48);

        for (const auto& session : folder_sessions) {
            bool is_today = (folder == today);
            bool is_new = (!last_session_start.has_value() ||
                          session.session_start > last_session_start.value());
            bool is_recent = (session.session_start > forty_eight_hours_ago);

            // Re-download if: new session, today's session, OR within last 48h (catch late EVE files)
            if (is_new || is_today || is_recent) {
                all_sessions.push_back(session);

                std::string reason = is_new ? "New session" :
                                    is_today ? "Re-downloading today's session" :
                                    "Re-downloading recent session (catch late EVE files)";

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
SessionDiscoveryService::groupLocalFolder(
    const std::string& dir_path,
    const std::string& date_folder) {

    if (!std::filesystem::exists(dir_path) || !std::filesystem::is_directory(dir_path)) {
        return {};
    }

    const std::chrono::hours SESSION_GAP_THRESHOLD(2);

    struct CheckpointFile {
        std::string name;
        std::string prefix;
        std::chrono::system_clock::time_point timestamp;
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

    for (const auto& entry : std::filesystem::directory_iterator(dir_path)) {
        if (!entry.is_regular_file()) continue;

        std::string filename = entry.path().filename().string();
        std::string name_lower = filename;
        std::transform(name_lower.begin(), name_lower.end(), name_lower.begin(), ::tolower);

        std::string prefix = extractPrefix(filename);
        if (prefix.empty()) continue;

        int size_kb = static_cast<int>(std::filesystem::file_size(entry.path()) / 1024);

        if (name_lower.find("_csl.edf") != std::string::npos) {
            csl_files[prefix] = {filename, size_kb};
            continue;
        } else if (name_lower.find("_eve.edf") != std::string::npos) {
            eve_files[prefix] = {filename, size_kb};
            continue;
        }

        bool is_brp = name_lower.find("_brp.edf") != std::string::npos;
        bool is_pld = name_lower.find("_pld.edf") != std::string::npos;
        bool is_sad = name_lower.find("_sad.edf") != std::string::npos;

        if (is_brp || is_pld || is_sad) {
            CheckpointFile cp;
            cp.name = filename;
            cp.prefix = prefix;
            cp.timestamp = parseTime(prefix);
            cp.size_kb = size_kb;
            cp.is_brp = is_brp;
            cp.is_pld = is_pld;
            cp.is_sad = is_sad;
            checkpoints.push_back(cp);
        }
    }

    if (checkpoints.empty()) return {};

    std::sort(checkpoints.begin(), checkpoints.end(),
              [](const CheckpointFile& a, const CheckpointFile& b) {
                  return a.timestamp < b.timestamp;
              });

    // Split into session groups by 2-hour gaps
    std::vector<std::vector<CheckpointFile>> session_groups;
    std::vector<CheckpointFile> current_group;
    current_group.push_back(checkpoints[0]);

    for (size_t i = 1; i < checkpoints.size(); ++i) {
        auto gap = std::chrono::duration_cast<std::chrono::hours>(
            checkpoints[i].timestamp - checkpoints[i-1].timestamp);
        if (gap >= SESSION_GAP_THRESHOLD) {
            session_groups.push_back(current_group);
            current_group.clear();
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

        // Match CSL/EVE to this session
        bool is_last_session = (group_idx == session_groups.size() - 1);

        for (const auto& [csl_prefix, csl_info] : csl_files) {
            auto csl_time = parseTime(csl_prefix);
            auto time_diff = std::chrono::abs(csl_time - session_start);
            bool time_match = time_diff < std::chrono::hours(12);
            if (is_last_session || time_match) {
                session.csl_file = csl_info.first;
                session.total_size_kb += csl_info.second;
                session.file_sizes_kb[csl_info.first] = csl_info.second;
                break;
            }
        }

        for (const auto& [eve_prefix, eve_info] : eve_files) {
            auto eve_time = parseTime(eve_prefix);
            auto time_diff = std::chrono::abs(eve_time - session_start);
            bool time_match = time_diff < std::chrono::hours(12);
            if (is_last_session || time_match) {
                session.eve_file = eve_info.first;
                session.total_size_kb += eve_info.second;
                session.file_sizes_kb[eve_info.first] = eve_info.second;
                break;
            }
        }

        sessions.push_back(session);
    }

    // Print summary
    for (const auto& session : sessions) {
        std::cout << "  Session " << session.session_prefix
                  << ": BRP=" << session.brp_files.size()
                  << " PLD=" << session.pld_files.size()
                  << " SAD=" << session.sad_files.size()
                  << " CSL=" << (session.csl_file.empty() ? "no" : "yes")
                  << " EVE=" << (session.eve_file.empty() ? "no" : "yes")
                  << " (" << session.total_size_kb << " KB)" << std::endl;
    }

    return sessions;
}

} // namespace hms_cpap
