#include "clients/FlashAirClient.h"
#include "utils/CardNames.h"
#include "utils/ConfigManager.h"
#include <algorithm>
#include <cctype>
#include <filesystem>
#include <fstream>
#include <iostream>

namespace hms_cpap {

namespace {

constexpr int kAttrHidden    = 0x02;
constexpr int kAttrSystem    = 0x04;
constexpr int kAttrVolume    = 0x08;
constexpr int kAttrDirectory = 0x10;

bool allDigits(const std::string& s) {
    return !s.empty() && std::all_of(s.begin(), s.end(),
                                     [](unsigned char c) { return std::isdigit(c); });
}

/// Back to where the file was before a request: its old length, or gone.
void restore(const std::string& local_path, size_t start_byte) {
    std::error_code ec;
    if (start_byte == 0) std::filesystem::remove(local_path, ec);
    else std::filesystem::resize_file(local_path, start_byte, ec);
}

} // namespace

FlashAirClient::FlashAirClient()
    : curl_(curl_easy_init()),
      base_url_(ConfigManager::get("FLASHAIR_BASE_URL", "http://flashair.local")) {
    if (!curl_) {
        throw std::runtime_error("Failed to initialize CURL");
    }
}

FlashAirClient::~FlashAirClient() {
    if (curl_) {
        curl_easy_cleanup(curl_);
    }
}

size_t FlashAirClient::WriteCallback(void* contents, size_t size, size_t nmemb, void* userp) {
    size_t total_size = size * nmemb;
    static_cast<std::string*>(userp)->append(static_cast<char*>(contents), total_size);
    return total_size;
}

size_t FlashAirClient::WriteFileCallback(void* contents, size_t size, size_t nmemb, void* userp) {
    size_t total_size = size * nmemb;
    static_cast<std::ofstream*>(userp)->write(static_cast<char*>(contents), total_size);
    return total_size;
}

void FlashAirClient::commonOptions(const std::string& url, long timeout) {
    curl_easy_setopt(curl_, CURLOPT_URL, url.c_str());
    curl_easy_setopt(curl_, CURLOPT_TIMEOUT, timeout);
    curl_easy_setopt(curl_, CURLOPT_CONNECTTIMEOUT, CONNECTION_TIMEOUT);
    // A missing file is a 307 to http://flashair/: never follow it.
    curl_easy_setopt(curl_, CURLOPT_FOLLOWLOCATION, 0L);
    // SDD-056: a .local name waits 5 s for an AAAA answer the card never gives.
    curl_easy_setopt(curl_, CURLOPT_IPRESOLVE, CURL_IPRESOLVE_V4);
}

// SDD-056: one entry per line, <dir>,<name>,<bytes>,<attr>,<fdate>,<ftime>,
// after a WLANSD_FILELIST header. A name may hold a comma, so the four numbers
// are taken from the right and the folder from the left.
std::vector<EzShareFileEntry> FlashAirClient::parseFileList(const std::string& body) {
    std::vector<EzShareFileEntry> entries;
    int unusable_names = 0;   // SDD-054 D7

    size_t pos = 0;
    while (pos < body.size()) {
        size_t eol = body.find('\n', pos);
        if (eol == std::string::npos) eol = body.size();
        std::string line = body.substr(pos, eol - pos);
        pos = eol + 1;
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (line.empty() || line == "WLANSD_FILELIST") continue;

        // The last four commas end the name; the first one ends the folder.
        size_t cut[4];
        size_t from = line.size();
        bool ok = true;
        for (int i = 3; i >= 0; --i) {
            if (from == 0) { ok = false; break; }
            const size_t c = line.rfind(',', from - 1);
            if (c == std::string::npos) { ok = false; break; }
            cut[i] = c;
            from = c;
        }
        if (!ok) continue;
        const size_t dir_end = line.find(',');
        if (dir_end == std::string::npos || dir_end >= cut[0]) continue;

        const std::string name  = line.substr(dir_end + 1, cut[0] - dir_end - 1);
        const std::string bytes = line.substr(cut[0] + 1, cut[1] - cut[0] - 1);
        const std::string attr  = line.substr(cut[1] + 1, cut[2] - cut[1] - 1);
        const std::string fdate = line.substr(cut[2] + 1, cut[3] - cut[2] - 1);
        const std::string ftime = line.substr(cut[3] + 1);
        if (!allDigits(bytes) || !allDigits(attr) || !allDigits(fdate) || !allDigits(ftime))
            continue;
        if (name.empty() || name == "." || name == "..") continue;

        const int a = std::stoi(attr);
        if (a & (kAttrHidden | kAttrSystem | kAttrVolume)) continue;
        if (!isCardNameUsable(name)) {
            ++unusable_names;
            continue;
        }

        EzShareFileEntry e;
        e.name = name;
        e.is_dir = (a & kAttrDirectory) != 0;
        // KB rounded down, as a local folder's are, and the exact size beside
        // it (SDD-056 D2 b).
        e.size_bytes = e.is_dir ? 0 : std::stoull(bytes);
        e.size_kb = static_cast<int>(e.size_bytes / 1024);
        const unsigned long d = std::stoul(fdate);
        const unsigned long t = std::stoul(ftime);
        e.year   = 1980 + static_cast<int>(d >> 9);
        e.month  = static_cast<int>((d >> 5) & 0x0F);
        e.day    = static_cast<int>(d & 0x1F);
        e.hour   = static_cast<int>(t >> 11);
        e.minute = static_cast<int>((t >> 5) & 0x3F);
        e.second = static_cast<int>(t & 0x1F) * 2;
        entries.push_back(std::move(e));
    }

    if (unusable_names > 0)
        std::cerr << "FlashAir: skipped " << unusable_names
                  << " listed entries whose names are not valid text (a damaged card?)"
                  << std::endl;
    return entries;
}

std::string FlashAirClient::encodeCardPath(const std::string& path) {
    static const char* kHex = "0123456789ABCDEF";
    std::string out = "/";
    out.reserve(path.size() + 8);
    for (char c : path) {
        const unsigned char u = static_cast<unsigned char>(c);
        if (c == '\\' || c == '/') {
            if (out.back() != '/') out += '/';
        } else if ((u >= 'A' && u <= 'Z') || (u >= 'a' && u <= 'z') ||
                   (u >= '0' && u <= '9') || u == '-' || u == '_' || u == '.' || u == '~') {
            out += c;
        } else {
            out += '%';
            out += kHex[u >> 4];
            out += kHex[u & 0x0F];
        }
    }
    return out;
}

std::vector<EzShareFileEntry> FlashAirClient::list(const std::string& dir) {
    const std::string url = base_url_ + "/command.cgi?op=100&DIR=" + dir;
    // A listing is a 200 (one way to survive an ez Share, D2), and the
    // FlashAir answers a missing folder 404 at once, so it is not retried.
    std::string response;
    commonOptions(url, CONNECTION_TIMEOUT);
    curl_easy_setopt(curl_, CURLOPT_WRITEFUNCTION, WriteCallback);
    curl_easy_setopt(curl_, CURLOPT_WRITEDATA, &response);
    curl_easy_setopt(curl_, CURLOPT_LOW_SPEED_LIMIT, 0L);

    const CURLcode res = curl_easy_perform(curl_);
    if (res != CURLE_OK) {
        const std::string error =
            std::string("HTTP GET failed (") + url + "): " + curl_easy_strerror(res);
        auto decision = http_fail_log_.onFailure(error);
        if (decision.log) std::cerr << decision.message << std::endl;
        throw std::runtime_error(error);
    }
    if (auto recovered = http_fail_log_.onSuccess(); recovered.log)
        std::cerr << "FlashAir " << recovered.message << std::endl;

    long http_code = 0;
    curl_easy_getinfo(curl_, CURLINFO_RESPONSE_CODE, &http_code);
    if (http_code != 200)
        throw ListingRefused("FlashAir answered HTTP " + std::to_string(http_code) + " for " + url);
    return parseFileList(response);
}

std::vector<std::string> FlashAirClient::listDateFolders() {
    std::vector<std::string> folders;
    for (const auto& e : list("/DATALOG"))
        if (e.is_dir && e.name.size() == 8 && allDigits(e.name)) folders.push_back(e.name);
    std::sort(folders.begin(), folders.end());
    return folders;
}

std::vector<EzShareFileEntry> FlashAirClient::listFiles(const std::string& date_folder) {
    std::cout << "FlashAir: listing " << date_folder << std::endl;
    std::vector<EzShareFileEntry> files;
    for (auto& e : list("/DATALOG/" + date_folder))
        if (!e.is_dir) files.push_back(std::move(e));
    std::cout << "FlashAir: found " << files.size() << " files in " << date_folder << std::endl;
    return files;
}

std::vector<EzShareFileEntry> FlashAirClient::listDir(const std::string& card_path) {
    std::cout << "FlashAir: listing dir '" << (card_path.empty() ? "<root>" : card_path)
              << "'" << std::endl;
    std::string dir = encodeCardPath(card_path);
    if (dir.size() > 1 && dir.back() == '/') dir.pop_back();
    return list(dir);
}

bool FlashAirClient::fetch(const std::string& url_path, const std::string& local_path,
                           size_t start_byte, size_t& bytes_downloaded) {
    bytes_downloaded = 0;
    const std::string url = base_url_ + url_path;

    std::filesystem::create_directories(std::filesystem::path(local_path).parent_path());
    std::ofstream output(local_path, std::ios::binary |
                                         (start_byte == 0 ? std::ios::trunc : std::ios::app));
    if (!output.is_open()) {
        std::cerr << "FlashAir: cannot open " << local_path << std::endl;
        return false;
    }

    struct curl_slist* headers = nullptr;
    if (start_byte > 0) {
        const std::string range = "Range: bytes=" + std::to_string(start_byte) + "-";
        headers = curl_slist_append(headers, range.c_str());
    }
    commonOptions(url, DOWNLOAD_TIMEOUT);
    curl_easy_setopt(curl_, CURLOPT_HTTPHEADER, headers);
    curl_easy_setopt(curl_, CURLOPT_WRITEFUNCTION, WriteFileCallback);
    curl_easy_setopt(curl_, CURLOPT_WRITEDATA, &output);
    // A stalled transfer fails in LOW_SPEED_TIME; what landed is kept and the
    // next burst resumes from it.
    curl_easy_setopt(curl_, CURLOPT_LOW_SPEED_LIMIT, LOW_SPEED_LIMIT);
    curl_easy_setopt(curl_, CURLOPT_LOW_SPEED_TIME, LOW_SPEED_TIME);

    const CURLcode res = curl_easy_perform(curl_);
    output.close();
    curl_slist_free_all(headers);
    curl_easy_setopt(curl_, CURLOPT_HTTPHEADER, nullptr);
    curl_easy_setopt(curl_, CURLOPT_LOW_SPEED_LIMIT, 0L);
    curl_easy_setopt(curl_, CURLOPT_LOW_SPEED_TIME, 0L);

    long http_code = 0;
    curl_easy_getinfo(curl_, CURLINFO_RESPONSE_CODE, &http_code);

    // A 307 (no such file) or any error reply is not the file.
    if (http_code != 200 && http_code != 206) {
        restore(local_path, start_byte);
        std::cerr << "FlashAir: HTTP " << http_code << " for " << url_path << std::endl;
        return false;
    }
    // CURLE_PARTIAL_FILE: a night file that grew while it was being read.
    if (res != CURLE_OK && res != CURLE_PARTIAL_FILE) {
        std::cerr << "FlashAir: download failed (" << url_path << "): "
                  << curl_easy_strerror(res) << std::endl;
        if (start_byte == 0) restore(local_path, 0);
        return false;
    }
    // A 200 to a range is the whole file appended after the bytes we had.
    if (http_code == 200 && start_byte > 0) {
        restore(local_path, start_byte);
        supports_range_ = false;
        std::cerr << "FlashAir: range ignored for " << url_path
                  << " (HTTP 200); no more ranged requests" << std::endl;
        return false;
    }

    curl_off_t got = 0;
    curl_easy_getinfo(curl_, CURLINFO_SIZE_DOWNLOAD_T, &got);
    bytes_downloaded = static_cast<size_t>(got);

    std::error_code ec;
    const auto total = std::filesystem::file_size(local_path, ec);
    if (start_byte == 0 && (ec || total == 0)) {
        std::cerr << "FlashAir: empty file " << url_path << std::endl;
        restore(local_path, 0);
        return false;
    }
    std::cout << "FlashAir: " << url_path << " -> " << bytes_downloaded
              << " bytes (total now: " << total << " bytes)" << std::endl;
    return true;
}

bool FlashAirClient::downloadFile(const std::string& date_folder,
                                  const std::string& filename,
                                  const std::string& local_path) {
    size_t got = 0;
    return fetch(encodeCardPath("DATALOG\\" + date_folder + "\\" + filename),
                 local_path, 0, got);
}

bool FlashAirClient::downloadFileRange(const std::string& date_folder,
                                       const std::string& filename,
                                       const std::string& local_path,
                                       size_t start_byte,
                                       size_t& bytes_downloaded) {
    return fetch(encodeCardPath("DATALOG\\" + date_folder + "\\" + filename),
                 local_path, start_byte, bytes_downloaded);
}

bool FlashAirClient::downloadRootFile(const std::string& filename,
                                      const std::string& local_path) {
    size_t got = 0;
    return fetch(encodeCardPath(filename), local_path, 0, got);
}

bool FlashAirClient::downloadByPath(const std::string& card_rel_path,
                                    const std::string& local_path) {
    size_t got = 0;
    return fetch(encodeCardPath(card_rel_path), local_path, 0, got);
}

} // namespace hms_cpap
