#include "clients/EzShareClient.h"
#include "utils/CardNames.h"
#include "parsers/CpapdashBridge.h"
#include "utils/ConfigManager.h"
#include "utils/AppConfig.h"
#include "utils/ArchiveRecordCount.h"
#include "utils/MachineClock.h"
#include "services/ListingPlan.h"
#include <chrono>
#include <cmath>
#include <cstdlib>
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

bool EzShareClient::resmedFormat() const {
    return ConfigManager::get("CPAP_FORMAT", AppConfig::formatForSource(
        ConfigManager::get("CPAP_SOURCE", "ezshare"))) == "resmed";
}

bool EzShareClient::listingFallback(const std::string& url, std::string& html) {
    // Empty and truncated 200 listings are normal through Mule/Miner. Only
    // two actual welcome-page replies establish the firmware limitation.
    if (!resmedFormat() || html.find("publicdir/welcome.htm") == std::string::npos) return false;
    html = httpGet(url);
    if (html.find("publicdir/welcome.htm") == std::string::npos) return false;
    no_listing_ = true;
    listing_attempts_ = 0;
    return true;
}

bool EzShareClient::retryListing() {
    if (!no_listing_ || ++listing_attempts_ < LISTING_RETRY_ATTEMPTS) return no_listing_;
    listing_attempts_ = 0;
    try {
        if (isListing(httpGet(base_url_ + "/dir?dir=A:DATALOG"))) no_listing_ = false;
    } catch (const std::exception&) {
        // Keep discovery working while the directory endpoint is unavailable.
    }
    return no_listing_;
}

void EzShareClient::beginRefresh() {
    cached_str_.clear();
    sessions_loaded_ = false;
    // Check for recovered listings.
    listing_attempts_ = LISTING_RETRY_ATTEMPTS;
    // SDD-069 P7: STR.edf is no longer read whole on every refresh. The daily
    // index reads it (listDateFolders), and so does the 65-min checkpoint after
    // a night stopped growing, here; never while a night grows.
    if (retryListing() && scope_.str_checkpoint) refreshSessions();
}

void EzShareClient::setBurstScope(const BurstScope& scope) {
    if (scope.now_s != scope_.now_s) scope_set_at_ = std::chrono::steady_clock::now();
    scope_ = scope;
}

bool EzShareClient::takeMachineOffset(long long& offset_s) {
    if (!offset_seen_) return false;
    offset_seen_ = false;
    offset_s = offset_s_;
    return true;
}

void EzShareClient::observeProbe(const std::string& path, uint64_t size, const std::string& tail) {
    auto& f = live_[path];
    const long long now = scope_.now_s;
    const std::string count = tail.size() >= 8 ? tail.substr(0, 8) : std::string();
    const long long duration =
        tail.size() >= 16 ? std::llround(std::atof(tail.substr(8, 8).c_str())) : 0;
    const bool first = f.size == 0;
    // SDD-069 P3: the size and the record count, both, unchanged. A count that
    // moves at the same size is a change (a file finalised in place).
    const bool moved = !first && (size != f.size || count != f.count);
    if (first || moved) {
        const auto slash = path.rfind('/');
        const std::string name = slash == std::string::npos ? path : path.substr(slash + 1);
        const long long records = std::atoll(count.c_str());
        // P5: a BRP that grew was written within the last burst, so the
        // machine's clock is its start plus its records. Its name is the start
        // on the machine's clock; the header says the same.
        if (moved && size > f.size && name.size() > 8 &&
            name.compare(name.size() - 8, 8, "_BRP.edf") == 0 && records > 0 && duration > 0 &&
            now > 0) {
            const long long named = machine_clock::nameTimeOf(name);
            const long long host_now = now + std::chrono::duration_cast<std::chrono::seconds>(
                                                 std::chrono::steady_clock::now() - scope_set_at_)
                                                 .count();
            if (named > 0) {
                offset_s_ = machine_clock::roundToStep(named + records * duration - host_now);
                offset_seen_ = true;
            }
        }
        f.last_growth = now;
        f.unchanged = 0;
    } else if (now > f.last_probe) {
        // Once a burst: a download's own probe in the same burst is no second look.
        ++f.unchanged;
    }
    f.size = size;
    f.count = count;
    if (duration > 0) f.duration_s = duration;
    f.last_probe = now;
}

