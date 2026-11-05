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
// /api/mule/{serial}/update), or from CpapDash's released firmware, read
// over TLS from here through the same public route the web flasher uses:
//
//   POST https://www.cpapdash.com/fwbake/v1/flasher/resolve
//        {"pairing_code", "role": "mule"|"miner"}
//   200  {"serial", "role", "version", "flash": [{"offset", "url"}, ...]}
//   202  {"status": "baking", "serial", "role", "version", ...}
//
// The pairing code printed on the unit is the route's gate, so the owner
// gives it once and it is kept here per unit. The mule image carries the
// unit's identity, so it is built for that serial (202 while it is built);
// the miner image is the one for the unit's SPI pinout. The application
// image is the flash entry at 0x20000; its URL is origin-relative and
// signed for an hour. Before a byte goes to the unit, the image must be the
// one asked for: an ESP32 application image of the right project, the
// released version, the mule's naming this unit's serial, the miner's being
// the variant the public fleet map lists for this unit.
//
// While the machine records (the unit streaming, or a night not landed yet)
// the request is warned: 409 with the warning unless it carries confirm,
// then it runs at once (devices/Recording.h). Nothing here is automatic.
// The release is read when the Devices page loads and when an update
// starts; no timer.
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

/// Where released firmware is resolved: <base>/v1/flasher/resolve and
/// <base>/v1/flasher/fleet-map; an image URL it answers is relative to the
/// base's origin.
inline constexpr const char* kReleaseBase   = "https://www.cpapdash.com/fwbake";
inline constexpr size_t      kImageMax      = 4 * 1024 * 1024;   // an image is under 2 MB
inline constexpr int         kPostTimeoutS  = 300;               // ~2 MB into the unit's flash
inline constexpr int         kFetchTimeoutS = 120;
inline constexpr int         kResolveTimeoutS = 20;

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
    /// A raw image to the unit.
    std::function<HttpReply(const std::string& url, const std::string& body, int timeout_s,
                            const Progress& progress)>
        post;
    /// A JSON request (the release resolve).
    std::function<HttpReply(const std::string& url, const std::string& json, int timeout_s)> post_json;
};

/// The real one: libcurl, the host's CA store for https.
Transport curlTransport();

struct Release {
    std::string version;
    std::string url;      // absolute; empty while the image is being built
    bool        baking{false};
};
struct Manifest {
    Release mule, miner;
};

/// One resolve answer for `role`: the version and the application image's
/// URL (the 0x20000 entry, made absolute against `base`'s origin), or a
/// 202's version with `baking` set. Refused, with `why`, when it is not
/// JSON, names another role, or is for a serial other than `serial`.
std::optional<Release> parseResolve(const std::string& json, long status, const std::string& role,
                                    const std::string& serial, const std::string& base,
                                    std::string* why = nullptr);

/// "https://host/fwbake" + "/fwbake/v1/x" -> "https://host/fwbake/v1/x".
std::string absoluteUrl(const std::string& base, const std::string& path);

/// The SPI variant a miner image's URL is for: "<v>/miner-<v>.bin" is
/// "official", "<v>/miner-<variant>-<v>.bin" is that variant; nullopt when
/// the path is neither.
std::optional<std::string> minerVariantOf(const std::string& url, const std::string& version);

/// The public fleet map's variant for the miner of `serial`, or nullopt
/// when the map does not list it (or is not a fleet map).
std::optional<std::string> fleetVariant(const std::string& json, const std::string& serial);

/// The checks an image downloaded from a release passes before it is sent:
/// a size within reason, an ESP32 application image whose project is
/// `role`, whose version is `version`, and for the mule one that carries
/// `serial`. False with `why` otherwise.
bool checkReleaseImage(const std::string& image, const std::string& role, const std::string& version,
                       const std::string& serial, std::string* why);

/// A pairing code as printed on the unit: six digits.
bool validPairingCode(const std::string& code);

/// Dotted numbers, left to right: "6.0.0" < "6.1.0" < "10.0.0". A field that is
/// not a number counts as 0.
int compareVersions(const std::string& a, const std::string& b);

/// `latest` is a version and it is above `have` (an unknown `have` counts as
/// below anything).
bool updateAvailable(const std::string& have, const std::string& latest);

class FirmwareUpdater {
public:
    struct Options {
        std::string          release_base{kReleaseBase};
        std::chrono::seconds manifest_ttl{600};      // one read serves the page for this long
        bool                 async{true};            // false: each step runs on the caller's thread
        std::string          jobs_dir;               // one <serial>.json per unit; empty: memory only
        std::string          pairing_file;           // {"<serial>": "<code>"}; empty: memory only
    };

    /// Watches `units` for the hellos and states that move a job on (it sets
    /// the service's observer).
    FirmwareUpdater(MuleService& units, Transport transport, Options opt);
    FirmwareUpdater(MuleService& units, Transport transport);   // the default options
    ~FirmwareUpdater();

    /// GET /api/mule/{serial}/firmware: the versions it runs, the latest
    /// released ones (under "latest"; how the read went under "manifest"),
    /// whether an update is available, whether a pairing code is kept,
    /// whether the machine records, and the job (legacy shape under "job",
    /// the job object under "ota"). `read_manifest` false: the release as
    /// last read, never a read now (the Devices page's 3 s refresh; it reads
    /// it when it loads).
    Json::Value firmware(const std::string& serial, bool read_manifest = true);

    /// POST /api/mule/{serial}/pairing {"code"}: keeps the unit's pairing
    /// code once CpapDash confirms it is this unit's (a miner resolve, which
    /// builds nothing). An empty code forgets it. 400 not six digits, 409 the
    /// code is another unit's or not known, 502 CpapDash out of reach.
    CommandAnswer setPairingCode(const std::string& serial, const std::string& code);

    /// POST /api/mule/{serial}/update. `target` is mule, miner or both; with
    /// an `image` (the owner's file) it is mule or miner. 404 unknown unit,
    /// 409 offline, already updating, nothing newer or no pairing code kept,
    /// 409 with the recording warning while the machine records and
    /// `confirm` is false, 502 the release out of reach, 503 the mule image
    /// still being built, 400 a bad request; else 202 with the job as it
    /// starts. The release is read fresh here: its image URLs are signed for
    /// an hour.
    CommandAnswer start(const std::string& serial, const std::string& target, std::string image = {},
                        const std::string& image_name = {}, bool confirm = false);

    /// GET /api/mule/{serial}/update: the job in the shape scripts read
    /// before the job object (kept one release), or null.
    Json::Value job(const std::string& serial) const;
    /// The job as devices/OtaJob.h's object, or null.
    Json::Value ota(const std::string& serial) const;

    /// The unit's latest release, read now or from the cache (`fresh` always
    /// reads); `why` says what went wrong.
    std::optional<Manifest> manifest(const std::string& serial, std::string* why = nullptr, bool fresh = false);

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
    /// One resolve for `role`; nullopt with `why` when it did not answer one.
    std::optional<Release> resolve(const std::string& serial, const std::string& code,
                                   const std::string& role, std::string* why);
    std::string pairingCode(const std::string& serial) const;
    void        savePairing();                                                    // mu_ held
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
    struct Cached {
        std::optional<Manifest>               m;
        std::string                           error;
        std::chrono::steady_clock::time_point at{};
        std::time_t                           wall{0};
    };
    std::map<std::string, Cached>                cached_;    // per unit: the release is the unit's
    std::map<std::string, std::string>           pairing_;   // serial -> pairing code
};

}  // namespace hms_cpap::mule
