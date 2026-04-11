#include "clients/EzShareClient.h"
#include "utils/ConfigManager.h"
#include <iostream>
#include <fstream>
#include <regex>
#include <filesystem>
#include <algorithm>
#include <ctime>

namespace hms_cpap {

std::chrono::system_clock::time_point EzShareFileEntry::getModTime() const {
    std::tm tm = {};
    tm.tm_year = year - 1900;
    tm.tm_mon  = month - 1;
    tm.tm_mday = day;
    tm.tm_hour = hour;
    tm.tm_min  = minute;
    tm.tm_sec  = second;
    tm.tm_isdst = -1;
    return std::chrono::system_clock::from_time_t(std::mktime(&tm));
}

EzShareClient::EzShareClient()
    : curl_(curl_easy_init()),
      base_url_(ConfigManager::get("EZSHARE_BASE_URL", "http://192.168.4.1")) {
    if (!curl_) {
        throw std::runtime_error("Failed to initialize CURL");
    }
}

EzShareClient::~EzShareClient() {
    if (curl_) {
        curl_easy_cleanup(curl_);
    }
}

size_t EzShareClient::WriteCallback(void* contents, size_t size, size_t nmemb, void* userp) {
    size_t total_size = size * nmemb;
    static_cast<std::string*>(userp)->append(static_cast<char*>(contents), total_size);
    return total_size;
}

size_t EzShareClient::WriteFileCallback(void* contents, size_t size, size_t nmemb, void* userp) {
    size_t total_size = size * nmemb;
    static_cast<std::ofstream*>(userp)->write(static_cast<char*>(contents), total_size);
    return total_size;
}

std::string EzShareClient::httpGet(const std::string& url) {
    std::string response;

    curl_easy_setopt(curl_, CURLOPT_URL, url.c_str());
    curl_easy_setopt(curl_, CURLOPT_WRITEFUNCTION, WriteCallback);
    curl_easy_setopt(curl_, CURLOPT_WRITEDATA, &response);
    curl_easy_setopt(curl_, CURLOPT_TIMEOUT, CONNECTION_TIMEOUT);
    curl_easy_setopt(curl_, CURLOPT_CONNECTTIMEOUT, CONNECTION_TIMEOUT);
    curl_easy_setopt(curl_, CURLOPT_FOLLOWLOCATION, 0L);

    CURLcode res = curl_easy_perform(curl_);
    if (res != CURLE_OK) {
        std::string error = std::string("HTTP GET failed (") + url + "): " + curl_easy_strerror(res);
        std::cerr << error << std::endl;
        throw std::runtime_error(error);
    }

    return response;
}

// Parse the ez Share HTML directory listing.
//
// Actual format (from real hardware):
//   <pre>
//   2026- 2- 4    0:18:10           1KB  <a href="..."> 20260204_001809_CSL.edf</a>
//   2026- 2- 3   20:50:32         <DIR>   <a href="dir?dir=..."> .</a>
//   </pre>
//
// Each line has: YYYY-MM-DD HH:MM:SS  SIZE  <a href>NAME</a>
// Files have size in KB; directories have <DIR>
std::vector<EzShareFileEntry> EzShareClient::parseDirectoryListing(const std::string& html) {
    std::vector<EzShareFileEntry> entries;

    // Full pattern capturing timestamp + size/DIR + name:
    //   YYYY- M- D   HH:MM:SS   SIZE_KB  <a href="..."> NAME</a>
    //   YYYY- M- D   HH:MM:SS   <DIR>    <a href="..."> NAME</a>
    std::regex entry_regex(
        R"((\d{4})-\s*(\d{1,2})-\s*(\d{1,2})\s+(\d{1,2}):\s*(\d{1,2}):\s*(\d{1,2})\s+)"
        R"((?:(\d+)KB|(?:&lt;DIR&gt;|<DIR>))\s+)"
        R"(<a\s+href="[^"]*">\s*([^<]+)</a>)",
        std::regex_constants::icase
    );

    std::sregex_iterator iter(html.begin(), html.end(), entry_regex);
    std::sregex_iterator end;

    for (; iter != end; ++iter) {
        const auto& match = *iter;
        EzShareFileEntry entry;

        // Timestamp (groups 1-6)
        entry.year   = std::stoi(match[1].str());
        entry.month  = std::stoi(match[2].str());
        entry.day    = std::stoi(match[3].str());
        entry.hour   = std::stoi(match[4].str());
        entry.minute = std::stoi(match[5].str());
        entry.second = std::stoi(match[6].str());

        // Size or DIR (group 7 = size if file, empty if dir)
        if (match[7].matched) {
            entry.size_kb = std::stoi(match[7].str());
            entry.is_dir = false;
        } else {
            entry.is_dir = true;
        }

        // Name (group 8)
        entry.name = match[8].str();

        // Trim whitespace from name
        while (!entry.name.empty() && entry.name.front() == ' ')
            entry.name.erase(entry.name.begin());
        while (!entry.name.empty() && entry.name.back() == ' ')
            entry.name.pop_back();

        // Skip . and ..
        if (entry.name == "." || entry.name == "..")
            continue;

        entries.push_back(std::move(entry));
    }

    return entries;
}

std::vector<std::string> EzShareClient::listDateFolders() {
    // GET /dir?dir=A:DATALOG → lists date folders like 20260203, 20260204
    std::string url = base_url_ + "/dir?dir=A:DATALOG";
    std::string html = httpGet(url);
    if (html.empty()) return {};

    auto entries = parseDirectoryListing(html);
    std::vector<std::string> folders;

    for (const auto& e : entries) {
        // Date folders are directories with 8-digit names
        if (e.is_dir && e.name.size() == 8 &&
            std::all_of(e.name.begin(), e.name.end(), ::isdigit)) {
            folders.push_back(e.name);
        }
    }

    std::sort(folders.begin(), folders.end());
    return folders;
}

std::vector<EzShareFileEntry> EzShareClient::listFiles(const std::string& date_folder) {
    // GET /dir?dir=A:DATALOG%5CYYYYMMDD → lists session files
    // %5C = backslash (required, forward slash returns 404)
    std::string url = base_url_ + "/dir?dir=A:DATALOG%5C" + date_folder;

    std::cout << "EzShare: listing " << date_folder << std::endl;

    std::string html = httpGet(url);
    if (html.empty()) {
        std::cerr << "EzShare: empty response for " << date_folder << std::endl;
        return {};
    }

    auto entries = parseDirectoryListing(html);

    // Filter to files only (skip directories)
    std::vector<EzShareFileEntry> files;
    for (auto& e : entries) {
        if (!e.is_dir) {
            files.push_back(std::move(e));
        }
    }

    std::cout << "EzShare: found " << files.size() << " files in " << date_folder << std::endl;
    return files;
}

bool EzShareClient::downloadFile(const std::string& date_folder,
                                  const std::string& filename,
                                  const std::string& local_path) {
    // Construct download URL: /download?file=DATALOG%5CYYYYMMDD%5Cfilename
    // Note: Card hrefs have a firmware bug (drops first char). We build URLs ourselves.
    // Backslash (%5C) is required; forward slashes return 404.
    std::string url = base_url_ + "/download?file=DATALOG%5C"
                    + date_folder + "%5C" + filename;

    std::cout << "EzShare: downloading " << filename << " (" << date_folder << ")" << std::endl;

    // Create parent directories
    std::filesystem::create_directories(
        std::filesystem::path(local_path).parent_path()
    );

    std::ofstream output(local_path, std::ios::binary);
    if (!output.is_open()) {
        std::cerr << "EzShare: cannot open " << local_path << std::endl;
        return false;
    }

    curl_easy_setopt(curl_, CURLOPT_URL, url.c_str());
    curl_easy_setopt(curl_, CURLOPT_WRITEFUNCTION, WriteFileCallback);
    curl_easy_setopt(curl_, CURLOPT_WRITEDATA, &output);
    curl_easy_setopt(curl_, CURLOPT_TIMEOUT, DOWNLOAD_TIMEOUT);
    curl_easy_setopt(curl_, CURLOPT_CONNECTTIMEOUT, CONNECTION_TIMEOUT);
    curl_easy_setopt(curl_, CURLOPT_FOLLOWLOCATION, 0L);

    CURLcode res = curl_easy_perform(curl_);
    output.close();

    // Accept CURLE_PARTIAL_FILE (18) - happens with growing files during active CPAP session
    if (res != CURLE_OK && res != CURLE_PARTIAL_FILE) {
        std::cerr << "EzShare: download failed (" << filename << "): "
                  << curl_easy_strerror(res) << std::endl;
        std::filesystem::remove(local_path);
        return false;
    }

    if (res == CURLE_PARTIAL_FILE) {
        std::cout << "EzShare: partial file downloaded (" << filename << ") - growing file OK" << std::endl;
    }

    // Check HTTP status
    long http_code = 0;
    curl_easy_getinfo(curl_, CURLINFO_RESPONSE_CODE, &http_code);
    if (http_code != 200) {
        std::cerr << "EzShare: HTTP " << http_code << " for " << filename << std::endl;
        std::filesystem::remove(local_path);
        return false;
    }

    auto file_size = std::filesystem::file_size(local_path);
    if (file_size == 0) {
        std::cerr << "EzShare: empty file " << filename << std::endl;
        std::filesystem::remove(local_path);
        return false;
    }

    std::cout << "EzShare: " << filename << " -> " << file_size << " bytes" << std::endl;
    return true;
}

bool EzShareClient::downloadRootFile(const std::string& filename,
                                      const std::string& local_path) {
    // Root-level file on SD card: /download?file=filename
    std::string url = base_url_ + "/download?file=" + filename;

    std::cout << "EzShare: downloading root file " << filename << std::endl;

    std::filesystem::create_directories(
        std::filesystem::path(local_path).parent_path()
    );

    std::ofstream output(local_path, std::ios::binary);
    if (!output.is_open()) {
        std::cerr << "EzShare: cannot open " << local_path << std::endl;
        return false;
    }

    curl_easy_setopt(curl_, CURLOPT_URL, url.c_str());
    curl_easy_setopt(curl_, CURLOPT_WRITEFUNCTION, WriteFileCallback);
    curl_easy_setopt(curl_, CURLOPT_WRITEDATA, &output);
    curl_easy_setopt(curl_, CURLOPT_TIMEOUT, DOWNLOAD_TIMEOUT);
    curl_easy_setopt(curl_, CURLOPT_CONNECTTIMEOUT, CONNECTION_TIMEOUT);
    curl_easy_setopt(curl_, CURLOPT_FOLLOWLOCATION, 0L);

    CURLcode res = curl_easy_perform(curl_);
    output.close();

    if (res != CURLE_OK && res != CURLE_PARTIAL_FILE) {
        std::cerr << "EzShare: root download failed (" << filename << "): "
                  << curl_easy_strerror(res) << std::endl;
        std::filesystem::remove(local_path);
        return false;
    }

    long http_code = 0;
    curl_easy_getinfo(curl_, CURLINFO_RESPONSE_CODE, &http_code);
    if (http_code != 200) {
        std::cerr << "EzShare: HTTP " << http_code << " for " << filename << std::endl;
        std::filesystem::remove(local_path);
        return false;
    }

    auto file_size = std::filesystem::file_size(local_path);
    if (file_size == 0) {
        std::cerr << "EzShare: empty file " << filename << std::endl;
        std::filesystem::remove(local_path);
        return false;
    }

    std::cout << "EzShare: " << filename << " -> " << file_size << " bytes" << std::endl;
    return true;
}

bool EzShareClient::downloadFileRange(const std::string& date_folder,
                                        const std::string& filename,
                                        const std::string& local_path,
                                        size_t start_byte,
                                        size_t& bytes_downloaded) {
    // Construct download URL
    std::string url = base_url_ + "/download?file=DATALOG%5C"
                    + date_folder + "%5C" + filename;

    std::cout << "EzShare: Range download " << filename << " from byte " << start_byte << std::endl;

    // Create parent directories
    std::filesystem::create_directories(
        std::filesystem::path(local_path).parent_path()
    );

    // Open file for append (binary mode)
    std::ios_base::openmode mode = std::ios::binary;
    if (start_byte == 0) {
        mode |= std::ios::trunc;  // Overwrite if starting from beginning
    } else {
        mode |= std::ios::app;    // Append if resuming
    }

    std::ofstream output(local_path, mode);
    if (!output.is_open()) {
        std::cerr << "EzShare: cannot open " << local_path << std::endl;
        return false;
    }

    // Set HTTP Range header
    std::string range_header = "Range: bytes=" + std::to_string(start_byte) + "-";
    struct curl_slist* headers = nullptr;
    headers = curl_slist_append(headers, range_header.c_str());

    curl_easy_setopt(curl_, CURLOPT_URL, url.c_str());
    curl_easy_setopt(curl_, CURLOPT_HTTPHEADER, headers);
    curl_easy_setopt(curl_, CURLOPT_WRITEFUNCTION, WriteFileCallback);
    curl_easy_setopt(curl_, CURLOPT_WRITEDATA, &output);
    curl_easy_setopt(curl_, CURLOPT_TIMEOUT, DOWNLOAD_TIMEOUT);
    curl_easy_setopt(curl_, CURLOPT_CONNECTTIMEOUT, CONNECTION_TIMEOUT);
    curl_easy_setopt(curl_, CURLOPT_FOLLOWLOCATION, 0L);

    CURLcode res = curl_easy_perform(curl_);
    output.close();

    // Clean up headers
    curl_slist_free_all(headers);
    curl_easy_setopt(curl_, CURLOPT_HTTPHEADER, nullptr);

    // Accept CURLE_OK or CURLE_PARTIAL_FILE (growing file)
    if (res != CURLE_OK && res != CURLE_PARTIAL_FILE) {
        std::cerr << "EzShare: Range download failed (" << filename << "): "
                  << curl_easy_strerror(res) << std::endl;
        return false;
    }

    // Check HTTP status (200 OK or 206 Partial Content)
    long http_code = 0;
    curl_easy_getinfo(curl_, CURLINFO_RESPONSE_CODE, &http_code);
    if (http_code != 200 && http_code != 206) {
        std::cerr << "EzShare: HTTP " << http_code << " for " << filename << std::endl;
        return false;
    }

    // Get downloaded size
    double download_size = 0;
    curl_easy_getinfo(curl_, CURLINFO_SIZE_DOWNLOAD, &download_size);
    bytes_downloaded = static_cast<size_t>(download_size);

    auto total_size = std::filesystem::file_size(local_path);
    std::cout << "EzShare: " << filename << " -> downloaded " << bytes_downloaded
              << " bytes (total now: " << total_size << " bytes)" << std::endl;

    return true;
}

bool EzShareClient::downloadSession(const std::string& date_folder,
                                     const std::string& local_dir) {
    std::cout << "EzShare: downloading session " << date_folder << std::endl;

    auto files = listFiles(date_folder);
    if (files.empty()) {
        std::cerr << "EzShare: no files in " << date_folder << std::endl;
        return false;
    }

    // Filter for EDF files (BRP, EVE, SAD, PLD, CSL)
    std::vector<std::string> edf_suffixes = {"_BRP.edf", "_EVE.edf", "_SAD.edf",
                                              "_SA2.edf", "_PLD.edf", "_CSL.edf"};
    std::vector<std::string> edf_files;

    for (const auto& f : files) {
        for (const auto& suffix : edf_suffixes) {
            // Case-insensitive suffix match
            if (f.name.size() >= suffix.size()) {
                std::string file_end = f.name.substr(f.name.size() - suffix.size());
                std::transform(file_end.begin(), file_end.end(), file_end.begin(), ::tolower);
                std::string suffix_lower = suffix;
                std::transform(suffix_lower.begin(), suffix_lower.end(), suffix_lower.begin(), ::tolower);
                if (file_end == suffix_lower) {
                    edf_files.push_back(f.name);
                    break;
                }
            }
        }
    }

    if (edf_files.empty()) {
        std::cerr << "EzShare: no EDF files in " << date_folder << std::endl;
        return false;
    }

    std::cout << "EzShare: downloading " << edf_files.size() << " EDF files" << std::endl;

    // Create local directory
    std::filesystem::create_directories(local_dir);

    int downloaded = 0;

    for (const auto& filename : edf_files) {
        std::string local_path = local_dir + "/" + filename;

        if (downloadFile(date_folder, filename, local_path)) {
            ++downloaded;
        } else {
            std::cerr << "EzShare: failed to download " << filename << std::endl;
        }
    }

    std::cout << "EzShare: session " << date_folder << " complete ("
              << downloaded << "/" << edf_files.size() << " files)" << std::endl;

    return downloaded > 0;
}

} // namespace hms_cpap
