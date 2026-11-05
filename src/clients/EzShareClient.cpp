#include "clients/EzShareClient.h"
#include "utils/CardNames.h"
#include "parsers/CpapdashBridge.h"
#include "utils/ConfigManager.h"
#include <chrono>
#include <atomic>
#include <iostream>
#include <thread>
#include <fstream>
#include <regex>
#include <filesystem>
#include <algorithm>
#include <cctype>
#include <cstdio>
#include <ctime>

namespace hms_cpap {

namespace {

bool isListing(const std::string& html) {
    if (html.find("publicdir/welcome.htm") != std::string::npos) return false;
    // Parent entries count too: an empty directory is still a listing.
    static const std::regex row(
        R"(\d{4}-\s*\d{1,2}-\s*\d{1,2}\s+\d{1,2}:\s*\d{1,2}:\s*\d{1,2}\s+)"
        R"((?:\d+KB|&lt;DIR&gt;|<DIR>)\s+<a\s+href="[^"]*">\s*[^<]+</a>)",
        std::regex::icase);
    return std::regex_search(html, row) ||
        std::regex_search(html, std::regex(R"(<pre>\s*</pre>)", std::regex::icase));
}

/// The text of an error reply, read back from where it landed in the file,
/// for the log line. A bridge says there why it could not fetch the file
/// ("ezShare request failed: card answered HTTP 404"), and the file is about
/// to be cut back, so this is the last chance to keep it. Printable characters
/// only, and short: a card's error page is HTML.
std::string errorReason(const std::string& path, size_t from) {
    std::ifstream f(path, std::ios::binary);
    if (!f.seekg(static_cast<std::streamoff>(from))) return "";
    char buf[120];
    f.read(buf, sizeof(buf));
    std::string text;
    for (std::streamsize i = 0; i < f.gcount(); ++i) {
        const unsigned char c = static_cast<unsigned char>(buf[i]);
        if (c >= 0x20 && c < 0x7f) text += static_cast<char>(c);
    }
    return text.empty() ? "" : ": " + text;
}

} // namespace

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

namespace {

size_t collectHeader(char* line, size_t size, size_t nmemb, void* userp) {
    static_cast<std::string*>(userp)->append(line, size * nmemb);
    return size * nmemb;
}

/// The span a reply's "Content-Range: bytes a-b/c" promises. `bytes` is 0 when
/// it has none, or one that does not parse.
struct PromisedRange {
    uint64_t first = 0;
    uint64_t bytes = 0;
};

PromisedRange promisedRange(std::string headers) {
    std::transform(headers.begin(), headers.end(), headers.begin(),
                   [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });
    const auto at = headers.find("content-range:");
    if (at == std::string::npos) return {};
    unsigned long long first = 0, last = 0;
    if (std::sscanf(headers.c_str() + at, "content-range: bytes %llu-%llu", &first, &last) != 2 ||
        last < first) {
        return {};
    }
    return {first, last - first + 1};
}

}  // namespace

size_t EzShareClient::WriteFileCallback(void* contents, size_t size, size_t nmemb, void* userp) {
    size_t total_size = size * nmemb;
    static_cast<std::ofstream*>(userp)->write(static_cast<char*>(contents), total_size);
    return total_size;
}

std::string EzShareClient::httpGet(const std::string& url) {
    // One way to survive an ez Share, D2: a listing is a 200. Any other status
    // used to be parsed as an empty listing. Asked three times, 400 ms apart,
    // as the cloud's bridge does, then raised for that folder alone.
    constexpr int kAttempts = 3;
    long http_code = 0;
    for (int attempt = 1; attempt <= kAttempts; ++attempt) {
        std::string response;

        curl_easy_setopt(curl_, CURLOPT_URL, url.c_str());
        curl_easy_setopt(curl_, CURLOPT_WRITEFUNCTION, WriteCallback);
        curl_easy_setopt(curl_, CURLOPT_WRITEDATA, &response);
        curl_easy_setopt(curl_, CURLOPT_TIMEOUT, CONNECTION_TIMEOUT);
        curl_easy_setopt(curl_, CURLOPT_CONNECTTIMEOUT, CONNECTION_TIMEOUT);
        curl_easy_setopt(curl_, CURLOPT_FOLLOWLOCATION, 0L);
        curl_easy_setopt(curl_, CURLOPT_LOW_SPEED_LIMIT, 0L);   // a listing has its own total

        CURLcode res = curl_easy_perform(curl_);
        if (res != CURLE_OK) {
            std::string error = std::string("HTTP GET failed (") + url + "): " + curl_easy_strerror(res);
            // An offline device fails every poll cycle; log the first one, then
            // summarise instead of repeating the same line thousands of times.
            auto decision = http_fail_log_.onFailure(error);
            if (decision.log) std::cerr << decision.message << std::endl;
            throw std::runtime_error(error);
        }

        if (auto recovered = http_fail_log_.onSuccess(); recovered.log)
            std::cerr << "ez Share " << recovered.message << std::endl;

        http_code = 0;
        curl_easy_getinfo(curl_, CURLINFO_RESPONSE_CODE, &http_code);
        // 0: a transport that reports no status (a test's file:// URL) is taken as is.
        if (http_code == 200 || http_code == 0) return response;
        if (attempt < kAttempts) std::this_thread::sleep_for(std::chrono::milliseconds(400));
    }
    throw ListingRefused("ez Share answered HTTP " + std::to_string(http_code) + " for " + url);
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
    int unusable_names = 0;   // SDD-054 D7

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

        // SDD-054 D7: a damaged card's 0xFF "names" are not names.
        if (!isCardNameUsable(entry.name)) {
            ++unusable_names;
            continue;
        }

        entries.push_back(std::move(entry));
    }

