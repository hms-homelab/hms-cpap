#include "mule/MuleFirmware.h"

#include "sleepbridge/SleepBridgeService.h"   // inspectImage: an ESP32 image's own version

#include <curl/curl.h>

#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <fstream>
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
    t.post_json = [](const std::string& url, const std::string& json, int timeout_s) {
        HttpReply r;
        CURL*     c = curl_easy_init();
        if (!c) {
            r.error = "curl_easy_init failed";
            return r;
        }
        struct curl_slist* headers = nullptr;
        headers = curl_slist_append(headers, "Content-Type: application/json");
        headers = curl_slist_append(headers, "Accept: application/json");
        curl_easy_setopt(c, CURLOPT_URL, url.c_str());
        curl_easy_setopt(c, CURLOPT_HTTPHEADER, headers);
        curl_easy_setopt(c, CURLOPT_POST, 1L);
        curl_easy_setopt(c, CURLOPT_POSTFIELDS, json.data());
        curl_easy_setopt(c, CURLOPT_POSTFIELDSIZE_LARGE, static_cast<curl_off_t>(json.size()));
        curl_easy_setopt(c, CURLOPT_WRITEFUNCTION, collect);
        curl_easy_setopt(c, CURLOPT_WRITEDATA, &r.body);
        curl_easy_setopt(c, CURLOPT_TIMEOUT, static_cast<long>(timeout_s));
        curl_easy_setopt(c, CURLOPT_CONNECTTIMEOUT, 15L);
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

// ── the release and versions ──

namespace {

std::optional<Json::Value> parseJson(const std::string& text) {
    Json::Value             v;
    Json::CharReaderBuilder rb;
    std::istringstream      ss(text);
    if (!Json::parseFromStream(rb, ss, &v, nullptr)) return std::nullopt;
    return v;
}

// The path of a URL, its query dropped: "https://h/a/b?x" -> "/a/b".
std::string pathOf(const std::string& url) {
    std::string p = url.substr(0, url.find('?'));
    const auto  scheme = p.find("://");
    if (scheme != std::string::npos) {
        const auto slash = p.find('/', scheme + 3);
        p = slash == std::string::npos ? "/" : p.substr(slash);
    }
    return p;
}

}  // namespace

std::string absoluteUrl(const std::string& base, const std::string& path) {
    if (path.rfind("https://", 0) == 0 || path.rfind("http://", 0) == 0) return path;
    const auto scheme = base.find("://");
    if (scheme == std::string::npos) return path;
    const auto  slash  = base.find('/', scheme + 3);
    std::string origin = slash == std::string::npos ? base : base.substr(0, slash);
    return origin + (path.empty() || path[0] != '/' ? "/" : "") + path;
}

std::optional<Release> parseResolve(const std::string& text, long status, const std::string& role,
                                    const std::string& serial, const std::string& base, std::string* why) {
    auto refuse = [why](const std::string& w) -> std::optional<Release> {
        if (why) *why = w;
        return std::nullopt;
    };
    const auto v = parseJson(text);
    if (!v || !v->isObject()) return refuse("CpapDash's answer for the " + role + " is not JSON");
    const Json::Value& o = *v;
    if (status != 200 && status != 202) {
        const std::string e = o.get("error", "").asString();
        return refuse("CpapDash answered HTTP " + std::to_string(status) + " for the " + role +
                      (e.empty() ? "" : ": " + e));
    }
    if (o.get("role", "").asString() != role) return refuse("CpapDash answered for another role than the " + role);
    // The pairing code is the unit's: an answer for another serial means the
    // code kept here is not this unit's, and its image would be another's.
    if (o.get("serial", "").asString() != serial)
        return refuse("the pairing code kept for " + serial + " belongs to " +
                      (o.get("serial", "").asString().empty() ? std::string("no unit")
                                                              : o.get("serial", "").asString()));
    Release r;
    r.version = o.get("version", "").asString();
    if (r.version.empty() || !(r.version[0] >= '0' && r.version[0] <= '9'))
        return refuse("CpapDash named no " + role + " version");
    if (status == 202) {
        if (o.get("status", "").asString() != "baking") return refuse("CpapDash's 202 for the " + role + " is not a build");
        r.baking = true;
        return r;
    }
    const Json::Value& flash = o["flash"];
    if (flash.isArray())
        for (const auto& e : flash)
            if (e.get("offset", "").asString() == "0x20000" && e["url"].isString() && !e["url"].asString().empty())
                r.url = absoluteUrl(base, e["url"].asString());
    if (r.url.empty()) return refuse("CpapDash's " + role + " release has no application image");
    if (r.url.rfind("https://", 0) != 0) return refuse("the " + role + " image is not served over https");
    return r;
}

std::optional<std::string> minerVariantOf(const std::string& url, const std::string& version) {
    if (version.empty()) return std::nullopt;
    const std::string p    = pathOf(url);
    const std::string tail = "/" + version + "/miner-";
    const auto        at   = p.rfind(tail);
    if (at == std::string::npos) return std::nullopt;
    std::string name = p.substr(at + tail.size());   // "<v>.bin" or "<variant>-<v>.bin"
    const std::string end = version + ".bin";
    if (name.size() < end.size() || name.compare(name.size() - end.size(), end.size(), end) != 0) return std::nullopt;
    name.resize(name.size() - end.size());
    if (name.empty()) return std::string("official");
    if (name.back() != '-' || name.size() < 2) return std::nullopt;
    name.pop_back();
    if (name.find_first_of("/-") != std::string::npos) return std::nullopt;
    return name;
}

std::optional<std::string> fleetVariant(const std::string& text, const std::string& serial) {
    const auto v = parseJson(text);
    if (!v || !v->isArray()) return std::nullopt;
    for (const auto& e : *v)
        if (e.isObject() && e.get("serial", "").asString() == serial && e.get("role", "").asString() == "miner" &&
            e["variant"].isString() && !e["variant"].asString().empty())
            return e["variant"].asString();
    return std::nullopt;
}

bool checkReleaseImage(const std::string& image, const std::string& role, const std::string& version,
                       const std::string& serial, std::string* why) {
    auto refuse = [why](const std::string& w) {
        if (why) *why = w;
        return false;
    };
    if (image.empty()) return refuse("the " + role + " image is empty");
    if (image.size() > kImageMax) return refuse("the " + role + " image is larger than a Mule & Miner image can be");
    std::string bad;
    const auto  info = sleepbridge::inspectImage(image, &bad);
    if (!info) return refuse("the " + role + " download is " + bad);
    if (info->project != role)
        return refuse("the download is not a " + role + " image: it says it is \"" + info->project + "\"");
    if (info->version != version)
        return refuse("the " + role + " image is version " + info->version + ", not the released " + version);
    // The mule carries its identity: an image baked for another unit would
    // boot as that unit.
    if (role == "mule" && image.find(serial) == std::string::npos)
        return refuse("the mule image is not built for " + serial);
    return true;
}

bool validPairingCode(const std::string& code) {
    return code.size() == 6 && code.find_first_not_of("0123456789") == std::string::npos;
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
    // The pairing codes the owners gave, one per unit.
    if (!opt_.pairing_file.empty()) {
        std::ifstream in(opt_.pairing_file);
        std::stringstream ss;
        ss << in.rdbuf();
        if (const auto v = parseJson(ss.str()); v && v->isObject())
            for (const auto& serial : v->getMemberNames())
                if ((*v)[serial].isString() && validPairingCode((*v)[serial].asString()))
                    pairing_[serial] = (*v)[serial].asString();
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

std::string FirmwareUpdater::pairingCode(const std::string& serial) const {
    std::lock_guard<std::mutex> lk(mu_);
    const auto it = pairing_.find(serial);
    return it == pairing_.end() ? "" : it->second;
}

void FirmwareUpdater::savePairing() {
    if (opt_.pairing_file.empty()) return;
    Json::Value v(Json::objectValue);
    for (const auto& [serial, code] : pairing_) v[serial] = code;
    Json::StreamWriterBuilder w;
    w["indentation"] = "  ";
    std::error_code ec;
    const std::filesystem::path p(opt_.pairing_file);
    if (p.has_parent_path()) std::filesystem::create_directories(p.parent_path(), ec);
    const std::string tmp = opt_.pairing_file + ".tmp";
    {
        std::ofstream out(tmp, std::ios::trunc);
        out << Json::writeString(w, v);
        if (!out) {
            std::cerr << "[mule] the pairing codes could not be written to " << tmp << std::endl;
            return;
        }
    }
    std::filesystem::rename(tmp, opt_.pairing_file, ec);
    if (ec) std::cerr << "[mule] the pairing codes could not be kept: " << ec.message() << std::endl;
}

std::optional<Release> FirmwareUpdater::resolve(const std::string& serial, const std::string& code,
                                                const std::string& role, std::string* why) {
    auto refuse = [why](const std::string& w) -> std::optional<Release> {
        if (why) *why = w;
        return std::nullopt;
    };
    if (!t_.post_json) return refuse("no way to reach CpapDash");
    Json::Value body(Json::objectValue);
    body["pairing_code"] = code;
    body["role"]         = role;
    Json::StreamWriterBuilder w;
    w["indentation"]    = "";
    const std::string url = opt_.release_base + "/v1/flasher/resolve";
    const HttpReply   r   = t_.post_json(url, Json::writeString(w, body), kResolveTimeoutS);
    if (!r.ok) return refuse("CpapDash could not be reached: " + r.error);
    if (r.status == 404) {
        const auto v = parseJson(r.body);
        const std::string e = v && v->isObject() ? v->get("error", "").asString() : "";
        return refuse(e.find("pairing") != std::string::npos
                          ? "CpapDash does not recognise the pairing code given for " + serial
                          : "CpapDash has no " + role + " release" + (e.empty() ? "" : " (" + e + ")"));
    }
    if (r.status == 429) return refuse("CpapDash asked to wait: too many checks from this network; try again later");
    return parseResolve(r.body, r.status, role, serial, opt_.release_base, why);
}

std::optional<Manifest> FirmwareUpdater::manifest(const std::string& serial, std::string* why, bool fresh) {
    if (!fresh) {
        std::lock_guard<std::mutex> lk(mu_);
        const auto it = cached_.find(serial);
        if (it != cached_.end() && it->second.wall &&
            std::chrono::steady_clock::now() - it->second.at < opt_.manifest_ttl) {
            if (why) *why = it->second.error;
            return it->second.m;
        }
    }
    std::string             err;
    std::optional<Manifest> m;
    const std::string       code = pairingCode(serial);
    if (code.empty()) {
        err = "no pairing code is kept for this unit: give the one printed on it to check for releases";
    } else {
        // The miner first: it builds nothing, and a refused code stops here.
        auto miner = resolve(serial, code, "miner", &err);
        std::optional<Release> mule;
        if (miner) mule = resolve(serial, code, "mule", &err);
        if (miner && mule) m = Manifest{*mule, *miner};
    }
    std::lock_guard<std::mutex> lk(mu_);
    auto& c = cached_[serial];
    c.m     = m;
    c.error = err;
    c.at    = std::chrono::steady_clock::now();
    c.wall  = std::time(nullptr);
    if (why) *why = err;
    return m;
}

CommandAnswer FirmwareUpdater::setPairingCode(const std::string& serial, const std::string& raw) {
    CommandAnswer a;
    auto refuse = [&a](int status, const std::string& why) {
        a.status        = status;
        a.body["ok"]    = false;
        a.body["error"] = why;
        return a;
    };
    std::string code;
    for (const char ch : raw)
        if (ch != ' ' && ch != '-') code += ch;   // "190 364" as printed
    if (code.empty()) {
        std::lock_guard<std::mutex> lk(mu_);
        pairing_.erase(serial);
        cached_.erase(serial);
        savePairing();
        a.status       = 200;
        a.body["ok"]   = true;
        a.body["kept"] = false;
        return a;
    }
    if (!validPairingCode(code)) return refuse(400, "a pairing code is the six digits printed on the unit");
    // CpapDash confirms it is this unit's code; the miner resolve builds nothing.
    std::string why;
    const auto  miner = resolve(serial, code, "miner", &why);
    if (!miner) {
        const bool unreachable = why.rfind("CpapDash could not be reached", 0) == 0 ||
                                 why.rfind("CpapDash asked to wait", 0) == 0 || why.find("HTTP 5") != std::string::npos;
        return refuse(unreachable ? 502 : 409, why);
    }
    {
        std::lock_guard<std::mutex> lk(mu_);
        pairing_[serial] = code;
        cached_.erase(serial);   // the next check reads the release with it
        savePairing();
    }
    std::cout << "[mule] " << serial << ": pairing code kept, CpapDash's miner release " << miner->version << std::endl;
    a.status       = 200;
    a.body["ok"]   = true;
    a.body["kept"] = true;
    return a;
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
    if (read_manifest && b.known) {
        m = manifest(serial, &why);
    } else {
        std::lock_guard<std::mutex> lk(mu_);
        const auto it = cached_.find(serial);
        if (it != cached_.end() && it->second.wall) {
            m   = it->second.m;
            why = it->second.error;
        } else {
            why = "the release has not been read yet";
        }
    }
    const bool  paired = !pairingCode(serial).empty();
    Json::Value man(Json::objectValue);
    man["url"]   = opt_.release_base + "/v1/flasher/resolve";
    man["error"] = why;
    {
        std::lock_guard<std::mutex> lk(mu_);
        const auto it  = cached_.find(serial);
        man["read_at"] = static_cast<Json::Int64>(it == cached_.end() ? 0 : it->second.wall);
    }
    v["manifest"]   = man;
    v["pairing_set"] = paired;
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
        if (pairingCode(serial).empty())
            return refuse(409, "no pairing code is kept for this unit: give the one printed on it, or upload "
                               "the image file instead");
        // Fresh: the image URLs are signed for an hour, and a mule being
        // built a minute ago may be ready now.
        std::string why;
        const auto  m = manifest(serial, &why, /*fresh=*/true);
        if (!m) return refuse(502, why + ". Upload the image file instead");
        mule_release  = m->mule;
        miner_release = m->miner;
        const bool mule_new  = updateAvailable(b.fw, m->mule.version);
        const bool miner_new = updateAvailable(b.miner_fw, m->miner.version);
        if (target == "mule" && !mule_new)
            return refuse(409, "the mule already runs " + b.fw + "; the latest release is " + m->mule.version);
        if (target == "miner" && !miner_new)
            return refuse(409, "the miner already runs " + b.miner_fw + "; the latest release is " + m->miner.version);
        if (target == "both" && !mule_new && !miner_new)
            return refuse(409, "the unit already runs mule " + b.fw + " and miner " + b.miner_fw);
        // The mule image is built for this unit on demand; this read asked
        // for it. Nothing is sent until it is there.
        if (mule_release.baking && (target == "mule" || (target == "both" && mule_new)))
            return refuse(503, "CpapDash is building this unit's mule " + mule_release.version +
                                   " image; press Update again in a few seconds");
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
    o.source         = image.empty() ? "release " + (target == "miner" ? miner_release.version : mule_release.version)
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
        // On "both" from the release, an image the unit already runs is
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
    const std::string serial = j.ota.serial;
    if (r.url.empty()) {
        std::lock_guard<std::mutex> lk(mu_);
        fail(j, "image_unavailable", "CpapDash has not finished building the " + target + " image; try again");
        return false;
    }
    // The miner image is per SPI pinout, and the wrong one kills the bus for
    // good: the image CpapDash chose must be the variant its public fleet map
    // lists for this unit, or nothing is sent.
    if (target == "miner") {
        const auto chosen = minerVariantOf(r.url, r.version);
        const HttpReply map = t_.get(opt_.release_base + "/v1/flasher/fleet-map", kResolveTimeoutS);
        const auto listed = map.ok && map.status == 200 ? fleetVariant(map.body, serial) : std::nullopt;
        std::string bad;
        if (!chosen)
            bad = "the miner image's name does not say which pinout it is for";
        else if (!map.ok || map.status != 200)
            bad = "the fleet map could not be read to confirm this unit's pinout (" +
                  (map.ok ? "HTTP " + std::to_string(map.status) : map.error) + ")";
        else if (!listed)
            bad = "the fleet map does not list " + serial + ", so its pinout cannot be confirmed";
        else if (*listed != *chosen)
            bad = "the miner image is for the " + *chosen + " pinout, but " + serial + " is " + *listed;
        if (!bad.empty()) {
            std::lock_guard<std::mutex> lk(mu_);
            fail(j, "image_rejected", bad + ": nothing was sent");
            return false;
        }
        note(j, "miner pinout " + *chosen + ", as the fleet map lists " + serial);
    }
    // The signature in the URL is not logged.
    note(j, "downloading " + target + " " + r.version + " from " + r.url.substr(0, r.url.find('?')));
    const HttpReply reply = t_.get(r.url, kFetchTimeoutS);
    if (!reply.ok || reply.status != 200 || reply.body.empty()) {
        std::lock_guard<std::mutex> lk(mu_);
        fail(j, "image_unavailable",
             "the " + target + " image could not be downloaded: " +
                 (reply.ok ? "HTTP " + std::to_string(reply.status) : reply.error));
        return false;
    }
    std::string why;
    if (!checkReleaseImage(reply.body, target, r.version, serial, &why)) {
        std::lock_guard<std::mutex> lk(mu_);
        fail(j, "image_rejected", why + ": nothing was sent");
        return false;
    }
    out = reply.body;
    note(j, target + " image: " + std::to_string(out.size()) + " bytes, " + target + " " + r.version + " checked");
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
