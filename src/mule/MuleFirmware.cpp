#include "mule/MuleFirmware.h"

#include "sleepbridge/SleepBridgeService.h"   // inspectImage: an ESP32 image's own version

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
    : units_(units), t_(std::move(transport)), opt_(std::move(opt)) {
    const std::time_t now = std::time(nullptr);
    next_job_ = static_cast<uint64_t>(now > 978307200 ? now - 978307200 : 0) * 10;
    // The jobs kept before this start: one cut before the unit took its
    // image failed (interrupted); one restarting waits for the unit's hello.
    for (auto& [serial, ota] : devices::otaLoadAll(opt_.jobs_dir)) {
        const bool was_ended = ota.ended();
        devices::otaAfterRestart(ota, now);
        auto j    = std::make_shared<Job>();
        j->ota    = ota;
        j->target = ota.steps.size() == 2 ? "both" : (ota.steps.empty() ? "mule" : ota.steps[0].target);
        // Its mule step, restarting: any hello from here on is its return.
        j->hellos_before = 0;
        jobs_[serial]    = j;
        if (!was_ended && ota.ended()) devices::otaSave(opt_.jobs_dir, serial, ota);
    }
    units_.setObserver([this](const std::string& serial, bool hello) { onUnit(serial, hello); });
}

FirmwareUpdater::FirmwareUpdater(MuleService& units, Transport transport)
    : FirmwareUpdater(units, std::move(transport), Options{}) {}

FirmwareUpdater::~FirmwareUpdater() {
    units_.setObserver({});
    waitIdle();
}

