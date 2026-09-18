#include "services/UpdateService.h"

#include <curl/curl.h>
#include <nlohmann/json.hpp>
#include <spdlog/spdlog.h>

#include <cctype>
#include <cstdlib>
#include <ctime>
#include <filesystem>
#include <sstream>

using json = nlohmann::json;

namespace hms_cpap {

namespace update {

namespace {

std::vector<long> versionParts(const std::string& v) {
    std::vector<long> parts;
    size_t i = (!v.empty() && (v[0] == 'v' || v[0] == 'V')) ? 1 : 0;
    while (i < v.size()) {
        if (!std::isdigit(static_cast<unsigned char>(v[i]))) break;
        long n = 0;
        while (i < v.size() && std::isdigit(static_cast<unsigned char>(v[i]))) {
            n = n * 10 + (v[i] - '0');
            ++i;
        }
        parts.push_back(n);
        if (i < v.size() && v[i] == '.') ++i;
        else break;  // "5.3.0-rc1": the suffix is not part of the number
    }
    return parts;
}

}  // namespace

int compareVersions(const std::string& a, const std::string& b) {
    auto pa = versionParts(a);
    auto pb = versionParts(b);
    size_t n = std::max(pa.size(), pb.size());
    for (size_t i = 0; i < n; ++i) {
        long x = i < pa.size() ? pa[i] : 0;
        long y = i < pb.size() ? pb[i] : 0;
        if (x < y) return -1;
        if (x > y) return 1;
    }
    return 0;
}

std::string platformKey() {
#if defined(_WIN32) && (defined(_M_X64) || defined(__x86_64__))
    return "windows-x64";
#elif defined(__APPLE__) && (defined(__aarch64__) || defined(__arm64__))
    return "macos-arm64";
#elif defined(__linux__) && defined(__arm__)
    return "linux-armhf";
#else
    return "";
#endif
}

bool isContainerised() {
    // Set, it decides either way: the image sets it to 1, and a test (or a
    // native install run somewhere that looks like a container) sets it to 0.
    if (const char* forced = std::getenv("HMS_CPAP_CONTAINER")) {
        std::string v = forced;
        return !v.empty() && v != "0" && v != "false";
    }
    if (std::getenv("SUPERVISOR_TOKEN")) return true;  // Home Assistant add-on
    std::error_code ec;
    return std::filesystem::exists("/.dockerenv", ec);
}

bool parseRelease(const std::string& body, ReleaseInfo& out) {
    json j = json::parse(body, nullptr, false);
    if (j.is_discarded() || !j.is_object() || !j.contains("tag_name")
        || !j["tag_name"].is_string()) {
        return false;
    }
    out = ReleaseInfo{};
    out.tag = j["tag_name"].get<std::string>();
    out.notes = j.value("body", "");
    out.html_url = j.value("html_url", "");
    out.prerelease = j.value("prerelease", false);
    out.draft = j.value("draft", false);
    if (j.contains("assets") && j["assets"].is_array()) {
        for (const auto& a : j["assets"]) {
            if (!a.is_object()) continue;
            std::string name = a.value("name", "");
            std::string url = a.value("browser_download_url", "");
            if (!name.empty() && !url.empty()) out.download_urls[name] = url;
        }
    }
    return true;
}

std::vector<UpdateAsset> assetsForPlatform(const std::string& manifest_json,
                                           const std::string& platform,
                                           const std::map<std::string, std::string>& download_urls) {
    std::vector<UpdateAsset> out;
    if (platform.empty()) return out;
    json j = json::parse(manifest_json, nullptr, false);
    if (j.is_discarded() || !j.is_object() || !j.contains("assets") || !j["assets"].is_array()) {
        return out;
    }
    for (const auto& a : j["assets"]) {
        if (!a.is_object() || a.value("platform", "") != platform) continue;
        UpdateAsset asset;
        asset.platform = platform;
        asset.kind = a.value("kind", "");
        asset.name = a.value("name", "");
        asset.sha256 = a.value("sha256", "");
        asset.size = a.value("size", 0LL);
        auto it = download_urls.find(asset.name);
        if (it == download_urls.end() || asset.sha256.size() != 64 || asset.size <= 0) continue;
        asset.url = it->second;
        out.push_back(std::move(asset));
    }
    return out;
}

}  // namespace update

namespace {

size_t writeBody(char* ptr, size_t size, size_t nmemb, void* userdata) {
    static_cast<std::string*>(userdata)->append(ptr, size * nmemb);
    return size * nmemb;
}

size_t readHeader(char* buf, size_t size, size_t nitems, void* userdata) {
    std::string line(buf, size * nitems);
    std::string lower = line;
    for (auto& c : lower) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    if (lower.rfind("etag:", 0) == 0) {
        std::string v = line.substr(5);
        size_t b = v.find_first_not_of(" \t");
        size_t e = v.find_last_not_of(" \t\r\n");
        *static_cast<std::string*>(userdata) = (b == std::string::npos) ? "" : v.substr(b, e - b + 1);
    }
    return size * nitems;
}

UpdateService::HttpResult curlGet(const std::string& url, const std::string& if_none_match) {
    UpdateService::HttpResult r;
    CURL* c = curl_easy_init();
    if (!c) return r;
    struct curl_slist* h = nullptr;
    // GitHub refuses API calls with no User-Agent.
    h = curl_slist_append(h, "User-Agent: hms-cpap-updater");
    h = curl_slist_append(h, "Accept: application/vnd.github+json");
    if (!if_none_match.empty()) {
        h = curl_slist_append(h, ("If-None-Match: " + if_none_match).c_str());
    }
    curl_easy_setopt(c, CURLOPT_URL, url.c_str());
    curl_easy_setopt(c, CURLOPT_HTTPHEADER, h);
    curl_easy_setopt(c, CURLOPT_FOLLOWLOCATION, 1L);  // asset downloads redirect to a CDN
    curl_easy_setopt(c, CURLOPT_WRITEFUNCTION, writeBody);
    curl_easy_setopt(c, CURLOPT_WRITEDATA, &r.body);
    curl_easy_setopt(c, CURLOPT_HEADERFUNCTION, readHeader);
    curl_easy_setopt(c, CURLOPT_HEADERDATA, &r.etag);
    curl_easy_setopt(c, CURLOPT_TIMEOUT, 30L);
    if (curl_easy_perform(c) == CURLE_OK) {
        curl_easy_getinfo(c, CURLINFO_RESPONSE_CODE, &r.status);
    }
    curl_slist_free_all(h);
    curl_easy_cleanup(c);
    return r;
}

std::string nowIsoUtc() {
    std::time_t t = std::time(nullptr);
    std::tm tm{};
#ifdef _WIN32
    gmtime_s(&tm, &t);
#else
    gmtime_r(&t, &tm);
#endif
    char buf[32];
    std::strftime(buf, sizeof(buf), "%Y-%m-%dT%H:%M:%SZ", &tm);
    return buf;
}

}  // namespace

UpdateService::UpdateService(std::string current_version, std::string repo, HttpGet http)
    : current_(std::move(current_version)), repo_(std::move(repo)),
      http_(http ? std::move(http) : HttpGet(curlGet)) {
    status_.current = current_;
    status_.containerised = update::isContainerised();
}

UpdateService::~UpdateService() { stop(); }

void UpdateService::start() {
    if (running_.exchange(true)) return;
    if (status_.containerised) {
        spdlog::info("Updates: running in a container, which updates by pulling its image; "
                     "the updater stays off");
        running_ = false;
        return;
    }
    worker_ = std::thread([this] { runLoop(); });
}

void UpdateService::stop() {
    if (!running_.exchange(false)) return;
    wake_.notify_all();
    if (worker_.joinable()) worker_.join();
}

UpdateStatus UpdateService::status() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return status_;
}

