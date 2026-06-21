#include "services/PrismaIngestion.h"
#include "utils/TimeCompat.h"
#include "miniz.h"
#include <filesystem>
#include <iostream>
#include <regex>
#include <algorithm>
#include <set>
#include <map>
#include <optional>

namespace hms_cpap {

namespace fs = std::filesystem;

// Turn one event/signal pair into a session record (shared by both the legacy
// events//signals layout and the newer combined SMART-max layout). Returns
// nullopt when the night is older than last_session_start (already ingested).
static std::optional<PrismaSessionFile> buildSessionFile(
    const std::string& date, int seq,
    const std::string& event_path, const std::string& signal_path,
    std::optional<std::chrono::system_clock::time_point> last_session_start)
{
    PrismaSessionFile sf;
    sf.date_folder = date;
    sf.sequence_number = seq;
    sf.event_path = event_path;
    sf.signal_path = signal_path;

    cpapdash::parser::EDFFile edf;
    if (edf.open(signal_path)) sf.session_start = edf.getStartTime();

    if (sf.session_start == std::chrono::system_clock::time_point{}) {
        std::tm tm{};
        tm.tm_year = std::stoi(date.substr(0, 4)) - 1900;
        tm.tm_mon  = std::stoi(date.substr(4, 2)) - 1;
        tm.tm_mday = std::stoi(date.substr(6, 2));
        sf.session_start = std::chrono::system_clock::from_time_t(mktime(&tm));
    }

    if (last_session_start && sf.session_start <= *last_session_start)
        return std::nullopt;
    return sf;
}

PrismaIngestion::PrismaIngestion(const std::string& data_dir)
    : data_dir_(data_dir), therapy_dir_(data_dir), config_dir_(data_dir) {}

bool PrismaIngestion::extractZip(const std::string& zip_path, const std::string& dest_dir) {
    mz_zip_archive zip{};
    if (!mz_zip_reader_init_file(&zip, zip_path.c_str(), 0)) {
        std::cerr << "PrismaIngestion: cannot open ZIP: " << zip_path << std::endl;
        return false;
    }

    int num_files = static_cast<int>(mz_zip_reader_get_num_files(&zip));
    for (int i = 0; i < num_files; ++i) {
        mz_zip_archive_file_stat stat;
        if (!mz_zip_reader_file_stat(&zip, i, &stat)) continue;

        std::string filename = stat.m_filename;
        fs::path dest = fs::path(dest_dir) / filename;

        if (mz_zip_reader_is_file_a_directory(&zip, i)) {
            fs::create_directories(dest);
            continue;
        }

        fs::create_directories(dest.parent_path());
        mz_zip_reader_extract_to_file(&zip, i, dest.string().c_str(), 0);
    }

    mz_zip_reader_end(&zip);
    return true;
}

bool PrismaIngestion::detectAndExtractZips() {
    fs::path therapy_zip = fs::path(data_dir_) / "therapy.pdat";
    fs::path config_zip = fs::path(data_dir_) / "config.pcfg";

    if (!fs::exists(therapy_zip) && !fs::exists(config_zip))
        return false;

    is_zip_mode_ = true;
    std::string cache_base = (fs::temp_directory_path() / "prisma_cache").string();

    if (fs::exists(therapy_zip)) {
        std::string therapy_cache = cache_base + "/therapy";

        bool needs_extract = !fs::exists(therapy_cache);
        if (!needs_extract) {
            auto zip_time = fs::last_write_time(therapy_zip);
            for (const auto& entry : fs::recursive_directory_iterator(therapy_cache)) {
                if (entry.is_regular_file()) {
                    needs_extract = (entry.last_write_time() < zip_time);
                    break;
                }
            }
        }

        if (needs_extract) {
            std::cout << "PrismaIngestion: extracting therapy.pdat..." << std::endl;
            fs::remove_all(therapy_cache);
            fs::create_directories(therapy_cache);
            if (!extractZip(therapy_zip.string(), therapy_cache)) return false;
        }
        therapy_dir_ = therapy_cache;
    }

    if (fs::exists(config_zip)) {
        std::string config_cache = cache_base + "/config";
        if (!fs::exists(config_cache)) {
            std::cout << "PrismaIngestion: extracting config.pcfg..." << std::endl;
            fs::create_directories(config_cache);
            if (!extractZip(config_zip.string(), config_cache)) return false;
        }
        config_dir_ = config_cache;
    }

    return true;
}

bool PrismaIngestion::findTherapyRoot(const std::string& base) {
    auto tryPath = [this](const std::string& path) -> bool {
        if (fs::is_directory(path + "/events") && fs::is_directory(path + "/signals")) {
            therapy_dir_ = path;
            return true;
        }
        return false;
    };

    if (tryPath(base)) return true;

    for (const auto& suffix : {
        "/mnt/flash/data/therapy",
        "/data/therapy",
        "/therapy"
    }) {
        if (tryPath(base + suffix)) return true;
    }
    return false;
}

bool PrismaIngestion::initialize() {
    if (initialized_) return true;

    // Try ZIP mode first (Prisma Line: therapy.pdat + config.pcfg)
    detectAndExtractZips();

    // Find the events/ + signals/ tree (works for both ZIP-extracted and raw dirs)
    if (findTherapyRoot(therapy_dir_) || findTherapyRoot(data_dir_)) {
        std::cout << "PrismaIngestion: " << (is_zip_mode_ ? "ZIP" : "raw directory")
                  << " mode, therapy at " << therapy_dir_ << std::endl;
    } else if (findCombinedRoot(therapy_dir_) || findCombinedRoot(data_dir_)) {
        // Newer firmware (SMART max): <serial>/<date>/<NNNN>/{event,signal,tc}
        combined_layout_ = true;
        std::cout << "PrismaIngestion: combined-layout mode, root at "
                  << combined_root_ << std::endl;
    } else {
        std::cerr << "PrismaIngestion: no events/signals or combined date tree found in "
                  << data_dir_ << std::endl;
        return false;
    }

    initialized_ = true;
    return true;
}

// Detect the combined SMART-max layout: a directory that directly holds 8-digit
// date folders, each containing one or more session subfolders with
// signal_*.wmedf files inside. `base` may be the date-tree root itself or its
// parent (the SD root with a <serial>/ folder in between).
bool PrismaIngestion::findCombinedRoot(const std::string& base) {
    if (!fs::is_directory(base)) return false;
    std::error_code ec;

    auto looksCombined = [](const std::string& root) -> bool {
        std::error_code e;
        for (const auto& d : fs::directory_iterator(root, e)) {
            if (!d.is_directory()) continue;
            std::string name = d.path().filename().string();
            if (name.size() != 8 || !std::all_of(name.begin(), name.end(), ::isdigit))
                continue;
            for (const auto& sub : fs::directory_iterator(d.path(), e)) {
                if (!sub.is_directory()) continue;
                for (const auto& f : fs::directory_iterator(sub.path(), e)) {
                    std::string fn = f.path().filename().string();
                    if (fn.rfind("signal_", 0) == 0 &&
                        fn.find(".wmedf") != std::string::npos)
                        return true;
                }
            }
        }
        return false;
    };

    if (looksCombined(base)) { combined_root_ = base; return true; }

    for (const auto& child : fs::directory_iterator(base, ec)) {
        if (child.is_directory() && looksCombined(child.path().string())) {
            combined_root_ = child.path().string();
            return true;
        }
    }
    return false;
}

std::string PrismaIngestion::deviceXmlPath() const {
    for (const auto& base : {config_dir_, data_dir_}) {
        for (const auto& suffix : {
            "/mnt/flash/conf/device.xml",
            "/conf/device.xml",
            "/device.xml"
        }) {
            std::string path = base + suffix;
            if (fs::exists(path)) return path;
        }
    }
    return "";
}

std::vector<PrismaSessionFile> PrismaIngestion::discoverSessions(
    std::optional<std::chrono::system_clock::time_point> last_session_start)
{
    std::vector<PrismaSessionFile> sessions;

    if (!initialized_ && !initialize()) return sessions;

    std::string last_date;
    if (last_session_start) {
        auto tt = std::chrono::system_clock::to_time_t(*last_session_start);
        std::tm tm{};
        localtime_r(&tt, &tm);
        char buf[9];
        std::strftime(buf, sizeof(buf), "%Y%m%d", &tm);
        last_date = buf;
    }

    if (combined_layout_)
        return discoverCombined(last_date, last_session_start);

    std::string events_dir = therapy_dir_ + "/events";
    std::string signals_dir = therapy_dir_ + "/signals";

    if (!fs::is_directory(events_dir) || !fs::is_directory(signals_dir)) {
        std::cerr << "PrismaIngestion: events/ or signals/ not found in "
                  << therapy_dir_ << std::endl;
        return sessions;
    }

    std::regex seq_re(R"((?:event|signal)_(\d+)\.\w+)");

    std::set<std::string> date_folders;
    for (const auto& entry : fs::directory_iterator(events_dir)) {
        if (!entry.is_directory()) continue;
        std::string name = entry.path().filename().string();
        if (name.size() == 8 && std::all_of(name.begin(), name.end(), ::isdigit)) {
            if (last_date.empty() || name >= last_date)
                date_folders.insert(name);
        }
    }

    for (const auto& date : date_folders) {
        std::string evt_dir = events_dir + "/" + date;
        std::string sig_dir = signals_dir + "/" + date;

        if (!fs::is_directory(sig_dir)) continue;

        std::map<int, std::pair<std::string, std::string>> pairs;

        for (const auto& entry : fs::directory_iterator(evt_dir)) {
            if (!entry.is_regular_file()) continue;
            std::string name = entry.path().filename().string();
            std::smatch m;
            if (std::regex_match(name, m, seq_re)) {
                int seq = std::stoi(m[1].str());
                pairs[seq].first = entry.path().string();
            }
        }

        for (const auto& entry : fs::directory_iterator(sig_dir)) {
            if (!entry.is_regular_file()) continue;
            std::string name = entry.path().filename().string();
            std::smatch m;
            if (std::regex_match(name, m, seq_re)) {
                int seq = std::stoi(m[1].str());
                pairs[seq].second = entry.path().string();
            }
        }

        for (const auto& [seq, files] : pairs) {
            if (files.second.empty()) continue;
            if (auto sf = buildSessionFile(date, seq, files.first, files.second,
                                           last_session_start))
                sessions.push_back(std::move(*sf));
        }
    }

    std::sort(sessions.begin(), sessions.end(),
              [](const PrismaSessionFile& a, const PrismaSessionFile& b) {
                  return a.session_start < b.session_start;
              });

    return sessions;
}

// Combined SMART-max layout: <combined_root_>/<YYYYMMDD>/<NNNN>/{event_*.xml,
// signal_*.wmedf}. Event and signal share a sequence number within the same
// session subfolder; a date may have more than one subfolder.
std::vector<PrismaSessionFile> PrismaIngestion::discoverCombined(
    const std::string& last_date,
    std::optional<std::chrono::system_clock::time_point> last_session_start)
{
    std::vector<PrismaSessionFile> sessions;
    std::regex seq_re(R"((?:event|signal)_(\d+)\.\w+)");
    std::error_code ec;

    std::set<std::string> date_folders;
    for (const auto& entry : fs::directory_iterator(combined_root_, ec)) {
        if (!entry.is_directory()) continue;
        std::string name = entry.path().filename().string();
        if (name.size() == 8 && std::all_of(name.begin(), name.end(), ::isdigit)) {
            if (last_date.empty() || name >= last_date)
                date_folders.insert(name);
        }
    }

    for (const auto& date : date_folders) {
        for (const auto& sub : fs::directory_iterator(combined_root_ + "/" + date, ec)) {
            if (!sub.is_directory()) continue;

            std::map<int, std::pair<std::string, std::string>> pairs;
            for (const auto& f : fs::directory_iterator(sub.path(), ec)) {
                if (!f.is_regular_file()) continue;
                std::string name = f.path().filename().string();
                std::smatch m;
                if (!std::regex_match(name, m, seq_re)) continue;
                int seq = std::stoi(m[1].str());
                if (name.rfind("event_", 0) == 0) pairs[seq].first = f.path().string();
                else                              pairs[seq].second = f.path().string();
            }

            for (const auto& [seq, files] : pairs) {
                if (files.second.empty()) continue;
                if (auto sf = buildSessionFile(date, seq, files.first, files.second,
                                               last_session_start))
                    sessions.push_back(std::move(*sf));
            }
        }
    }

    std::sort(sessions.begin(), sessions.end(),
              [](const PrismaSessionFile& a, const PrismaSessionFile& b) {
                  return a.session_start < b.session_start;
              });

    return sessions;
}

std::string PrismaIngestion::stageSession(const PrismaSessionFile& session) {
    std::string staged_dir = (fs::temp_directory_path() / "prisma_staged" /
        (session.date_folder + "_" + std::to_string(session.sequence_number))).string();

    fs::create_directories(staged_dir);

    for (const auto& entry : fs::directory_iterator(staged_dir))
        fs::remove(entry.path());

    if (!session.event_path.empty() && fs::exists(session.event_path)) {
        fs::copy_file(session.event_path,
                      fs::path(staged_dir) / fs::path(session.event_path).filename(),
                      fs::copy_options::overwrite_existing);
    }
    if (fs::exists(session.signal_path)) {
        fs::copy_file(session.signal_path,
                      fs::path(staged_dir) / fs::path(session.signal_path).filename(),
                      fs::copy_options::overwrite_existing);
    }

    std::string dev_xml = deviceXmlPath();
    if (!dev_xml.empty()) {
        fs::copy_file(dev_xml, fs::path(staged_dir) / "device.xml",
                      fs::copy_options::overwrite_existing);
    }

    return staged_dir;
}

} // namespace hms_cpap
