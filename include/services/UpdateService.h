#pragma once
//
// SDD-041: the app learns about its own updates, from GitHub, and hands them
// to whatever can install them.
//
// CHECK: once a day and on demand it asks the GitHub Releases API for the newest
// stable release, reads that release's manifest.json (the file the release
// workflow publishes with every asset's size and SHA-256), and keeps the answer
// for the dashboard banner and Settings.
//
// APPLY: the service never replaces itself (§3.4). It prepares, then hands off:
//   - under the desktop supervisor (HMS_CPAP_SUPERVISED=1) it downloads this
//     platform's release file, checks it against the manifest, writes
//     update/pending.json, and exits with kExitApplyUpdate. The supervisor
//     starts the helper that swaps the install (D8, D9);
//   - under systemd on the Pi (HMS_CPAP_UPDATER=systemd) it writes
//     update/request naming the version. A root unit fetches and verifies the
//     release itself, because it must not trust a file this user wrote (D10);
//   - anywhere else nothing can install it, and Apply is not offered.
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

/// The exit code that means "install the update I staged", not a failure. The
/// supervisor (desktop/qt ChildProcess) matches it by value.
constexpr int kExitApplyUpdate = 42;

/// One file of a release that fits this machine, as the manifest describes it.
struct UpdateAsset {
    std::string platform;   ///< windows-x64, macos-arm64, linux-armhf
    std::string kind;       ///< installer, dmg, zip
    std::string name;
    std::string url;        ///< the release's browser_download_url for it
    std::string sha256;
    long long size = 0;
};

/// What the helper (or the Pi's root unit) wrote after the last attempt.
struct UpdateResult {
    bool present = false;   ///< a result file exists at all
    bool ok = false;
    std::string version;    ///< the version it tried to install
    std::string step;       ///< where it failed: download, verify, install, preflight, start, health
    std::string message;
    std::string at;         ///< ISO-8601 UTC
};

/// What the last check found. `available` is the only field the banner needs;
/// the rest is for Settings and for the log.
struct UpdateStatus {
    std::string current;        ///< HMS_CPAP_VERSION
    std::string latest;         ///< newest stable release, "" until a check succeeds
    bool available = false;     ///< latest is newer AND has an asset for this platform
    bool containerised = false; ///< D5: Docker / add-on, never offered anything
    bool can_apply = false;     ///< available AND something here can install it
    std::string installer;      ///< supervisor, systemd, or "" when nothing here installs updates
    bool applying = false;      ///< an apply is downloading or handing off
    std::string apply_step;     ///< downloading, verifying, handing_off; "" when idle
    bool auto_update = false;   ///< D1 opt-in, echoed for Settings
    std::string database;       ///< sqlite, postgresql, mysql: D6 says who owns a data rollback
    bool checked = false;       ///< a check has completed at least once
    std::string checked_at;     ///< ISO-8601 UTC of the last completed check
    std::string notes;          ///< the release's own notes
    std::string release_url;    ///< the release page
    std::string error;          ///< why the last check or apply failed, "" when it did not
    std::vector<UpdateAsset> assets;  ///< this platform's files in that release
    UpdateResult last_result;   ///< the last install attempt, as its helper reported it
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

/// Who can install an update for this process.
enum class Handoff { None, Supervisor, Systemd };
Handoff handoffMode();

/// The manifest `kind` the handoff installs: the DMG on macOS and the Setup.exe
/// on Windows (D8, the whole install), the zip on the Pi. "" when none.
std::string assetKindFor(Handoff handoff, const std::string& platform);

/// Lowercase hex SHA-256 of a file, "" when it cannot be read.
std::string sha256File(const std::string& path);

/// Read the helper's result.json. `present` is false when there is none.
UpdateResult readResult(const std::string& path);

/// D6: a consistent copy of a SQLite database, taken while it is in use
/// (VACUUM INTO). Returns "" on success, else why not. An existing [dest] is
/// replaced.
std::string backupSqlite(const std::string& src, const std::string& dest);

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

    /// What apply needs from the rest of the app. Each has a working default
    /// except `busy`, `backup` and `auto_update`, which main.cpp supplies.
    struct Hooks {
        /// §3.5: true while session data is still arriving.
        std::function<bool()> busy;
        /// D6: back up a SQLite database into [dir] before handing off.
        /// Returns "" on success (or when there is nothing to back up), else why not.
        std::function<std::string(const std::string& dir)> backup;
        /// The D1 opt-in, read live so a Settings change needs no restart.
        std::function<bool()> auto_update;
        /// The database engine, for D6's warning.
        std::function<std::string()> database;
        /// Download [url] to [path]. Default: libcurl, following redirects.
        std::function<bool(const std::string& url, const std::string& path)> download;
        /// Leave for the supervisor. Default: std::exit after a short delay, so
        /// the answer to the request that asked for it has gone out.
        std::function<void(int code)> exit;
    };

    enum class ApplyRefusal { None, NotAvailable, NoInstaller, Busy, AlreadyApplying };

    explicit UpdateService(std::string current_version,
                           std::string repo = "hms-homelab/hms-cpap",
                           HttpGet http = nullptr);
    ~UpdateService();

    /// Where update/ lives (the data dir). Also reads the last result.
    void setDataDir(const std::string& dir);
    void setHooks(Hooks hooks);
    /// Override the handoff mode (tests). Default: update::handoffMode().
    void setHandoff(update::Handoff h);
    /// Override the platform key (tests). Default: update::platformKey().
    void setPlatform(const std::string& platform);

    /// D2: a background check once a day, the first one shortly after start.
    void start();
    void stop();

    /// The last answer. Cheap; this is what the API returns.
    UpdateStatus status() const;

    /// Check now and return the answer: the "check now" button, and the tests.
    UpdateStatus checkNow();

    /// Start installing the available update. Returns at once; progress shows in
    /// status(). `now` skips the §3.5 wait for an idle collector (the user said
    /// "now"). A refusal changes nothing.
    ApplyRefusal apply(bool now);

    /// Wait for an apply started by apply() to finish its part. Tests, shutdown.
    void waitForApply();

private:
    void runLoop();
    void runApply(UpdateStatus snapshot);
    void failApply(const std::string& why);
    std::string updateDir() const;

    std::string current_;
    std::string repo_;
    HttpGet http_;
    Hooks hooks_;
    update::Handoff handoff_;
    std::string platform_;
    std::string data_dir_;

    mutable std::mutex mutex_;
    UpdateStatus status_;
    std::string etag_;              ///< for If-None-Match: an unchanged release is a 304
    std::string last_release_body_; ///< reused on a 304

    std::thread worker_;
    std::thread apply_thread_;
    std::atomic<bool> running_{false};
    std::condition_variable wake_;
    std::mutex wake_mutex_;
};

}  // namespace hms_cpap