bool EzShareClient::settled(const LiveFile& f) const {
    // P3, read off the probe: held to its end, then two bursts with the same
    // size and count. A record longer than the burst takes as many bursts as
    // cover two records. P4: a stalled transfer keeps it open.
    if (f.short_copy || f.last_probe == 0 || f.unchanged < 2) return false;
    return f.last_probe - f.last_growth >= 2 * f.duration_s;
}

std::set<std::string> EzShareClient::probedThisBurst(const std::string& folder) {
    // The folder's known files by session: a file goes with the BRP named
    // within 15 s of it (CSL and EVE are named a few seconds before it), or is
    // a session of its own (a mask-on that wrote no BRP).
    const std::string prefix = folder + "/";
    std::vector<std::pair<std::string, long long>> names;
    std::vector<long long> brps;
    for (auto it = discovered_.lower_bound(prefix);
         it != discovered_.end() && it->first.compare(0, prefix.size(), prefix) == 0; ++it) {
        const std::string name = it->first.substr(prefix.size());
        const long long t = machine_clock::nameTimeOf(name);
        if (t == 0) continue;
        names.emplace_back(name, t);
        if (name.find("_BRP.edf") != std::string::npos) brps.push_back(t);
    }
    std::map<long long, std::vector<std::string>> sessions;
    for (const auto& [name, t] : names) {
        long long key = t;
        for (const auto b : brps)
            if (std::llabs(b - t) <= 15 && (key == t || std::llabs(b - t) < std::llabs(key - t))) key = b;
        sessions[key].push_back(name);
    }

    std::set<std::string> out;
    const long long now = scope_.now_s;
    bool later_brp = false;
    for (auto s = sessions.rbegin(); s != sessions.rend(); ++s) {
        bool has_brp = false, brp_open = false, any_open = false;
        long long growth = 0, probed = 0;
        for (const auto& name : s->second) {
            const auto& f = live_["DATALOG/" + folder + "/" + name];
            growth = std::max(growth, f.last_growth);
            probed = std::max(probed, f.last_probe);
            const bool open = !settled(f);
            any_open = any_open || open;
            if (name.find("_BRP.edf") != std::string::npos) {
                has_brp = true;
                brp_open = brp_open || open;
            }
        }
        // P6: the open session is probed every burst. One proven off (P3), or
        // followed by a new BRP (SDD-053 D3), only at SDD-054 D4's checkpoints
        // after its last growth, on the burst that first passes each.
        bool due = !later_brp && (has_brp ? brp_open : any_open);
        for (const auto cp : kGrowthCheckpoints) {
            const long long at = growth + std::chrono::duration_cast<std::chrono::seconds>(cp).count();
            if (now >= at && probed < at) due = true;
        }
        if (due) out.insert(s->second.begin(), s->second.end());
        if (has_brp) later_brp = true;
    }
    return out;
}

bool EzShareClient::needsFullRefresh(const std::string& card_path, const std::string& local_path) {
    if (!no_listing_) return false;
    std::string path = card_path;
    std::replace(path.begin(), path.end(), '\\', '/');
    if (failed_downloads_.count(path)) return true;
    std::string lower = path;
    std::transform(lower.begin(), lower.end(), lower.begin(),
                   [](unsigned char c) { return std::tolower(c); });
    std::string expected;
    size_t offset = 0;
    std::string local = local_path;
    if (lower.size() >= 4 && lower.substr(lower.size() - 4) == ".edf") {
        const auto found = record_counts_.find(path);
        if (found == record_counts_.end() || found->second.empty()) return true;
        expected = found->second;
        if (signalEdfRecordCountMatches(local_path, expected)) return false;
        offset = 236;
    } else if (lower == "identification.json" || lower == "settings/currentsettings.json" ||
               (lower.size() >= 4 && lower.substr(lower.size() - 4) == ".crc")) {
        auto crc = path;
        if (lower.substr(lower.size() - 4) != ".crc") {
            crc.replace(crc.size() - 5, 5, ".crc");
            local.replace(local.size() - 5, 5, ".crc");
        }
        const auto size = probe(crc);
        if (!size || size > 1024) return true;
        expected = readRange(crc, 0, size - 1);
        if (expected.empty()) return true;
    } else {
        // No cheap version exists for journal.jnl. Keep its captured copy;
        // the collector's ordinary listing/size checks still admit new files.
        return false;
    }
    if (offset == 0) {
        std::error_code ec;
        if (std::filesystem::file_size(local, ec) != expected.size() || ec) return true;
    }
    std::ifstream input(local, std::ios::binary);
    input.seekg(offset);
    std::string actual(expected.size(), '\0');
    input.read(actual.data(), actual.size());
    return !input || actual != expected;
}