    if (unusable_names > 0)
        std::cerr << "EzShare: skipped " << unusable_names
                  << " listed entries whose names are not valid text (a damaged card?)"
                  << std::endl;

    return entries;
}

std::vector<std::string> EzShareClient::listDateFolders() {
    if (no_listing_) {
        refreshSessions();
        std::vector<std::string> folders;
        for (const auto& pair : starts_) folders.push_back(pair.first);
        return folders;
    }
    // GET /dir?dir=A:DATALOG → lists date folders like 20260203, 20260204
    std::string url = base_url_ + "/dir?dir=A:DATALOG";
    std::string html = httpGet(url);

    auto entries = parseDirectoryListing(html);
    if (entries.empty() && !isListing(html)) no_listing_ = true;
    if (no_listing_) {
        refreshSessions();
        std::vector<std::string> folders;
        for (const auto& pair : starts_) folders.push_back(pair.first);
        return folders;
    }
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
    if (no_listing_) return enumerateFiles(date_folder);
    // GET /dir?dir=A:DATALOG%5CYYYYMMDD → lists session files
    // %5C = backslash (required, forward slash returns 404)
    std::string url = base_url_ + "/dir?dir=A:DATALOG%5C" + date_folder;

    std::cout << "EzShare: listing " << date_folder << std::endl;

    std::string html = httpGet(url);
    auto entries = parseDirectoryListing(html);
    if (entries.empty() && !isListing(html)) {
        no_listing_ = true;
        refreshSessions();
        return enumerateFiles(date_folder);
    }

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
    if (no_listing_) {
        size_t got = 0;
        return downloadChunks("DATALOG/" + date_folder + "/" + filename, local_path, 0, got);
    }
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
    // D8: a whole-file transfer stalls out like a ranged one, instead of
    // holding the card for the full 300 s.
    curl_easy_setopt(curl_, CURLOPT_LOW_SPEED_LIMIT, LOW_SPEED_LIMIT);
    curl_easy_setopt(curl_, CURLOPT_LOW_SPEED_TIME, LOW_SPEED_TIME);

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
        std::cerr << "EzShare: HTTP " << http_code << " for " << filename
                  << errorReason(local_path, 0) << std::endl;
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
    if (no_listing_) {
        size_t got = 0;
        std::string name = filename;
        if (name == "STR.EDF") name = "STR.edf";
        return downloadChunks(name, local_path, 0, got);
    }
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
    // D8: a stalled whole-file transfer fails in LOW_SPEED_TIME.
    curl_easy_setopt(curl_, CURLOPT_LOW_SPEED_LIMIT, LOW_SPEED_LIMIT);
    curl_easy_setopt(curl_, CURLOPT_LOW_SPEED_TIME, LOW_SPEED_TIME);

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
        std::cerr << "EzShare: HTTP " << http_code << " for " << filename
                  << errorReason(local_path, 0) << std::endl;
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
    bytes_downloaded = 0;
    if (no_listing_ && !chunking_) {
        return downloadChunks(date_folder.empty() ? filename : "DATALOG/" + date_folder + "/" + filename,
                              local_path, start_byte, bytes_downloaded);
    }
    // Construct download URL. No date folder is a file at the card root (a
    // Luna card's raw files, SDD-060 D4).
    std::string url = date_folder.empty()
                          ? base_url_ + "/download?file=" + encodeCardPath(filename)
                          : base_url_ + "/download?file=DATALOG%5C" + date_folder + "%5C" +
                                filename;

    if (no_listing_) url = base_url_ + "/" + filename;
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
    std::string range_header = "Range: bytes=" + std::to_string(start_byte) + "-" +
        (no_listing_ ? std::to_string(chunk_end_) : "");
    struct curl_slist* headers = nullptr;
    headers = curl_slist_append(headers, range_header.c_str());

    curl_easy_setopt(curl_, CURLOPT_URL, url.c_str());
    curl_easy_setopt(curl_, CURLOPT_HTTPHEADER, headers);
    curl_easy_setopt(curl_, CURLOPT_WRITEFUNCTION, WriteFileCallback);
    curl_easy_setopt(curl_, CURLOPT_WRITEDATA, &output);
    // A listing-free card moves about 1 KB/s, so an 8 KiB piece may need
    // longer than 8 s in total; it is cut only when it goes quiet for 8 s.
    curl_easy_setopt(curl_, CURLOPT_TIMEOUT, no_listing_ ? 60L : DOWNLOAD_TIMEOUT);
    curl_easy_setopt(curl_, CURLOPT_CONNECTTIMEOUT, CONNECTION_TIMEOUT);
    curl_easy_setopt(curl_, CURLOPT_FOLLOWLOCATION, 0L);
    // A transfer that has stalled fails in LOW_SPEED_TIME rather than holding
    // the burst for the whole DOWNLOAD_TIMEOUT; what landed is kept and the
    // next burst resumes from it.
    curl_easy_setopt(curl_, CURLOPT_LOW_SPEED_LIMIT, no_listing_ ? 1L : LOW_SPEED_LIMIT);
    curl_easy_setopt(curl_, CURLOPT_LOW_SPEED_TIME, no_listing_ ? 8L : LOW_SPEED_TIME);
    std::string reply_headers;
    curl_easy_setopt(curl_, CURLOPT_HEADERFUNCTION, collectHeader);
    curl_easy_setopt(curl_, CURLOPT_HEADERDATA, &reply_headers);

    CURLcode res = curl_easy_perform(curl_);
    output.close();

    // Clean up headers, and the stall limit, which no other request wants.
    curl_slist_free_all(headers);
    curl_easy_setopt(curl_, CURLOPT_HTTPHEADER, nullptr);
    curl_easy_setopt(curl_, CURLOPT_HEADERFUNCTION, nullptr);
    curl_easy_setopt(curl_, CURLOPT_HEADERDATA, nullptr);
    curl_easy_setopt(curl_, CURLOPT_LOW_SPEED_LIMIT, 0L);
    curl_easy_setopt(curl_, CURLOPT_LOW_SPEED_TIME, 0L);

    long http_code = 0;
    curl_easy_getinfo(curl_, CURLINFO_RESPONSE_CODE, &http_code);

    // An error reply's body is not the file. Written in append mode it lands
    // after the bytes we had, the caller keeps it as progress, and the next
    // burst resumes past it: a bridge answering 502 grew every file by its
    // error text once a burst, and soon asked the card for ranges past the
    // real end of the file. Put the file back exactly as it was.
    char* content_type = nullptr;
    curl_easy_getinfo(curl_, CURLINFO_CONTENT_TYPE, &content_type);
    // Firmware 4.4.0 answers a range that spans the whole file with a plain
    // 200 and the full body; any other 200 to a bounded range is refused.
    const bool whole_file = no_listing_ && http_code == 200 && start_byte == 0 &&
        chunk_end_ + 1 == chunk_total_;
    if (http_code >= 300 || (no_listing_ &&
        ((http_code != 206 && !whole_file) || !content_type ||
         std::string(content_type).find("text/plain") != 0))) {
        const std::string reason = errorReason(local_path, start_byte);
        std::error_code ec;
        if (start_byte == 0) std::filesystem::remove(local_path, ec);
        else std::filesystem::resize_file(local_path, start_byte, ec);
        std::cerr << "EzShare: HTTP " << http_code << " for " << filename << reason << std::endl;
        return false;
    }

    // Accept CURLE_OK or CURLE_PARTIAL_FILE (growing file)
    if (res != CURLE_OK && res != CURLE_PARTIAL_FILE &&
        !(no_listing_ && res == CURLE_OPERATION_TIMEDOUT)) {
        std::cerr << "EzShare: Range download failed (" << filename << "): "
                  << curl_easy_strerror(res) << std::endl;
        return false;
    }
    // A 200 to a range past byte 0 is the whole file, just appended after the
    // bytes we already had. Cut it back to where it was. One of these is not a
    // verdict: a bridge can lose a reply's status and send a good range as a
    // 200. Only a run of them, with no 206 between, means the server ignores
    // Range and the caller's full download has to replace the file.
    if (http_code == 200 && start_byte > 0) {
        std::error_code ec;
        std::filesystem::resize_file(local_path, start_byte, ec);
        const bool give_up = ++range_ignored_in_a_row_ >= RANGE_IGNORED_LIMIT;
        if (give_up) supports_range_ = false;
        std::cerr << "EzShare: range ignored for " << filename << " (HTTP 200); "
                  << (give_up ? "no more ranged requests" : "the next burst asks again")
                  << std::endl;
        return false;
    }
    if (http_code != 200 && http_code != 206) {
        std::cerr << "EzShare: HTTP " << http_code << " for " << filename << std::endl;
        return false;
    }

    if (http_code == 206) range_ignored_in_a_row_ = 0;

    // Get downloaded size
    double download_size = 0;
    curl_easy_getinfo(curl_, CURLINFO_SIZE_DOWNLOAD, &download_size);
    bytes_downloaded = static_cast<size_t>(download_size);

    // A reply that finished normally but holds less than it promised lost a
    // chunk from the middle: a bridge can do that and still close the reply.
    // What follows the hole is at the wrong offset, and the next read would
    // resume past it, so unlike a cut transfer none of it is kept. A cut ends
    // in CURLE_PARTIAL_FILE, and what landed before it is good.
    //
    // A range that starts anywhere but where we asked would land at the wrong
    // offset just the same, so it is put back too. A reply LONGER than its
    // span is not a hole: a file still being written can be sent past the end
    // it declared. The span it promised is good and is kept; the rest is
    // asked for again, so a live file is not refused all night.
    const auto put_back = [&] {
        std::error_code ec;
        if (start_byte == 0) std::filesystem::remove(local_path, ec);
        else std::filesystem::resize_file(local_path, start_byte, ec);
    };
    const PromisedRange promised = promisedRange(reply_headers);
    if (http_code == 206 && ((no_listing_ && (promised.bytes == 0 ||
        promised.bytes > chunk_end_ - start_byte + 1)) ||
        (promised.bytes > 0 && promised.first != start_byte))) {
        put_back();
        std::cerr << "EzShare: " << filename << " came back from byte " << promised.first
                  << ", asked from " << start_byte << "; put it back" << std::endl;
        return false;
    }
    if (res == CURLE_OK && http_code == 206 && promised.bytes > 0 && bytes_downloaded < promised.bytes) {
        put_back();
        std::cerr << "EzShare: " << filename << " came back with " << bytes_downloaded << " of "
                  << promised.bytes << " bytes; put it back" << std::endl;
        return false;
    }
    if (http_code == 206 && promised.bytes > 0 && bytes_downloaded > promised.bytes) {
        std::error_code ec;
        std::filesystem::resize_file(local_path, start_byte + promised.bytes, ec);
        bytes_downloaded = static_cast<size_t>(promised.bytes);
    }

    auto total_size = std::filesystem::file_size(local_path);
    std::cout << "EzShare: " << filename << " -> downloaded " << bytes_downloaded
              << " bytes (total now: " << total_size << " bytes)" << std::endl;

    return true;
}

