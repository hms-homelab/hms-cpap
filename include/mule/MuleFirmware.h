#pragma once
//
// Updating a Mule & Miner's firmware from here, over the local network
// (docs/MULE_LOCAL_STREAM.md, "Updating the unit's firmware").
//
// The unit's own control server takes an image as a plain POST:
//
//   POST http://<unit-ip>/api/update/mule    raw body, the mule image
//   POST http://<unit-ip>/api/update/miner   raw body, the miner image,
//                                            which the mule flashes into
//                                            the miner over their link
//
// 200 {"status":"ok", ...} once flashed; the mule restarts right after its
// own image, and comes back here with its new version in its hello. 503
// "Firmware update in progress" while one is already running. The unit's
// address is the peer of its stream connection, so nothing is configured.
//
// The image comes from the owner (a file uploaded to POST
// /api/mule/{serial}/update), or from the published manifest, read over
// TLS from here: {"mule": {"version", "url"}, "miner": {...}}. While the
// machine records (the unit streaming, or a night not landed yet) the
// request is warned: 409 with the warning unless it carries confirm, then
// it runs at once (devices/Recording.h). Nothing here is automatic.
//
// The job is devices/OtaJob.h's: a step per image, mule then miner. Each
// step is preparing (the image downloaded), transferring (bytes the unit's
// httpd took), verifying (every byte went, the unit flashing), restarting
// (the unit took it), done. No clock ends a step: the mule's step ends on
// its next hello, which also starts the miner's; the miner's ends on the
// first state that shows its new version. A unit that does not come back
// stays "restarting", with its last word shown as a fact. The job is kept
// as one JSON file per unit, so a restart of hms-cpap reads it back.
//
// The HTTP calls are a seam (Transport), so the suite drives the whole job
// against a fake unit with no socket.
//
#include "devices/OtaJob.h"
#include "mule/MuleService.h"

#include <json/json.h>

#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <ctime>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

namespace hms_cpap::mule {

inline constexpr const char* kManifestUrl   = "https://fw.cpapdash.com/public/latest.json";
inline constexpr size_t      kImageMax      = 4 * 1024 * 1024;   // an image is under 2 MB
inline constexpr int         kPostTimeoutS  = 300;               // ~2 MB into the unit's flash
inline constexpr int         kFetchTimeoutS = 120;

struct HttpReply {
    bool        ok{false};     // the request completed (any status)
    long        status{0};
    std::string body;
    std::string error;         // when !ok: what curl said
};

/// Bytes sent so far, of the total.
using Progress = std::function<void(size_t sent, size_t total)>;

struct Transport {
    std::function<HttpReply(const std::string& url, int timeout_s)> get;
    std::function<HttpReply(const std::string& url, const std::string& body, int timeout_s,
                            const Progress& progress)>
        post;
};

/// The real one: libcurl, the host's CA store for https.
Transport curlTransport();

struct Release {
    std::string version;
    std::string url;
};
struct Manifest {
    Release mule, miner;
};

/// {"mule": {"version", "url"}, "miner": {"version", "url"}}.
std::optional<Manifest> parseManifest(const std::string& json, std::string* why = nullptr);

/// Dotted numbers, left to right: "6.0.0" < "6.1.0" < "10.0.0". A field that is
/// not a number counts as 0.
int compareVersions(const std::string& a, const std::string& b);

/// `latest` is a version and it is above `have` (an unknown `have` counts as
/// below anything).
bool updateAvailable(const std::string& have, const std::string& latest);

class FirmwareUpdater {
public:
    struct Options {
        std::string          manifest_url{kManifestUrl};
        std::chrono::seconds manifest_ttl{600};      // one read serves the page for this long
        bool                 async{true};            // false: each step runs on the caller's thread
        std::string          jobs_dir;               // one <serial>.json per unit; empty: memory only
    };

    /// Watches `units` for the hellos and states that move a job on (it sets
    /// the service's observer).
    FirmwareUpdater(MuleService& units, Transport transport, Options opt);
    FirmwareUpdater(MuleService& units, Transport transport);   // the default options
    ~FirmwareUpdater();

    /// GET /api/mule/{serial}/firmware: the versions it runs, the manifest's,
    /// whether an update is available, whether the machine records, and the
    /// job (legacy shape under "job", the job object under "ota").
    /// `read_manifest` false: the manifest as last read, never a read now
    /// (the Devices page's 3 s refresh; it reads it when it loads).
    Json::Value firmware(const std::string& serial, bool read_manifest = true);

    /// POST /api/mule/{serial}/update. `target` is mule, miner or both; with
    /// an `image` (the owner's file) it is mule or miner. 404 unknown unit,
    /// 409 offline, already updating or nothing newer, 409 with the recording
    /// warning while the machine records and `confirm` is false, 502 the
    /// manifest out of reach, 400 a bad request; else 202 with the job as it
    /// starts.
    CommandAnswer start(const std::string& serial, const std::string& target, std::string image = {},
                        const std::string& image_name = {}, bool confirm = false);

    /// GET /api/mule/{serial}/update: the job in the shape scripts read
    /// before the job object (kept one release), or null.
    Json::Value job(const std::string& serial) const;
    /// The job as devices/OtaJob.h's object, or null.
    Json::Value ota(const std::string& serial) const;

    /// The manifest, read now or from the cache; `why` says what went wrong.
    std::optional<Manifest> manifest(std::string* why = nullptr);

    /// A unit's hello or state (MuleService's observer): ends a step the
    /// unit's return or its new version resolves, and starts the next.
    void onUnit(const std::string& serial, bool hello);

    /// Tests: returns once every step started so far has run.
    void waitIdle();

private:
    struct Job {
        devices::OtaJob ota;
        std::string target;              // mule, miner, both
        std::vector<std::string> log;
        std::string mule_image, miner_image;   // an owner's file, held until its step runs
        Release     mule_release, miner_release;
        uint64_t    hellos_before{0};          // the unit's sessions when the mule step began
    };

    /// Runs the current step: fetch, send, then restarting (mule) or the
    /// unit's state (miner). On the worker, or inline when not async.
    void runStep(std::shared_ptr<Job> j);
    void launch(std::shared_ptr<Job> j);
    bool sendImage(Job& j, const std::string& target, const std::string& image);   // mu_ not held
    bool fetchImage(Job& j, const Release& r, std::string& out);                 // mu_ not held
    void note(Job& j, const std::string& what);                                   // takes mu_
    void fail(Job& j, const std::string& reason, const std::string& detail);     // mu_ held
    void changed(Job& j);                                                         // mu_ held
    /// Moves past steps that need nothing; the job is done after the last.
    void advance(Job& j);                                                         // mu_ held
    static Json::Value jobJson(const Job& j);

    MuleService& units_;
    Transport    t_;
    Options      opt_;

    mutable std::mutex                           mu_;
    std::map<std::string, std::shared_ptr<Job>>  jobs_;    // the last job per unit
    std::mutex                                   workers_mu_;
    std::vector<std::thread>                     workers_; // one per step that ran; joined at the end
    uint64_t                                     next_job_{0};
    std::optional<Manifest>                      cached_;
    std::string                                  cached_error_;
    std::chrono::steady_clock::time_point        cached_at_{};
    std::time_t                                  cached_wall_{0};
};

}  // namespace hms_cpap::mule