std::vector<std::string> EzShareClient::listDateFolders() {
    // The nights STR.edf names, and (SDD-069 P7) the ones found by their
    // aliases: while a night grows the STR is not read, and tonight's folder
    // must not look gone from the index.
    const auto knownFolders = [this] {
        std::set<std::string> folders;
        for (const auto& pair : starts_) folders.insert(pair.first);
        for (const auto& pair : alias_starts_)
            if (!pair.second.empty()) folders.insert(pair.first);
        return std::vector<std::string>(folders.begin(), folders.end());
    };
    if (retryListing()) {
        refreshSessions();
        return knownFolders();
    }
    // GET /dir?dir=A:DATALOG → lists date folders like 20260203, 20260204
    std::string url = base_url_ + "/dir?dir=A:DATALOG";
    std::string html = httpGet(url);

    listingFallback(url, html);
    if (html.empty()) {
        std::cerr << "EzShare: empty response listing DATALOG" << std::endl;
        return {};
    }
    auto entries = parseDirectoryListing(html);
    if (no_listing_) {
        refreshSessions();
        return knownFolders();
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
    if (retryListing()) return enumerateFiles(date_folder);
    // GET /dir?dir=A:DATALOG%5CYYYYMMDD → lists session files
    // %5C = backslash (required, forward slash returns 404)
    std::string url = base_url_ + "/dir?dir=A:DATALOG%5C" + date_folder;

    std::cout << "EzShare: listing " << date_folder << std::endl;

    std::string html = httpGet(url);
    if (listingFallback(url, html)) {
        refreshSessions();
        return enumerateFiles(date_folder);
    }

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
        if (name == "STR.edf" && !cached_str_.empty()) {
            std::filesystem::create_directories(std::filesystem::path(local_path).parent_path());
            std::ofstream output(local_path, std::ios::binary);
            output.write(cached_str_.data(), cached_str_.size());
            return static_cast<bool>(output);
        }
        // SDD-069 P7: no STR.edf while a night grows. At 0.5 to 1 KB/s it is
        // minutes of the link the night's records need; it is read again once
        // the growth stops (the session's close, the 65-min checkpoint).
        if (name == "STR.edf" && scope_.night_growing) {
            if (str_refused_at_ != scope_.now_s)
                std::cerr << "ez Share: STR.edf is not read while the night grows" << std::endl;
            str_refused_at_ = scope_.now_s;
            return false;
        }
        if (!downloadChunks(name, local_path, 0, got)) return false;
        if (name == "STR.edf") {
            {
                std::ifstream input(local_path, std::ios::binary);
                cached_str_.assign(std::istreambuf_iterator<char>(input), {});
            }
            // Whoever asked for it, it is this refresh's session index too.
            loadSessions(local_path);
        }
        return true;
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
// small ranged GET avoids broken HEAD, returns the exact size and, for
// EDFs, the record count that changes when a same-length file is finalised.
uint64_t EzShareClient::probe(const std::string& path, bool* confirmed_missing) {
    if (confirmed_missing) *confirmed_missing = false;
    std::string body, headers;
    const std::string url = base_url_ + "/" + path;
    curl_easy_setopt(curl_, CURLOPT_URL, url.c_str());
    std::string lower = path;
    std::transform(lower.begin(), lower.end(), lower.begin(), [](unsigned char c) { return std::tolower(c); });
    const bool edf = lower.size() >= 4 && lower.substr(lower.size() - 4) == ".edf";
    // SDD-069 P3: bytes 236 to 251, the record count and the record duration,
    // in the same one request.
    const unsigned first_byte = edf ? 236 : 0;
    const unsigned last_byte = edf ? 251 : 0;
    if (edf) record_counts_.erase(path);
    curl_easy_setopt(curl_, CURLOPT_RANGE, edf ? "236-251" : "0-0");
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
    if (confirmed_missing && result == CURLE_OK &&
        (status == 404 || status == 410 ||
         (status == 200 && body.find("publicdir/welcome.htm") != std::string::npos)))
        *confirmed_missing = true;
    const bool plain = result == CURLE_OK && type &&
        std::string(type).find("text/plain") == 0 && body.size() == last_byte - first_byte + 1;
    // A one-byte file is wholly covered by bytes=0-0, so it comes back as 200.
    const bool hit = plain && status == 206;
    if (!edf && plain && status == 200) {
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
                    &first, &last, &total) != 3 || first != first_byte || last != last_byte) return 0;
    if (edf) record_counts_[path] = body.substr(0, 8);
    observeProbe(path, total, edf ? body : std::string());
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
            if (++failures >= 5) {
                // SDD-069 P4: a transfer that stopped before the end keeps
                // the file's session open.
                live_[path].short_copy = true;
                return false;
            }
            continue;
        }
        offset += got;
        downloaded += got;
        failures = 0;
    }
    live_[path].short_copy = false;
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
    // SDD-069 P7: not while a night grows; the last STR read stands.
    if (sessions_loaded_ || scope_.night_growing) return;
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
    const auto path = (dir / "STR.edf").string();
    if (!downloadRootFile("STR.edf", path))
        throw std::runtime_error("ez Share: could not read STR.edf for listing-free discovery");
    if (!sessions_loaded_) loadSessions(path);
}