// Firmware 4.4.0 answers a missing path with a 200 HTML welcome page. A
// one-byte GET both avoids its broken HEAD and gives the exact file size.
uint64_t EzShareClient::probe(const std::string& path) {
    std::string body, headers;
    const std::string url = base_url_ + "/" + path;
    curl_easy_setopt(curl_, CURLOPT_URL, url.c_str());
    curl_easy_setopt(curl_, CURLOPT_RANGE, "0-0");
    curl_easy_setopt(curl_, CURLOPT_TIMEOUT, 8L);
    curl_easy_setopt(curl_, CURLOPT_LOW_SPEED_LIMIT, 0L);
    curl_easy_setopt(curl_, CURLOPT_WRITEFUNCTION, WriteCallback);
    curl_easy_setopt(curl_, CURLOPT_WRITEDATA, &body);
    curl_easy_setopt(curl_, CURLOPT_HEADERFUNCTION, collectHeader);
    curl_easy_setopt(curl_, CURLOPT_HEADERDATA, &headers);
    CURLcode result = CURLE_OK;
    for (int attempt = 0; attempt < 3; ++attempt) {
        body.clear();
        headers.clear();
        result = curl_easy_perform(curl_);
        if (result == CURLE_OK) break;
    }
    long status = 0;
    char* type = nullptr;
    curl_easy_getinfo(curl_, CURLINFO_RESPONSE_CODE, &status);
    curl_easy_getinfo(curl_, CURLINFO_CONTENT_TYPE, &type);
    const bool plain = result == CURLE_OK && type &&
        std::string(type).find("text/plain") == 0 && body.size() == 1;
    // A one-byte file is wholly covered by bytes=0-0, so it comes back as 200.
    const bool hit = plain && status == 206;
    if (plain && status == 200) {
        curl_easy_setopt(curl_, CURLOPT_RANGE, nullptr);
        curl_easy_setopt(curl_, CURLOPT_HEADERFUNCTION, nullptr);
        curl_easy_setopt(curl_, CURLOPT_HEADERDATA, nullptr);
        return 1;
    }
    curl_easy_setopt(curl_, CURLOPT_RANGE, nullptr);
    curl_easy_setopt(curl_, CURLOPT_HEADERFUNCTION, nullptr);
    curl_easy_setopt(curl_, CURLOPT_HEADERDATA, nullptr);
    std::transform(headers.begin(), headers.end(), headers.begin(),
                   [](unsigned char c) { return std::tolower(c); });
    auto at = headers.find("content-range:");
    unsigned long long first = 0, last = 0, total = 0;
    if (!hit || at == std::string::npos ||
        std::sscanf(headers.c_str() + at, "content-range: bytes %llu-%llu/%llu",
                    &first, &last, &total) != 3 || first != 0 || last != 0) return 0;
    return total;
}

