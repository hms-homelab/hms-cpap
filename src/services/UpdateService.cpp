#include "services/UpdateService.h"

#include <curl/curl.h>
#include <nlohmann/json.hpp>
#include <openssl/evp.h>
#include <spdlog/spdlog.h>
#include <sqlite3.h>

#include <cctype>
#include <cstdlib>
#include <ctime>
#include <filesystem>
#include <fstream>
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

Handoff handoffMode() {
    // The Pi's unit sets this; install.sh is what installs the root side. It
    // is checked FIRST because that unit also sets HMS_CPAP_SUPERVISED=1 (so
    // systemd brings the service back after a settings restart). Read the
    // other way round, a Pi would exit 42 for a helper that does not exist,
    // systemd would restart it, and it would download again, for ever.
    if (const char* u = std::getenv("HMS_CPAP_UPDATER"); u && std::string(u) == "systemd") {
        return Handoff::Systemd;
    }
    // The desktop supervisor sets this for its child (desktop/qt ChildProcess),
    // and only it can run the helper that outlives it.
    if (const char* s = std::getenv("HMS_CPAP_SUPERVISED"); s && std::string(s) == "1") {
        return Handoff::Supervisor;
    }
    return Handoff::None;
}

std::string assetKindFor(Handoff handoff, const std::string& platform) {
    if (handoff == Handoff::Supervisor) {
        if (platform == "macos-arm64") return "dmg";
        if (platform == "windows-x64") return "installer";
    }
    if (handoff == Handoff::Systemd && platform == "linux-armhf") return "zip";
    return "";
}

std::string sha256File(const std::string& path) {
    std::ifstream in(path, std::ios::binary);
    if (!in) return "";
    EVP_MD_CTX* ctx = EVP_MD_CTX_new();
    if (!ctx) return "";
    EVP_DigestInit_ex(ctx, EVP_sha256(), nullptr);
    std::vector<char> buf(1 << 16);
    while (in) {
        in.read(buf.data(), static_cast<std::streamsize>(buf.size()));
        if (in.gcount() > 0) EVP_DigestUpdate(ctx, buf.data(), static_cast<size_t>(in.gcount()));
    }
    unsigned char digest[EVP_MAX_MD_SIZE];
    unsigned int len = 0;
    EVP_DigestFinal_ex(ctx, digest, &len);
    EVP_MD_CTX_free(ctx);
    static const char* hex = "0123456789abcdef";
    std::string out;
    out.reserve(len * 2);
    for (unsigned int i = 0; i < len; ++i) {
        out += hex[digest[i] >> 4];
        out += hex[digest[i] & 0xf];
    }
    return out;
}

UpdateResult readResult(const std::string& path) {
    UpdateResult r;
    std::ifstream in(path);
    if (!in) return r;
    std::stringstream ss;
    ss << in.rdbuf();
    json j = json::parse(ss.str(), nullptr, false);
    if (j.is_discarded() || !j.is_object()) return r;
    r.present = true;
    r.ok = j.value("ok", false);
    r.version = j.value("version", "");
    r.step = j.value("step", "");
    r.message = j.value("message", "");
    r.at = j.value("at", "");
    return r;
}

