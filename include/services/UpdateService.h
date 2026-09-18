#pragma once
//
// SDD-041: the app learns about its own updates, from GitHub.
//
// This half only CHECKS: once a day and on demand it asks the GitHub Releases
// API for the newest stable release, reads that release's manifest.json (the
// file the release workflow publishes with every asset's size and SHA-256), and
// keeps the answer for the dashboard banner and Settings. Applying an update is
// the supervisor's job (SDD-041 §3.4): a running binary cannot replace itself.
//
// Out of scope by decision (D5): a container. Docker and the Home Assistant
// add-on update by pulling an image, so there the check reports "containerised"
// and offers nothing.
//
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <functional>
#include <map>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace hms_cpap {

/// One file of a release that fits this machine, as the manifest describes it.
struct UpdateAsset {
    std::string platform;   ///< windows-x64, macos-arm64, linux-armhf
    std::string kind;       ///< installer, dmg, zip
    std::string name;
    std::string url;        ///< the release's browser_download_url for it
    std::string sha256;
    long long size = 0;
};

/// What the last check found. `available` is the only field the banner needs;
/// the rest is for Settings and for the log.
struct UpdateStatus {
    std::string current;        ///< HMS_CPAP_VERSION
    std::string latest;         ///< newest stable release, "" until a check succeeds
    bool available = false;     ///< latest is newer AND has an asset for this platform
    bool containerised = false; ///< D5: Docker / add-on, never offered anything
    bool checked = false;       ///< a check has completed at least once
    std::string checked_at;     ///< ISO-8601 UTC of the last completed check
    std::string notes;          ///< the release's own notes
    std::string release_url;    ///< the release page
    std::string error;          ///< why the last check failed, "" when it did not
    std::vector<UpdateAsset> assets;  ///< this platform's files in that release
};

namespace update {

/// Compare dotted versions numerically ("5.3.0" > "5.2.19"). A leading "v" is
/// ignored, and a missing part counts as 0. Returns -1, 0 or 1.
int compareVersions(const std::string& a, const std::string& b);

/// This build's platform key, as the manifest spells it. "" for a platform the
/// release does not publish, which means nothing is ever offered.
std::string platformKey();

/// D5: is this process running in a container that updates by pulling an
/// image? HMS_CPAP_CONTAINER decides when set (1 or 0); otherwise Home
/// Assistant's SUPERVISOR_TOKEN or /.dockerenv.
bool isContainerised();

/// The fields of GitHub's /releases/latest this needs. False when the JSON is
/// not a release. `download_urls` maps each asset name to its download URL.
struct ReleaseInfo {
    std::string tag;
    std::string notes;
    std::string html_url;
    bool prerelease = false;
    bool draft = false;
    std::map<std::string, std::string> download_urls;
};
bool parseRelease(const std::string& json, ReleaseInfo& out);

/// The manifest's assets for [platform], each given its download URL from the
/// release. An asset the manifest lists but the release does not carry is left
/// out, because offering a file nobody can download is worse than offering none.
std::vector<UpdateAsset> assetsForPlatform(const std::string& manifest_json,
                                           const std::string& platform,
                                           const std::map<std::string, std::string>& download_urls);

}  // namespace update

class UpdateService {
public:
    /// One HTTP GET: status code, body, and the response's ETag. Injectable so
    /// the check can be tested without the network.
    struct HttpResult {
        long status = 0;
        std::string body;
        std::string etag;
    };
    using HttpGet = std::function<HttpResult(const std::string& url,
                                             const std::string& if_none_match)>;

    explicit UpdateService(std::string current_version,
                           std::string repo = "hms-homelab/hms-cpap",
                           HttpGet http = nullptr);
    ~UpdateService();

    /// D2: a background check once a day, the first one shortly after start.
    void start();
    void stop();

    /// The last answer. Cheap; this is what the API returns.
    UpdateStatus status() const;

    /// Check now and return the answer: the "check now" button, and the tests.
    UpdateStatus checkNow();

private:
    void runLoop();

    std::string current_;
    std::string repo_;
    HttpGet http_;

    mutable std::mutex mutex_;
    UpdateStatus status_;
    std::string etag_;              ///< for If-None-Match: an unchanged release is a 304
    std::string last_release_body_; ///< reused on a 304

    std::thread worker_;
    std::atomic<bool> running_{false};
    std::condition_variable wake_;
    std::mutex wake_mutex_;
};

}  // namespace hms_cpap