bool EzShareClient::downloadChunks(const std::string& path, const std::string& local,
                                   size_t start, size_t& downloaded) {
    downloaded = 0;
    const auto total = probe(path);
    if (!total || start > total) return false;
    size_t offset = start;
    int failures = 0;
    while (offset < total) {
        chunk_end_ = std::min<uint64_t>(offset + 8192, total) - 1;
        chunk_total_ = total;
        chunking_ = true;
        size_t got = 0;
        const bool ok = downloadFileRange("", path, local, offset, got);
        chunking_ = false;
        if (!ok || got == 0) {
            if (++failures >= 5) return false;
            continue;
        }
        offset += got;
        downloaded += got;
        failures = 0;
    }
    return true;
}

std::string EzShareClient::folderForStart(std::time_t start) {
    auto tm = *std::localtime(&start);
    if (tm.tm_hour < 12) {
        --tm.tm_mday;
        tm.tm_isdst = -1;
        std::mktime(&tm);
    }
    char folder[16];
    std::strftime(folder, sizeof(folder), "%Y%m%d", &tm);
    return folder;
}

void EzShareClient::refreshSessions() {
    const auto now = std::chrono::steady_clock::now();
    if (!starts_.empty() && now - sessions_read_at_ < SESSIONS_REFRESH) return;
    // A private temporary directory keeps concurrent sources and unrelated
    // collector staging files apart. Only a complete STR is parsed.
    static std::atomic<unsigned long> sequence{0};
    auto dir = std::filesystem::temp_directory_path() /
        ("hms-ezshare-str-" + std::to_string(
            std::chrono::steady_clock::now().time_since_epoch().count()) + "-" +
         std::to_string(sequence++));
    std::filesystem::create_directory(dir);
    struct Cleanup {
        std::filesystem::path dir;
        ~Cleanup() { std::error_code ec; std::filesystem::remove_all(dir, ec); }
    } cleanup{dir};
    size_t got = 0;
    const auto path = (dir / "STR.edf").string();
    if (!downloadChunks("STR.edf", path, 0, got))
        throw std::runtime_error("ez Share: could not read STR.edf for listing-free discovery");
    auto records = EDFParser::parseSTRFile(path, "ezshare");
    starts_.clear();
    for (const auto& record : records) {
        for (const auto& pair : record.mask_pairs) {
            const auto start = std::chrono::system_clock::to_time_t(pair.first);
            starts_[folderForStart(start)].push_back(start);
        }
    }
    sessions_read_at_ = now;
}

