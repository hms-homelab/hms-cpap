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
// TLS from here: {"mule": {"version", "url"}, "miner": {...}}. An update
// runs only in the unit's downtime: while a night is open on it (streaming,
// or streamed and not yet landed) the request is refused, and the job checks
// again right before each image goes.
//
// The HTTP calls are a seam (Transport), so the suite drives the whole job
// against a fake unit with no socket.
//
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
        std::chrono::seconds reconnect_wait{150};    // the mule's restart, then its hello here
        std::chrono::milliseconds poll{1000};        // how often the wait looks
        bool                 async{true};            // false: start() runs the job before it returns
    };

    FirmwareUpdater(MuleService& units, Transport transport, Options opt);
    FirmwareUpdater(MuleService& units, Transport transport);   // the default options
    ~FirmwareUpdater();

    /// GET /api/mule/{serial}/firmware: the versions it runs, the manifest's,
    /// whether an update is available, whether a night is open, and the job.
    Json::Value firmware(const std::string& serial);

    /// POST /api/mule/{serial}/update. `target` is mule, miner or both; with
    /// an `image` (the owner's file) it is mule or miner. 404 unknown unit,
    /// 409 a night open, offline, already updating or nothing newer, 502 the
    /// manifest or an image out of reach, 400 a bad request; else 202 with
    /// the job as it starts.
    CommandAnswer start(const std::string& serial, const std::string& target, std::string image = {},
                        const std::string& image_name = {});

    /// GET /api/mule/{serial}/update: the job, as it runs or as it ended.
    Json::Value job(const std::string& serial) const;

    /// The manifest, read now or from the cache; `why` says what went wrong.
    std::optional<Manifest> manifest(std::string* why = nullptr);

private:
    struct Job {
        std::string serial;
        std::string target;              // mule, miner, both
        std::string source;              // "file <name>" or "manifest <version>"
        std::string phase{"queued"};     // queued, downloading, sending, waiting, done, failed
        std::string step;                // which image: mule or miner
        size_t      sent{0}, total{0};
        std::string error;
        std::string fw_before, miner_fw_before;
        std::string fw_after, miner_fw_after;
        std::time_t started{0}, finished{0};
        std::vector<std::string> log;
        bool        running{false};
    };

    void run(std::shared_ptr<Job> j, std::string mule_image, std::string miner_image,
             Release mule_release, Release miner_release);
    bool sendImage(Job& j, const std::string& target, const std::string& image);   // mu_ not held
    bool fetchImage(Job& j, const Release& r, std::string& out);                 // mu_ not held
    bool waitForReturn(Job& j, uint64_t hellos_before);                           // mu_ not held
    void note(Job& j, const std::string& what);                                   // takes mu_
    static Json::Value jobJson(const Job& j);

    MuleService& units_;
    Transport    t_;
    Options      opt_;

    mutable std::mutex                           mu_;
    std::map<std::string, std::shared_ptr<Job>>  jobs_;    // the last job per unit
    std::thread                                  worker_;
    std::optional<Manifest>                      cached_;
    std::string                                  cached_error_;
    std::chrono::steady_clock::time_point        cached_at_{};
    std::time_t                                  cached_wall_{0};
};

}  // namespace hms_cpap::mule