UpdateStatus UpdateService::checkNow() {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (status_.containerised) return status_;
    }

    std::string etag;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        etag = etag_;
    }

    // /releases/latest already excludes drafts and pre-releases (D7).
    // HMS_CPAP_UPDATE_URL points the check at a fake release endpoint, which is
    // how SDD-041 §5's end-to-end test publishes "N+1" without a real tag.
    std::string api = "https://api.github.com/repos/" + repo_ + "/releases/latest";
    if (const char* fake = std::getenv("HMS_CPAP_UPDATE_URL"); fake && *fake) api = fake;
    HttpResult rel = http_(api, etag);

    std::string release_body;
    std::string new_etag = etag;
    if (rel.status == 304) {
        std::lock_guard<std::mutex> lock(mutex_);
        release_body = last_release_body_;
    } else if (rel.status == 200) {
        release_body = rel.body;
        new_etag = rel.etag;
    }

    auto fail = [&](const std::string& why) {
        std::lock_guard<std::mutex> lock(mutex_);
        // One quiet line, and the last good answer stands: a network blip is
        // not a reason to take a banner away or to put one up.
        if (status_.error != why) spdlog::warn("Updates: check failed: {}", why);
        status_.error = why;
        status_.checked_at = nowIsoUtc();
        return status_;
    };

    if (release_body.empty()) {
        return fail(rel.status == 0 ? "GitHub unreachable"
                                    : "GitHub answered " + std::to_string(rel.status));
    }

    update::ReleaseInfo info;
    if (!update::parseRelease(release_body, info)) return fail("release is not readable");
    if (info.draft || info.prerelease) return fail("latest release is not a stable release");

    UpdateStatus next;
    next.current = current_;
    next.latest = info.tag.rfind('v', 0) == 0 ? info.tag.substr(1) : info.tag;
    next.checked = true;
    next.checked_at = nowIsoUtc();
    next.notes = info.notes;
    next.release_url = info.html_url;

    bool newer = update::compareVersions(next.latest, current_) > 0;
    if (newer) {
        auto m = info.download_urls.find("manifest.json");
        if (m == info.download_urls.end()) {
            // A release from before SDD-041 has no checksums, so nothing is
            // offered from it: an unverifiable download is not an update.
            next.error = "release has no manifest.json";
        } else {
            HttpResult man = http_(m->second, "");
            if (man.status != 200) {
                return fail("manifest.json answered " + std::to_string(man.status));
            }
            next.assets = update::assetsForPlatform(man.body, update::platformKey(),
                                                    info.download_urls);
            next.available = !next.assets.empty();
        }
    }

    std::lock_guard<std::mutex> lock(mutex_);
    if (next.available && (!status_.available || status_.latest != next.latest)) {
        spdlog::info("Updates: {} is available (running {})", next.latest, current_);
    }
    etag_ = new_etag;
    last_release_body_ = release_body;
    status_ = next;
    return status_;
}

void UpdateService::runLoop() {
    // First check shortly after start, not during it: startup is busy enough.
    auto next = std::chrono::steady_clock::now() + std::chrono::minutes(2);
    while (running_) {
        {
            std::unique_lock<std::mutex> lock(wake_mutex_);
            wake_.wait_until(lock, next, [this] { return !running_.load(); });
        }
        if (!running_) break;
        try {
            checkNow();
        } catch (const std::exception& e) {
            spdlog::warn("Updates: check threw: {}", e.what());
        }
        next = std::chrono::steady_clock::now() + std::chrono::hours(24);
    }
}

}  // namespace hms_cpap
