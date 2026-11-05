#include "sleepbridge/SleepBridgeService.h"

#include "mule/MuleFirmware.h"   // compareVersions, updateAvailable

#include <openssl/evp.h>

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>

namespace hms_cpap::sleepbridge {

namespace fs = std::filesystem;

namespace {

/// Seconds on the steady clock: the night rule and the update timeout are
/// measured on it, since std::time_t only counts whole seconds.
double monoNow() {
    return std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count();
}

std::string sha256Hex(const std::string& bytes) {
    unsigned char md[EVP_MAX_MD_SIZE];
    unsigned int  n = 0;
    EVP_MD_CTX*   ctx = EVP_MD_CTX_new();
    EVP_DigestInit_ex(ctx, EVP_sha256(), nullptr);
    EVP_DigestUpdate(ctx, bytes.data(), bytes.size());
    EVP_DigestFinal_ex(ctx, md, &n);
    EVP_MD_CTX_free(ctx);
    static const char* hex = "0123456789abcdef";
    std::string out;
    out.reserve(2 * n);
    for (unsigned int i = 0; i < n; i++) {
        out.push_back(hex[md[i] >> 4]);
        out.push_back(hex[md[i] & 0xF]);
    }
    return out;
}

/// A fixed-size, NUL-padded field of the app descriptor.
std::string field(const std::string& s, size_t at, size_t n) {
    const std::string raw = s.substr(at, n);
    return raw.substr(0, raw.find('\0'));
}

Json::Value stepJson(const StepReport& r) {
    Json::Value v(Json::objectValue);
    v["batches_fed"]     = r.batches_fed;
    v["records_fed"]     = r.records_fed;
    v["records_skipped"] = r.records_skipped;
    v["cards_fed"]       = r.cards_fed;
    v["learned_fed"]     = r.learned_fed;
    v["written"]         = r.written;
    v["removed"]         = r.removed;
    v["refused_paths"]   = r.refused_paths;
    v["unreadable"]      = r.unreadable;
    v["holding"]         = r.holding;
    v["holding_reason"]  = r.holding_reason;
    v["held_records"]    = static_cast<Json::UInt64>(r.held_records);
    v["held_files"]      = static_cast<Json::UInt64>(r.held_files);
    v["complete_files"]  = static_cast<Json::UInt64>(r.complete_files);
    // The card's layout the rebuild trusts, and how it was confirmed: what
    // batches/geometry.json keeps across a restart.
    if (r.geometry.valid()) {
        Json::Value g(Json::objectValue);
        g["layout"] = std::to_string(r.geometry.sectors_per_cluster) + ":" +
                      std::to_string(r.geometry.data_begin_lba);
        g["sectors_per_cluster"] = r.geometry.sectors_per_cluster;
        g["data_begin_lba"]      = r.geometry.data_begin_lba;
        g["root_cluster"]        = r.geometry.root_cluster;
        g["fat_begin_lba"]       = r.geometry.fat_begin_lba;
        g["fat_sectors"]         = r.geometry.fat_sectors;
        g["fats"]                = r.geometry.fats;
        g["cluster_count"]       = r.geometry.cluster_count;
        g["cid"]                 = r.geometry.cid;
        g["origin"]              = r.geometry.origin;
        g["confirmed_by"]        = r.geometry.confirmed_by;
        g["session"]             = r.geometry.session;
        g["since"]               = static_cast<Json::UInt64>(r.geometry.since);
        v["geometry"]            = g;
    }
    return v;
}

}  // namespace

bool validBridgeHost(const std::string& host) {
    if (host.empty()) return true;   // the cloud
    if (host.size() > 253 || host.front() == '.' || host.front() == '-' || host.back() == '.' ||
        host.back() == '-')
        return false;
    for (const char ch : host) {
        const bool ok = (ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') ||
                        (ch >= '0' && ch <= '9') || ch == '.' || ch == '-';
        if (!ok) return false;   // no ':', '/', space: a port, scheme or path
    }
    return host.find("..") == std::string::npos;
}

bool validBridgePort(int port) { return port >= 1 && port <= 65535; }

std::string configShapeError(const Json::Value& req) {
    const bool get = req.isMember("get");
    const bool set = req.isMember("set");
    if (get == set) return "config takes {\"get\": true} or {\"set\": {key: value, ...}}, one of the two";
    if (get) return req["get"].isBool() && req["get"].asBool() ? "" : "get is true";
    const Json::Value& s = req["set"];
    if (!s.isObject() || s.empty()) return "set is an object of one or more keys";
    if (s.size() > 16) return "set takes at most 16 keys at a time";
    for (const auto& k : s.getMemberNames()) {
        if (k == "stream_url" || k == "stream_port")
            return k + " is the stream command's: use stream to move the bridge";
        if (k.empty() || k.size() > 32 ||
            !std::all_of(k.begin(), k.end(), [](char c) { return (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_'; }))
            return "\"" + k + "\" is not a setting's name";
        const Json::Value& v = s[k];
        if (v.isString()) {
            if (v.asString().size() > 64) return k + " is at most 64 characters";
        } else if (v.isBool()) {
        } else if (v.isIntegral()) {
            // A JSON number holds an integer exactly up to 2^53.
            if (v.isInt64() && (v.asInt64() > (int64_t{1} << 53) || v.asInt64() < -(int64_t{1} << 53)))
                return k + " is out of range";
            if (!v.isInt64()) return k + " is out of range";
        } else {
            return k + " is a string, true or false, or a whole number";
        }
    }
    return "";
}

bool configTouchesNetwork(const Json::Value& req) {
    const Json::Value& s = req["set"];
    return s.isObject() && (s.isMember("ssid") || s.isMember("pass"));
}

// An ESP32 application image: a 24-byte image header whose first byte is
// 0xE9, an 8-byte segment header, then the app descriptor in the first
// segment: magic 0xABCD5432 (little-endian), secure_version(4), reserved(8),
// version[32], project_name[32], time[16], date[16], ...
std::optional<ImageInfo> inspectImage(const std::string& image, std::string* why) {
    constexpr size_t kDesc = 24 + 8;
    auto fail = [&](const char* w) -> std::optional<ImageInfo> {
        if (why) *why = w;
        return std::nullopt;
    };
    if (image.size() < kDesc + 256) return fail("too short to be a firmware image");
    if (static_cast<uint8_t>(image[0]) != 0xE9) return fail("not an ESP32 application image (no image magic)");
    const auto* d = reinterpret_cast<const uint8_t*>(image.data()) + kDesc;
    const uint32_t magic = d[0] | (d[1] << 8) | (d[2] << 16) | (static_cast<uint32_t>(d[3]) << 24);
    if (magic != 0xABCD5432u) return fail("not an ESP32 application image (no app descriptor)");
    ImageInfo info;
    info.version = field(image, kDesc + 16, 32);
    info.project = field(image, kDesc + 48, 32);
    const std::string t = field(image, kDesc + 80, 16), dt = field(image, kDesc + 96, 16);
    info.built = dt + (dt.empty() || t.empty() ? "" : " ") + t;
    return info;
}

std::string otaCommandFrame(uint64_t req_id, size_t size, const std::string& sha256_hex) {
    Json::Value f(Json::objectValue);
    f["t"]      = "cmd";
    f["cmd"]    = "ota";
    f["req_id"] = static_cast<Json::UInt64>(req_id);
    f["target"] = "tapper";
    f["size"]   = static_cast<Json::UInt64>(size);
    f["sha256"] = sha256_hex;
    Json::StreamWriterBuilder w;
    w["indentation"] = "";
    return Json::writeString(w, f);
}

std::string firmwareChunkFrame(uint64_t req_id, uint32_t seq, bool last, const std::string& body) {
    FrameHeader h;
    h.req_id = static_cast<uint32_t>(req_id);
    h.seq    = seq;
    h.flags  = last ? 1 : 0;
    h.kind   = kKindFirmware;
    return encodeFrameHeader(h) + body;
}

SleepBridgeService::SleepBridgeService(Options opt, RebuilderFactory factory)
    : opt_(std::move(opt)), factory_(std::move(factory)), store_(opt_.root) {
    // Asked once: a factory that answers nullptr is a build without the rebuild.
    if (factory_) available_ = factory_(RebuildStart{}) != nullptr;
    // Seconds since 2001: rises across restarts, below 2^31 until 2069.
    const std::time_t now = std::time(nullptr);
    next_req_ = static_cast<uint64_t>(now > 978307200 ? now - 978307200 : 0) + 1;
    // The update jobs kept before this start: one cut by the restart before
    // the bridge took the image failed; one restarting waits for its hello.
    for (auto& [sender, job] : devices::otaLoadAll(opt_.jobs_dir)) {
        const bool was_ended = job.ended();
        devices::otaAfterRestart(job, now);
        UpdateJob u;
        u.job    = job;
        u.req_id = std::strtoull(job.job_id.c_str(), nullptr, 10);
        bridges_[sender].update = u;
        if (!was_ended && job.ended()) {
            devices::otaSave(opt_.jobs_dir, sender, job);
            std::cout << "[sleepbridge] " << sender << " update was cut by a restart: failed, interrupted"
                      << std::endl;
        }
    }
}

SleepBridgeService::~SleepBridgeService() { stop(); }

void SleepBridgeService::start() {
    if (!available_ || thread_.joinable()) return;
    {
        std::lock_guard<std::mutex> lk(q_mu_);
        stop_ = false;
        for (const auto& s : store_.senders()) dirty_.insert(s);
        ++events_;
    }
    thread_ = std::thread([this] { loop(); });
}

void SleepBridgeService::stop() {
    {
        std::lock_guard<std::mutex> lk(q_mu_);
        stop_ = true;
    }
    q_cv_.notify_all();
    if (thread_.joinable()) thread_.join();
    waitForUpdateSend();
}

void SleepBridgeService::wake(const std::string& sender) {
    {
        std::lock_guard<std::mutex> lk(q_mu_);
        dirty_.insert(sender);
        ++events_;
    }
    q_cv_.notify_all();
}

void SleepBridgeService::loop() {
    const auto quiet = std::chrono::milliseconds(static_cast<int64_t>(opt_.quiet_s * 1000));
    const auto most  = std::chrono::milliseconds(static_cast<int64_t>(opt_.most_s * 1000));
    const auto tick  = std::chrono::milliseconds(static_cast<int64_t>(opt_.tick_s * 1000));
    std::unique_lock<std::mutex> lk(q_mu_);
    while (!stop_) {
        if (opt_.on_tick) {
            if (!q_cv_.wait_for(lk, tick, [&] { return stop_ || !dirty_.empty(); })) {
                lk.unlock();
                opt_.on_tick();
                lk.lock();
                continue;
            }
        } else {
            q_cv_.wait(lk, [&] { return stop_ || !dirty_.empty(); });
        }
        if (stop_) break;
        // Quiet first: every new batch moves the rebuild on by `quiet`, but
        // never past `most` from the first one, so a night that writes
        // without pause is still rebuilt while it is written.
        const auto due = std::chrono::steady_clock::now() + most;
        for (;;) {
            const uint64_t seen = events_;
            const auto     now  = std::chrono::steady_clock::now();
            if (now >= due) break;
            const auto wait = std::min<std::chrono::steady_clock::duration>(quiet, due - now);
            if (!q_cv_.wait_for(lk, wait, [&] { return stop_ || events_ != seen; })) break;
            if (stop_) return;
        }
        std::set<std::string> todo;
        todo.swap(dirty_);
        lk.unlock();
        for (const auto& s : todo) rebuildNow(s);
        lk.lock();
    }
}

StepReport SleepBridgeService::rebuildNow(const std::string& sender) {
    StepReport r;
    LearnedUse learned;
    if (!available_) return r;
    {
        std::lock_guard<std::mutex> lk(rebuild_mu_);
        auto& w = workers_[sender];
        if (!w) {
            offerLearned(sender);
            w = std::make_unique<RebuildWorker>(store_, sender, factory_);
        }
        r = w->step();
        learned = w->learned();
    }
    if (r.written || r.removed || r.refused_paths || r.unreadable)
        std::cout << "[sleepbridge] " << sender << " rebuilt: " << r.written << " written, "
                  << r.complete_files << " complete, " << r.held_files << " held"
                  << (r.holding ? " (holding: " + r.holding_reason + ")" : std::string())
                  << std::endl;
    {
        std::lock_guard<std::mutex> lk(mu_);
        auto& b     = bridges_[sender];
        b.last_step = r;
        b.stepped   = true;
        b.learned   = learned;
        if (!r.machine.empty()) b.card_machine = r.machine;
    }
    if (r.written > 0 && opt_.on_rebuilt) opt_.on_rebuilt(sender, r);
    return r;
}

void SleepBridgeService::offerLearned(const std::string& sender) {
    const fs::path bdir = store_.batchesDir(sender);
    std::error_code ec;
    if (fs::exists(bdir / kBridgeLearned, ec) || fs::exists(bdir / kGeometryFile, ec)) return;
    const Json::Value meta = readLearnedMeta(store_.root());
    if (!meta.isObject()) return;
    const std::string boot = meta.get("boot_sha256", "").asString();
    for (const auto& other : store_.senders()) {
        if (other == sender) continue;
        const Json::Value held = readBridgeLearned(store_.batchesDir(other));
        if (held.isObject() && held.get("boot_sha256", "").asString() == boot) {
            std::cout << "[sleepbridge] " << sender << " first seen; the learned card is " << other
                      << "'s, not offered" << std::endl;
            return;
        }
    }
    std::string bytes, why;
    {
        std::ifstream f(fs::path(store_.root()) / kLearnedDir / kLearnedCardFile, std::ios::binary);
        if (!f) return;
        bytes.assign(std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>());
    }
    Json::Value summary = meta;
    summary.removeMember("learned_at");
    summary.removeMember("file_name");
    if (giveBridgeLearned(bdir.string(), bytes, summary, &why))
        std::cout << "[sleepbridge] " << sender << " first seen; takes the learned card (volume "
                  << meta.get("volume_serial", "").asString() << ")" << std::endl;
    else
        std::cout << "[sleepbridge] " << sender << " learned card not given: " << why << std::endl;
}

CommandAnswer SleepBridgeService::learnCard(const std::string& cbl1, const std::string& file_name) {
    CommandAnswer a;
    std::string   why;
    const auto    card = decodeLearnedCard(cbl1, &why);
    if (!card) {
        a.status        = 400;
        a.body["error"] = why;
        return a;
    }
    if (!keepLearnedCard(store_.root(), *card, cbl1, file_name, &why)) {
        a.status        = 500;
        a.body["error"] = "The card's layout could not be kept: " + why;
        return a;
    }
    const Json::Value summary = learnedSummary(*card);
    Json::Value bridges(Json::arrayValue);
    std::vector<std::string> given;
    {
        std::lock_guard<std::mutex> lk(rebuild_mu_);
        for (const auto& sender : store_.senders()) {
            Json::Value v(Json::objectValue);
            v["sender"] = sender;
            const fs::path bdir = store_.batchesDir(sender);
            RebuildGeometry g;
            {
                std::ifstream f(bdir / kGeometryFile, std::ios::binary);
                const std::string text((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
                if (const auto parsed = parseGeometryJson(text)) g = *parsed;
            }
            // SDD-061 D5: a bridge that read its own card's layout, and it is
            // another one, gets nothing.
            if (knowsOwnLayout(g) && !sameLayout(*card, g)) {
                v["applied"] = false;
                v["reason"]  = "it reads another card (layout " + std::to_string(g.sectors_per_cluster) +
                              ":" + std::to_string(g.data_begin_lba) + ")";
            } else if (!giveBridgeLearned(bdir.string(), cbl1, summary, &why)) {
                v["applied"] = false;
                v["reason"]  = "not written: " + why;
            } else {
                v["applied"] = true;
                workers_.erase(sender);   // a fresh rebuilder, the learn fed first
                given.push_back(sender);
            }
            std::cout << "[sleepbridge] " << sender << " learned card "
                      << (v["applied"].asBool() ? "given" : "not given: " + v["reason"].asString())
                      << std::endl;
            bridges.append(v);
        }
    }
    for (const auto& s : given) wake(s);
    std::cout << "[sleepbridge] card learned: volume " << card->volume_serial << ", layout "
              << card->sectors_per_cluster << ':' << card->data_begin_lba << ", " << card->sectors.size()
              << " sector(s), " << given.size() << " bridge(s) take it" << std::endl;
    a.status         = 201;
    a.body           = summary;
    a.body["stored"] = true;
    a.body["bridges"] = bridges;
    a.body["rebuild_in_build"] = available_;
    return a;
}

std::unique_ptr<Conversation> SleepBridgeService::open(const Admission& who, Outlet out, BinaryOutlet bin) {
    uint64_t id;
    {
        std::lock_guard<std::mutex> lk(mu_);
        id         = next_conn_++;
        conns_[id] = Conn{who, std::move(out), std::move(bin), false};
    }
    return std::make_unique<Conversation>(store_, who, id);
}

// The bridge's own hello: this connection is its session.
void SleepBridgeService::startSession(const std::string& sender, uint64_t conn) {
    const std::time_t now = std::time(nullptr);
    bool first = false;
    Admission who;
    {
        std::lock_guard<std::mutex> lk(mu_);
        auto c = conns_.find(conn);
        if (c == conns_.end()) {
            who.sender = sender;
        } else {
            who = c->second.who;
            if (!c->second.session) {
                c->second.session = true;
                ++bridges_[sender].sessions;
            }
        }
        auto& b = bridges_[sender];
        first        = b.first_seen == 0;
        if (first) b.first_seen = now;
        b.machine    = who.machine;
        b.chip_id    = who.chip_id;
        b.last_heard = now;
        b.live       = conn;
    }
    // The bridge's own record: who it said it was. Never a credential.
    const fs::path rec = fs::path(store_.batchesDir(who.sender)) / "bridge.json";
    std::error_code ec;
    if (first || !fs::exists(rec, ec)) {
        Json::Value v(Json::objectValue);
        v["sender"]     = who.sender;
        v["machine"]    = who.machine;
        v["chip_id"]    = who.chip_id;
        v["first_seen"] = static_cast<Json::Int64>(now);
        Json::StreamWriterBuilder wb;
        wb["indentation"] = "";
        const std::string text = Json::writeString(wb, v);
        std::string err;
        durableWrite(rec.string(), text.data(), text.size(), &err);
    }
    std::cout << "[sleepbridge] " << who.sender << " connected"
              << (who.machine.empty() ? std::string() : " machine=" + who.machine) << std::endl;
}

void SleepBridgeService::apply(const std::string& sender, const Effects& fx) {
    for (const auto& line : fx.log) std::cout << "[sleepbridge] " << sender << " " << line << std::endl;
    if (fx.probe) {
        // A test of this address: answered, and nothing of it is kept.
        std::cout << "[sleepbridge] " << sender << " test hello answered" << std::endl;
        return;
    }
    if (fx.hello) startSession(sender, fx.conn);
    {
        std::lock_guard<std::mutex> lk(mu_);
        auto& b      = bridges_[sender];
        b.last_heard = std::time(nullptr);
        if (fx.state) b.state = *fx.state;
        if (fx.banked) {
            b.last_batch      = b.last_heard;
            b.last_batch_mono = monoNow();
        }

        if (b.update && !b.update->job.ended()) b.update->job.last_heard = b.last_heard;
        if (fx.hello && b.update && !b.update->job.ended() &&
            (b.update->job.phase() == "restarting" || b.update->job.phase() == "verifying")) {
            // The bridge is back after its update (or restarted before it
            // answered): what it runs now is the outcome. Its old version,
            // when the image was another, means the new one did not take.
            auto&       u     = *b.update;
            auto&       s     = *u.job.current();
            s.version_after   = b.state.get("fw_version", "").asString();
            const bool  old   = !s.version_target.empty() && s.version_target != s.version_before &&
                               s.version_after == s.version_before;
            if (old) {
                s.phase    = "failed";
                u.job.reason = "back_on_old_version";
                u.job.detail = "the bridge came back on " + s.version_before;
            } else {
                s.phase = "done";
            }
            u.job.ended_at = b.last_heard;
            jobChanged(sender, u);
            std::cout << "[sleepbridge] " << sender << " back after the update, firmware "
                      << (s.version_after.empty() ? "?" : s.version_after)
                      << (old ? ": the new firmware did not take" : "") << std::endl;
        }
        bool update_answered = false;
        if (fx.result && b.update && b.update->req_id == fx.result->req_id &&
            (b.update->job.phase() == "transferring" || b.update->job.phase() == "verifying")) {
            auto& u         = *b.update;
            update_answered = true;
            u.result        = fx.result->result;
            u.answered_at   = b.last_heard;
            if (u.result.get("ok", false).asBool()) {
                u.job.current()->phase = "restarting";   // ended by its next hello
            } else {
                u.job.current()->phase = "failed";
                u.job.reason   = "refused_image";
                u.job.detail   = u.result.get("error", "the bridge refused the image").asString();
                u.job.ended_at = b.last_heard;
            }
            jobChanged(sender, u);
            std::cout << "[sleepbridge] " << sender << " update (req_id " << u.req_id << ") answered: "
                      << (u.job.phase() == "restarting" ? "ok, the bridge is restarting into it"
                                                        : "error=" + u.job.detail)
                      << std::endl;
        }

        if (fx.log_text) {
            const auto& t = *fx.log_text;
            auto c = std::find_if(b.commands.begin(), b.commands.end(), [&](const Command& k) {
                return static_cast<uint32_t>(k.req_id) == t.req_id;
            });
            if (c == b.commands.end()) {
                std::cout << "[sleepbridge] " << sender << " log text for unknown req_id "
                          << t.req_id << ", ignored" << std::endl;
            } else if (c->logs.size() + t.text.size() > kCommandLogMax) {
                c->logs.append(t.text, 0, kCommandLogMax - c->logs.size());
                c->logs_cut = true;
            } else {
                c->logs += t.text;
            }
        }
        if (fx.result && !update_answered) {
            auto c = std::find_if(b.commands.begin(), b.commands.end(), [&](const Command& k) {
                return k.req_id == fx.result->req_id;
            });
            if (c == b.commands.end()) {
                std::cout << "[sleepbridge] " << sender << " result for unknown req_id "
                          << fx.result->req_id << ", ignored" << std::endl;
            } else {
                c->state       = "answered";
                c->answered_at = b.last_heard;
                c->result      = fx.result->result;
                // A config answer's values never carry a secret on from here,
                // not even the bridge's own "***".
                if (c->cmd == "config" && c->result.isObject() && c->result["values"].isObject())
                    for (const char* k : {"pass", "pair", "jwt"}) c->result["values"].removeMember(k);
                std::cout << "[sleepbridge] " << sender << " " << c->cmd << " (req_id " << c->req_id
                          << ") answered: ok=" << (c->result.get("ok", false).asBool() ? "true" : "false")
                          << (c->result.isMember("error") ? " error=" + c->result["error"].asString()
                                                          : std::string())
                          << std::endl;
            }
        }
    }
    if (fx.hello) flush(sender);
    if (fx.hello && feed_) feed_->refreshAsync();   // the release check: on a hello, no timer
    if (fx.banked || fx.card) wake(sender);
}

void SleepBridgeService::closed(const Conversation& conv) {
    const std::string& sender = conv.who().sender;
    std::lock_guard<std::mutex> lk(mu_);
    auto c = conns_.find(conv.conn());
    const bool session = c != conns_.end() && c->second.session;
    if (c != conns_.end()) conns_.erase(c);
    if (!session) return;   // a test hello, or nothing said: no session to end
    auto it = bridges_.find(sender);
    if (it != bridges_.end()) {
        if (it->second.sessions > 0) --it->second.sessions;
        if (it->second.live == conv.conn()) it->second.live = 0;
    }
    std::cout << "[sleepbridge] " << sender << " disconnected" << std::endl;
}

void SleepBridgeService::flush(const std::string& sender) {
    // Frames picked under the lock, sent outside it, then marked sent.
    std::vector<std::pair<uint64_t, std::string>> todo;
    Outlet out;
    {
        std::lock_guard<std::mutex> lk(mu_);
        auto b = bridges_.find(sender);
        if (b == bridges_.end() || !b->second.live) return;
        auto c = conns_.find(b->second.live);
        if (c == conns_.end() || !c->second.out) return;
        out = c->second.out;
        Json::StreamWriterBuilder w;
        w["indentation"] = "";
        for (const auto& k : b->second.commands)
            if (k.state == "queued") todo.emplace_back(k.req_id, Json::writeString(w, k.frame));
    }
    for (const auto& [req, text] : todo) {
        if (!out(text)) return;   // the connection went: the rest wait for the next one
        std::lock_guard<std::mutex> lk(mu_);
        for (auto& k : bridges_[sender].commands)
            if (k.req_id == req && k.state == "queued") {
                k.state   = "sent";
                k.sent_at = std::time(nullptr);
                // A secret lived in the frame only until it went.
                if (k.frame["set"].isObject())
                    for (const char* s : {"pass", "pair", "jwt"})
                        if (k.frame["set"].isMember(s)) k.frame["set"][s] = "***";
                std::cout << "[sleepbridge] " << sender << " " << k.cmd << " sent (req_id " << req
                          << ")" << std::endl;
            }
    }
}

CommandAnswer SleepBridgeService::queueCommand(const std::string& sender, const Json::Value& req) {
    CommandAnswer a;
    auto refuse = [&a](int status, const std::string& why) {
        a.status        = status;
        a.body["ok"]    = false;
        a.body["error"] = why;
        return a;
    };
    if (!req.isObject() || !req["cmd"].isString()) return refuse(400, "a command is {\"cmd\": name, ...}");
    const std::string cmd = req["cmd"].asString();
    const auto& known     = localCommands();
    if (std::find(known.begin(), known.end(), cmd) == known.end())
        return refuse(400, "a Sleep Bridge takes stream, reboot, logs, restate, config_reset or config "
                           "from hms-cpap, not \"" + cmd + "\"");

    Json::Value frame(Json::objectValue);
    frame["t"]   = "cmd";
    frame["cmd"] = cmd;
    if (cmd == "stream") {
        // "" is back to the cloud; otherwise an IP address or a hostname and
        // nothing else, and the port as its own field (8894 when left out):
        // the bridge builds ws://<url>:<port>/sleepbridge/v1/device itself.
        if (!req["url"].isString())
            return refuse(400, "stream needs a url: \"\" for the cloud, or an IP address or hostname");
        const std::string url = req["url"].asString();
        if (!validBridgeHost(url))
            return refuse(400, "the url is an IP address or hostname only (e.g. 192.168.1.20), "
                               "with no port, scheme or path; \"\" for the cloud. The port is its own "
                               "field, \"port\"");
        frame["url"] = url;
        if (req.isMember("port")) {
            if (!req["port"].isInt() || !validBridgePort(req["port"].asInt()))
                return refuse(400, "the port is a number from 1 to 65535 (8894 when left out)");
            frame["port"] = req["port"].asInt();
        }
    } else if (cmd == "logs") {
        int n = 200;
        if (req.isMember("n")) {
            if (!req["n"].isInt() || req["n"].asInt() < 1 || req["n"].asInt() > 5000)
                return refuse(400, "n is a number of log lines, 1 to 5000");
            n = req["n"].asInt();
        }
        frame["n"] = n;
    } else if (cmd == "config") {
        const std::string why = configShapeError(req);
        if (!why.empty()) return refuse(400, why);
        if (req.isMember("set")) frame["set"] = req["set"];
        else frame["get"] = true;
    }
    // The person saw the recording warning: said to the bridge too (SDD-062
    // D6; a bridge that does not read it ignores it).
    const bool confirm = devices::confirmed(req);
    if (cmd == "config" && confirm) frame["confirm"] = true;

    // What the command list shows: the frame's arguments, every secret "***".
    Json::Value shown(Json::objectValue);
    for (const auto& k : frame.getMemberNames())
        if (k != "t" && k != "cmd" && k != "req_id") shown[k] = frame[k];
    if (shown["set"].isObject())
        for (const auto& k : shown["set"].getMemberNames())
            if (secretConfigKey(k)) shown["set"][k] = "***";

    const bool warned = cmd == "reboot" || cmd == "config_reset" || cmd == "stream" ||
                        (cmd == "config" && configTouchesNetwork(req));
    {
        std::lock_guard<std::mutex> lk(mu_);
        const bool seen = bridges_.count(sender) != 0;
        if (!seen) {
            const auto s = store_.senders();
            if (std::find(s.begin(), s.end(), sender) == s.end())
                return refuse(404, "no Sleep Bridge named \"" + sender + "\" has connected here");
        }
        auto& b = bridges_[sender];
        if (cmd == "config") {
            const std::string fw = b.state.get("fw_version", "").asString();
            if (fw.empty() || mule::compareVersions(fw, kConfigSince) < 0)
                return refuse(409, "this Sleep Bridge needs firmware " + std::string(kConfigSince) +
                                       " for its settings" + (fw.empty() ? "" : " (it runs " + fw + ")"));
        }
        if (warned && !confirm) {
            const devices::Recording rec = recordingLocked(sender);
            if (rec.warns()) {
                a.status = 409;
                a.body   = devices::recordingWarning(rec);
                return a;
            }
        }
        Command c;
        c.req_id       = next_req_++;
        c.cmd          = cmd;
        frame["req_id"] = static_cast<Json::UInt64>(c.req_id);
        c.frame        = frame;
        c.shown        = shown;
        c.queued_at    = std::time(nullptr);
        b.commands.push_back(std::move(c));
        while (b.commands.size() > kCommandsKept) {
            // The oldest that is not waiting goes first; a queued one stays.
            auto old = std::find_if(b.commands.begin(), b.commands.end(),
                                    [](const Command& k) { return k.state != "queued"; });
            b.commands.erase(old == b.commands.end() ? b.commands.begin() : old);
        }
        a.body["req_id"] = static_cast<Json::UInt64>(b.commands.back().req_id);
    }
    flush(sender);
    {
        std::lock_guard<std::mutex> lk(mu_);
        for (const auto& k : bridges_[sender].commands)
            if (k.req_id == a.body["req_id"].asUInt64()) a.body["command"] = commandJson(k);
    }
    a.body["ok"] = true;
    return a;
}

CommandAnswer SleepBridgeService::cancelCommand(uint64_t req_id) {
    CommandAnswer a;
    std::lock_guard<std::mutex> lk(mu_);
    for (auto& [sender, b] : bridges_) {
        auto c = std::find_if(b.commands.begin(), b.commands.end(),
                              [&](const Command& k) { return k.req_id == req_id; });
        if (c == b.commands.end()) continue;
        if (c->state != "queued") {
            a.status        = 409;
            a.body["ok"]    = false;
            a.body["error"] = "the command was already sent to the bridge; it cannot be taken back";
            a.body["command"] = commandJson(*c);
            return a;
        }
        std::cout << "[sleepbridge] " << sender << " " << c->cmd << " (req_id " << req_id
                  << ") cancelled before it was sent" << std::endl;
        b.commands.erase(c);
        a.body["ok"]        = true;
        a.body["req_id"]    = static_cast<Json::UInt64>(req_id);
        a.body["cancelled"] = true;
        return a;
    }
    a.status        = 404;
    a.body["ok"]    = false;
    a.body["error"] = "no command " + std::to_string(req_id) + " is listed here";
    return a;
}

CommandAnswer SleepBridgeService::forget(const std::string& sender) {
    CommandAnswer a;
    std::lock_guard<std::mutex> lk(mu_);
    const auto it = bridges_.find(sender);
    if (it != bridges_.end() && it->second.sessions > 0) {
        a.status        = 409;
        a.body["ok"]    = false;
        a.body["error"] = "this device is connected";
        return a;
    }
    size_t dropped = 0;
    if (it != bridges_.end()) {
        for (const auto& c : it->second.commands)
            if (c.state == "queued") ++dropped;
        bridges_.erase(it);
    }
    if (!opt_.jobs_dir.empty()) {
        std::error_code ec;
        fs::remove(fs::path(opt_.jobs_dir) / (sender + ".json"), ec);
    }
    std::cout << "[sleepbridge] " << sender << " forgotten (" << dropped
              << " queued command(s) dropped; its batches and files stay)" << std::endl;
    a.body["ok"]              = true;
    a.body["dropped_queued"]  = static_cast<Json::UInt64>(dropped);
    return a;
}

devices::Recording SleepBridgeService::recording(const std::string& sender) const {
    std::lock_guard<std::mutex> lk(mu_);
    return recordingLocked(sender);
}

devices::Recording SleepBridgeService::recordingLocked(const std::string& sender) const {
    devices::Recording r;
    const auto it = bridges_.find(sender);
    if (it == bridges_.end() || it->second.last_heard == 0) {
        // Nothing heard from it since this service started: its card may be
        // written right now, and nothing here would know.
        r.state  = "unknown";
        r.reason = "the Sleep Bridge has not been heard from since hms-cpap started";
        return r;
    }
    const Bridge& b = it->second;
    if (b.last_batch != 0) {
        const double ago = monoNow() - b.last_batch_mono;
        if (ago < opt_.night_quiet_s) {
            r.state  = "yes";
            const int min = static_cast<int>(ago / 60);
            r.reason = "the machine wrote to its card " +
                       (min < 1 ? std::string("less than a minute") : std::to_string(min) + " min") + " ago";
        }
    }
    return r;
}

Json::Value SleepBridgeService::commandJson(const Command& c) {
    Json::Value v(Json::objectValue);
    v["req_id"] = static_cast<Json::UInt64>(c.req_id);
    v["cmd"]    = c.cmd;
    v["args"]   = c.shown;
    v["state"]       = c.state;   // queued (bridge not online), sent, answered
    v["queued_at"]   = static_cast<Json::Int64>(c.queued_at);
    v["sent_at"]     = static_cast<Json::Int64>(c.sent_at);
    v["answered_at"] = static_cast<Json::Int64>(c.answered_at);
    v["result"]      = c.result;
    if (c.cmd == "logs") {
        v["logs"]     = c.logs;
        v["logs_cut"] = c.logs_cut;
    }
    // A reboot restarts the bridge before it can answer; its next hello is
    // the sign it came back.
    if (c.cmd == "reboot") v["answered_by_bridge"] = false;
    return v;
}

// ── updates ─────────────────────────────────────────────────────────────────

bool SleepBridgeService::nightOpen(const std::string& sender) const {
    std::lock_guard<std::mutex> lk(mu_);
    const auto it = bridges_.find(sender);
    if (it == bridges_.end() || it->second.last_batch == 0) return false;
    return monoNow() - it->second.last_batch_mono < opt_.night_quiet_s;
}

void SleepBridgeService::jobChanged(const std::string& sender, UpdateJob& u) {
    u.job.updated_at = std::time(nullptr);
    if (u.job.ended() && !u.job.ended_at) u.job.ended_at = u.job.updated_at;
    devices::otaSave(opt_.jobs_dir, sender, u.job);
}

// The shape scripts read before the job object, kept one release beside it.
Json::Value SleepBridgeService::updateJson(const UpdateJob& u) {
    const devices::OtaJob& j = u.job;
    const devices::OtaStep* s = j.current();
    const std::string p = j.phase();
    const std::string legacy = p == "preparing"    ? "downloading"
                               : p == "transferring" ? "sending"
                               : p == "verifying"    ? "waiting"
                                                     : p;
    Json::Value v(Json::objectValue);
    v["req_id"]        = static_cast<Json::UInt64>(u.req_id);
    v["phase"]         = legacy;
    v["running"]       = !j.ended();
    v["total"]         = static_cast<Json::UInt64>(s ? s->total : 0);
    v["sent"]          = static_cast<Json::UInt64>(s ? s->bytes : 0);
    v["image_version"] = u.image.version.empty() && s ? s->version_target : u.image.version;
    v["image_project"] = u.image.project;
    v["image_built"]   = u.image.built;
    v["sha256"]        = u.sha256;
    v["fw_before"]     = s ? s->version_before : "";
    v["fw_after"]      = s ? s->version_after : "";
    v["error"]         = j.detail.empty() ? j.reason : j.detail;
    v["started_at"]    = static_cast<Json::Int64>(j.started_at);
    v["sent_at"]       = static_cast<Json::Int64>(u.sent_at);
    v["answered_at"]   = static_cast<Json::Int64>(u.answered_at);
    v["ended_at"]      = static_cast<Json::Int64>(j.ended_at);
    v["result"]        = u.result;
    return v;
}

Json::Value SleepBridgeService::update(const std::string& sender) const {
    std::lock_guard<std::mutex> lk(mu_);
    const auto it = bridges_.find(sender);
    if (it == bridges_.end() || !it->second.update) return Json::Value(Json::nullValue);
    return updateJson(*it->second.update);
}

Json::Value SleepBridgeService::ota(const std::string& sender) const {
    std::lock_guard<std::mutex> lk(mu_);
    const auto it = bridges_.find(sender);
    if (it == bridges_.end() || !it->second.update) return Json::Value(Json::nullValue);
    return devices::otaJson(it->second.update->job);
}

void SleepBridgeService::failJob(const std::string& sender, uint64_t req_id, const std::string& reason,
                                 const std::string& detail) {
    auto b = bridges_.find(sender);
    if (b == bridges_.end() || !b->second.update || b->second.update->req_id != req_id) return;
    auto& u = *b->second.update;
    if (u.job.ended()) return;
    u.job.current()->phase = "failed";
    u.job.reason           = reason;
    u.job.detail           = detail;
    jobChanged(sender, u);
    std::cout << "[sleepbridge] " << sender << " update (req_id " << req_id << ") failed: " << reason << ", "
              << detail << std::endl;
}

void SleepBridgeService::waitForUpdateSend() {
    if (update_thread_.joinable()) update_thread_.join();
}

std::optional<CommandAnswer> SleepBridgeService::updateRefusal(const std::string& sender, bool confirm,
                                                               bool* recording_now) {
    CommandAnswer a;
    auto refuse = [&a](int status, const std::string& why) {
        a.status        = status;
        a.body["ok"]    = false;
        a.body["error"] = why;
        return std::optional<CommandAnswer>(a);
    };
    const std::time_t now = std::time(nullptr);
    auto it = bridges_.find(sender);
    if (it == bridges_.end()) return refuse(404, "no Sleep Bridge named \"" + sender + "\" has connected here");
    Bridge& b = it->second;
    const bool online = b.sessions > 0 && now - b.last_heard < kSilenceSec && b.live != 0;
    if (!online) return refuse(409, "the bridge is not connected here right now");
    for (auto& [s, other] : bridges_) {
        if (!other.update || other.update->job.ended()) continue;
        const std::string p = other.update->job.phase();
        if (p == "preparing" || p == "transferring" || p == "verifying")
            return refuse(409, "an update is already running on " + s);
    }
    // A unit that took the last image is updated again once it is heard
    // from: its hello ends that job.
    if (b.update && !b.update->job.ended())
        return refuse(409, "the bridge is restarting into the last update; wait for its hello");
    const devices::Recording rec = recordingLocked(sender);
    *recording_now = rec.warns();
    if (rec.warns() && !confirm) {
        a.status = 409;
        a.body   = devices::recordingWarning(rec);
        return a;
    }
    return std::nullopt;
}

SleepBridgeService::UpdateJob SleepBridgeService::newJob(const std::string& sender, const std::string& source,
                                                         const std::string& target_version, size_t total,
                                                         bool confirmed_while_recording) {
    const std::time_t now = std::time(nullptr);
    Bridge&   b = bridges_[sender];
    UpdateJob u;
    u.req_id                          = next_req_++;
    u.job.job_id                      = std::to_string(u.req_id);
    u.job.serial                      = sender;
    u.job.kind                        = "tapper";
    u.job.source                      = source;
    u.job.confirmed_while_recording   = confirmed_while_recording;
    u.job.started_at                  = now;
    u.job.updated_at                  = now;
    u.job.last_heard                  = b.last_heard;
    devices::OtaStep s;
    s.target         = "tapper";
    s.total          = total;
    s.version_before = b.state.get("fw_version", "").asString();
    s.version_target = target_version;
    u.job.steps.push_back(s);
    return u;
}

CommandAnswer SleepBridgeService::startUpdate(const std::string& sender, const std::string& image, bool confirm,
                                              const std::string& file_name) {
    CommandAnswer a;
    auto refuse = [&a](int status, const std::string& why) {
        a.status        = status;
        a.body["ok"]    = false;
        a.body["error"] = why;
        return a;
    };
    if (image.empty()) return refuse(400, "the body is the firmware image (a .bin file)");
    if (image.size() > kImageMax) return refuse(400, "the image is larger than a Sleep Bridge image can be");
    std::string why;
    const auto  info = inspectImage(image, &why);
    if (!info) return refuse(400, why);
    if (info->project != kBridgeProject)
        return refuse(400, "not a Sleep Bridge image: it says it is \"" + info->project + "\"");

    uint64_t req_id;
    {
        std::lock_guard<std::mutex> lk(mu_);
        bool recording_now = false;
        if (auto r = updateRefusal(sender, confirm, &recording_now)) return *r;
        UpdateJob u = newJob(sender, "file " + (file_name.empty() ? std::string("(unnamed)") : file_name),
                             info->version, image.size(), recording_now && confirm);
        u.image                 = *info;
        u.sha256                = sha256Hex(image);
        u.job.current()->phase  = "transferring";
        req_id                  = u.req_id;
        bridges_[sender].update = u;
        jobChanged(sender, *bridges_[sender].update);
    }
    waitForUpdateSend();   // the last job's thread, long done
    update_thread_ = std::thread([this, sender, image] { sendUpdate(sender, image); });
    a.body["ok"]     = true;
    a.body["req_id"] = static_cast<Json::UInt64>(req_id);
    a.body["update"] = update(sender);
    a.body["job"]    = ota(sender);
    return a;
}

CommandAnswer SleepBridgeService::startReleaseUpdate(const std::string& sender, bool confirm) {
    CommandAnswer a;
    auto refuse = [&a](int status, const std::string& why) {
        a.status        = status;
        a.body["ok"]    = false;
        a.body["error"] = why;
        return a;
    };
    if (!feed_ || !feed_->enabled() || !opt_.fetch)
        return refuse(409, "online updates are off here: update the bridge from a firmware file");
    std::string fw;
    std::optional<std::string> variant;
    {
        std::lock_guard<std::mutex> lk(mu_);
        const auto it = bridges_.find(sender);
        if (it == bridges_.end()) return refuse(404, "no Sleep Bridge named \"" + sender + "\" has connected here");
        fw      = it->second.state.get("fw_version", "").asString();
        variant = bridgeVariant(it->second.state);
    }
    const Json::Value check = feed_->answer(fw, variant);
    if (!check["available"].asBool()) {
        std::string why = check.get("error", "").asString();
        if (why.empty())
            why = "the bridge already runs " + fw + "; the release is " + check["version"].asString();
        return refuse(409, why);
    }
    const auto rel = feed_->release(*variant);
    if (!rel) return refuse(409, "the release has no image for this Sleep Bridge's board");

    uint64_t req_id;
    {
        std::lock_guard<std::mutex> lk(mu_);
        bool recording_now = false;
        if (auto r = updateRefusal(sender, confirm, &recording_now)) return *r;
        UpdateJob u = newJob(sender, "release " + rel->first, rel->first, rel->second.size,
                             recording_now && confirm);
        u.job.current()->phase  = "preparing";
        u.sha256                = rel->second.sha256;
        req_id                  = u.req_id;
        bridges_[sender].update = u;
        jobChanged(sender, *bridges_[sender].update);
    }
    std::cout << "[sleepbridge] " << sender << " update (req_id " << req_id << "): release " << rel->first
              << ", downloading" << std::endl;
    waitForUpdateSend();
    update_thread_ = std::thread([this, sender, img = rel->second] { fetchAndSend(sender, img); });
    a.body["ok"]     = true;
    a.body["req_id"] = static_cast<Json::UInt64>(req_id);
    a.body["update"] = update(sender);
    a.body["job"]    = ota(sender);
    return a;
}

// On the update's thread: the release image, downloaded and checked against
// the feed before anything goes to the bridge.
void SleepBridgeService::fetchAndSend(const std::string& sender, FeedImage img) {
    uint64_t req_id = 0;
    {
        std::lock_guard<std::mutex> lk(mu_);
        auto b = bridges_.find(sender);
        if (b == bridges_.end() || !b->second.update) return;
        req_id = b->second.update->req_id;
    }
    auto fail = [&](const std::string& detail) {
        std::lock_guard<std::mutex> lk(mu_);
        failJob(sender, req_id, "image_unavailable", detail);
    };
    const FeedReply r = opt_.fetch(img.url, kImageTimeoutS);
    if (!r.ok) return fail("the image could not be downloaded: " + r.error);
    if (r.status != 200) return fail("the image could not be downloaded: HTTP " + std::to_string(r.status));
    if (r.body.size() != img.size || sha256Hex(r.body) != img.sha256)
        return fail("the image does not match the release");
    std::string why;
    const auto  info = inspectImage(r.body, &why);
    if (!info) return fail("the release image is not a firmware image: " + why);
    if (info->project != kBridgeProject) return fail("the release image is not a Sleep Bridge image");
    {
        std::lock_guard<std::mutex> lk(mu_);
        auto b = bridges_.find(sender);
        if (b == bridges_.end() || !b->second.update || b->second.update->req_id != req_id) return;
        auto& u = *b->second.update;
        u.image                = *info;
        u.job.current()->phase = "transferring";
        jobChanged(sender, u);
    }
    sendUpdate(sender, r.body);
}

// On its own thread: the command, then the chunks, on the bridge's session.
// Each frame is handed to the connection in turn; the TCP window paces the
// bridge's flash. A connection that goes meanwhile fails the job.
void SleepBridgeService::sendUpdate(const std::string& sender, std::string image) {
    Outlet       out;
    BinaryOutlet bin;
    uint64_t     req_id;
    std::string  sha;
    {
        std::lock_guard<std::mutex> lk(mu_);
        auto b = bridges_.find(sender);
        if (b == bridges_.end() || !b->second.update) return;
        req_id = b->second.update->req_id;
        sha    = sha256Hex(image);
        b->second.update->sha256 = sha;
        auto c = conns_.find(b->second.live);
        if (c != conns_.end()) {
            out = c->second.out;
            bin = c->second.bin;
        }
    }
    auto fail = [&](const std::string& reason, const std::string& detail) {
        std::lock_guard<std::mutex> lk(mu_);
        failJob(sender, req_id, reason, detail);
    };
    if (!out || !bin) return fail("offline", "the bridge's connection takes no frames");
    if (!out(otaCommandFrame(req_id, image.size(), sha)))
        return fail("connection_lost", "the connection went before the command");
    std::cout << "[sleepbridge] " << sender << " update (req_id " << req_id << "): sending " << image.size()
              << " B" << std::endl;
    uint32_t seq = 0;
    for (size_t off = 0; off < image.size(); off += kFirmwareChunk, seq++) {
        const size_t n    = std::min(kFirmwareChunk, image.size() - off);
        const bool   last = off + n >= image.size();
        if (!bin(firmwareChunkFrame(req_id, seq, last, image.substr(off, n))))
            return fail("connection_lost", "the connection went during the image, after " + std::to_string(off) + " B");
        std::lock_guard<std::mutex> lk(mu_);
        auto b = bridges_.find(sender);
        if (b == bridges_.end() || !b->second.update || b->second.update->req_id != req_id) return;
        auto& u = *b->second.update;
        auto* s = u.job.current();
        s->bytes = off + n;
        s->total = image.size();
        if (s->phase == "transferring" && last) {
            s->phase  = "verifying";   // every byte went; the bridge checks, then answers
            u.sent_at = std::time(nullptr);
            jobChanged(sender, u);
        } else {
            u.job.updated_at = std::time(nullptr);
        }
    }
}

Json::Value SleepBridgeService::status() const {
    Json::Value out(Json::objectValue);
    out["available"] = available_;
    out["port"]      = opt_.port;
    out["path"]      = kRoute;
    out["version"]   = rebuilderVersion();
    if (!available_) out["message"] = "The Sleep Bridge is not in this build";
    Json::Value list(Json::arrayValue);
    const std::time_t now = std::time(nullptr);

    std::map<std::string, Bridge> snap;
    {
        std::lock_guard<std::mutex> lk(mu_);
        snap = bridges_;
    }
    // Bridges banked here before this start that have not connected since.
    for (const auto& s : store_.senders()) snap.emplace(s, Bridge{});

    for (const auto& [sender, b] : snap) {
        Json::Value v(Json::objectValue);
        v["sender"]     = sender;
        v["machine"]    = b.machine;
        v["chip_id"]    = b.chip_id;
        v["online"]     = b.sessions > 0 && now - b.last_heard < kSilenceSec;
        v["last_heard"] = static_cast<Json::Int64>(b.last_heard);
        v["last_batch"] = static_cast<Json::Int64>(b.last_batch);
        v["state"]      = b.state;
        const std::string fw = b.state.get("fw_version", "").asString();
        v["fw_version"] = fw;
        v["night_open"] = b.last_batch != 0 && monoNow() - b.last_batch_mono < opt_.night_quiet_s;
        {
            std::lock_guard<std::mutex> lk(mu_);
            const devices::Recording rec = recordingLocked(sender);
            v["recording"]        = rec.state;
            v["recording_reason"] = rec.reason;
        }
        // config (get and set) needs firmware 1.0.1.
        v["config_capable"] = !fw.empty() && mule::compareVersions(fw, kConfigSince) >= 0;
        const auto variant = bridgeVariant(b.state);
        v["variant"] = variant ? Json::Value(*variant) : Json::Value(Json::nullValue);
        if (feed_) v["release"] = feed_->answer(fw, variant);
        else v["release"] = Json::Value(Json::nullValue);
        v["update"] = b.update ? updateJson(*b.update) : Json::Value(Json::nullValue);
        v["ota"]    = b.update ? devices::otaJson(b.update->job) : Json::Value(Json::nullValue);
        v["card_root"]  = store_.cardRoot(sender);
        // Which machine's card the root holds, and the card format to read
        // it as: a Luna's root is read as "bmc", a ResMed's as "resmed".
        v["card_machine"] = b.card_machine;
        v["card_format"]  = formatForMachine(b.card_machine);
        if (b.stepped) v["rebuild"] = stepJson(b.last_step);
        // SDD-061 D8: the card learned on a computer, and whether it latched.
        const Json::Value held = readBridgeLearned(store_.batchesDir(sender));
        if (held.isObject()) {
            LearnedUse use = b.learned;
            if (use.state.empty()) use.summary = held;
            const auto verdict =
                learnedVerdict(use, b.stepped && !use.state.empty(), b.last_step.geometry,
                               b.last_step.holding, b.last_step.holding_reason);
            Json::Value l(Json::objectValue);
            l["volume_serial"] = held.get("volume_serial", "").asString();
            l["boot_sha256"]   = held.get("boot_sha256", "").asString();
            l["read_at"]       = held.get("read_at", "").asString();
            l["fat"]           = held.get("fat", "").asString();
            l["state"]         = use.state;
            l["sectors_fed"]   = use.sectors;
            l["verdict"]       = verdict.verdict;
            l["reason"]        = verdict.reason;
            v["learned"]       = l;
        } else {
            v["learned"] = Json::Value(Json::nullValue);
        }
        Json::Value cmds(Json::arrayValue);
        for (auto it = b.commands.rbegin(); it != b.commands.rend(); ++it) cmds.append(commandJson(*it));
        v["commands"] = cmds;
        list.append(v);
    }
    out["bridges"] = list;
    out["learned_card"] = readLearnedMeta(store_.root());   // null until a card is learned
    Json::Value names(Json::arrayValue);
    for (const auto& n : localCommands()) names.append(n);
    out["commands"] = names;   // what POST /api/sleepbridge/command takes
    return out;
}

}  // namespace hms_cpap::sleepbridge