std::vector<EzShareFileEntry> EzShareClient::enumerateFiles(const std::string& folder) {
    std::vector<EzShareFileEntry> files;
    const auto it = starts_.find(folder);
    if (it == starts_.end()) return files;
    std::set<std::string> returned;
    for (auto start : it->second) {
        auto& names = session_names_[start];
        const auto candidate = [](std::time_t at, const std::string& type) {
            auto tm = *std::localtime(&at);
            char prefix[32];
            std::strftime(prefix, sizeof(prefix), "%Y%m%d_%H%M%S_", &tm);
            return std::string(prefix) + type + ".edf";
        };
        auto find = [&](const std::string& type, std::time_t from, int count) {
            if (names.count(type)) return;
            for (int second = 0; second < count; ++second) {
                auto name = candidate(from + second, type);
                const auto size = probe("DATALOG/" + folder + "/" + name);
                if (size) {
                    names[type] = name;
                    EzShareFileEntry e; e.name = name; e.size_bytes = size;
                    e.size_kb = (size + 1023) / 1024;
                    discovered_[folder + "/" + name] = e;
                    return;
                }
            }
        };
        find("BRP", start, 60);
        if (!names.count("BRP")) continue;
        // Recover the BRP's second without interpreting a timestamp as UTC.
        auto tm = *std::localtime(&start);
        tm.tm_sec = std::stoi(names["BRP"].substr(13, 2));
        const auto brp = std::mktime(&tm);
        for (const auto* type : {"PLD", "SA2", "SAD", "EVE", "CSL"}) {
            if ((std::string(type) == "SAD" && names.count("SA2")) ||
                (std::string(type) == "SA2" && names.count("SAD"))) continue;
            find(type, brp - 15, 31);
        }
        for (const auto& name : names) {
            if (!returned.insert(name.second).second) continue;
            auto& e = discovered_[folder + "/" + name.second];
            const auto size = probe("DATALOG/" + folder + "/" + name.second);
            if (!size) continue;
            e.size_bytes = size; e.size_kb = (size + 1023) / 1024;
            files.push_back(e);
        }
    }
    return files;
}

