#pragma once
//
// The public Sleep Bridge release feed: one JSON document naming the latest
// release for each board variant, and one public image per variant.
//
//   {"schema": 1, "component": "tapper", "version": "1.0.1",
//    "released_at": "2026-11-05T07:00:00Z",
//    "variants": {"": {"url": "https://.../tapper-1.0.1.bin",
//                      "size": 1136528, "sha256": "<64 hex>"}}}
//
// "" is the standard board. The feed is read over HTTPS with the certificate
// verified, on a bridge's hello and when the Devices page loads, and kept in
// memory until the next of those: no timer. Nothing is ever sent to a bridge
// from it unasked; it only says whether an update is available, and the
// owner's Update button uses it.
//
// Until the feed is published the address answers 403: that is its own
// state ("not_published"), and the page offers the file upload instead.
// An empty address turns the check off.
//
// The image an update downloads from it is checked against the feed's size
// and SHA-256 before a single byte goes to the bridge.
//
#include <json/json.h>

#include <atomic>
#include <cstddef>
#include <functional>
#include <map>
#include <mutex>
#include <optional>
#include <string>
#include <thread>

namespace hms_cpap::sleepbridge {

inline constexpr const char* kReleaseFeedUrl = "https://fw.cpapdash.com/public/tapper/latest.json";
inline constexpr int         kFeedTimeoutS   = 20;
inline constexpr int         kImageTimeoutS  = 120;

/// One HTTP GET: completed (any status) or not, and what came back.
struct FeedReply {
    bool        ok{false};
    long        status{0};
    std::string body;
    std::string error;
};
using FeedGet = std::function<FeedReply(const std::string& url, int timeout_s)>;

struct FeedImage {
    std::string url;
    size_t      size{0};
    std::string sha256;   // lower-case hex
};

struct Feed {
    std::string version;
    std::string released_at;
    std::map<std::string, FeedImage> variants;
};

/// The document above; nullopt with `why` when it is not one. An image whose
/// url is not https, whose size is 0 or whose sha256 is not 64 hex digits
/// makes the whole document refused.
std::optional<Feed> parseFeed(const std::string& text, std::string* why = nullptr);

/// The bridge's variant, from its hello's state: nullopt when it states none
/// (such a bridge is offered no release: a wrong-variant image breaks the
/// capture), else the variant ("" the standard board).
std::optional<std::string> bridgeVariant(const Json::Value& state);

class ReleaseFeed {
public:
    ReleaseFeed(std::string url, FeedGet get);
    ~ReleaseFeed();
    ReleaseFeed(const ReleaseFeed&) = delete;
    ReleaseFeed& operator=(const ReleaseFeed&) = delete;

    bool enabled() const { return !url_.empty() && static_cast<bool>(get_); }
    const std::string& url() const { return url_; }

    /// Reads the feed now, on the caller's thread.
    void refresh();
    /// Reads it on a thread of its own, unless a read is already running.
    void refreshAsync();
    /// Tests: returns once a refreshAsync has finished.
    void wait();

    /// off, unchecked, ok, not_published, failed
    std::string state() const;

    /// For a bridge running `fw` with `variant`:
    ///   {"state", "version", "available", "variant", "size", "error"?}
    /// `available` only when the feed answered, lists the bridge's variant,
    /// and its version is above `fw`.
    Json::Value answer(const std::string& fw, const std::optional<std::string>& variant) const;

    /// The release image for `variant` and the version, when the feed holds
    /// one; nullopt otherwise.
    std::optional<std::pair<std::string, FeedImage>> release(const std::string& variant) const;

private:
    std::string url_;
    FeedGet     get_;

    mutable std::mutex  mu_;
    std::string         state_{"unchecked"};
    std::string         error_;
    std::optional<Feed> feed_;

    std::atomic<bool> busy_{false};
    std::thread       thread_;
};

}  // namespace hms_cpap::sleepbridge
