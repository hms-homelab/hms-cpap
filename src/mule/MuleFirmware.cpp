#include "mule/MuleFirmware.h"

#include <curl/curl.h>

#include <algorithm>
#include <cstdlib>
#include <iostream>
#include <sstream>

namespace hms_cpap::mule {

// ── the real transport ──

namespace {

size_t collect(void* data, size_t size, size_t n, void* userp) {
    static_cast<std::string*>(userp)->append(static_cast<const char*>(data), size * n);
    return size * n;
}

int onProgress(void* userp, curl_off_t, curl_off_t, curl_off_t ultotal, curl_off_t ulnow) {
    const auto* p = static_cast<const Progress*>(userp);
    if (p && *p) (*p)(static_cast<size_t>(ulnow), static_cast<size_t>(ultotal));
    return 0;
}

HttpReply perform(CURL* c, const std::string& url) {
    HttpReply r;
    const CURLcode res = curl_easy_perform(c);
    if (res != CURLE_OK) {
        r.error = std::string(curl_easy_strerror(res)) + " (" + url + ")";
        return r;
    }
    r.ok = true;
    curl_easy_getinfo(c, CURLINFO_RESPONSE_CODE, &r.status);
    return r;
}

}  // namespace

Transport curlTransport() {
    Transport t;
    t.get = [](const std::string& url, int timeout_s) {
        HttpReply r;
        CURL*     c = curl_easy_init();
        if (!c) {
            r.error = "curl_easy_init failed";
            return r;
        }
        curl_easy_setopt(c, CURLOPT_URL, url.c_str());
        curl_easy_setopt(c, CURLOPT_WRITEFUNCTION, collect);
        curl_easy_setopt(c, CURLOPT_WRITEDATA, &r.body);
        curl_easy_setopt(c, CURLOPT_TIMEOUT, static_cast<long>(timeout_s));
        curl_easy_setopt(c, CURLOPT_CONNECTTIMEOUT, 15L);
        curl_easy_setopt(c, CURLOPT_FOLLOWLOCATION, 1L);
        curl_easy_setopt(c, CURLOPT_MAXREDIRS, 3L);
        curl_easy_setopt(c, CURLOPT_MAXFILESIZE_LARGE, static_cast<curl_off_t>(kImageMax));
        const HttpReply out = [&] {
            HttpReply p = perform(c, url);
            p.body      = std::move(r.body);
            return p;
        }();
        curl_easy_cleanup(c);
        return out;
    };
    t.post = [](const std::string& url, const std::string& body, int timeout_s, const Progress& progress) {
        HttpReply r;
        CURL*     c = curl_easy_init();
        if (!c) {
            r.error = "curl_easy_init failed";
            return r;
        }
        // The unit's server reads a raw body of Content-Length bytes; it does
        // not answer "Expect: 100-continue", which curl would otherwise send
        // for a body this size and wait a second on.
        struct curl_slist* headers = nullptr;
        headers = curl_slist_append(headers, "Content-Type: application/octet-stream");
        headers = curl_slist_append(headers, "Expect:");
        curl_easy_setopt(c, CURLOPT_URL, url.c_str());
        curl_easy_setopt(c, CURLOPT_HTTPHEADER, headers);
        curl_easy_setopt(c, CURLOPT_POST, 1L);
        curl_easy_setopt(c, CURLOPT_POSTFIELDS, body.data());
        curl_easy_setopt(c, CURLOPT_POSTFIELDSIZE_LARGE, static_cast<curl_off_t>(body.size()));
        curl_easy_setopt(c, CURLOPT_WRITEFUNCTION, collect);
        curl_easy_setopt(c, CURLOPT_WRITEDATA, &r.body);
        curl_easy_setopt(c, CURLOPT_TIMEOUT, static_cast<long>(timeout_s));
        curl_easy_setopt(c, CURLOPT_CONNECTTIMEOUT, 10L);
        curl_easy_setopt(c, CURLOPT_NOPROGRESS, 0L);
        curl_easy_setopt(c, CURLOPT_XFERINFOFUNCTION, onProgress);
        curl_easy_setopt(c, CURLOPT_XFERINFODATA, &progress);
        const HttpReply out = [&] {
            HttpReply p = perform(c, url);
            p.body      = std::move(r.body);
            return p;
        }();
        curl_slist_free_all(headers);
        curl_easy_cleanup(c);
        return out;
    };
    return t;
}

// ── the manifest and versions ──

std::optional<Manifest> parseManifest(const std::string& text, std::string* why) {
    Json::Value             v;
    Json::CharReaderBuilder rb;
    std::istringstream      ss(text);
    if (!Json::parseFromStream(rb, ss, &v, nullptr) || !v.isObject()) {
        if (why) *why = "the manifest is not a JSON object";
        return std::nullopt;
    }
    Manifest m;
    for (const auto& [name, into] : {std::pair{"mule", &m.mule}, std::pair{"miner", &m.miner}}) {
        const Json::Value& e = v[name];
        if (!e.isObject() || !e["version"].isString() || !e["url"].isString() ||
            e["version"].asString().empty() || e["url"].asString().empty()) {
            if (why) *why = std::string("the manifest has no ") + name + " {version, url}";
            return std::nullopt;
        }
        into->version = e["version"].asString();
        into->url     = e["url"].asString();
    }
    return m;
}

int compareVersions(const std::string& a, const std::string& b) {
    auto fields = [](const std::string& s) {
        std::vector<long> out;
        std::string       cur;
        for (const char ch : s + ".") {
            if (ch == '.') {
                out.push_back(cur.empty() ? 0 : std::strtol(cur.c_str(), nullptr, 10));
                cur.clear();
            } else if (ch >= '0' && ch <= '9') {
                cur += ch;
            } else {
                break;   // "6.1.0-rc1": the tail is ignored
            }
        }
        return out;
    };
    const auto fa = fields(a), fb = fields(b);
    for (size_t i = 0; i < std::max(fa.size(), fb.size()); i++) {
        const long x = i < fa.size() ? fa[i] : 0, y = i < fb.size() ? fb[i] : 0;
        if (x != y) return x < y ? -1 : 1;
    }
    return 0;
}

bool updateAvailable(const std::string& have, const std::string& latest) {
    if (latest.empty() || !(latest[0] >= '0' && latest[0] <= '9')) return false;
    if (have.empty() || !(have[0] >= '0' && have[0] <= '9')) return true;
    return compareVersions(have, latest) < 0;
}

// ── the updater ──

FirmwareUpdater::FirmwareUpdater(MuleService& units, Transport transport, Options opt)
    : units_(units), t_(std::move(transport)), opt_(std::move(opt)) {}

FirmwareUpdater::FirmwareUpdater(MuleService& units, Transport transport)
    : FirmwareUpdater(units, std::move(transport), Options{}) {}

FirmwareUpdater::~FirmwareUpdater() {
    if (worker_.joinable()) worker_.join();
}

std::optional<Manifest> FirmwareUpdater::manifest(std::string* why) {
    {
        std::lock_guard<std::mutex> lk(mu_);
        const auto age = std::chrono::steady_clock::now() - cached_at_;
        if (cached_wall_ && age < opt_.manifest_ttl) {
            if (why) *why = cached_error_;
            return cached_;
        }
    }
    std::string             err;
    std::optional<Manifest> m;
    if (!t_.get) {
        err = "no way to read the manifest";
    } else {
        const HttpReply r = t_.get(opt_.manifest_url, kFetchTimeoutS);
        if (!r.ok) {
            err = "the manifest could not be read: " + r.error;
        } else if (r.status != 200) {
            err = "the manifest answered HTTP " + std::to_string(r.status) +
                  (r.status == 403 ? " (it is not published for reading without a key)" : "");
        } else {
            m = parseManifest(r.body, &err);
        }
    }
    std::lock_guard<std::mutex> lk(mu_);
    cached_       = m;
    cached_error_ = err;
    cached_at_    = std::chrono::steady_clock::now();
    cached_wall_  = std::time(nullptr);
    if (why) *why = err;
    return m;
}

Json::Value FirmwareUpdater::jobJson(const Job& j) {
    Json::Value v(Json::objectValue);
    v["target"]          = j.target;
    v["source"]          = j.source;
    v["phase"]           = j.phase;
    v["step"]            = j.step;
    v["sent"]            = static_cast<Json::UInt64>(j.sent);
    v["total"]           = static_cast<Json::UInt64>(j.total);
    v["error"]           = j.error;
    v["fw_before"]       = j.fw_before;
    v["miner_fw_before"] = j.miner_fw_before;
    v["fw_after"]        = j.fw_after;
    v["miner_fw_after"]  = j.miner_fw_after;
    v["started"]         = static_cast<Json::Int64>(j.started);
    v["finished"]        = static_cast<Json::Int64>(j.finished);
    v["running"]         = j.running;
    Json::Value log(Json::arrayValue);
    for (const auto& l : j.log) log.append(l);
    v["log"] = log;
    return v;
}

Json::Value FirmwareUpdater::job(const std::string& serial) const {
    std::lock_guard<std::mutex> lk(mu_);
    const auto it = jobs_.find(serial);
    return it == jobs_.end() ? Json::Value(Json::nullValue) : jobJson(*it->second);
}

Json::Value FirmwareUpdater::firmware(const std::string& serial) {
    const MuleService::Brief b = units_.brief(serial);
    Json::Value v(Json::objectValue);
    v["serial"]     = serial;
    v["known"]      = b.known;
    v["online"]     = b.online;
    v["ip"]         = b.ip;
    v["fw"]         = b.fw;
    v["miner_fw"]   = b.miner_fw;
    v["night_open"] = b.night_open;

    std::string why;
    const auto  m = manifest(&why);
    Json::Value man(Json::objectValue);
    man["url"]   = opt_.manifest_url;
    man["error"] = why;
    {
        std::lock_guard<std::mutex> lk(mu_);
        man["read_at"] = static_cast<Json::Int64>(cached_wall_);
    }
    v["manifest"] = man;
    Json::Value latest(Json::objectValue), avail(Json::objectValue);
    latest["mule"]  = m ? m->mule.version : "";
    latest["miner"] = m ? m->miner.version : "";
    avail["mule"]   = m && updateAvailable(b.fw, m->mule.version);
    avail["miner"]  = m && updateAvailable(b.miner_fw, m->miner.version);
    v["latest"]           = latest;
    v["update_available"] = avail;
    v["job"]              = job(serial);
    return v;
}

CommandAnswer FirmwareUpdater::start(const std::string& serial, const std::string& target, std::string image,
                                     const std::string& image_name) {
    CommandAnswer a;
    auto refuse = [&a](int status, const std::string& why) {
        a.status        = status;
        a.body["ok"]    = false;
        a.body["error"] = why;
        return a;
    };
    if (target != "mule" && target != "miner" && target != "both")
        return refuse(400, "target is mule, miner or both");
    if (!image.empty() && target == "both")
        return refuse(400, "an uploaded image is one of mule or miner: say which");
    if (!image.empty() && image.size() > kImageMax)
        return refuse(400, "the image is larger than a Mule & Miner image can be");

    const MuleService::Brief b = units_.brief(serial);
    if (!b.known) return refuse(404, "no Mule & Miner \"" + serial + "\" has connected here");
    if (b.night_open)
        return refuse(409, "a night is open on this unit: the update waits for its downtime (no mask on, "
                           "and the night landed here)");
    if (!b.online || b.ip.empty()) return refuse(409, "the unit is not connected here right now");

    std::shared_ptr<Job> j;
    {
        std::lock_guard<std::mutex> lk(mu_);
        for (const auto& [s, running] : jobs_)
            if (running->running) return refuse(409, "an update is already running on " + s);
    }
    if (worker_.joinable()) worker_.join();   // the last job ended: its thread is done

    Release mule_release, miner_release;
    if (image.empty()) {
        std::string why;
        const auto  m = manifest(&why);
        if (!m) return refuse(502, why + ". Upload the image file instead");
        mule_release  = m->mule;
        miner_release = m->miner;
        const bool mule_new  = updateAvailable(b.fw, m->mule.version);
        const bool miner_new = updateAvailable(b.miner_fw, m->miner.version);
        if (target == "mule" && !mule_new)
            return refuse(409, "the mule already runs " + b.fw + "; the manifest has " + m->mule.version);
        if (target == "miner" && !miner_new)
            return refuse(409, "the miner already runs " + b.miner_fw + "; the manifest has " + m->miner.version);
        if (target == "both" && !mule_new && !miner_new)
            return refuse(409, "the unit already runs mule " + b.fw + " and miner " + b.miner_fw);
    }

    j                  = std::make_shared<Job>();
    j->serial          = serial;
    j->target          = target;
    j->source          = image.empty() ? "manifest " + (target == "miner" ? miner_release.version : mule_release.version)
                                       : "file " + (image_name.empty() ? std::string("(unnamed)") : image_name);
    j->fw_before       = b.fw;
    j->miner_fw_before = b.miner_fw;
    j->started         = std::time(nullptr);
    j->running         = true;
    {
        std::lock_guard<std::mutex> lk(mu_);
        jobs_[serial] = j;
    }
    std::string mule_image, miner_image;
    if (!image.empty()) (target == "mule" ? mule_image : miner_image) = std::move(image);

    a.status      = 202;
    a.body["ok"]  = true;
    a.body["job"] = jobJson(*j);
    if (opt_.async) {
        worker_ = std::thread([this, j, mi = std::move(mule_image), ni = std::move(miner_image), mule_release,
                               miner_release]() mutable {
            run(j, std::move(mi), std::move(ni), mule_release, miner_release);
        });
    } else {
        run(j, std::move(mule_image), std::move(miner_image), mule_release, miner_release);
        a.body["job"] = job(serial);
    }
    return a;
}

void FirmwareUpdater::note(Job& j, const std::string& what) {
    std::lock_guard<std::mutex> lk(mu_);
    j.log.push_back(what);
    std::cout << "[mule] " << j.serial << " update: " << what << std::endl;
}

bool FirmwareUpdater::fetchImage(Job& j, const Release& r, std::string& out) {
    {
        std::lock_guard<std::mutex> lk(mu_);
        j.phase = "downloading";
        j.sent = j.total = 0;
    }
    note(j, "downloading " + j.step + " " + r.version + " from " + r.url);
    const HttpReply reply = t_.get(r.url, kFetchTimeoutS);
    if (!reply.ok || reply.status != 200 || reply.body.empty()) {
        std::lock_guard<std::mutex> lk(mu_);
        j.phase = "failed";
        j.error = "the " + j.step + " image could not be downloaded: " +
                  (reply.ok ? "HTTP " + std::to_string(reply.status) : reply.error);
        return false;
    }
    out = reply.body;
    note(j, j.step + " image: " + std::to_string(out.size()) + " bytes");
    return true;
}

bool FirmwareUpdater::sendImage(Job& j, const std::string& target, const std::string& image) {
    // The unit's downtime is checked again right here: a night can open
    // between the button and this image, and between the two images.
    const MuleService::Brief b = units_.brief(j.serial);
    if (b.night_open) {
        std::lock_guard<std::mutex> lk(mu_);
        j.phase = "failed";
        j.error = "a night opened on the unit: the " + target + " image was not sent";
        return false;
    }
    if (!b.online || b.ip.empty()) {
        std::lock_guard<std::mutex> lk(mu_);
        j.phase = "failed";
        j.error = "the unit is not connected here: the " + target + " image was not sent";
        return false;
    }
    const std::string url = "http://" + b.ip + "/api/update/" + target;
    {
        std::lock_guard<std::mutex> lk(mu_);
        j.phase = "sending";
        j.step  = target;
        j.sent  = 0;
        j.total = image.size();
    }
    note(j, "sending the " + target + " image (" + std::to_string(image.size()) + " bytes) to " + url);
    Job* jp = &j;
    const HttpReply reply = t_.post(url, image, kPostTimeoutS, [this, jp](size_t sent, size_t total) {
        std::lock_guard<std::mutex> lk(mu_);
        jp->sent = sent;
        if (total) jp->total = total;
    });
    if (!reply.ok) {
        std::lock_guard<std::mutex> lk(mu_);
        j.phase = "failed";
        j.error = "the " + target + " image did not get through: " + reply.error;
        return false;
    }
    if (reply.status != 200) {
        std::lock_guard<std::mutex> lk(mu_);
        j.phase = "failed";
        j.error = "the unit refused the " + target + " image: HTTP " + std::to_string(reply.status) +
                  (reply.body.empty() ? "" : " " + reply.body.substr(0, 200));
        return false;
    }
    {
        std::lock_guard<std::mutex> lk(mu_);
        j.sent = j.total = image.size();
    }
    note(j, "the unit took the " + target + " image: " + reply.body.substr(0, 200));
    return true;
}

bool FirmwareUpdater::waitForReturn(Job& j, uint64_t hellos_before) {
    {
        std::lock_guard<std::mutex> lk(mu_);
        j.phase = "waiting";
    }
    note(j, "waiting for the unit to restart and come back here");
    const auto until = std::chrono::steady_clock::now() + opt_.reconnect_wait;
    while (std::chrono::steady_clock::now() < until) {
        const MuleService::Brief b = units_.brief(j.serial);
        if (b.hellos > hellos_before && b.online) {
            note(j, "the unit is back: mule " + b.fw + ", miner " + b.miner_fw);
            return true;
        }
        std::this_thread::sleep_for(opt_.poll);
    }
    std::lock_guard<std::mutex> lk(mu_);
    j.phase = "failed";
    j.error = "the unit has not come back within " + std::to_string(opt_.reconnect_wait.count()) +
              " s of taking the mule image";
    return false;
}

void FirmwareUpdater::run(std::shared_ptr<Job> jp, std::string mule_image, std::string miner_image,
                          Release mule_release, Release miner_release) {
    Job&       j        = *jp;
    const bool do_mule  = j.target == "mule" || j.target == "both";
    const bool do_miner = j.target == "miner" || j.target == "both";
    bool       ok       = true;

    // The mule first: it restarts after its image and must be back before
    // the miner's goes through it. On "both" from the manifest, an image
    // the unit already runs is not sent again.
    if (do_mule) {
        const MuleService::Brief before = units_.brief(j.serial);
        const bool skip = j.target == "both" && mule_image.empty() &&
                          !updateAvailable(before.fw, mule_release.version);
        if (skip) {
            note(j, "mule " + before.fw + " is current: not sent");
        } else {
            {
                std::lock_guard<std::mutex> lk(mu_);
                j.step = "mule";
            }
            ok = !mule_image.empty() || fetchImage(j, mule_release, mule_image);
            ok = ok && sendImage(j, "mule", mule_image);
            mule_image.clear();
            ok = ok && waitForReturn(j, before.hellos);
        }
    }
    if (ok && do_miner) {
        const MuleService::Brief before = units_.brief(j.serial);
        const bool skip = j.target == "both" && miner_image.empty() &&
                          !updateAvailable(before.miner_fw, miner_release.version);
        if (skip) {
            note(j, "miner " + before.miner_fw + " is current: not sent");
        } else {
            {
                std::lock_guard<std::mutex> lk(mu_);
                j.step = "miner";
            }
            ok = !miner_image.empty() || fetchImage(j, miner_release, miner_image);
            ok = ok && sendImage(j, "miner", miner_image);
            miner_image.clear();
            if (ok) note(j, "the mule is flashing the miner; its new version shows in the unit's next state");
        }
    }
    const MuleService::Brief after = units_.brief(j.serial);
    std::lock_guard<std::mutex> lk(mu_);
    if (ok) j.phase = "done";
    j.fw_after       = after.fw;
    j.miner_fw_after = after.miner_fw;
    j.finished       = std::time(nullptr);
    j.running        = false;
    std::cout << "[mule] " << j.serial << " update " << j.phase << (j.error.empty() ? "" : ": " + j.error)
              << std::endl;
}

}  // namespace hms_cpap::mule