std::string EzShareClient::encodeCardPath(const std::string& path) {
    // ez Share requires backslash separators URL-encoded as %5C; forward slashes 404.
    // Everything outside the URL's unreserved set is percent-encoded too: a card
    // can hold "System Volume Information " (spaces, one trailing), and a raw
    // space is a malformed URL that curl refuses before anything is sent.
    static const char* kHex = "0123456789ABCDEF";
    std::string out;
    out.reserve(path.size() + 8);
    for (char c : path) {
        const unsigned char u = static_cast<unsigned char>(c);
        if ((u >= 'A' && u <= 'Z') || (u >= 'a' && u <= 'z') || (u >= '0' && u <= '9') ||
            u == '-' || u == '_' || u == '.' || u == '~') {
            out += c;
        } else {
            out += '%';
            out += kHex[u >> 4];
            out += kHex[u & 0x0F];
        }
    }
    return out;
}

std::vector<EzShareFileEntry> EzShareClient::listDir(const std::string& card_path) {
    if (no_listing_) {
        if (card_path == "DATALOG") {
            std::vector<EzShareFileEntry> entries;
            for (const auto& folder : starts_) {
                EzShareFileEntry e; e.name = folder.first; e.is_dir = true;
                entries.push_back(e);
            }
            return entries;
        }
        const std::vector<std::string> names = card_path.empty()
            ? std::vector<std::string>{"STR.edf", "Identification.json", "Identification.crc", "journal.jnl"}
            : card_path == "SETTINGS"
                ? std::vector<std::string>{"CurrentSettings.json", "CurrentSettings.crc"}
                : std::vector<std::string>{};
        std::vector<EzShareFileEntry> entries;
        for (const auto& name : names) {
            auto size = probe(card_path.empty() ? name : card_path + "/" + name);
            if (size) { EzShareFileEntry e; e.name = name; e.size_bytes = size;
                e.size_kb = (size + 1023) / 1024; entries.push_back(e); }
        }
        if (card_path.empty()) {
            EzShareFileEntry e; e.is_dir = true; e.name = "SETTINGS"; entries.push_back(e);
        }
        return entries;
    }
    // "" -> card root (/dir?dir=A:), otherwise /dir?dir=A:<backslash-path>.
    // Unlike listFiles(), this returns directories too — the residue sweep
    // recurses into every non-DATALOG subdir.
    std::string url = base_url_ + "/dir?dir=A:" + encodeCardPath(card_path);

    std::cout << "EzShare: listing dir '" << (card_path.empty() ? "<root>" : card_path)
              << "'" << std::endl;

    std::string html = httpGet(url);
    auto entries = parseDirectoryListing(html);
    if (entries.empty() && !isListing(html)) {
        no_listing_ = true;
        return listDir(card_path);
    }
    return entries;  // includes both files and <DIR> entries
}