void EzShareClient::loadSessions(const std::string& path) {
    auto records = EDFParser::parseSTRFile(path, "ezshare");
    starts_.clear();
    for (const auto& record : records) {
        for (const auto& pair : record.mask_pairs) {
            const auto start = std::chrono::system_clock::to_time_t(pair.first);
            starts_[folderForStart(start)].push_back(start);
        }
        // STR has a fixed number of mask-on slots per day (20 on an
        // AirSense 11). A day with more sessions counts them in MaskEvents
        // but has no start for the rest, so they cannot be found here.
        const auto listed = static_cast<int>(record.mask_pairs.size());
        if (listed >= 10 && record.mask_events > listed) {
            const auto first = std::chrono::system_clock::to_time_t(record.mask_pairs.front().first);
            std::cerr << "ez Share: STR.edf lists " << listed << " session starts for "
                      << folderForStart(first) << " but counts " << record.mask_events
                      << " mask events; sessions past its last slot may be missing"
                      << " without a directory listing" << std::endl;
        }
    }
    sessions_loaded_ = true;
}

std::string EzShareClient::readRange(const std::string& path, size_t first, size_t last) {
    std::string body;
    const std::string url = base_url_ + "/" + path;
    const auto range = std::to_string(first) + "-" + std::to_string(last);
    curl_easy_setopt(curl_, CURLOPT_URL, url.c_str());
    curl_easy_setopt(curl_, CURLOPT_RANGE, range.c_str());
    curl_easy_setopt(curl_, CURLOPT_TIMEOUT, 8L);
    curl_easy_setopt(curl_, CURLOPT_LOW_SPEED_LIMIT, 0L);
    curl_easy_setopt(curl_, CURLOPT_WRITEFUNCTION, WriteCallback);
    curl_easy_setopt(curl_, CURLOPT_WRITEDATA, &body);
    CURLcode result = CURLE_OK;
    for (int attempt = 0; attempt < 3; ++attempt) {
        body.clear();
        result = curl_easy_perform(curl_);
        if (result == CURLE_OK) break;
    }
    curl_easy_setopt(curl_, CURLOPT_RANGE, nullptr);
    long status = 0;
    char* type = nullptr;
    curl_easy_getinfo(curl_, CURLINFO_RESPONSE_CODE, &status);
    curl_easy_getinfo(curl_, CURLINFO_CONTENT_TYPE, &type);
    if (result != CURLE_OK || (status != 206 && !(status == 200 && first == 0)) || !type ||
        std::string(type).find("text/plain") != 0 || body.size() != last - first + 1)
        return {};
    return body;
}