void FirmwareUpdater::waitIdle() {
    for (;;) {
        std::vector<std::thread> ws;
        {
            std::lock_guard<std::mutex> lk(workers_mu_);
            ws.swap(workers_);
        }
        if (ws.empty()) return;
        for (auto& w : ws)
            if (w.joinable()) w.join();
    }
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

// The shape scripts read before the job object, kept one release beside it.
Json::Value FirmwareUpdater::jobJson(const Job& j) {
    const devices::OtaJob&  o = j.ota;
    const devices::OtaStep* s = o.current();
    const std::string       p = o.phase();
    std::string legacy = p == "preparing"                         ? "downloading"
                         : (p == "transferring" || p == "verifying") ? "sending"
                         : p == "restarting"                        ? "waiting"
                                                                    : p;
    // A miner step restarting means the mule took and flashed it: done, as
    // the old shape said.
    if (p == "restarting" && s && s->target == "miner") legacy = "done";
    auto version = [&](const std::string& target, bool after) -> std::string {
        for (const auto& st : o.steps)
            if (st.target == target) return after ? st.version_after : st.version_before;
        return "";
    };
    Json::Value v(Json::objectValue);
    v["target"]          = j.target;
    v["source"]          = o.source;
    v["phase"]           = legacy;
    v["step"]            = s ? s->target : "";
    v["sent"]            = static_cast<Json::UInt64>(s ? s->bytes : 0);
    v["total"]           = static_cast<Json::UInt64>(s ? s->total : 0);
    v["error"]           = o.detail.empty() ? o.reason : o.detail;
    v["fw_before"]       = version("mule", false);
    v["miner_fw_before"] = version("miner", false);
    v["fw_after"]        = version("mule", true);
    v["miner_fw_after"]  = version("miner", true);
    v["started"]         = static_cast<Json::Int64>(o.started_at);
    v["finished"]        = static_cast<Json::Int64>(o.ended_at);
    v["running"]         = !o.ended();
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

Json::Value FirmwareUpdater::ota(const std::string& serial) const {
    std::lock_guard<std::mutex> lk(mu_);
    const auto it = jobs_.find(serial);
    return it == jobs_.end() ? Json::Value(Json::nullValue) : devices::otaJson(it->second->ota);
}

void FirmwareUpdater::changed(Job& j) {
    j.ota.updated_at = std::time(nullptr);
    if (j.ota.ended() && !j.ota.ended_at) j.ota.ended_at = j.ota.updated_at;
    devices::otaSave(opt_.jobs_dir, j.ota.serial, j.ota);
}

void FirmwareUpdater::fail(Job& j, const std::string& reason, const std::string& detail) {
    if (auto* s = j.ota.current()) s->phase = "failed";
    j.ota.reason = reason;
    j.ota.detail = detail;
    changed(j);
    j.log.push_back("failed: " + reason + ", " + detail);
    std::cout << "[mule] " << j.ota.serial << " update failed: " << reason << ", " << detail << std::endl;
}

void FirmwareUpdater::advance(Job& j) {
    while (j.ota.current() && (j.ota.current()->phase == "done" || j.ota.current()->phase == "skipped"))
        ++j.ota.step;
    if (!j.ota.current()) {
        // Past the last step: the job reads as its last step's outcome.
        j.ota.step = j.ota.steps.empty() ? 0 : j.ota.steps.size() - 1;
        if (!j.ota.steps.empty() && j.ota.steps.back().phase == "skipped") j.ota.steps.back().phase = "done";
    }
    changed(j);
}

Json::Value FirmwareUpdater::firmware(const std::string& serial, bool read_manifest) {
    const MuleService::Brief b = units_.brief(serial);
    Json::Value v(Json::objectValue);
    v["serial"]     = serial;
    v["known"]      = b.known;
    v["online"]     = b.online;
    v["ip"]         = b.ip;
    v["fw"]         = b.fw;
    v["miner_fw"]   = b.miner_fw;
    v["night_open"] = b.night_open;

    std::string             why;
    std::optional<Manifest> m;
    if (read_manifest) {
        m = manifest(&why);
    } else {
        std::lock_guard<std::mutex> lk(mu_);
        m   = cached_;
        why = cached_wall_ ? cached_error_ : "the manifest has not been read yet";
    }
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
    v["ota"]              = ota(serial);
    const devices::Recording rec = units_.recording(serial);
    v["recording"]        = rec.state;
    v["recording_reason"] = rec.reason;
    return v;
}

CommandAnswer FirmwareUpdater::start(const std::string& serial, const std::string& target, std::string image,
                                     const std::string& image_name, bool confirm) {
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
    if (!b.online || b.ip.empty()) return refuse(409, "the unit is not connected here right now");

    std::shared_ptr<Job> j;
    {
        std::lock_guard<std::mutex> lk(mu_);
        for (const auto& [s, other] : jobs_) {
            if (other->ota.ended()) continue;
            const std::string p = other->ota.phase();
            if (s == serial)
                return refuse(409, "the last update to this unit has not ended (" + p +
                                       "); it ends when the unit is heard from again");
            if (p == "preparing" || p == "transferring" || p == "verifying")
                return refuse(409, "an update is already running on " + s);
        }
    }

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

    // While the machine records: the warning, and nothing sent unless the
    // person confirmed it (SDD-062 D4).
    const devices::Recording rec = units_.recording(serial);
    if (rec.warns() && !confirm) {
        a.status = 409;
        a.body   = devices::recordingWarning(rec);
        return a;
    }

    const std::time_t now = std::time(nullptr);
    j                = std::make_shared<Job>();
    j->target        = target;
    j->mule_release  = mule_release;
    j->miner_release = miner_release;
    j->hellos_before = b.hellos;
    auto& o          = j->ota;
    o.serial         = serial;
    o.kind           = "mule_miner";
    o.source         = image.empty() ? "manifest " + (target == "miner" ? miner_release.version : mule_release.version)
                                     : "file " + (image_name.empty() ? std::string("(unnamed)") : image_name);
    o.confirmed_while_recording = rec.warns() && confirm;
    o.started_at = o.updated_at = now;
    o.last_heard = b.last_heard;
    // The version an owner's file carries, read from its app descriptor.
    std::string file_version;
    if (!image.empty())
        if (const auto info = sleepbridge::inspectImage(image, nullptr)) file_version = info->version;
    auto addStep = [&](const std::string& t) {
        devices::OtaStep s;
        s.target         = t;
        s.version_before = t == "mule" ? b.fw : b.miner_fw;
        s.version_target = !image.empty() ? file_version : (t == "mule" ? mule_release.version : miner_release.version);
        // On "both" from the manifest, an image the unit already runs is
        // not sent again.
        if (target == "both" && image.empty() &&
            !updateAvailable(s.version_before, s.version_target)) {
            s.phase         = "skipped";
            s.version_after = s.version_before;
        }
        o.steps.push_back(s);
    };
    if (target == "mule" || target == "both") addStep("mule");
    if (target == "miner" || target == "both") addStep("miner");
    if (!image.empty()) (target == "mule" ? j->mule_image : j->miner_image) = std::move(image);
    {
        std::lock_guard<std::mutex> lk(mu_);
        o.job_id      = std::to_string(++next_job_);
        jobs_[serial] = j;
        for (const auto& s : o.steps)
            if (s.phase == "skipped") j->log.push_back(s.target + " " + s.version_before + " is current: not sent");
        advance(*j);
    }

    a.status = 202;
    a.body["ok"] = true;
    launch(j);
    a.body["job"] = job(serial);
    a.body["ota"] = ota(serial);
    return a;
}

void FirmwareUpdater::launch(std::shared_ptr<Job> j) {
    if (!opt_.async) return runStep(std::move(j));
    std::lock_guard<std::mutex> lk(workers_mu_);
    workers_.emplace_back([this, j] { runStep(j); });
}

void FirmwareUpdater::onUnit(const std::string& serial, bool hello) {
    const MuleService::Brief b = units_.brief(serial);
    std::shared_ptr<Job> next;
    {
        std::lock_guard<std::mutex> lk(mu_);
        const auto it = jobs_.find(serial);
        if (it == jobs_.end()) return;
        Job& j = *it->second;
        if (j.ota.ended()) return;
        j.ota.last_heard = b.last_heard;
        devices::OtaStep* s = j.ota.current();
        if (!s) return;
        if (s->target == "mule" && hello && b.hellos > j.hellos_before &&
            (s->phase == "restarting" || s->phase == "verifying")) {
            // The mule is back: what it runs is the outcome.
            s->version_after = b.fw;
            const bool old = !s->version_target.empty() && s->version_target != s->version_before &&
                             b.fw == s->version_before;
            if (old) {
                fail(j, "back_on_old_version", "the unit came back on mule " + b.fw);
                return;
            }
            s->phase = "done";
            j.log.push_back("the unit is back: mule " + b.fw + ", miner " + b.miner_fw);
            std::cout << "[mule] " << serial << " update: the unit is back on mule " << b.fw << std::endl;
            advance(j);
            if (!j.ota.ended()) next = it->second;   // the miner's step, now
        } else if (s->target == "miner" && s->phase == "restarting") {
            // The miner runs what the unit's state says. A version that moved
            // is the outcome; the old one, on a hello of the mule, means the
            // new image did not take.
            const bool moved = !b.miner_fw.empty() && b.miner_fw != s->version_before;
            const bool same_image = !s->version_target.empty() && s->version_target == s->version_before;
            if (moved || same_image) {
                s->version_after = b.miner_fw;
                s->phase         = "done";
                j.log.push_back("the miner runs " + b.miner_fw);
                std::cout << "[mule] " << serial << " update: the miner runs " << b.miner_fw << std::endl;
                advance(j);
            } else if (hello && !s->version_target.empty() && b.miner_fw == s->version_before) {
                s->version_after = b.miner_fw;
                fail(j, "back_on_old_version", "the miner still runs " + b.miner_fw);
            } else {
                j.ota.updated_at = std::time(nullptr);
            }
        }
    }
    if (next) launch(next);
}

void FirmwareUpdater::note(Job& j, const std::string& what) {
    std::lock_guard<std::mutex> lk(mu_);
    j.log.push_back(what);
    std::cout << "[mule] " << j.ota.serial << " update: " << what << std::endl;
}

bool FirmwareUpdater::fetchImage(Job& j, const Release& r, std::string& out) {
    std::string target;
    {
        std::lock_guard<std::mutex> lk(mu_);
        auto* s = j.ota.current();
        target   = s->target;
        s->phase = "preparing";
        s->bytes = s->total = 0;
        changed(j);
    }
    note(j, "downloading " + target + " " + r.version + " from " + r.url);
    const HttpReply reply = t_.get(r.url, kFetchTimeoutS);
    if (!reply.ok || reply.status != 200 || reply.body.empty()) {
        std::lock_guard<std::mutex> lk(mu_);
        fail(j, "image_unavailable",
             "the " + target + " image could not be downloaded: " +
                 (reply.ok ? "HTTP " + std::to_string(reply.status) : reply.error));
        return false;
    }
    out = reply.body;
    note(j, target + " image: " + std::to_string(out.size()) + " bytes");
    return true;
}

bool FirmwareUpdater::sendImage(Job& j, const std::string& target, const std::string& image) {
    const MuleService::Brief b = units_.brief(j.ota.serial);
    if (!b.online || b.ip.empty()) {
        std::lock_guard<std::mutex> lk(mu_);
        fail(j, "offline", "the unit is not connected here: the " + target + " image was not sent");
        return false;
    }
    const std::string url = "http://" + b.ip + "/api/update/" + target;
    size_t idx = 0;   // this step, whatever the job's current step becomes meanwhile
    {
        std::lock_guard<std::mutex> lk(mu_);
        idx      = j.ota.step;
        auto* s  = &j.ota.steps[idx];
        s->phase = "transferring";
        s->bytes = 0;
        s->total = image.size();
        if (target == "mule") j.hellos_before = b.hellos;
        changed(j);
    }
    note(j, "sending the " + target + " image (" + std::to_string(image.size()) + " bytes) to " + url);
    Job* jp = &j;
    const HttpReply reply = t_.post(url, image, kPostTimeoutS, [this, jp, idx](size_t sent, size_t total) {
        std::lock_guard<std::mutex> lk(mu_);
        auto* s = &jp->ota.steps[idx];
        if (s->phase != "transferring") return;
        s->bytes = sent;
        if (total) s->total = total;
        jp->ota.updated_at = std::time(nullptr);
        // Every byte went: the unit is writing and checking it.
        if (s->total && s->bytes >= s->total) {
            s->phase = "verifying";
            changed(*jp);
        }
    });
    std::lock_guard<std::mutex> lk(mu_);
    auto* s = &j.ota.steps[idx];
    // The unit's own return (its hello) may have ended this step already.
    const bool open = s->phase == "transferring" || s->phase == "verifying";
    if (!reply.ok) {
        if (open) fail(j, "connection_lost", "the " + target + " image did not get through: " + reply.error);
        return false;
    }
    if (reply.status != 200) {
        if (open)
            fail(j, "refused_image",
                 "HTTP " + std::to_string(reply.status) + (reply.body.empty() ? "" : " " + reply.body.substr(0, 200)));
        return false;
    }
    if (open) s->bytes = s->total = image.size();
    // The mule restarts into its image and comes back with a hello; the mule
    // flashed the miner inside this request, and the miner's new version
    // comes in the unit's next state. A hello that came first already ended
    // the step.
    if (open) s->phase = "restarting";
    j.log.push_back("the unit took the " + target + " image: " + reply.body.substr(0, 200));
    std::cout << "[mule] " << j.ota.serial << " update: the unit took the " << target << " image" << std::endl;
    changed(j);
    return true;
}

void FirmwareUpdater::runStep(std::shared_ptr<Job> jp) {
    Job&        j = *jp;
    std::string target, image;
    Release     release;
    {
        std::lock_guard<std::mutex> lk(mu_);
        if (j.ota.ended()) return;
        const auto* s = j.ota.current();
        if (!s || s->phase != "queued") return;
        target  = s->target;
        image   = std::move(target == "mule" ? j.mule_image : j.miner_image);
        release = target == "mule" ? j.mule_release : j.miner_release;
    }
    if (image.empty() && !fetchImage(j, release, image)) return;
    sendImage(j, target, image);
}

}  // namespace hms_cpap::mule
