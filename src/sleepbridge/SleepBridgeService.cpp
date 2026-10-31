#include "sleepbridge/SleepBridgeService.h"

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>

namespace hms_cpap::sleepbridge {

namespace fs = std::filesystem;

namespace {

Json::Value stepJson(const StepReport& r) {
    Json::Value v(Json::objectValue);
    v["batches_fed"]     = r.batches_fed;
    v["records_fed"]     = r.records_fed;
    v["records_skipped"] = r.records_skipped;
    v["cards_fed"]       = r.cards_fed;
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

SleepBridgeService::SleepBridgeService(Options opt, RebuilderFactory factory)
    : opt_(std::move(opt)), factory_(std::move(factory)), store_(opt_.root) {
    // Asked once: a factory that answers nullptr is a build without the rebuild.
    if (factory_) available_ = factory_(RebuildStart{}) != nullptr;
    // Seconds since 2001: rises across restarts, below 2^31 until 2069.
    const std::time_t now = std::time(nullptr);
    next_req_ = static_cast<uint64_t>(now > 978307200 ? now - 978307200 : 0) + 1;
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
    std::unique_lock<std::mutex> lk(q_mu_);
    while (!stop_) {
        q_cv_.wait(lk, [&] { return stop_ || !dirty_.empty(); });
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
    if (!available_) return r;
    {
        std::lock_guard<std::mutex> lk(rebuild_mu_);
        auto& w = workers_[sender];
        if (!w) w = std::make_unique<RebuildWorker>(store_, sender, factory_);
        r = w->step();
    }
    if (r.written || r.removed || r.refused_paths || r.unreadable)
        std::cout << "[sleepbridge] " << sender << " rebuilt: " << r.written << " written, "
                  << r.complete_files << " complete, " << r.held_files << " held"
                  << (r.holding ? " (holding: " + r.holding_reason + ")" : std::string())
                  << std::endl;
    std::lock_guard<std::mutex> lk(mu_);
    auto& b     = bridges_[sender];
    b.last_step = r;
    b.stepped   = true;
    if (!r.machine.empty()) b.card_machine = r.machine;
    return r;
}

std::unique_ptr<Conversation> SleepBridgeService::open(const Admission& who, Outlet out) {
    uint64_t id;
    {
        std::lock_guard<std::mutex> lk(mu_);
        id         = next_conn_++;
        conns_[id] = Conn{who, std::move(out), false};
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
        if (fx.banked) b.last_batch = b.last_heard;

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
        if (fx.result) {
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
                std::cout << "[sleepbridge] " << sender << " " << c->cmd << " (req_id " << c->req_id
                          << ") answered: ok=" << (c->result.get("ok", false).asBool() ? "true" : "false")
                          << (c->result.isMember("error") ? " error=" + c->result["error"].asString()
                                                          : std::string())
                          << std::endl;
            }
        }
    }
    if (fx.hello) flush(sender);
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
        return refuse(400, "a Sleep Bridge takes stream, reboot, logs, restate or config_reset "
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
    }

    {
        std::lock_guard<std::mutex> lk(mu_);
        const bool seen = bridges_.count(sender) != 0;
        if (!seen) {
            const auto s = store_.senders();
            if (std::find(s.begin(), s.end(), sender) == s.end())
                return refuse(404, "no Sleep Bridge named \"" + sender + "\" has connected here");
        }
        auto& b = bridges_[sender];
        Command c;
        c.req_id       = next_req_++;
        c.cmd          = cmd;
        frame["req_id"] = static_cast<Json::UInt64>(c.req_id);
        c.frame        = frame;
        c.queued_at    = std::time(nullptr);
        b.commands.push_back(std::move(c));
        while (b.commands.size() > kCommandsKept) b.commands.erase(b.commands.begin());
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

Json::Value SleepBridgeService::commandJson(const Command& c) {
    Json::Value v(Json::objectValue);
    v["req_id"] = static_cast<Json::UInt64>(c.req_id);
    v["cmd"]    = c.cmd;
    Json::Value args(Json::objectValue);
    for (const auto& k : c.frame.getMemberNames())
        if (k != "t" && k != "cmd" && k != "req_id") args[k] = c.frame[k];
    v["args"]        = args;
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
        v["card_root"]  = store_.cardRoot(sender);
        // Which machine's card the root holds, and the card format to read
        // it as: a Luna's root is read as "bmc", a ResMed's as "resmed".
        v["card_machine"] = b.card_machine;
        v["card_format"]  = formatForMachine(b.card_machine);
        if (b.stepped) v["rebuild"] = stepJson(b.last_step);
        Json::Value cmds(Json::arrayValue);
        for (auto it = b.commands.rbegin(); it != b.commands.rend(); ++it) cmds.append(commandJson(*it));
        v["commands"] = cmds;
        list.append(v);
    }
    out["bridges"] = list;
    Json::Value names(Json::arrayValue);
    for (const auto& n : localCommands()) names.append(n);
    out["commands"] = names;   // what POST /api/sleepbridge/command takes
    return out;
}

}  // namespace hms_cpap::sleepbridge