std::set<std::time_t> EzShareClient::scanAliases(const std::string& folder, int misses_allowed,
                                                 std::set<std::time_t>* fresh) {
    if (folder.size() != 8 || !std::all_of(folder.begin(), folder.end(), ::isdigit)) return {};
    // Session files in a noon-split folder are named after its own day or
    // the next one. Each name date is its own alias stem; in a month the two
    // stems share their first six digits and so one numbering.
    std::tm day{};
    day.tm_year = std::stoi(folder.substr(0, 4)) - 1900;
    day.tm_mon = std::stoi(folder.substr(4, 2)) - 1;
    day.tm_mday = std::stoi(folder.substr(6, 2)) + 1;
    day.tm_hour = 12;
    day.tm_isdst = -1;
    std::mktime(&day);
    char next[16];
    std::strftime(next, sizeof(next), "%Y%m%d", &day);
    auto& found = alias_starts_[folder];
    std::set<std::string> tried;
    for (const auto& stem : {folder, std::string(next)}) {
        // Two stems of one month share their six-digit alias base and its numbering.
        auto& number = alias_next_[folder][stem.substr(0, 6)];
        if (number == 0) number = 1;
        // A missing alias ends the run; a short tolerance covers a number
        // freed by a deleted file or taken by a name of the other stem.
        // SDD-069 P1: that tolerance is the full scan's; any other burst asks
        // for the next number only, and a miss is "nothing new".
        for (int misses = 0, n = number; misses < misses_allowed && n < 1000000; ++n) {
            const auto digits = std::to_string(n);
            const auto alias = stem.substr(0, 7 - digits.size()) + "~" + digits + ".EDF";
            if (!tried.insert(alias).second) break; // the other stem's numbering
            // Bytes 168-183 of an EDF header are its start, "dd.mm.yyhh.mm.ss".
            const auto stamp = readRange("DATALOG/" + folder + "/" + alias, 168, 183);
            std::tm tm{};
            if (stamp.size() != 16 ||
                std::sscanf(stamp.c_str(), "%2d.%2d.%2d%2d.%2d.%2d", &tm.tm_mday, &tm.tm_mon,
                            &tm.tm_year, &tm.tm_hour, &tm.tm_min, &tm.tm_sec) != 6) {
                ++misses;
                continue;
            }
            misses = 0;
            number = n + 1;
            ++alias_generation_[folder];
            tm.tm_mon -= 1;
            tm.tm_year += 100;
            tm.tm_isdst = -1;
            const auto start = std::mktime(&tm);
            if (found.insert(start).second && fresh) fresh->insert(start);
        }
    }
    return found;
}