bool EzShareClient::downloadByPath(const std::string& card_rel_path,
                                    const std::string& local_path) {
    if (no_listing_) {
        size_t got = 0;
        std::string path = card_rel_path;
        std::replace(path.begin(), path.end(), '\\', '/');
        return downloadChunks(path, local_path, 0, got);
    }
    // Generic single-file download by card-relative (backslash) path.
    std::string url = base_url_ + "/download?file=" + encodeCardPath(card_rel_path);

    std::cout << "EzShare: downloading by path " << card_rel_path << std::endl;

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
    // D8: a stalled whole-file transfer fails in LOW_SPEED_TIME.
    curl_easy_setopt(curl_, CURLOPT_LOW_SPEED_LIMIT, LOW_SPEED_LIMIT);
    curl_easy_setopt(curl_, CURLOPT_LOW_SPEED_TIME, LOW_SPEED_TIME);

    CURLcode res = curl_easy_perform(curl_);
    output.close();

    if (res != CURLE_OK && res != CURLE_PARTIAL_FILE) {
        std::cerr << "EzShare: download-by-path failed (" << card_rel_path << "): "
                  << curl_easy_strerror(res) << std::endl;
        std::filesystem::remove(local_path);
        return false;
    }

    long http_code = 0;
    curl_easy_getinfo(curl_, CURLINFO_RESPONSE_CODE, &http_code);
    if (http_code != 200) {
        std::cerr << "EzShare: HTTP " << http_code << " for " << card_rel_path
                  << errorReason(local_path, 0) << std::endl;
        std::filesystem::remove(local_path);
        return false;
    }

    auto file_size = std::filesystem::file_size(local_path);
    if (file_size == 0) {
        std::cerr << "EzShare: empty file " << card_rel_path << std::endl;
        std::filesystem::remove(local_path);
        return false;
    }

    std::cout << "EzShare: " << card_rel_path << " -> " << file_size << " bytes" << std::endl;
    return true;
}

} // namespace hms_cpap