std::string backupSqlite(const std::string& src, const std::string& dest) {
    std::error_code ec;
    if (!std::filesystem::exists(src, ec)) return "no database at " + src;
    std::filesystem::remove(dest, ec);   // VACUUM INTO refuses an existing file
    sqlite3* db = nullptr;
    if (sqlite3_open_v2(src.c_str(), &db, SQLITE_OPEN_READONLY, nullptr) != SQLITE_OK) {
        std::string why = db ? sqlite3_errmsg(db) : "cannot open";
        sqlite3_close(db);
        return why;
    }
    sqlite3_busy_timeout(db, 10000);
    sqlite3_stmt* stmt = nullptr;
    std::string why;
    if (sqlite3_prepare_v2(db, "VACUUM INTO ?", -1, &stmt, nullptr) != SQLITE_OK) {
        why = sqlite3_errmsg(db);
    } else {
        sqlite3_bind_text(stmt, 1, dest.c_str(), -1, SQLITE_TRANSIENT);
        if (sqlite3_step(stmt) != SQLITE_DONE) why = sqlite3_errmsg(db);
    }
    sqlite3_finalize(stmt);
    sqlite3_close(db);
    return why;
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

size_t writeFile(char* ptr, size_t size, size_t nmemb, void* userdata) {
    auto* out = static_cast<std::ofstream*>(userdata);
    out->write(ptr, static_cast<std::streamsize>(size * nmemb));
    return out->good() ? size * nmemb : 0;
}

bool curlDownload(const std::string& url, const std::string& path) {
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    if (!out) return false;
    CURL* c = curl_easy_init();
    if (!c) return false;
    struct curl_slist* h = curl_slist_append(nullptr, "User-Agent: hms-cpap-updater");
    curl_easy_setopt(c, CURLOPT_URL, url.c_str());
    curl_easy_setopt(c, CURLOPT_HTTPHEADER, h);
    curl_easy_setopt(c, CURLOPT_FOLLOWLOCATION, 1L);   // GitHub hands assets to a CDN
    curl_easy_setopt(c, CURLOPT_FAILONERROR, 1L);      // a 404 page is not the file
    curl_easy_setopt(c, CURLOPT_WRITEFUNCTION, writeFile);
    curl_easy_setopt(c, CURLOPT_WRITEDATA, &out);
    curl_easy_setopt(c, CURLOPT_CONNECTTIMEOUT, 30L);
    // No total timeout: a slow line may take minutes for the DMG. Abort only
    // when it stalls, below 1 KB/s for a minute.
    curl_easy_setopt(c, CURLOPT_LOW_SPEED_LIMIT, 1024L);
    curl_easy_setopt(c, CURLOPT_LOW_SPEED_TIME, 60L);
    const bool ok = curl_easy_perform(c) == CURLE_OK;
    curl_slist_free_all(h);
    curl_easy_cleanup(c);
    out.close();
    return ok && out.good();
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
      http_(http ? std::move(http) : HttpGet(curlGet)),
      handoff_(update::handoffMode()), platform_(update::platformKey()) {
    status_.current = current_;
    status_.containerised = update::isContainerised();
    hooks_.download = curlDownload;
    hooks_.exit = [](int code) {
        std::this_thread::sleep_for(std::chrono::milliseconds(750));
        std::exit(code);
    };
}

UpdateService::~UpdateService() {
    stop();
    waitForApply();
}

void UpdateService::setDataDir(const std::string& dir) {
    std::lock_guard<std::mutex> lock(mutex_);
    data_dir_ = dir;
    status_.last_result = update::readResult(updateDir() + "/result.json");
    const auto& r = status_.last_result;
    if (r.present && !r.ok) {
        spdlog::warn("Updates: the last update to {} failed at {}: {}", r.version, r.step, r.message);
    } else if (r.present && r.ok && r.version == current_) {
        spdlog::info("Updates: now running {}, installed by the updater", current_);
    }
}

void UpdateService::setHooks(Hooks hooks) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!hooks.download) hooks.download = hooks_.download;
    if (!hooks.exit) hooks.exit = hooks_.exit;
    hooks_ = std::move(hooks);
}

void UpdateService::setHandoff(update::Handoff h) {
    std::lock_guard<std::mutex> lock(mutex_);
    handoff_ = h;
}

void UpdateService::setPlatform(const std::string& platform) {
    std::lock_guard<std::mutex> lock(mutex_);
    platform_ = platform;
}

std::string UpdateService::updateDir() const {
    return (std::filesystem::path(data_dir_) / "update").string();
}

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
    UpdateStatus s = status_;
    // Derived on every read so a Settings change or a finished check shows at once.
    s.auto_update = hooks_.auto_update ? hooks_.auto_update() : false;
    s.database = hooks_.database ? hooks_.database() : "";
    const std::string kind = update::assetKindFor(handoff_, platform_);
    bool has_kind = false;
    for (const auto& a : s.assets) has_kind = has_kind || a.kind == kind;
    s.can_apply = s.available && !s.containerised && !kind.empty() && has_kind;
    s.installer = (s.containerised || kind.empty()) ? ""
                : handoff_ == update::Handoff::Systemd ? "systemd" : "supervisor";
    return s;
}

UpdateService::ApplyRefusal UpdateService::apply(bool now) {
    UpdateStatus snapshot = status();
    if (!snapshot.available) return ApplyRefusal::NotAvailable;
    if (!snapshot.can_apply) return ApplyRefusal::NoInstaller;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (status_.applying) return ApplyRefusal::AlreadyApplying;
        // §3.5: not while a night is still arriving, unless the user said now.
        if (!now && hooks_.busy && hooks_.busy()) return ApplyRefusal::Busy;
        status_.applying = true;
        status_.apply_step = "downloading";
        status_.error.clear();
    }
    if (apply_thread_.joinable()) apply_thread_.join();
    apply_thread_ = std::thread([this, snapshot] { runApply(snapshot); });
    return ApplyRefusal::None;
}

void UpdateService::waitForApply() {
    if (apply_thread_.joinable()) apply_thread_.join();
}

void UpdateService::failApply(const std::string& why) {
    std::lock_guard<std::mutex> lock(mutex_);
    spdlog::warn("Updates: not applied: {}", why);
    status_.applying = false;
    status_.apply_step.clear();
    status_.error = why;
}