std::vector<EzShareFileEntry> EzShareClient::enumerateFiles(const std::string& folder) {
    std::vector<EzShareFileEntry> files;
    std::set<std::string> returned;
    const auto candidate = [](std::time_t at, const std::string& type) {
        auto tm = *std::localtime(&at);
        char prefix[32];
        std::strftime(prefix, sizeof(prefix), "%Y%m%d_%H%M%S_", &tm);
        return std::string(prefix) + type + ".edf";
    };
    // SDD-069 P1, P6: a burst that is not a full scan (the daily index, an
    // SDD-054 D6 reason) asks the card only what can have changed: the next
    // alias of each stem, and the files of the open session.
    const bool full = scope_.full_scan;
    const auto probed = full ? std::set<std::string>{} : probedThisBurst(folder);
    const auto add = [&](const std::string& name) {
        if (!returned.insert(name).second) return;
        const auto key = folder + "/" + name;
        const auto known = discovered_.find(key);
        const bool held = known != discovered_.end() && known->second.size_bytes > 0;
        if (held && !full && !probed.count(name)) {
            files.push_back(known->second);
            return;
        }
        const auto size = probe("DATALOG/" + folder + "/" + name);
        if (!size) {
            // P4: a file found before that does not answer (HTML, a stall, no
            // reply) decides nothing; it stays as it was.
            if (held) files.push_back(known->second);
            return;
        }
        auto& e = discovered_[key];
        e.name = name; e.size_bytes = size; e.size_kb = (size + 1023) / 1024;
        files.push_back(e);
    };
    // Every file found through its alias has a known start second, so its
    // long name is one of a few candidates.
    std::set<std::time_t> fresh;
    const auto aliased = scanAliases(folder, full ? 3 : 1, &fresh);
    if (full && ++alias_scan_attempts_[folder] % LISTING_RETRY_ATTEMPTS == 0)
        alias_missing_[folder].clear();
    for (auto start : aliased) {
        auto& names = alias_names_[folder][start];
        // P6: outside a full scan, only a new start or the open session looks
        // for its other files; a settled one keeps the names it has.
        if (!full && !fresh.count(start) &&
            std::none_of(names.begin(), names.end(),
                         [&](const std::string& n) { return probed.count(n) > 0; })) {
            for (const auto& name : names) add(name);
            continue;
        }
        auto& missing = alias_missing_[folder][start];
        auto& checked = alias_checked_generation_[folder][start];
        if (checked != alias_generation_[folder] || names.empty()) missing.clear();
        checked = alias_generation_[folder];
        {
            for (const auto* type : {"BRP", "PLD", "SA2", "SAD", "EVE", "CSL", "TCV"}) {
                const auto hasType = [&](const std::string& wanted) {
                    return std::any_of(names.begin(), names.end(), [&](const std::string& name) {
                        return name.size() >= wanted.size() + 4 &&
                            name.substr(name.size() - wanted.size() - 4) == wanted + ".edf";
                    });
                };
                if (missing.count(type) || hasType(type) || (std::string(type) == "SAD" && hasType("SA2")) ||
                    (std::string(type) == "SA2" && hasType("SAD"))) continue;
                bool all_missing = true;
                for (int delta : {0, -1, 1, -2, 2, -3, 3}) {
                    const auto name = candidate(start + delta, type);
                    bool confirmed_missing = false;
                    const auto size = probe("DATALOG/" + folder + "/" + name, &confirmed_missing);
                    all_missing = all_missing && confirmed_missing;
                    if (size) {
                        names.insert(name);
                        break;
                    }
                }
                if (!hasType(type) && all_missing) missing.insert(type);
            }
        }
        if (names.empty() && unresolved_aliases_.insert({folder, start}).second)
            std::cerr << "ez Share: unresolved alias start " << start << " in " << folder
                      << "; searching its STR minute" << std::endl;
        for (const auto& name : names) add(name);
    }
    const auto it = starts_.find(folder);
    if (it == starts_.end()) return files;
    if (!full) {
        // P1: the STR-minute search is the full scan's. What it found stays.
        std::map<std::time_t, int> seen;
        for (auto start : it->second) {
            const auto known = session_names_.find({start, seen[start]++});
            if (known == session_names_.end()) continue;
            for (const auto& name : known->second) add(name.second);
        }
        return files;
    }
    // Names already taken by an earlier session of this folder; a session
    // that shares its STR minute with another must find the next file set.
    std::set<std::string> claimed;
    for (const auto& known : session_names_)
        for (const auto& name : known.second) claimed.insert(name.second);
    for (const auto& known : alias_names_[folder])
        claimed.insert(known.second.begin(), known.second.end());
    std::map<std::time_t, int> seen;
    for (auto start : it->second) {
        const int ordinal = seen[start]++;
        auto& names = session_names_[{start, ordinal}];
        // Each resolved BRP covers only one STR start, even when several
        // sessions share its minute. An unresolved alias covers none.
        std::set<std::string> covered;
        for (const auto& alias : alias_names_[folder]) {
            if (alias.first < start || alias.first >= start + 60) continue;
            for (const auto& name : alias.second)
                if (name.find("_BRP.edf") != std::string::npos) covered.insert(name);
        }
        if (static_cast<size_t>(ordinal) < covered.size()) continue;
        auto find = [&](const std::string& type, std::time_t from, int count) {
            if (names.count(type)) return;
            for (int second = 0; second < count; ++second) {
                auto name = candidate(from + second, type);
                if (claimed.count(name)) continue;
                const auto size = probe("DATALOG/" + folder + "/" + name);
                if (size) {
                    names[type] = name;
                    claimed.insert(name);
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
        for (const auto* type : {"PLD", "SA2", "SAD", "EVE", "CSL", "TCV"}) {
            if ((std::string(type) == "SAD" && names.count("SA2")) ||
                (std::string(type) == "SA2" && names.count("SAD"))) continue;
            // TCV is a checkpoint companion with the same filename prefix
            // (SessionDiscoveryService pairs it by that exact prefix).
            if (std::string(type) == "TCV") find(type, brp, 1);
            else find(type, brp - 15, 31);
        }
        for (const auto& name : names) add(name.second);
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
    if (retryListing()) {
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
    if (listingFallback(url, html)) {
        return listDir(card_path);
    }
    if (html.empty()) {
        std::cerr << "EzShare: empty response listing '" << card_path << "'" << std::endl;
        return {};
    }
    return parseDirectoryListing(html);  // includes both files and <DIR> entries
}

bool EzShareClient::downloadByPath(const std::string& card_rel_path,
                                    const std::string& local_path) {
    if (no_listing_) {
        size_t got = 0;
        std::string path = card_rel_path;
        std::replace(path.begin(), path.end(), '\\', '/');
        const bool ok = downloadChunks(path, local_path, 0, got);
        if (ok) failed_downloads_.erase(path);
        else failed_downloads_.insert(path);
        return ok;
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
