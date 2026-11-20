#include "services/OscarHelperHost.h"

#include <chrono>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>

namespace hms_cpap::oscar::helper {

namespace fs = std::filesystem;

namespace {

constexpr const char* kTag = "OSCAR helper: ";

std::string isoNow() {
    const std::time_t t = std::time(nullptr);
    std::tm tm{};
#ifdef _WIN32
    gmtime_s(&tm, &t);
#else
    gmtime_r(&t, &tm);
#endif
    char buf[32];
    std::strftime(buf, sizeof(buf), "%Y-%m-%dT%H:%M:%SZ", &tm);
    return buf;
}

bool readAll(const fs::path& p, std::string& out) {
    std::ifstream in(p, std::ios::binary);
    if (!in) return false;
    std::ostringstream s;
    s << in.rdbuf();
    out = s.str();
    return true;
}

/// A PC name as it is shown: printable, at most 64 characters.
std::string cleanName(const std::string& n) {
    std::string out;
    for (unsigned char c : n) {
        if (c < 0x20 || c == 0x7f) continue;
        out.push_back(static_cast<char>(c));
        if (out.size() >= 64) break;
    }
    return out.empty() ? "PC" : out;
}

Json::Value refusal(const std::string& error, const std::string& helper) {
    Json::Value v(Json::objectValue);
    v["error"] = error;
    if (!helper.empty()) v["helper"] = helper;
    return v;
}

}  // namespace

// ── Trust ───────────────────────────────────────────────────────────────────

Trust::Trust(std::string file) : file_(std::move(file)) { load(); }

void Trust::load() {
    std::string text;
    if (file_.empty() || !readAll(file_, text)) return;
    Json::CharReaderBuilder b;
    Json::Value v;
    std::string err;
    std::istringstream in(text);
    if (!Json::parseFromStream(b, in, &v, &err) || !v.isObject()) return;
    name_      = v["name"].asString();
    hash_      = v["token_sha256"].asString();
    paired_at_ = v["paired_at"].asString();
    if (hash_.size() != 64) name_.clear(), hash_.clear();
}

bool Trust::save() const {
    if (file_.empty()) return true;
    std::error_code ec;
    fs::create_directories(fs::path(file_).parent_path(), ec);
    const std::string tmp = file_ + ".tmp";
    {
        std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
        if (!out) return false;
        if (hash_.empty()) {
            out << "{}\n";
        } else {
            Json::Value v(Json::objectValue);
            v["name"]         = name_;
            v["token_sha256"] = hash_;   // never the token itself
            v["paired_at"]    = paired_at_;
            out << frame(v) << "\n";
        }
    }
    fs::rename(tmp, file_, ec);
    return !ec;
}

std::optional<std::string> Trust::remembered() const {
    std::lock_guard<std::mutex> lk(mu_);
    if (hash_.empty()) return std::nullopt;
    return name_;
}

bool Trust::matches(const std::string& token) const {
    std::lock_guard<std::mutex> lk(mu_);
    return !hash_.empty() && !token.empty() && sha256Hex(token) == hash_;
}

std::string Trust::pair(const std::string& pc_name) {
    std::lock_guard<std::mutex> lk(mu_);
    if (!hash_.empty()) return "";
    const std::string token = newToken();
    if (token.empty()) return "";
    name_      = cleanName(pc_name);
    hash_      = sha256Hex(token);
    paired_at_ = isoNow();
    if (!save()) std::cerr << kTag << "could not keep the helper in " << file_ << std::endl;
    return token;
}

bool Trust::forget() {
    std::lock_guard<std::mutex> lk(mu_);
    if (hash_.empty()) return false;
    name_.clear();
    hash_.clear();
    paired_at_.clear();
    save();
    return true;
}

// ── Host ────────────────────────────────────────────────────────────────────

Host::Host(Options o)
    : opt_(std::move(o)), trust_(opt_.state_dir.empty() ? "" : (fs::path(opt_.state_dir) / "helper.json").string()) {}

Host::~Host() { shutdown(); }

void Host::shutdown() {
    {
        std::lock_guard<std::mutex> lk(mu_);
        stopping_ = true;
    }
    cv_.notify_all();
}

Admission Host::admit(const std::string& authorization, const std::string& pc_name, bool probe) {
    Admission a;
    a.pc_name = cleanName(pc_name);
    if (probe) {
        a.as = Admit::Probe;
        return a;
    }
    std::string token;
    static const std::string kBearer = "Bearer ";
    if (authorization.rfind(kBearer, 0) == 0) token = authorization.substr(kBearer.size());
    const auto known = trust_.remembered();
    if (!token.empty()) {
        if (trust_.matches(token)) {
            a.as = Admit::Helper;
            a.pc_name = *known;
            return a;
        }
        a.status = 401;
        a.body = known ? refusal("has_helper", *known) : refusal("forgotten", "");
    } else if (!known) {
        a.as = Admit::Pairing;
        return a;
    } else {
        a.status = 401;
        a.body = refusal("has_helper", *known);
    }
    // Logged once per connection; nothing is read (D4).
    std::cout << kTag << "refused a helper on " << a.pc_name << " ("
              << a.body["error"].asString() << ")" << std::endl;
    return a;
}

uint64_t Host::open(const Admission& a, Outlets out) {
    auto c = std::make_shared<Conn>();
    c->as = a.as;
    c->pc_name = a.pc_name;
    c->out = std::move(out);
    std::lock_guard<std::mutex> lk(mu_);
    c->id = next_id_++;
    conns_[c->id] = c;
    return c->id;
}

Json::Value Host::ackBody() const {
    Json::Value h(Json::objectValue);
    h["name"]       = opt_.name;
    h["install_id"] = opt_.install_id;
    h["version"]    = opt_.version;
    h["helper"]     = trust_.remembered().value_or("");
    Json::Value v(Json::objectValue);
    v["t"]        = "ack";
    v["hms_cpap"] = h;
    return v;
}

void Host::sendText(Conn& c, const Json::Value& v) {
    if (c.open && c.out.text && !c.out.text(frame(v))) c.open = false;
}

void Host::onText(uint64_t id, const std::string& text) {
    bool hello_event = false;
    std::shared_ptr<Conn> closing;
    {
        std::lock_guard<std::mutex> lk(mu_);
        const auto it = conns_.find(id);
        if (it == conns_.end()) return;
        const auto c = it->second;
        const auto f = parse(text);
        if (!f) return;
        const std::string t = (*f)["t"].asString();

        if (t == "hello") {
            if (c->as == Admit::Probe || (*f)["probe"].asBool()) {
                // D3: a probe is answered and is never a session.
                sendText(*c, ackBody());
                closing = c;
            } else if ((*f)["proto"].asInt() != kProto) {
                // D8: a protocol this build does not speak.
                Json::Value e(Json::objectValue);
                e["t"] = "error";
                e["error"] = "proto";
                sendText(*c, e);
                update_needed_ = true;
                update_version_ = (*f)["version"].asString();
                std::cout << kTag << c->pc_name << " speaks protocol " << (*f)["proto"].asInt()
                          << ", hms-cpap " << update_version_ << ": not read" << std::endl;
                closing = c;
            } else {
                if (c->as == Admit::Pairing) {
                    const std::string token = trust_.pair(c->pc_name);
                    if (token.empty()) {
                        // Another helper was remembered since the upgrade.
                        Json::Value e = refusal("has_helper", trust_.remembered().value_or(""));
                        e["t"] = "error";
                        sendText(*c, e);
                        closing = c;
                    } else {
                        Json::Value p(Json::objectValue);
                        p["t"] = "paired";
                        p["token"] = token;   // sent once, never logged
                        sendText(*c, p);
                        c->as = Admit::Helper;
                        std::cout << kTag << "remembered " << c->pc_name << std::endl;
                    }
                }
                if (!closing) {
                    c->hello = true;
                    c->version = (*f)["version"].asString();
                    c->files.clear();
                    for (auto& s : filesFromJson((*f)["files"])) c->files[s.path] = s;
                    update_needed_ = false;
                    last_version_ = c->version;
                    // One helper: a newer session replaces an older one.
                    if (active_ && active_ != c) {
                        active_->open = false;
                        if (active_->out.close) active_->out.close();
                    }
                    active_ = c;
                    sendText(*c, ackBody());
                    hello_event = true;
                    std::cout << kTag << c->pc_name << " connected (hms-cpap " << c->version << ", "
                              << c->files.size() << " card file(s) held)" << std::endl;
                }
            }
        } else if (c->hello && (t == "result" || t == "file_ack")) {
            c->replies[(*f)["req_id"].asUInt()] = *f;
        } else if (c->hello && t == "slice_ready") {
            const uint32_t req = (*f)["req_id"].asUInt();
            const bool whole = c->incoming.count(req) == (*f)["chunks"].asUInt();
            c->slices[req] = whole ? c->incoming.take(req) : std::string();
            Json::Value r = *f;
            r["whole"] = whole;
            c->replies[req] = r;
        }
    }
    cv_.notify_all();
    if (closing && closing->out.close) closing->out.close();
    if (hello_event && opt_.on_hello) opt_.on_hello("the OSCAR helper connected");
}

void Host::onBinary(uint64_t id, const std::string& bytes) {
    std::lock_guard<std::mutex> lk(mu_);
    const auto it = conns_.find(id);
    if (it == conns_.end() || !it->second->hello) return;
    const auto ch = parseChunk(bytes);
    if (!ch || ch->kind != kKindBytes) return;
    it->second->incoming.add(*ch);
}

void Host::closed(uint64_t id) {
    {
        std::lock_guard<std::mutex> lk(mu_);
        const auto it = conns_.find(id);
        if (it == conns_.end()) return;
        it->second->open = false;
        if (active_ == it->second) {
            std::cout << kTag << active_->pc_name << " disconnected" << std::endl;
            active_.reset();
        }
        conns_.erase(it);
    }
    cv_.notify_all();
}

bool Host::forget() {
    std::shared_ptr<Conn> c;
    {
        std::lock_guard<std::mutex> lk(mu_);
        if (!trust_.forget()) return false;
        c = active_;
        active_.reset();
        if (c) c->open = false;
    }
    cv_.notify_all();
    std::cout << kTag << "forgotten; the next helper is remembered" << std::endl;
    if (c && c->out.close) c->out.close();
    return true;
}

bool Host::remembered() const { return trust_.remembered().has_value(); }

Json::Value Host::status() const {
    Json::Value v(Json::objectValue);
    const auto name = trust_.remembered();
    std::lock_guard<std::mutex> lk(mu_);
    v["remembered"] = name.has_value();
    v["name"]       = name.value_or("");
    v["connected"]  = active_ != nullptr && active_->open;
    v["version"]    = active_ ? active_->version : last_version_;
    v["update"]     = update_needed_;
    if (update_needed_) v["update_version"] = update_version_;
    return v;
}

std::optional<Json::Value> Host::await(std::unique_lock<std::mutex>& lk, const std::shared_ptr<Conn>& c,
                                       uint32_t req) {
    cv_.wait(lk, [&] { return stopping_ || !c->open || active_ != c || c->replies.count(req); });
    const auto it = c->replies.find(req);
    if (it == c->replies.end()) return std::nullopt;
    Json::Value v = it->second;
    c->replies.erase(it);
    return v;
}

RemoteRead Host::read(const std::string& card_root, const Want& want, const std::string& slice_dir) {
    RemoteRead r;
    std::unique_lock<std::mutex> lk(mu_);
    const auto offline = [&] {
        r.outcome = update_needed_ ? Outcome::HelperUpdate : Outcome::HelperOffline;
        r.message = update_needed_ ? "update hms-cpap on " + trust_.remembered().value_or("the helper's PC")
                                   : "waiting for " + trust_.remembered().value_or("the helper's PC");
        return r;
    };
    const auto c = active_;
    if (!c || !c->open) return offline();

    // D5.1: every P-SERIES file whose size or mtime differs from its copy.
    std::vector<FileStamp> held;
    for (const auto& [p, s] : c->files) held.push_back(s);
    const auto changed = changedFiles(card_root, held);
    for (const auto& f : changed) {
        std::string bytes;
        if (!readAll(fs::path(card_root) / f.path, bytes)) continue;   // gone since the walk
        const uint32_t req = next_req_++;
        for (const auto& ch : chunks(req, bytes, opt_.chunk_bytes))
            if (c->open && c->out.binary && !c->out.binary(ch)) c->open = false;
        Json::Value fr(Json::objectValue);
        fr["t"]       = "file_ready";
        fr["req_id"]  = req;
        fr["path"]    = f.path;
        fr["chunks"]  = static_cast<Json::UInt>(chunks(0, bytes, opt_.chunk_bytes).size());
        fr["size"]    = static_cast<Json::UInt64>(bytes.size());
        fr["md5"]     = md5Hex(bytes);
        fr["mtime"]   = static_cast<Json::Int64>(f.mtime);
        sendText(*c, fr);
        const auto ack = await(lk, c, req);
        if (!ack) return offline();
        if (!(*ack)["ok"].asBool()) {
            r.outcome = Outcome::Failed;
            r.message = "the helper could not keep " + f.path;
            return r;
        }
        FileStamp kept = f;
        kept.size = bytes.size();
        c->files[f.path] = kept;
    }

    // D5.2: the import, on the helper's copy.
    const uint32_t req = next_req_++;
    Json::Value cmd(Json::objectValue);
    cmd["t"] = "cmd";
    cmd["cmd"] = "import";
    cmd["req_id"] = req;
    sendText(*c, cmd);
    const auto res = await(lk, c, req);
    if (!res) return offline();
    r.outcome = outcomeFromName((*res)["outcome"].asString());
    r.version = (*res)["version"].asString();
    r.message = (*res)["message"].asString();
    if (r.outcome != Outcome::Imported && r.outcome != Outcome::NothingNew) return r;

    // D5.3: what the store half wants. Asked without the lock: it reads
    // hms-cpap's own database.
    const auto heads = headsFromJson((*res)["heads"]);
    std::vector<std::string> keys;
    std::vector<std::pair<long long, long long>> spans;
    for (const auto& h : heads) {
        keys.push_back(h.key);
        spans.emplace_back(h.start_ms, h.end_ms);
    }
    lk.unlock();
    const auto wanted = want ? want(keys, spans) : std::vector<std::string>{};
    lk.lock();
    if (wanted.empty()) return r;
    if (!c->open || active_ != c) return offline();

    // D5.4: the small oscar.db.
    const uint32_t wreq = next_req_++;
    Json::Value w(Json::objectValue);
    w["t"] = "cmd";
    w["cmd"] = "want";
    w["req_id"] = wreq;
    Json::Value ks(Json::arrayValue);
    for (const auto& k : wanted) ks.append(k);
    w["keys"] = ks;
    sendText(*c, w);
    const auto slice = await(lk, c, wreq);
    if (!slice) return offline();
    if ((*slice)["t"].asString() == "result") {   // the helper could not write it
        r.outcome = outcomeFromName((*slice)["outcome"].asString());
        r.message = (*slice)["message"].asString();
        return r;
    }
    std::string bytes = std::move(c->slices[wreq]);
    c->slices.erase(wreq);
    const bool ok = (*slice)["whole"].asBool() && bytes.size() == (*slice)["size"].asUInt64() &&
                    md5Hex(bytes) == (*slice)["md5"].asString();
    Json::Value ack(Json::objectValue);
    ack["t"] = "file_ack";
    ack["req_id"] = wreq;
    ack["ok"] = ok;
    sendText(*c, ack);
    if (!ok) {
        r.outcome = Outcome::Failed;
        r.message = "the helper's sessions arrived damaged";
        return r;
    }
    lk.unlock();
    std::error_code ec;
    fs::remove_all(slice_dir, ec);
    fs::create_directories(slice_dir, ec);
    {
        std::ofstream out(fs::path(slice_dir) / "oscar.db", std::ios::binary | std::ios::trunc);
        out.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
        if (!out) {
            r.outcome = Outcome::Failed;
            r.message = "cannot keep the helper's sessions in " + slice_dir;
            return r;
        }
    }
    r.slice_dir = slice_dir;
    return r;
}

}  // namespace hms_cpap::oscar::helper
