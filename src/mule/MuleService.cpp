#include "mule/MuleService.h"

#include "mule/PosixTz.h"
#include "services/As11StrWriter.h"
#include "sleepbridge/BatchStore.h"   // durableWrite

#include <cpapdash/parser/AirSense11Summary.h>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>

namespace hms_cpap::mule {

namespace fs = std::filesystem;

namespace {

std::string compact(const Json::Value& v) {
    Json::StreamWriterBuilder w;
    w["indentation"] = "";
    return Json::writeString(w, v);
}

std::optional<Json::Value> parseObject(const std::string& text) {
    Json::Value v;
    Json::CharReaderBuilder rb;
    std::istringstream ss(text);
    if (!Json::parseFromStream(rb, ss, &v, nullptr) || !v.isObject()) return std::nullopt;
    return v;
}

std::string errorFrame(const std::string& why) {
    Json::Value e(Json::objectValue);
    e["t"]     = "error";
    e["error"] = why;
    return compact(e);
}

bool hostChar(char c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '.' ||
           c == '-';
}

bool isDate(const std::string& s) {
    if (s.size() != 10 || s[4] != '-' || s[7] != '-') return false;
    for (size_t i = 0; i < s.size(); i++)
        if (i != 4 && i != 7 && (s[i] < '0' || s[i] > '9')) return false;
    return true;
}

bool writeText(const fs::path& p, const std::string& text) {
    std::string err;
    if (sleepbridge::durableWrite(p.string(), text.data(), text.size(), &err)) return true;
    std::cerr << "[mule] " << err << std::endl;
    return false;
}

// Two days before the newest day a Summary carries (the machine revises the
// day it is on), "YYYY-MM-DD"; "" when it carries none.
std::string fromAfter(const std::string& bytes) {
    using cpapdash::parser::AirSense11SummaryParser;
    const auto days = AirSense11SummaryParser::parse(reinterpret_cast<const uint8_t*>(bytes.data()),
                                                     bytes.size());
    if (days.empty()) return {};
    auto newest = days.front().day_start;
    for (const auto& d : days) newest = std::max(newest, d.day_start);
    const auto t = std::chrono::system_clock::to_time_t(newest - std::chrono::hours(48));
    std::tm tm{};
#ifdef _WIN32
    gmtime_s(&tm, &t);
#else
    gmtime_r(&t, &tm);
#endif
    char buf[16];
    std::snprintf(buf, sizeof(buf), "%04d-%02d-%02d", tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday);
    return buf;
}

}  // namespace

const std::vector<std::string>& localCommands() {
    static const std::vector<std::string> k = {"config",   "reboot",    "logs",          "restate",
                                               "reset",    "as11_pair", "as11_download",
                                               // SDD-065: the DreamStation 2 through the miner.
                                               "ds2_bond", "ds2_read"};
    return k;
}

bool validSerial(const std::string& s) {
    if (s.empty() || s.size() > 64 || s.front() == '.') return false;
    for (const char c : s)
        if (!hostChar(c) && c != '_') return false;
    return true;
}

bool validUnitHost(const std::string& h) {
    if (h.empty()) return true;   // the cloud
    if (h.size() > 63 || h.front() == '.' || h.front() == '-' || h.back() == '.' || h.back() == '-')
        return false;
    return std::all_of(h.begin(), h.end(), hostChar);
}

std::optional<FrameHeader> parseHeader(const std::string& f) {
    if (f.size() < kHeaderSize) return std::nullopt;
    const auto* p = reinterpret_cast<const uint8_t*>(f.data());
    FrameHeader h;
    h.req_id = p[0] | (p[1] << 8) | (p[2] << 16) | (static_cast<uint32_t>(p[3]) << 24);
    h.seq    = p[4] | (p[5] << 8) | (p[6] << 16) | (static_cast<uint32_t>(p[7]) << 24);
    h.flags  = static_cast<uint16_t>(p[8] | (p[9] << 8));
    h.kind   = static_cast<uint16_t>(p[10] | (p[11] << 8));
    return h;
}

std::string encodeHeader(const FrameHeader& h) {
    std::string s(kHeaderSize, '\0');
    for (int i = 0; i < 4; i++) {
        s[i]     = static_cast<char>((h.req_id >> (8 * i)) & 0xFF);
        s[4 + i] = static_cast<char>((h.seq >> (8 * i)) & 0xFF);
    }
    s[8]  = static_cast<char>(h.flags & 0xFF);
    s[9]  = static_cast<char>(h.flags >> 8);
    s[10] = static_cast<char>(h.kind & 0xFF);
    s[11] = static_cast<char>(h.kind >> 8);
    return s;
}

// ── the service ──

MuleService::MuleService(Options opt, std::shared_ptr<IAs11NightSink> sink)
    : opt_(std::move(opt)), sink_(std::move(sink)) {
    // Seconds since 2001: rises across restarts, below 2^31 until 2069, so a
    // late result for a command sent before a restart never lands on a new one.
    const std::time_t now = std::time(nullptr);
    next_req_ = static_cast<uint64_t>(now > 978307200 ? now - 978307200 : 0) + 1;
    // The units seen before this start are listed before they connect again.
    std::error_code ec;
    std::lock_guard<std::mutex> lk(mu_);
    for (const auto& d : fs::directory_iterator(opt_.root, ec))
        if (d.is_directory() && validSerial(d.path().filename().string()) &&
            fs::exists(d.path() / "unit.json"))
            unit(d.path().filename().string());
}

void MuleService::setLive(std::function<void()> night_changed, std::shared_ptr<LivePublisher> publisher) {
    std::lock_guard<std::mutex> lk(mu_);
    night_changed_ = std::move(night_changed);
    live_          = std::move(publisher);
}

std::string MuleService::unitDir(const std::string& serial) const {
    return (fs::path(opt_.root) / serial).string();
}
std::string MuleService::blocksDir(const std::string& serial) const {
    return (fs::path(opt_.root) / serial / "blocks").string();
}
std::string MuleService::machineRoot(const std::string& machine) const {
    // The night writer's folder for this machine (As11NightWriter::rootFor).
    return (fs::path(opt_.data_dir) / "as11" / (machine.empty() ? "unknown" : machine)).string();
}

MuleService::Unit& MuleService::unit(const std::string& serial) {
    Unit& u = units_[serial];
    if (u.loaded) return u;
    u.loaded = true;
    std::ifstream in(fs::path(unitDir(serial)) / "unit.json");
    if (!in) return u;
    std::stringstream ss;
    ss << in.rdbuf();
    const auto v = parseObject(ss.str());
    if (!v) return u;
    u.machine      = (*v).get("machine", "").asString();
    u.tz           = (*v).get("tz", "").asString();
    u.summary_from = (*v).get("summary_from", "2000-01-01").asString();
    u.streaming    = (*v).get("streaming", false).asBool();
    u.end_pending  = (*v).get("end_pending", false).asBool();
    u.first_seen   = static_cast<std::time_t>((*v).get("first_seen", 0).asInt64());
    u.fw           = (*v).get("fw", "").asString();
    u.miner_fw     = (*v).get("miner_fw", "").asString();
    u.mode         = (*v).get("mode", "").asString();
    if (!isDate(u.summary_from)) u.summary_from = "2000-01-01";
    return u;
}

void MuleService::save(const std::string& serial, const Unit& u) const {
    // The unit's own record: who it is and where its night stands. Never a
    // credential (it sends none).
    Json::Value v(Json::objectValue);
    v["serial"]       = serial;
    v["machine"]      = u.machine;
    v["tz"]           = u.tz;
    v["summary_from"] = u.summary_from;
    v["streaming"]    = u.streaming;
    v["end_pending"]  = u.end_pending;
    v["first_seen"]   = static_cast<Json::Int64>(u.first_seen);
    v["fw"]           = u.fw;
    v["miner_fw"]     = u.miner_fw;
    v["mode"]         = u.mode;
    writeText(fs::path(unitDir(serial)) / "unit.json", compact(v));
}

uint64_t MuleService::open(const std::string& serial, Outlet out, const std::string& peer_ip) {
    std::lock_guard<std::mutex> lk(mu_);
    const uint64_t id = next_conn_++;
    conns_[id]        = Conn{serial, std::move(out), peer_ip, false};
    return id;
}

MuleService::Brief MuleService::brief(const std::string& serial) const {
    Brief b;
    std::lock_guard<std::mutex> lk(mu_);
    const auto it = units_.find(serial);
    if (it == units_.end()) return b;
    const Unit& u = it->second;
    b.known      = true;
    b.connected  = u.sessions > 0;
    b.online     = u.sessions > 0 && std::time(nullptr) - u.last_heard < kSilenceSec;
    b.night_open = u.streaming || u.end_pending;
    b.ip         = u.ip;
    b.fw         = u.fw;
    b.miner_fw   = u.miner_fw;
    b.machine    = u.machine;
    b.hellos     = u.hellos;
    b.last_heard = u.last_heard;
    b.state      = u.state;
    for (const auto& c : u.commands)
        if (c.state == "queued" || (c.state == "sent" && c.conn == u.live))
            b.unanswered.push_back(c.cmd);
    return b;
}

void MuleService::send(uint64_t conn, const std::string& text) {
    Outlet out;
    {
        std::lock_guard<std::mutex> lk(mu_);
        const auto c = conns_.find(conn);
        if (c == conns_.end() || !c->second.out) return;
        out = c->second.out;
    }
    out(text);
}

void MuleService::closed(uint64_t conn) {
    std::lock_guard<std::mutex> lk(mu_);
    const auto c = conns_.find(conn);
    if (c == conns_.end()) return;
    const Conn gone = c->second;
    conns_.erase(c);
    if (!gone.session) return;   // a test hello, or nothing said
    auto it = units_.find(gone.serial);
    if (it != units_.end()) {
        if (it->second.sessions > 0) --it->second.sessions;
        if (it->second.live == conn) it->second.live = 0;
    }
    std::cout << "[mule] " << gone.serial << " disconnected" << std::endl;
}

void MuleService::onText(uint64_t conn, const std::string& text) {
    std::string serial;
    {
        std::lock_guard<std::mutex> lk(mu_);
        const auto c = conns_.find(conn);
        if (c == conns_.end()) return;
        serial = c->second.serial;
    }
    const auto v = parseObject(text);
    if (!v || !(*v)["t"].isString()) {
        send(conn, errorFrame("a text frame is one JSON object with a string \"t\""));
        return;
    }
    const std::string t = (*v)["t"].asString();
    if (t == "hello") return hello(conn, serial, *v);
    if (t == "state") {
        if ((*v)["state"].isObject()) stateOf(serial, (*v)["state"]);
        return;
    }
    if (t == "result") {
        result(serial, *v);
        flush(serial);
        return;
    }
    send(conn, errorFrame("unknown frame type \"" + t + "\""));
}

void MuleService::hello(uint64_t conn, const std::string& serial, const Json::Value& f) {
    Json::Value ack(Json::objectValue);
    ack["t"]           = "ack";
    ack["server_time"] = static_cast<Json::Int64>(std::time(nullptr));
    ack["ping_s"]      = kPingSec;

    // A test of this address (a unit about to switch here): the ack and
    // nothing else. It is not the unit's session, and nothing of it is kept.
    if (f["test"].isBool() && f["test"].asBool()) {
        send(conn, compact(ack));
        std::cout << "[mule] " << serial << " test hello answered" << std::endl;
        return;
    }
    const std::string mode = f.get("mode", "").asString();
    // SDD-065: a miner in DreamStation 2 mode reads no card; the mule's hello
    // still says "card" (only AirSense 11 mode has its own name), and its
    // state carries ds2.enabled.
    const bool ds2 = mode == "ds2_ble" || (f["state"].isObject() && f["state"]["ds2"].isObject() &&
                                           f["state"]["ds2"].get("enabled", false).asBool());
    if (mode == "card" && !ds2) {
        send(conn, errorFrame("this unit reads a card: use Home network mode"));
        std::cout << "[mule] " << serial << " is in card mode: refused" << std::endl;
        return;
    }
    send(conn, compact(ack));
    bool first = false;
    {
        std::lock_guard<std::mutex> lk(mu_);
        auto c = conns_.find(conn);
        if (c == conns_.end()) return;
        Unit& u = unit(serial);
        if (!c->second.session) {
            c->second.session = true;
            ++u.sessions;
        }
        const std::time_t now = std::time(nullptr);
        first        = u.first_seen == 0;
        if (first) u.first_seen = now;
        u.last_heard  = now;
        u.live        = conn;
        u.ip          = c->second.ip;
        ++u.hellos;
        u.fw          = f.get("fw", "").asString();
        u.miner_fw    = f.get("miner_fw", "").asString();
        u.mode        = ds2 ? "ds2_ble" : mode;
        u.boot_reason = f.get("boot_reason", "").asString();
        save(serial, u);
    }
    std::cout << "[mule] " << serial << (first ? " connected for the first time" : " connected")
              << " (fw " << f.get("fw", "?").asString() << ", miner " << f.get("miner_fw", "?").asString()
              << ")" << std::endl;
    if (f["state"].isObject()) stateOf(serial, f["state"]);
    flush(serial);
    std::function<void(const std::string&, bool)> observer;
    {
        std::lock_guard<std::mutex> lk(mu_);
        observer = observer_;
    }
    if (observer) observer(serial, true);
}

void MuleService::applyClock(const std::string& serial, Unit& u) {
    (void)serial;
    if (u.machine.empty() || u.tz.empty()) return;
    const std::string key = u.machine + "|" + u.tz;
    if (u.clock_for == key) return;
    auto* writer = dynamic_cast<As11NightWriter*>(sink_.get());
    if (!writer) return;
    if (auto clock = posixTzClock(u.tz)) {
        writer->setClock(u.machine, *clock);
        u.clock_for = key;
    } else {
        std::cerr << "[mule] " << serial << " tz \"" << u.tz
                  << "\" is not a POSIX zone this reads: the host's clock names the night" << std::endl;
        u.clock_for = key;
    }
}

void MuleService::stateOf(const std::string& serial, const Json::Value& state) {
    bool landed = false, went_on = false, went_off = false;
    // The stream's state on this run's first word from the unit, said live
    // even when it did not change: `streaming` outlives a restart, and a night
    // that ran through one has no start to report.
    std::optional<bool> first_state;
    std::string machine;
    {
        std::lock_guard<std::mutex> lk(mu_);
        Unit& u      = unit(serial);
        u.last_heard = std::time(nullptr);
        u.state      = state;
        u.state.removeMember("resp_events");
        bool dirty = false;
        if (state["tz"].isString() && state["tz"].asString() != u.tz) {
            u.tz  = state["tz"].asString();
            dirty = true;
        }
        // The miner's version rides every state: after a miner update it
        // changes here before the mule's next hello.
        if (state["miner_fw_version"].isString() && !state["miner_fw_version"].asString().empty() &&
            state["miner_fw_version"].asString() != u.miner_fw) {
            u.miner_fw = state["miner_fw_version"].asString();
            dirty      = true;
        }
        applyClock(serial, u);
        const Json::Value& a = state["as11"];
        if (a.isObject() && a["streaming"].isBool()) {
            const bool streaming = a["streaming"].asBool();
            const int64_t buffered = a["stream_buffered"].isNumeric() ? a["stream_buffered"].asInt64() : 0;
            if (streaming) {
                if (!u.streaming || u.end_pending) dirty = true;
                if (!u.streaming) went_on = true;
                u.streaming   = true;
                u.end_pending = false;
            } else if (u.streaming) {
                u.streaming   = false;
                u.end_pending = true;
                dirty         = true;
                went_off      = true;
                std::cout << "[mule] " << serial << " stream ended" << std::endl;
            }
            // The night has landed once the stream is over and the unit holds
            // none of it any more.
            if (u.end_pending && !streaming && buffered == 0) {
                u.end_pending = false;
                landed        = true;
                dirty         = true;
            }
            if (!u.therapy_said) {
                u.therapy_said = true;
                if (!went_on && !went_off) first_state = streaming;
            }
        }
        if (dirty) save(serial, u);
        machine = u.machine;
    }
    // Therapy on and off, live, before the events the same frame carries.
    if (live_ && first_state) live_->therapy(serial, machine, *first_state, /*restated=*/true);
    if (live_ && went_on) live_->therapy(serial, machine, true);
    if (state["resp_events"].isArray()) feedEvents(serial, state["resp_events"]);
    if (live_ && went_off) live_->therapy(serial, machine, false);
    if ((went_on || went_off) && night_changed_) night_changed_();
    if (landed) nightLanded(serial);
    std::function<void(const std::string&, bool)> observer;
    {
        std::lock_guard<std::mutex> lk(mu_);
        observer = observer_;
    }
    if (observer) observer(serial, false);
}

void MuleService::setObserver(std::function<void(const std::string& serial, bool hello)> observer) {
    std::lock_guard<std::mutex> lk(mu_);
    observer_ = std::move(observer);
}

devices::Recording MuleService::recording(const std::string& serial) const {
    devices::Recording r;
    std::lock_guard<std::mutex> lk(mu_);
    const auto it = units_.find(serial);
    if (it == units_.end()) {
        r.state  = "unknown";
        r.reason = "the unit has not been heard from since hms-cpap started";
        return r;
    }
    const Unit& u = it->second;
    if (u.streaming) {
        r.state  = "yes";
        r.reason = "the unit is streaming the machine's night";
    } else if (u.end_pending) {
        r.state  = "yes";
        r.reason = "the night the unit streamed has not landed here yet";
    } else if (u.last_heard == 0) {
        r.state  = "unknown";
        r.reason = "the unit has not been heard from since hms-cpap started";
    }
    return r;
}

CommandAnswer MuleService::cancelCommand(uint64_t req_id) {
    CommandAnswer a;
    std::lock_guard<std::mutex> lk(mu_);
    for (auto& [serial, u] : units_) {
        auto c = std::find_if(u.commands.begin(), u.commands.end(),
                              [&](const Command& k) { return k.req_id == req_id; });
        if (c == u.commands.end()) continue;
        if (c->state != "queued") {
            a.status          = 409;
            a.body["ok"]      = false;
            a.body["error"]   = "the command was already sent to the unit; it cannot be taken back";
            a.body["command"] = commandJson(*c);
            return a;
        }
        std::cout << "[mule] " << serial << " " << c->cmd << " (req_id " << req_id
                  << ") cancelled before it was sent" << std::endl;
        u.commands.erase(c);
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

CommandAnswer MuleService::forget(const std::string& serial) {
    CommandAnswer a;
    std::lock_guard<std::mutex> lk(mu_);
    const auto it = units_.find(serial);
    if (it != units_.end() && it->second.sessions > 0) {
        a.status        = 409;
        a.body["ok"]    = false;
        a.body["error"] = "this device is connected";
        return a;
    }
    size_t dropped = 0;
    if (it != units_.end()) {
        for (const auto& c : it->second.commands)
            if (c.state == "queued") ++dropped;
        units_.erase(it);
    }
    std::cout << "[mule] " << serial << " forgotten (" << dropped
              << " queued command(s) dropped; its blocks and nights stay)" << std::endl;
    a.body["ok"]             = true;
    a.body["dropped_queued"] = static_cast<Json::UInt64>(dropped);
    return a;
}

void MuleService::feedEvents(const std::string& serial, const Json::Value& events) {
    std::vector<as11::RespEvent> todo;
    std::string machine;
    {
        std::lock_guard<std::mutex> lk(mu_);
        Unit& u = unit(serial);
        for (const auto& ev : events) {
            if (!ev.isObject()) continue;
            if (u.machine.empty()) {
                u.pending_events.push_back(ev);   // until a block names the machine
                continue;
            }
            as11::RespEvent e;
            e.event           = ev.get("event", "").asString();
            e.reportTime      = ev.get("reportTime", "").asString();
            e.durationSeconds = ev.get("durationSeconds", -1).asInt();
            e.backdateSeconds = ev.get("backdateSeconds", -1).asInt();
            todo.push_back(std::move(e));
        }
        u.events += todo.size();
        machine = u.machine;
    }
    for (const auto& e : todo) {
        sink_->onEvent(machine, e);                       // the EVE, at once
        if (live_) live_->event(serial, machine, e);      // the broker, at once
    }
    if (!todo.empty() && night_changed_) night_changed_();   // the database and the UI
}

void MuleService::onBinary(uint64_t conn, const std::string& frame) {
    std::string serial;
    {
        std::lock_guard<std::mutex> lk(mu_);
        const auto c = conns_.find(conn);
        if (c == conns_.end()) return;
        serial = c->second.serial;
    }
    const auto h = parseHeader(frame);
    if (!h) {
        send(conn, errorFrame("a binary frame starts with the 12-byte header"));
        return;
    }
    if (h->kind == kKindBlock) return block(conn, serial, *h, frame);
    if (h->kind == kKindAs11 || h->kind == kKindDs2) return pullBytes(serial, *h, frame);
    std::cout << "[mule] " << serial << " binary kind " << h->kind << " ignored" << std::endl;
}

void MuleService::block(uint64_t conn, const std::string& serial, const FrameHeader&,
                        const std::string& frame) {
    const auto* body = reinterpret_cast<const uint8_t*>(frame.data()) + kHeaderSize;
    const size_t n   = frame.size() - kHeaderSize;
    const auto b     = as11::decodeCds1(body, n);
    int64_t start    = 0;
    if (b) {
        start = b->start_ms;
    } else if (n >= 16 && std::memcmp(body, "CDS1", 4) == 0) {
        // Not a block this reads, but it names its start: kept aside and
        // acked, so one bad block cannot hold the unit's stream up for good.
        for (int i = 7; i >= 0; i--) start = (start << 8) | body[8 + i];
    }
    if (start <= 0) {
        send(conn, errorFrame("a stream block is one CDS1 block"));
        return;
    }

    std::vector<Json::Value> pending;
    std::string machine;
    {
        std::lock_guard<std::mutex> lk(mu_);
        Unit& u = unit(serial);
        if (b && !b->serial.empty() && b->serial != u.machine) {
            u.machine = b->serial;
            applyClock(serial, u);
            save(serial, u);
            std::cout << "[mule] " << serial << " streams the AirSense 11 " << u.machine << std::endl;
        }
        pending.swap(u.pending_events);
        machine = u.machine;
    }

    // On disk first: the unit frees its copy on block_ack.
    const fs::path path = fs::path(blocksDir(serial)) / (std::to_string(start) + (b ? ".cds1" : ".bad"));
    std::error_code ec;
    const bool resent = fs::exists(path, ec);
    if (!resent) {
        std::string err;
        if (!sleepbridge::durableWrite(path.string(), body, n, &err)) {
            std::cerr << "[mule] " << serial << " block " << start << " not stored: " << err << std::endl;
            send(conn, errorFrame("the block could not be stored here"));
            return;   // no ack: the unit sends it again
        }
    }
    if (b) {
        sink_->onBlock(*b);   // the writer keeps a block once, by its start
        if (live_) live_->block(serial, b->start_ms, b->endMs());
    }
    if (!pending.empty()) {
        Json::Value arr(Json::arrayValue);
        for (auto& e : pending) arr.append(e);
        feedEvents(serial, arr);
    }
    {
        std::lock_guard<std::mutex> lk(mu_);
        Unit& u      = unit(serial);
        u.last_block = u.last_heard = std::time(nullptr);
        if (resent) ++u.blocks_resent; else ++u.blocks_stored;
    }
    Json::Value ack(Json::objectValue);
    ack["t"]        = "block_ack";
    ack["start_ms"] = static_cast<Json::Int64>(start);
    send(conn, compact(ack));
    // The night grew: the host's sync picks the new seconds up now.
    if (b && !resent && night_changed_) night_changed_();
}

void MuleService::pullBytes(const std::string& serial, const FrameHeader& h, const std::string& frame) {
    std::lock_guard<std::mutex> lk(mu_);
    Unit& u = unit(serial);
    auto c  = std::find_if(u.commands.begin(), u.commands.end(), [&](const Command& k) {
        return static_cast<uint32_t>(k.req_id) == h.req_id;
    });
    if (c == u.commands.end()) {
        std::cout << "[mule] " << serial << " bytes for unknown req_id " << h.req_id << ", ignored"
                  << std::endl;
        return;
    }
    if (h.seq == 0 && !c->bytes_done) c->bytes.clear();   // a pull sent again starts over
    const size_t add = frame.size() - kHeaderSize;
    if (c->bytes.size() + add > kPullMax) {
        c->bytes_cut = true;
    } else {
        c->bytes.append(frame, kHeaderSize, add);
    }
    if (h.flags & 1) c->bytes_done = true;
}

void MuleService::result(const std::string& serial, const Json::Value& f) {
    const Json::Value& req = f["req_id"];
    if (!req.isNumeric() || req.asDouble() < 0) {
        std::cout << "[mule] " << serial << " result without a req_id, ignored" << std::endl;
        return;
    }
    const uint64_t id = static_cast<uint64_t>(req.asDouble());
    std::optional<Answered>              answered;
    std::function<void(const Answered&)> hook;
    {
        std::lock_guard<std::mutex> lk(mu_);
        Unit& u = unit(serial);
        auto c  = std::find_if(u.commands.begin(), u.commands.end(),
                               [&](const Command& k) { return k.req_id == id; });
        if (c == u.commands.end()) {
            std::cout << "[mule] " << serial << " result for unknown req_id " << id << ", ignored" << std::endl;
            return;
        }
        Json::Value r(Json::objectValue);
        r["ok"] = f.get("ok", false).asBool();
        if (f.isMember("error")) r["error"] = f["error"];
        if (f.isMember("result")) r["result"] = f["result"];
        c->state       = "answered";
        c->answered_at = u.last_heard = std::time(nullptr);
        c->result      = r;
        std::cout << "[mule] " << serial << " " << c->cmd << " (req_id " << c->req_id
                  << ") answered: ok=" << (r["ok"].asBool() ? "true" : "false")
                  << (r.isMember("error") ? " error=" + r["error"].asString() : std::string()) << std::endl;
        if (c->cmd == "as11_download") afterPull(serial, *c);
        // SDD-065: the DreamStation 2's answers go to whoever keeps its records.
        if ((c->cmd == "ds2_read" || c->cmd == "ds2_bond") && answer_hook_) {
            Answered a;
            a.serial     = serial;
            a.req_id     = c->req_id;
            a.cmd        = c->cmd;
            a.result     = c->result;
            a.bytes_done = c->bytes_done;
            a.bytes_cut  = c->bytes_cut;
            a.automatic  = c->automatic;
            for (const auto& k : c->frame.getMemberNames())
                if (k != "t" && k != "cmd" && k != "req_id") a.args[k] = c->frame[k];
            c->bytes_kept = c->bytes.size();
            a.bytes.swap(c->bytes);   // handed over, not held twice
            answered = std::move(a);
            hook     = answer_hook_;
        }
    }
    if (answered && hook) hook(*answered);
}

void MuleService::setAnswerHook(std::function<void(const Answered&)> hook) {
    std::lock_guard<std::mutex> lk(mu_);
    answer_hook_ = std::move(hook);
}

void MuleService::setOutcome(uint64_t req_id, const std::string& outcome) {
    std::lock_guard<std::mutex> lk(mu_);
    for (auto& [serial, u] : units_)
        for (auto& c : u.commands)
            if (c.req_id == req_id) {
                c.outcome = outcome;
                return;
            }
}

void MuleService::afterPull(const std::string& serial, Command& c) {
    const std::string spool = c.frame.get("spool", "Summary").asString();
    const std::string from  = c.frame.get("from", "").asString();
    Unit& u                 = units_[serial];
    auto  done              = [&](const std::string& what) {
        c.outcome = what;
        std::cout << "[mule] " << serial << " " << spool << ": " << what << std::endl;
    };
    if (!c.result.get("ok", false).asBool()) return done("not read: " + c.result.get("error", "").asString());
    if (c.bytes_cut) return done("more than " + std::to_string(kPullMax) + " bytes: not kept");
    if (c.bytes.empty()) return done("no bytes came");
    if (u.machine.empty()) return done("not kept: no block has named the machine yet");
    const std::string root = machineRoot(u.machine);

    if (spool == "Identification") {
        return done(writeIdentification(root, c.bytes) ? "Identification.json kept"
                                                       : "Identification.json not written");
    }
    if (spool == "Settings") {
        const fs::path p = fs::path(root) / "AS11" / ("settings-" + from + ".as11set");
        return done(writeText(p, c.bytes) ? "settings history kept (" + std::to_string(c.bytes.size()) + " B)"
                                          : "settings history not written");
    }
    if (!keepSummary(root, from, std::vector<uint8_t>(c.bytes.begin(), c.bytes.end())))
        return done("Summary not written");
    const std::string next = fromAfter(c.bytes);
    if (!next.empty() && next > u.summary_from) {
        u.summary_from = next;
        save(serial, u);
    }
    const auto str = writeStrFromSummaries(root, strLayoutDir(opt_.data_dir));
    done(str.written ? "Summary kept; STR.edf " +
                           (str.created ? "created, " + std::to_string(str.total) + " day(s)"
                                        : std::to_string(str.appended) + " day(s) appended, " +
                                              std::to_string(str.replaced) + " revised, " +
                                              std::to_string(str.total) + " in the file")
                     : "Summary kept; STR.edf unchanged: " + str.why);
}

void MuleService::nightLanded(const std::string& serial) {
    std::string machine;
    {
        std::lock_guard<std::mutex> lk(mu_);
        machine = unit(serial).machine;
    }
    if (machine.empty()) {
        std::cout << "[mule] " << serial << " stream over, but no block named the machine" << std::endl;
        return;
    }
    std::cout << "[mule] " << serial << " night landed: completing " << machine << "'s files" << std::endl;
    sink_->onStreamEnd(machine);
    {
        std::lock_guard<std::mutex> lk(mu_);
        Unit& u = unit(serial);
        auto  owed = [&](const std::string& spool) {
            for (const auto& c : u.commands)
                if (c.automatic && c.state != "answered" && c.frame.get("spool", "").asString() == spool)
                    return false;
            return true;
        };
        auto pull = [&](const std::string& spool) {
            if (!owed(spool)) return;
            Json::Value f(Json::objectValue);
            f["from"]  = u.summary_from;
            f["spool"] = spool;
            enqueue(u, "as11_download", f, true);
        };
        std::error_code ec;
        if (!fs::exists(fs::path(machineRoot(machine)) / "Identification.json", ec)) pull("Identification");
        pull("Summary");
        pull("Settings");
    }
    flush(serial);
}

uint64_t MuleService::enqueue(Unit& u, const std::string& cmd, Json::Value frame, bool automatic) {
    Command c;
    c.req_id        = next_req_++;
    c.cmd           = cmd;
    frame["t"]      = "cmd";
    frame["cmd"]    = cmd;
    frame["req_id"] = static_cast<Json::UInt64>(c.req_id);
    c.frame         = frame;
    c.automatic     = automatic;
    c.queued_at     = std::time(nullptr);
    u.commands.push_back(std::move(c));
    while (u.commands.size() > kCommandsKept) {
        // The oldest answered goes first; a queued one is never dropped.
        auto old = std::find_if(u.commands.begin(), u.commands.end(),
                                [](const Command& k) { return k.state != "queued"; });
        if (old == u.commands.end()) break;
        u.commands.erase(old);
    }
    return u.commands.back().req_id;
}

void MuleService::flush(const std::string& serial) {
    // One command in flight per unit, the socket's rule: the next goes when
    // the last is answered (or never will be: reboot and reset restart the
    // unit, and a command sent on an earlier session no longer holds this one).
    uint64_t    req = 0;
    std::string text;
    Outlet      out;
    {
        std::lock_guard<std::mutex> lk(mu_);
        auto it = units_.find(serial);
        if (it == units_.end() || !it->second.live) return;
        Unit& u = it->second;
        auto  c = conns_.find(u.live);
        if (c == conns_.end() || !c->second.out) return;
        const std::time_t now = std::time(nullptr);
        for (const auto& k : u.commands)
            if (k.state == "sent" && k.conn == u.live && now - k.sent_at < kInFlightSec &&
                k.cmd != "reboot" && k.cmd != "reset")
                return;
        auto next = std::find_if(u.commands.begin(), u.commands.end(),
                                 [](const Command& k) { return k.state == "queued"; });
        if (next == u.commands.end()) return;
        next->state   = "sent";
        next->conn    = u.live;
        next->sent_at = now;
        req           = next->req_id;
        text          = compact(next->frame);
        out           = c->second.out;
        std::cout << "[mule] " << serial << " " << next->cmd << " sent (req_id " << req << ")"
                  << (next->automatic ? " after the night" : "") << std::endl;
    }
    if (out(text)) return;
    std::lock_guard<std::mutex> lk(mu_);
    for (auto& k : units_[serial].commands)
        if (k.req_id == req && k.state == "sent") k.state = "queued";   // waits for the next session
}

CommandAnswer MuleService::queueCommand(const std::string& serial, const Json::Value& req, bool automatic) {
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
        return refuse(400, "a Mule & Miner takes config (hms_url, hms_port, as11_enabled, o2_enabled, "
                           "ble_machine), reboot, logs, restate, reset, as11_pair, as11_download, ds2_bond or "
                           "ds2_read from hms-cpap, not \"" + cmd + "\"");

    Json::Value f(Json::objectValue);
    if (cmd == "config") {
        // The stream's target: hms_url, hms_port, one or both. "" sends the
        // unit back to the cloud; the port alone changes only the port of the
        // hms-cpap the unit already points at (8894 when never set).
        // The two Bluetooth switches: as11_enabled (the machine's own
        // Bluetooth mode) and o2_enabled (the O2 ring), booleans, each one on
        // its own. The unit relays either to the miner, which stores it and
        // restarts; the unit's next state shows what applied.
        // SDD-065: ble_machine ("as11", "ds2" or "") is the machine the miner
        // reads over Bluetooth, on its own too; it supersedes as11_enabled.
        static const std::vector<std::string> keys = {"hms_url", "hms_port", "as11_enabled", "o2_enabled",
                                                      "ble_machine"};
        Json::Value cfg = req["config"].isObject() ? req["config"] : Json::Value(Json::objectValue);
        if (!req["config"].isObject())
            for (const auto& k : keys)
                if (req.isMember(k)) cfg[k] = req[k];
        for (const auto& k : cfg.getMemberNames())
            if (std::find(keys.begin(), keys.end(), k) == keys.end())
                return refuse(400, "config takes {\"hms_url\": address, \"hms_port\": port} (one or both), "
                                   "{\"as11_enabled\": true|false}, {\"o2_enabled\": true|false} or "
                                   "{\"ble_machine\": \"as11\"|\"ds2\"|\"\"}, and no other key");
        if (cfg.empty())
            return refuse(400, "config takes hms_url, hms_port or both; or as11_enabled; or o2_enabled; or ble_machine");
        const bool target = cfg.isMember("hms_url") || cfg.isMember("hms_port");
        const bool as11   = cfg.isMember("as11_enabled");
        const bool o2     = cfg.isMember("o2_enabled");
        const bool bm     = cfg.isMember("ble_machine");
        if ((target ? 1 : 0) + (as11 ? 1 : 0) + (o2 ? 1 : 0) + (bm ? 1 : 0) > 1)
            return refuse(400, "config sets one thing at a time: the target (hms_url, hms_port), as11_enabled, "
                               "o2_enabled, or ble_machine");
        if (as11 && !cfg["as11_enabled"].isBool()) return refuse(400, "as11_enabled is true or false");
        if (o2 && !cfg["o2_enabled"].isBool()) return refuse(400, "o2_enabled is true or false");
        if (bm && !(cfg["ble_machine"].isString() &&
                    (cfg["ble_machine"].asString() == "as11" || cfg["ble_machine"].asString() == "ds2" ||
                     cfg["ble_machine"].asString().empty())))
            return refuse(400, "ble_machine is \"as11\", \"ds2\" or \"\" (none)");
        if (cfg.isMember("hms_url") && (!cfg["hms_url"].isString() || !validUnitHost(cfg["hms_url"].asString())))
            return refuse(400, "hms_url is an IP address or hostname only (e.g. 192.168.1.20), with "
                               "no scheme, port or path; \"\" for the cloud. The port is hms_port");
        if (cfg.isMember("hms_port") &&
            (!cfg["hms_port"].isInt() || cfg["hms_port"].asInt() < 1 || cfg["hms_port"].asInt() > 65535))
            return refuse(400, "hms_port is a number from 1 to 65535 (8894 when never set)");
        f["config"] = cfg;
    } else if (cmd == "reboot") {
        if (req.isMember("target")) {
            const std::string t = req["target"].isString() ? req["target"].asString() : "";
            if (t != "mule" && t != "miner" && t != "both")
                return refuse(400, "reboot's target is mule, miner or both");
            f["target"] = t;
        }
    } else if (cmd == "logs") {
        int n = 200;
        if (req.isMember("n")) {
            if (!req["n"].isInt() || req["n"].asInt() < 1 || req["n"].asInt() > 5000)
                return refuse(400, "n is a number of log lines, 1 to 5000");
            n = req["n"].asInt();
        }
        f["n"] = n;
    } else if (cmd == "as11_pair") {
        // {} or start opens the exchange, code confirms it, cancel drops it.
        if (req.isMember("code")) {
            const std::string code = req["code"].isString() ? req["code"].asString() : "";
            if (code.empty() || code.size() > 8 ||
                !std::all_of(code.begin(), code.end(), [](char ch) { return ch >= '0' && ch <= '9'; }))
                return refuse(400, "code is the digits the machine shows");
            f["code"] = code;
        } else if (req.get("cancel", false).asBool()) {
            f["cancel"] = true;
        } else {
            f["start"] = true;
        }
    } else if (cmd == "as11_download") {
        const std::string from  = req.get("from", "").asString();
        const std::string spool = req.get("spool", "Summary").asString();
        if (!isDate(from)) return refuse(400, "as11_download needs from: YYYY-MM-DD");
        if (spool != "Summary" && spool != "Identification" && spool != "Settings")
            return refuse(400, "spool is Summary, Identification or Settings");
        f["from"]  = from;
        f["spool"] = spool;
    } else if (cmd == "ds2_bond") {
        // SDD-065 (push-c3 SDD-043 D5): start shows a code on the machine and
        // in the unit's answer; confirm carries that code back; cancel; forget.
        const std::string action = req.get("action", "").asString();
        if (action != "start" && action != "confirm" && action != "cancel" && action != "forget")
            return refuse(400, "ds2_bond's action is start, confirm, cancel or forget");
        f["action"] = action;
        if (action == "confirm") {
            const std::string code = req["code"].isString() ? req["code"].asString() : "";
            if (code.size() != 6 ||
                !std::all_of(code.begin(), code.end(), [](char ch) { return ch >= '0' && ch <= '9'; }))
                return refuse(400, "confirm carries the six digits the unit showed");
            f["code"] = code;
        }
        for (const char* k : {"just_works", "self_confirm"}) {
            if (!req.isMember(k)) continue;
            if (action != "start" || !req[k].isBool()) return refuse(400, std::string(k) + " is true or false, on start only");
            if (req[k].asBool()) f[k] = true;
        }
    } else if (cmd == "ds2_read") {
        // {machine?, streams?: {"1".."4": "<20 hex>" | ""}, max_records?}:
        // the bookmarks are this host's, so the read resumes where the last
        // one stopped. Built by the DreamStation 2 reads (MuleDs2), not typed.
        if (req.isMember("machine")) {
            const std::string m = req["machine"].isString() ? req["machine"].asString() : "#";
            if (m.size() > 16 || !std::all_of(m.begin(), m.end(), [](char ch) {
                    return (ch >= '0' && ch <= '9') || (ch >= 'A' && ch <= 'Z') || (ch >= 'a' && ch <= 'z');
                }))
                return refuse(400, "machine is the DreamStation's serial: up to 16 letters and digits");
            if (!m.empty()) f["machine"] = m;
        }
        if (req.isMember("streams")) {
            if (!req["streams"].isObject()) return refuse(400, "streams is {\"1\": bookmark, ...}");
            Json::Value s(Json::objectValue);
            for (const auto& k : req["streams"].getMemberNames()) {
                const Json::Value& b = req["streams"][k];
                const std::string h  = b.isString() ? b.asString() : "#";
                const bool hex       = h.size() == 20 && std::all_of(h.begin(), h.end(), [](char ch) {
                    return (ch >= '0' && ch <= '9') || (ch >= 'a' && ch <= 'f') || (ch >= 'A' && ch <= 'F');
                });
                if ((k != "1" && k != "2" && k != "3" && k != "4") || !(h.empty() || hex))
                    return refuse(400, "streams' keys are 1 to 4, each a bookmark of 20 hex digits or \"\"");
                s[k] = h;
            }
            f["streams"] = s;
        }
        if (req.isMember("max_records")) {
            if (!req["max_records"].isInt() || req["max_records"].asInt() < 0 || req["max_records"].asInt() > 65535)
                return refuse(400, "max_records is 0 (no cap) to 65535");
            f["max_records"] = req["max_records"].asInt();
        }
    }

    // What restarts the unit or its miner (the stream target, the Bluetooth
    // switches, a reboot, a reset) is warned while the machine records.
    const bool warned = cmd == "config" || cmd == "reboot" || cmd == "reset";
    if (warned && !devices::confirmed(req)) {
        {
            std::lock_guard<std::mutex> lk(mu_);
            if (!units_.count(serial)) return refuse(404, "no Mule & Miner \"" + serial + "\" has connected here");
        }
        const devices::Recording rec = recording(serial);
        if (rec.warns()) {
            a.status = 409;
            a.body   = devices::recordingWarning(rec);
            return a;
        }
    }
    uint64_t id = 0;
    {
        std::lock_guard<std::mutex> lk(mu_);
        if (!units_.count(serial)) return refuse(404, "no Mule & Miner \"" + serial + "\" has connected here");
        id = enqueue(units_[serial], cmd, f, automatic);
    }
    flush(serial);
    std::lock_guard<std::mutex> lk(mu_);
    for (const auto& k : units_[serial].commands)
        if (k.req_id == id) a.body["command"] = commandJson(k);
    a.body["req_id"] = static_cast<Json::UInt64>(id);
    a.body["ok"]     = true;
    return a;
}

Json::Value MuleService::commandJson(const Command& c) {
    Json::Value v(Json::objectValue);
    v["req_id"] = static_cast<Json::UInt64>(c.req_id);
    v["cmd"]    = c.cmd;
    Json::Value args(Json::objectValue);
    for (const auto& k : c.frame.getMemberNames())
        if (k != "t" && k != "cmd" && k != "req_id") args[k] = c.frame[k];
    v["args"]        = args;
    v["automatic"]   = c.automatic;
    v["state"]       = c.state;   // queued (unit offline or busy), sent, answered
    v["queued_at"]   = static_cast<Json::Int64>(c.queued_at);
    v["sent_at"]     = static_cast<Json::Int64>(c.sent_at);
    v["answered_at"] = static_cast<Json::Int64>(c.answered_at);
    v["result"]      = c.result;
    if (c.cmd == "as11_download") {
        v["bytes"]   = static_cast<Json::UInt64>(c.bytes.size());
        v["outcome"] = c.outcome;
    }
    if (c.cmd == "ds2_read") {
        v["bytes"]   = static_cast<Json::UInt64>(std::max(c.bytes_kept, c.bytes.size()));
        v["outcome"] = c.outcome;
    }
    // A reboot or a reset restarts the unit before it can answer; its next
    // hello (or, after a reset, its setup page) is the sign it was taken.
    if (c.cmd == "reboot" || c.cmd == "reset") v["answered_by_unit"] = false;
    return v;
}

Json::Value MuleService::status() const {
    Json::Value out(Json::objectValue);
    out["port"] = opt_.port;
    out["path"] = kRoute;
    Json::Value list(Json::arrayValue);
    const std::time_t now = std::time(nullptr);
    std::lock_guard<std::mutex> lk(mu_);
    for (const auto& [serial, u] : units_) {
        Json::Value v(Json::objectValue);
        v["serial"]        = serial;
        v["machine"]       = u.machine;
        v["online"]        = u.sessions > 0 && now - u.last_heard < kSilenceSec;
        v["first_seen"]    = static_cast<Json::Int64>(u.first_seen);
        v["last_heard"]    = static_cast<Json::Int64>(u.last_heard);
        v["last_block"]    = static_cast<Json::Int64>(u.last_block);
        v["blocks_stored"] = static_cast<Json::UInt64>(u.blocks_stored);
        v["blocks_resent"] = static_cast<Json::UInt64>(u.blocks_resent);
        v["events"]        = static_cast<Json::UInt64>(u.events);
        v["streaming"]     = u.streaming;
        v["night_landing"] = u.end_pending;
        v["fw"]            = u.fw;
        v["miner_fw"]      = u.miner_fw;
        v["ip"]            = u.ip;
        v["mode"]          = u.mode;
        v["tz"]            = u.tz;
        v["summary_from"]  = u.summary_from;
        v["card_root"]     = u.machine.empty() ? std::string() : machineRoot(u.machine);
        v["state"]         = u.state;
        Json::Value cmds(Json::arrayValue);
        for (auto it = u.commands.rbegin(); it != u.commands.rend(); ++it) cmds.append(commandJson(*it));
        v["commands"] = cmds;
        list.append(v);
    }
    out["units"] = list;
    Json::Value names(Json::arrayValue);
    for (const auto& n : localCommands()) names.append(n);
    out["commands"] = names;   // what POST /api/mule/command takes
    return out;
}

}  // namespace hms_cpap::mule
