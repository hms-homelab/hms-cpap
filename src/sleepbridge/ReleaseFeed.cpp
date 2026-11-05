#include "sleepbridge/ReleaseFeed.h"

#include "mule/MuleFirmware.h"   // compareVersions, updateAvailable

#include <iostream>
#include <sstream>

namespace hms_cpap::sleepbridge {

namespace {

bool hex64(const std::string& s) {
    if (s.size() != 64) return false;
    for (const char c : s)
        if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'))) return false;
    return true;
}

std::string lower(std::string s) {
    for (auto& c : s)
        if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
    return s;
}

}  // namespace

std::optional<Feed> parseFeed(const std::string& text, std::string* why) {
    auto fail = [&](const std::string& w) -> std::optional<Feed> {
        if (why) *why = w;
        return std::nullopt;
    };
    Json::Value             v;
    Json::CharReaderBuilder rb;
    std::istringstream      ss(text);
    if (!Json::parseFromStream(rb, ss, &v, nullptr) || !v.isObject()) return fail("the feed is not a JSON object");
    if (v.isMember("component") && v["component"].asString() != "tapper")
        return fail("the feed is not a Sleep Bridge's");
    if (!v["version"].isString() || v["version"].asString().empty()) return fail("the feed names no version");
    if (!v["variants"].isObject()) return fail("the feed lists no variants");
    Feed f;
    f.version     = v["version"].asString();
    f.released_at = v.get("released_at", "").asString();
    for (const auto& name : v["variants"].getMemberNames()) {
        const Json::Value& e = v["variants"][name];
        if (!e.isObject() || !e["url"].isString() || !e["sha256"].isString() || !e["size"].isIntegral())
            return fail("variant \"" + name + "\" is not {url, size, sha256}");
        FeedImage img;
        img.url    = e["url"].asString();
        img.size   = static_cast<size_t>(e["size"].asUInt64());
        img.sha256 = lower(e["sha256"].asString());
        if (img.url.rfind("https://", 0) != 0) return fail("variant \"" + name + "\": the image is not on https");
        if (img.size == 0) return fail("variant \"" + name + "\": the image size is 0");
        if (!hex64(img.sha256)) return fail("variant \"" + name + "\": sha256 is not 64 hex digits");
        f.variants[name] = img;
    }
    return f;
}

std::optional<std::string> bridgeVariant(const Json::Value& state) {
    if (!state.isObject() || !state["variant"].isString()) return std::nullopt;
    return state["variant"].asString();
}

ReleaseFeed::ReleaseFeed(std::string url, FeedGet get) : url_(std::move(url)), get_(std::move(get)) {
    if (!enabled()) state_ = "off";
}

ReleaseFeed::~ReleaseFeed() { wait(); }

void ReleaseFeed::refresh() {
    if (!enabled()) return;
    const FeedReply r = get_(url_, kFeedTimeoutS);
    std::string state, error;
    std::optional<Feed> feed;
    if (!r.ok) {
        state = "failed";
        error = "the release feed could not be read: " + r.error;
    } else if (r.status == 403 || r.status == 404) {
        // The public path is not open yet: the file upload is the update.
        state = "not_published";
        error = "the release feed is not published yet (HTTP " + std::to_string(r.status) + ")";
    } else if (r.status != 200) {
        state = "failed";
        error = "the release feed answered HTTP " + std::to_string(r.status);
    } else {
        std::string why;
        feed = parseFeed(r.body, &why);
        state = feed ? "ok" : "failed";
        if (!feed) error = "the release feed is malformed: " + why;
    }
    {
        std::lock_guard<std::mutex> lk(mu_);
        const bool changed = state != state_ || (feed && feed_ && feed->version != feed_->version);
        state_ = state;
        error_ = error;
        feed_  = feed;
        if (changed)
            std::cout << "[sleepbridge] release feed: " << state
                      << (feed ? ", version " + feed->version : (error.empty() ? "" : ", " + error)) << std::endl;
    }
}

void ReleaseFeed::refreshAsync() {
    if (!enabled()) return;
    bool expected = false;
    if (!busy_.compare_exchange_strong(expected, true)) return;   // one read at a time
    if (thread_.joinable()) thread_.join();                      // the last read, long done
    thread_ = std::thread([this] {
        refresh();
        busy_ = false;
    });
}

void ReleaseFeed::wait() {
    if (thread_.joinable()) thread_.join();
}

std::string ReleaseFeed::state() const {
    std::lock_guard<std::mutex> lk(mu_);
    return state_;
}

Json::Value ReleaseFeed::answer(const std::string& fw, const std::optional<std::string>& variant) const {
    std::lock_guard<std::mutex> lk(mu_);
    Json::Value v(Json::objectValue);
    v["state"]     = state_;
    v["available"] = false;
    v["version"]   = feed_ ? feed_->version : "";
    v["variant"]   = variant ? Json::Value(*variant) : Json::Value(Json::nullValue);
    v["size"]      = 0;
    if (!error_.empty()) v["error"] = error_;
    if (!feed_) return v;
    if (!variant) {
        v["error"] = "this Sleep Bridge does not say which board it is: update it from a file";
        return v;
    }
    const auto it = feed_->variants.find(*variant);
    if (it == feed_->variants.end()) {
        v["error"] = "the release has no image for this Sleep Bridge's board: update it from a file";
        return v;
    }
    v["size"]      = static_cast<Json::UInt64>(it->second.size);
    v["available"] = mule::updateAvailable(fw, feed_->version);
    return v;
}

std::optional<std::pair<std::string, FeedImage>> ReleaseFeed::release(const std::string& variant) const {
    std::lock_guard<std::mutex> lk(mu_);
    if (!feed_) return std::nullopt;
    const auto it = feed_->variants.find(variant);
    if (it == feed_->variants.end()) return std::nullopt;
    return std::make_pair(feed_->version, it->second);
}

}  // namespace hms_cpap::sleepbridge
