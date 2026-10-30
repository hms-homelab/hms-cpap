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
    return v;
}

}  // namespace

SleepBridgeService::SleepBridgeService(Options opt, RebuilderFactory factory)
    : opt_(std::move(opt)), factory_(std::move(factory)), store_(opt_.root) {
    // Asked once: a factory that answers nullptr is a build without the rebuild.
    if (factory_) available_ = factory_(std::string()) != nullptr;
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

std::unique_ptr<Conversation> SleepBridgeService::open(const Admission& who) {
    const std::time_t now = std::time(nullptr);
    bool first = false;
    {
        std::lock_guard<std::mutex> lk(mu_);
        auto& b = bridges_[who.sender];
        first        = b.first_seen == 0;
        if (first) b.first_seen = now;
        b.machine    = who.machine;
        b.chip_id    = who.chip_id;
        b.last_heard = now;
        ++b.sessions;
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
    return std::make_unique<Conversation>(store_, who);
}

void SleepBridgeService::apply(const std::string& sender, const Effects& fx) {
    for (const auto& line : fx.log) std::cout << "[sleepbridge] " << sender << " " << line << std::endl;
    {
        std::lock_guard<std::mutex> lk(mu_);
        auto& b      = bridges_[sender];
        b.last_heard = std::time(nullptr);
        if (fx.state) b.state = *fx.state;
        if (fx.banked) b.last_batch = b.last_heard;
    }
    if (fx.banked || fx.card) wake(sender);
}

void SleepBridgeService::closed(const std::string& sender) {
    std::lock_guard<std::mutex> lk(mu_);
    auto it = bridges_.find(sender);
    if (it != bridges_.end() && it->second.sessions > 0) --it->second.sessions;
    std::cout << "[sleepbridge] " << sender << " disconnected" << std::endl;
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
        list.append(v);
    }
    out["bridges"] = list;
    return out;
}

}  // namespace hms_cpap::sleepbridge
