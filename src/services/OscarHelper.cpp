#include "services/OscarHelper.h"

#include "services/OscarCardReader.h"
#include "services/OscarDb.h"

#include <nlohmann/json.hpp>

#include <ctime>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>

namespace hms_cpap::oscar::helper {

namespace fs = std::filesystem;

namespace {

constexpr const char* kTag = "OSCAR helper: ";

std::optional<nlohmann::json> readJson(const std::string& path) {
    std::ifstream in(path, std::ios::binary);
    if (!in) return std::nullopt;
    try {
        return nlohmann::json::parse(in);
    } catch (...) {
        return std::nullopt;
    }
}

std::string str(const nlohmann::json& j, const char* k) {
    return j.contains(k) && j[k].is_string() ? j[k].get<std::string>() : std::string();
}

bool readAll(const fs::path& p, std::string& out) {
    std::ifstream in(p, std::ios::binary);
    if (!in) return false;
    std::ostringstream s;
    s << in.rdbuf();
    out = s.str();
    return true;
}

}  // namespace

// ── config.json ─────────────────────────────────────────────────────────────

HelperConfig loadHelperConfig(const std::string& config_path) {
    HelperConfig c;
    const auto j = readJson(config_path);
    if (!j || !j->is_object()) return c;
    if (j->contains("oscar_helper") && (*j)["oscar_helper"].is_object()) {
        const auto& h = (*j)["oscar_helper"];
        c.host       = str(h, "host");
        c.token      = str(h, "token");
        c.install_id = str(h, "install_id");
        c.hms_name   = str(h, "hms_name");
        if (h.contains("port") && h["port"].is_number_integer()) c.port = h["port"].get<int>();
        if (c.port <= 0 || c.port > 65535) c.port = kDefaultPort;
    }
    if (j->contains("oscar") && (*j)["oscar"].is_object()) {
        const auto& o = (*j)["oscar"];
        c.oscar.program  = str(o, "program");
        c.oscar.data_dir = str(o, "data_dir");
        if (!str(o, "profile").empty()) c.oscar.profile = str(o, "profile");
    }
    return c;
}

bool saveHelperConfig(const std::string& config_path, const HelperConfig& c, bool set_role) {
    nlohmann::json j = readJson(config_path).value_or(nlohmann::json::object());
    if (!j.is_object()) j = nlohmann::json::object();
    if (set_role) j["role"] = "oscar_helper";   // D1, F9: one role per install
    auto& h = j["oscar_helper"];
    h["host"]       = c.host;
    h["port"]       = c.port;
    h["token"]      = c.token;
    h["install_id"] = c.install_id;
    h["hms_name"]   = c.hms_name;
    j["oscar"]["program"]  = c.oscar.program;
    j["oscar"]["data_dir"] = c.oscar.data_dir;
    j["oscar"]["profile"]  = c.oscar.profile;
    std::error_code ec;
    fs::create_directories(fs::path(config_path).parent_path(), ec);
    const std::string tmp = config_path + ".tmp";
    {
        std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
        if (!out) return false;
        out << j.dump(2);
        if (!out) return false;
    }
    fs::rename(tmp, config_path, ec);
    return !ec;
}

std::string configRole(const std::string& config_path) {
    const auto j = readJson(config_path);
    return j && j->is_object() ? str(*j, "role") : "";
}

// ── the helper ──────────────────────────────────────────────────────────────

Helper::Helper(Options o) : opt_(std::move(o)), runner_(opt_.runner) {
    if (!runner_) runner_ = std::make_shared<SystemProcessRunner>();
    state_["state"] = "connecting";
}

Helper::~Helper() { stop(); }

void Helper::start() {
    std::lock_guard<std::mutex> lk(mu_);
    if (worker_.joinable()) return;
    stop_ = false;
    worker_ = std::thread([this] { run(); });
}

void Helper::stop() {
    {
        std::lock_guard<std::mutex> lk(mu_);
        stop_ = true;
    }
    cv_.notify_all();
    if (worker_.joinable()) worker_.join();
}

void Helper::connectAgain() {
    if (opt_.load && opt_.save) {
        HelperConfig c = opt_.load();
        c.token.clear();
        opt_.save(c);
    }
    {
        std::lock_guard<std::mutex> lk(mu_);
        ++gen_;
    }
    cv_.notify_all();
}

Json::Value Helper::state() const {
    std::lock_guard<std::mutex> lk(mu_);
    return state_;
}

std::string Helper::cardRoot(const std::string& install_id) const {
    return (fs::path(opt_.data_dir) / "oscar-helper" / (install_id.empty() ? "unknown" : install_id)).string();
}

void Helper::setState(const std::string& s, const std::string& message) {
    Json::Value now;
    {
        std::lock_guard<std::mutex> lk(mu_);
        state_["state"] = s;
        state_["message"] = message;
        now = state_;
    }
    if (opt_.on_state) opt_.on_state(now);
}

void Helper::sendText(CpapDashLink& link, const Json::Value& v) {
    if (!link.sendText(frame(v))) link_lost_ = true;
}

void Helper::run() {
    std::chrono::milliseconds backoff = opt_.backoff_min;
    for (;;) {
        uint64_t gen = 0;
        {
            std::lock_guard<std::mutex> lk(mu_);
            if (stop_) return;
            gen = gen_;
        }
        const End e = runOnce();
        std::optional<std::chrono::milliseconds> wait;
        switch (e) {
            case End::Stop:
                return;
            case End::Closed:
                backoff = opt_.backoff_min;
                wait = backoff;
                break;
            case End::Unreachable:
                wait = backoff;
                backoff = std::min(backoff * 2, opt_.backoff_max);
                break;
            case End::Refused:
            case End::Update:
            case End::NoAddress:
                break;   // until "Connect again" or a restart (a new address restarts the helper)
        }
        std::unique_lock<std::mutex> lk(mu_);
        const auto done = [&] { return stop_ || gen_ != gen; };
        if (wait) cv_.wait_for(lk, *wait, done);
        else cv_.wait(lk, done);
        if (stop_) return;
        if (gen_ != gen) backoff = opt_.backoff_min;
    }
}

Helper::End Helper::runOnce() {
    HelperConfig cfg = opt_.load ? opt_.load() : HelperConfig{};
    uint64_t gen = 0;
    {
        std::lock_guard<std::mutex> lk(mu_);
        gen = gen_;
        state_["host"] = cfg.host;
        state_["port"] = cfg.port;
        state_["hms_name"] = cfg.hms_name;
    }
    if (cfg.host.empty()) {
        setState("no_address");
        return End::NoAddress;
    }
    CpapDashEndpoint ep;
    ep.tls = false;
    ep.host = cfg.host;
    ep.port = static_cast<uint16_t>(cfg.port);
    ep.path = kRoute;
    ep.headers.emplace_back(kHeaderName, opt_.pc_name);
    auto link = opt_.factory ? opt_.factory(ep) : nullptr;
    if (!link) {
        setState("unreachable", "this build has no WebSocket client");
        return End::Unreachable;
    }
    setState("connecting");
    const bool had_token = !cfg.token.empty();
    const auto c = link->connect(cfg.token, "", "hms-cpap/" + opt_.version, opt_.connect_timeout);
    if (!c.ok) {
        if (c.http_status == 401 || c.http_status == 403) {
            Json::Value body;
            Json::CharReaderBuilder b;
            std::string err;
            std::istringstream in(c.http_body);
            Json::parseFromStream(b, in, &body, &err);
            const std::string other = body.isObject() ? body["helper"].asString() : "";
            {
                std::lock_guard<std::mutex> lk(mu_);
                state_["helper"] = other;
            }
            // D4: a token refused is this PC forgotten; no token refused is
            // another PC already reading.
            if (had_token) {
                std::cout << kTag << "hms-cpap at " << cfg.host << " forgot this PC" << std::endl;
                setState("forgotten");
            } else {
                std::cout << kTag << "hms-cpap at " << cfg.host << " already reads OSCAR on " << other << std::endl;
                setState("refused");
            }
            return End::Refused;
        }
        std::cerr << kTag << "hms-cpap at " << cfg.host << ":" << cfg.port << " unreachable: " << c.error
                  << std::endl;
        setState("unreachable", c.error);
        return End::Unreachable;
    }

    link_lost_ = false;
    incoming_.clear();
    {
        std::lock_guard<std::mutex> lk(mu_);
        state_["helper"] = "";
    }
    Json::Value hello(Json::objectValue);
    hello["t"] = "hello";
    hello["proto"] = kProto;
    hello["version"] = opt_.version;
    hello["name"] = opt_.pc_name;
    hello["files"] = toJson(cfg.install_id.empty() ? std::vector<FileStamp>{} : listCard(cardRoot(cfg.install_id)));
    sendText(*link, hello);

    End end = End::Closed;
    for (;;) {
        {
            std::lock_guard<std::mutex> lk(mu_);
            if (stop_) {
                end = End::Stop;
                break;
            }
            if (gen_ != gen) break;   // Connect again
        }
        const std::string st = state()["state"].asString();
        if (st == "update" || st == "refused") {
            end = st == "update" ? End::Update : End::Refused;
            break;
        }
        if (link_lost_) break;
        const auto f = link->next(std::chrono::milliseconds(200));
        if (!f) continue;
        if (f->kind == CpapDashLink::Frame::Kind::Ping) continue;
        if (f->kind == CpapDashLink::Frame::Kind::Closed) break;
        if (f->kind == CpapDashLink::Frame::Kind::Binary) {
            if (const auto ch = parseChunk(f->text); ch && ch->kind == kKindBytes) incoming_.add(*ch);
            continue;
        }
        if (const auto v = parse(f->text)) handle(*link, *v, cfg);
    }
    link->close();
    if (end == End::Closed) {
        const std::string st = state()["state"].asString();
        if (st != "update" && st != "refused") setState("connecting", "the connection ended");
    }
    return end;
}

void Helper::handle(CpapDashLink& link, const Json::Value& f, HelperConfig& cfg) {
    const std::string t = f["t"].asString();
    if (t == "paired") {
        cfg.token = f["token"].asString();   // D4: kept, never logged
        if (opt_.save) opt_.save(cfg);
        std::cout << kTag << "remembered by hms-cpap at " << cfg.host << std::endl;
        return;
    }
    if (t == "ack") {
        const auto& h = f["hms_cpap"];
        const std::string id = h["install_id"].asString();
        const bool changed = id != cfg.install_id || h["name"].asString() != cfg.hms_name;
        if (!id.empty()) cfg.install_id = id;
        cfg.hms_name = h["name"].asString();
        if (changed && opt_.save) opt_.save(cfg);
        {
            std::lock_guard<std::mutex> lk(mu_);
            state_["hms_name"] = cfg.hms_name;
        }
        std::cout << kTag << "connected to hms-cpap at " << cfg.host << std::endl;
        setState("connected");
        return;
    }
    if (t == "error") {
        const std::string e = f["error"].asString();
        if (e == "proto") {
            std::cerr << kTag << "hms-cpap at " << cfg.host << " speaks another protocol: update hms-cpap" << std::endl;
            setState("update");
        } else {
            {
                std::lock_guard<std::mutex> lk(mu_);
                state_["helper"] = f["helper"].asString();
            }
            setState("refused");
        }
        return;
    }
    if (t == "file_ready") {
        const uint32_t req = f["req_id"].asUInt();
        const std::string path = f["path"].asString();
        Json::Value ack(Json::objectValue);
        ack["t"] = "file_ack";
        ack["req_id"] = req;
        ack["ok"] = false;
        const bool whole = incoming_.count(req) == f["chunks"].asUInt();
        std::string bytes = incoming_.take(req);
        // D5.1: a P-SERIES relative path only, for the hms-cpap it reads for.
        if (!safeCardPath(path) || cfg.install_id.empty()) {
            std::cerr << kTag << "refused a file outside P-SERIES/: " << path << std::endl;
            sendText(link, ack);
            return;
        }
        if (!whole || bytes.size() != f["size"].asUInt64() || md5Hex(bytes) != f["md5"].asString()) {
            std::cerr << kTag << path << " arrived damaged" << std::endl;
            sendText(link, ack);
            return;
        }
        const fs::path root(cardRoot(cfg.install_id));
        const fs::path dst = root / fs::path(path);
        std::error_code ec;
        // A link anywhere on the way out of the copy is never followed.
        for (fs::path p = dst.parent_path(); p != root && p.has_relative_path(); p = p.parent_path()) {
            if (fs::is_symlink(p, ec)) {
                sendText(link, ack);
                return;
            }
        }
        fs::create_directories(dst.parent_path(), ec);
        if (fs::is_symlink(dst, ec)) fs::remove(dst, ec);
        const fs::path tmp = dst.string() + ".part";
        {
            std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
            out.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
            if (!out) {
                sendText(link, ack);
                return;
            }
        }
        fs::rename(tmp, dst, ec);
        if (ec) {
            sendText(link, ack);
            return;
        }
        setMtimeSeconds(dst.string(), f["mtime"].asInt64());
        ack["ok"] = true;
        sendText(link, ack);
        return;
    }
    if (t == "cmd" && f["cmd"].asString() == "import") {
        const uint32_t req = f["req_id"].asUInt();
        setState("importing");
        // D5.2: exactly the native reader's OSCAR half.
        const ImportRun run = runImport(cfg.oscar, cardRoot(cfg.install_id), *runner_);
        Json::Value r(Json::objectValue);
        r["t"]       = "result";
        r["req_id"]  = req;
        r["outcome"] = outcomeName(run.outcome);
        r["version"] = run.version;
        r["schema"]  = run.schema;
        r["message"] = run.message;
        std::vector<Head> heads;
        for (const auto& h : run.heads) heads.push_back({sessionKey(h), h.start_ms, h.end_ms});
        r["heads"] = headsJson(heads);
        {
            std::lock_guard<std::mutex> lk(mu_);
            state_["last_import_at"] = static_cast<Json::Int64>(std::time(nullptr));
            state_["last_outcome"] = outcomeName(run.outcome);
            state_["oscar_version"] = run.version;
        }
        sendText(link, r);
        std::cout << kTag << "import: " << outcomeName(run.outcome) << ", " << heads.size() << " session(s) in OSCAR"
                  << (run.message.empty() ? "" : " (" + run.message + ")") << std::endl;
        setState("connected", run.message);
        return;
    }
    if (t == "cmd" && f["cmd"].asString() == "want") {
        const uint32_t req = f["req_id"].asUInt();
        std::vector<std::string> keys;
        for (const auto& k : f["keys"])
            if (k.isString()) keys.push_back(k.asString());
        const auto failed = [&](Outcome o, const std::string& why) {
            Json::Value r(Json::objectValue);
            r["t"] = "result";
            r["req_id"] = req;
            r["outcome"] = outcomeName(o);
            r["message"] = why;
            sendText(link, r);
        };
        std::string why;
        auto db = OscarDb::open(cfg.oscar.data_dir, &why);
        if (!db) return failed(Outcome::Failed, why);
        const auto g = db->gate();
        if (!g.ok) return failed(Outcome::NotSupported, g.why);
        const fs::path slice = fs::path(opt_.data_dir) / "oscar-helper" / "slice" / "oscar.db";
        if (!db->writeSlice(cfg.oscar.profile, keys, slice.string(), &why)) return failed(Outcome::Failed, why);
        db.reset();
        std::string bytes;
        if (!readAll(slice, bytes)) return failed(Outcome::Failed, "the sessions could not be read back");
        std::error_code ec;
        fs::remove_all(slice.parent_path(), ec);
        const auto parts = chunks(req, bytes, opt_.chunk_bytes);
        for (const auto& p : parts)
            if (!link.sendBinary(p)) link_lost_ = true;
        Json::Value r(Json::objectValue);
        r["t"] = "slice_ready";
        r["req_id"] = req;
        r["chunks"] = static_cast<Json::UInt>(parts.size());
        r["size"] = static_cast<Json::UInt64>(bytes.size());
        r["md5"] = md5Hex(bytes);
        sendText(link, r);
        std::cout << kTag << "sent " << keys.size() << " session(s), " << bytes.size() << " bytes" << std::endl;
        return;
    }
    // file_ack of a slice, anything else: nothing to do.
}

}  // namespace hms_cpap::oscar::helper