void UpdateService::runApply(UpdateStatus snapshot) {
    namespace fs = std::filesystem;
    Hooks hooks;
    update::Handoff handoff;
    std::string dir, platform;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        hooks = hooks_;
        handoff = handoff_;
        platform = platform_;
        dir = updateDir();
    }
    std::error_code ec;
    fs::create_directories(dir, ec);
    if (ec) return failApply("cannot create " + dir + ": " + ec.message());

    const std::string kind = update::assetKindFor(handoff, platform);
    const UpdateAsset* asset = nullptr;
    for (const auto& a : snapshot.assets) if (a.kind == kind) asset = &a;
    if (!asset) return failApply("the release has no " + kind + " for this platform");

    if (handoff == update::Handoff::Systemd) {
        // D10: root fetches and verifies the release itself; all this side
        // says is which version. The backup still happens here, as the user
        // who owns the database.
        if (hooks.backup) {
            const std::string why = hooks.backup(dir);
            if (!why.empty()) return failApply("database backup failed: " + why);
        }
        std::ofstream req(dir + "/request", std::ios::trunc);
        req << snapshot.latest << "\n";
        req.close();
        if (!req) return failApply("cannot write " + dir + "/request");
        std::lock_guard<std::mutex> lock(mutex_);
        status_.apply_step = "handing_off";
        spdlog::info("Updates: asked systemd to install {}", snapshot.latest);
        return;   // the root unit stops this service; nothing more to do here
    }

    // Supervisor: download, prove it, stage it, leave.
    const std::string final_path = dir + "/" + asset->name;
    const std::string part = final_path + ".part";
    fs::remove(part, ec);
    spdlog::info("Updates: downloading {} for {}", asset->name, snapshot.latest);
    if (!hooks.download(asset->url, part)) {
        fs::remove(part, ec);
        return failApply("download of " + asset->name + " failed");
    }
    {
        std::lock_guard<std::mutex> lock(mutex_);
        status_.apply_step = "verifying";
    }
    const auto size = static_cast<long long>(fs::file_size(part, ec));
    if (ec || size != asset->size) {
        fs::remove(part, ec);
        return failApply(asset->name + " is " + std::to_string(size) + " bytes, the manifest says " +
                         std::to_string(asset->size));
    }
    const std::string sha = update::sha256File(part);
    if (sha != asset->sha256) {
        fs::remove(part, ec);
        return failApply(asset->name + " does not match the manifest's SHA-256");
    }
    fs::rename(part, final_path, ec);
    if (ec) return failApply("cannot stage " + final_path + ": " + ec.message());

    if (hooks.backup) {
        const std::string why = hooks.backup(dir);
        if (!why.empty()) return failApply("database backup failed: " + why);
    }

    json pending = {
        {"version", snapshot.latest},
        {"from", current_},
        {"file", fs::absolute(final_path).string()},
        {"name", asset->name},
        {"kind", asset->kind},
        {"platform", asset->platform},
        {"sha256", asset->sha256},
        {"size", asset->size},
    };
    std::ofstream out(dir + "/pending.json", std::ios::trunc);
    out << pending.dump(2);
    out.close();
    if (!out) return failApply("cannot write " + dir + "/pending.json");

    {
        std::lock_guard<std::mutex> lock(mutex_);
        status_.apply_step = "handing_off";
    }
    spdlog::info("Updates: {} verified and staged; handing over to the supervisor", snapshot.latest);
    hooks.exit(kExitApplyUpdate);
}

// Every return goes through status(), so "check now" answers with the same
// derived fields (can_apply, installer, database) as GET /api/update. The raw
// status_ lacks them, and returning it hid the Apply button until a reload.
UpdateStatus UpdateService::checkNow() {
    if (const auto s = status(); s.containerised) return s;

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
        {
            std::lock_guard<std::mutex> lock(mutex_);
            // One quiet line, and the last good answer stands: a network blip
            // is not a reason to take a banner away or to put one up.
            if (status_.error != why) spdlog::warn("Updates: check failed: {}", why);
            status_.error = why;
            status_.checked_at = nowIsoUtc();
        }
        return status();
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
            std::string platform;
            {
                std::lock_guard<std::mutex> lock(mutex_);
                platform = platform_;
            }
            next.assets = update::assetsForPlatform(man.body, platform,
                                                    info.download_urls);
            next.available = !next.assets.empty();
        }
    }

    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (next.available && (!status_.available || status_.latest != next.latest)) {
            spdlog::info("Updates: {} is available (running {})", next.latest, current_);
        }
        etag_ = new_etag;
        last_release_body_ = release_body;
        // A check does not undo what it did not do: an apply in flight, the
        // last install's result and the container verdict all carry over.
        next.containerised = status_.containerised;
        next.applying = status_.applying;
        next.apply_step = status_.apply_step;
        next.last_result = status_.last_result;
        if (status_.applying && next.error.empty()) next.error = status_.error;
        status_ = next;
    }
    return status();
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
            const auto s = checkNow();
            // D1 opt-in: apply by itself, but only when idle (§3.5). A busy
            // collector means try again in half an hour, not tomorrow.
            if (s.available && hooks_.auto_update && hooks_.auto_update()) {
                const auto refusal = apply(false);
                if (refusal == ApplyRefusal::Busy) {
                    spdlog::info("Updates: {} waits for the collector to go idle", s.latest);
                    next = std::chrono::steady_clock::now() + std::chrono::minutes(30);
                    continue;
                }
            }
        } catch (const std::exception& e) {
            spdlog::warn("Updates: check threw: {}", e.what());
        }
        next = std::chrono::steady_clock::now() + std::chrono::hours(24);
    }
}

}  // namespace hms_cpap
