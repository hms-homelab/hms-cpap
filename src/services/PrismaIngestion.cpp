#include "services/PrismaIngestion.h"
#include "utils/TimeCompat.h"
#include "miniz.h"
#include <filesystem>
#include <iostream>
#include <regex>
#include <algorithm>
#include <set>

namespace hms_cpap {

namespace fs = std::filesystem;

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
    if (!findTherapyRoot(therapy_dir_) && !findTherapyRoot(data_dir_)) {
        std::cerr << "PrismaIngestion: no events/signals directories found in "
                  << data_dir_ << std::endl;
        return false;
    }

    std::cout << "PrismaIngestion: " << (is_zip_mode_ ? "ZIP" : "raw directory")
              << " mode, therapy at " << therapy_dir_ << std::endl;

    initialized_ = true;
    return true;
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

    std::string events_dir = therapy_dir_ + "/events";
    std::string signals_dir = therapy_dir_ + "/signals";

    if (!fs::is_directory(events_dir) || !fs::is_directory(signals_dir)) {
        std::cerr << "PrismaIngestion: events/ or signals/ not found in "
                  << therapy_dir_ << std::endl;
        return sessions;
    }

    std::string last_date;
    if (last_session_start) {
        auto tt = std::chrono::system_clock::to_time_t(*last_session_start);
        std::tm tm{};
        localtime_r(&tt, &tm);
        char buf[9];
        std::strftime(buf, sizeof(buf), "%Y%m%d", &tm);
        last_date = buf;
    }

    std::regex seq_re(R"((?:event|signal)_(\d{6})\.\w+)");

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

            PrismaSessionFile sf;
            sf.date_folder = date;
            sf.sequence_number = seq;
            sf.event_path = files.first;
            sf.signal_path = files.second;

            cpapdash::parser::EDFFile edf;
            if (edf.open(files.second)) {
                sf.session_start = edf.getStartTime();
            }

            if (sf.session_start == std::chrono::system_clock::time_point{}) {
                std::tm tm{};
                int y = std::stoi(date.substr(0, 4));
                int mo = std::stoi(date.substr(4, 2));
                int d = std::stoi(date.substr(6, 2));
                tm.tm_year = y - 1900;
                tm.tm_mon = mo - 1;
                tm.tm_mday = d;
                sf.session_start = std::chrono::system_clock::from_time_t(mktime(&tm));
            }

            if (last_session_start && sf.session_start <= *last_session_start)
                continue;

            sessions.push_back(std::move(sf));
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
