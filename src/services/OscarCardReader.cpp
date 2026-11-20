#include "services/OscarCardReader.h"

#include "services/As11NightImport.h"
#include "services/OscarDb.h"
#include "services/RemovedNights.h"
#include "utils/SessionEnd.h"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <filesystem>
#include <iostream>

namespace hms_cpap::oscar {

namespace fs = std::filesystem;
using std::chrono::system_clock;

namespace {

constexpr const char* kTag = "OSCAR: ";

std::string upper(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(),
                   [](unsigned char c) { return static_cast<char>(std::toupper(c)); });
    return s;
}

long long mtimeOf(const fs::path& p) {
    std::error_code ec;
    const auto t = fs::last_write_time(p, ec);
    if (ec) return 0;
    return static_cast<long long>(t.time_since_epoch().count());
}

/// A session file's number from its stem: eight hex digits on a DreamStation
/// 2 card, ten decimal digits on an older Philips card.
std::optional<long long> numberOf(const std::string& stem) {
    if (stem.empty()) return std::nullopt;
    const bool hex = stem.size() == 8 &&
                     std::all_of(stem.begin(), stem.end(), [](unsigned char c) { return std::isxdigit(c); });
    const bool dec = std::all_of(stem.begin(), stem.end(), [](unsigned char c) { return std::isdigit(c); });
    try {
        if (hex) return std::stoll(stem, nullptr, 16);
        if (dec) return std::stoll(stem, nullptr, 10);
    } catch (...) {
    }
    return std::nullopt;
}

/// The card files of one session, card-relative, source "card".
std::vector<SessionFileRef> cardFilesOf(const std::string& card_root, long long number) {
    std::vector<SessionFileRef> refs;
    std::optional<std::string> identity;
    std::error_code ec;
    const fs::path root(card_root);
    for (fs::recursive_directory_iterator it(root / "P-SERIES", fs::directory_options::skip_permission_denied, ec),
         end;
         !ec && it != end; it.increment(ec)) {
        if (!it->is_regular_file(ec)) continue;
        const auto& p = it->path();
        const auto ext = upper(p.extension().string());
        const auto rel = p.lexically_relative(root).generic_string();
        if (!identity && upper(p.filename().string()) == "PROP.BIN") identity = rel;
        if (ext != ".B01" && ext != ".B02" && ext != ".B05") continue;
        const auto n = numberOf(p.stem().string());
        if (!n || *n != number) continue;
        std::string kind = ext.substr(1);
        std::transform(kind.begin(), kind.end(), kind.begin(),
                       [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        refs.push_back({kind, rel, kSourceCard});
    }
    // A row with no file is never known to be the card's (SDD-077 D1): the
    // card's identity file stands for it when no session file matched.
    if (refs.empty() && identity) refs.push_back({"prop", *identity, kSourceCard});
    std::sort(refs.begin(), refs.end(),
              [](const SessionFileRef& a, const SessionFileRef& b) { return a.rel_path < b.rel_path; });
    return refs;
}

}  // namespace

OscarCardReader::OscarCardReader(Options o) : opt_(std::move(o)), runner_(opt_.runner) {
    if (!runner_) runner_ = std::make_shared<SystemProcessRunner>();
}

OscarCardReader::~OscarCardReader() { stop(); }

void OscarCardReader::start() {
    std::lock_guard<std::mutex> lk(mu_);
    if (worker_.joinable()) return;
    stop_ = false;
    worker_ = std::thread([this] { run(); });
}

void OscarCardReader::stop() {
    {
        std::lock_guard<std::mutex> lk(mu_);
        stop_ = true;
    }
    cv_.notify_all();
    if (worker_.joinable()) worker_.join();
}

void OscarCardReader::request(const std::string& why) {
    {
        std::lock_guard<std::mutex> lk(mu_);
        // Already asked for, or asked for again while one runs: one read
        // after it covers every close in between.
        pending_ = true;
        why_ = why;
    }
    cv_.notify_all();
}

bool OscarCardReader::noteCloses(const std::string& card_root, const std::vector<std::string>& rels) {
    if (card_root.empty()) return false;
    std::vector<std::string> closes;
    if (rels.empty()) {
        std::error_code ec;
        const fs::path root(card_root);
        for (fs::recursive_directory_iterator it(root / "P-SERIES", fs::directory_options::skip_permission_denied,
                                                 ec),
             end;
             !ec && it != end; it.increment(ec)) {
            if (!it->is_regular_file(ec)) continue;
            const auto rel = it->path().lexically_relative(root).generic_string();
            if (isSessionClose(rel)) closes.push_back(rel);
        }
    } else {
        for (const auto& r : rels)
            if (isSessionClose(r)) closes.push_back(r);
    }
    bool any = false;
    {
        std::lock_guard<std::mutex> lk(mu_);
        for (const auto& rel : closes) {
            const fs::path p = fs::path(card_root) / rel;
            std::error_code ec;
            const uint64_t size = fs::file_size(p, ec);
            if (ec) continue;
            const std::pair<long long, uint64_t> stamp{mtimeOf(p), size};
            // One key whichever way the root was spelled (a trailing slash).
            std::string norm = fs::path(card_root).lexically_normal().generic_string();
            while (norm.size() > 1 && norm.back() == '/') norm.pop_back();
            const std::string key = norm + "|" + rel;
            auto it = closes_.find(key);
            if (it != closes_.end() && it->second == stamp) continue;
            closes_[key] = stamp;
            any = true;
        }
    }
    if (any) request("a session closed on the card");
    return any;
}

Json::Value OscarCardReader::status() const {
    std::lock_guard<std::mutex> lk(mu_);
    Json::Value v(Json::objectValue);
    const Settings s = opt_.settings ? opt_.settings() : Settings{};
    v["set_up"]        = s.set_up();
    v["profile"]       = s.profile;
    v["reading"]       = running_;
    v["state"]         = s.set_up() || last_state_ != "never" ? last_state_ : outcomeName(Outcome::NotSetUp);
    v["version"]       = last_version_;
    v["message"]       = last_message_;
    v["last_read_at"]  = static_cast<Json::Int64>(last_read_at_);
    v["sessions"]      = last_stored_;
    v["containerised"] = opt_.containerised;
    return v;
}

void OscarCardReader::run() {
    for (;;) {
        std::string why;
        {
            std::unique_lock<std::mutex> lk(mu_);
            cv_.wait(lk, [&] { return stop_ || pending_; });
            if (stop_) return;
            pending_ = false;
            running_ = true;
            why = why_;
        }
        std::cout << kTag << "reading the card (" << why << ")" << std::endl;
        ReadResult r;
        try {
            r = readOnce();
        } catch (const std::exception& e) {
            r.outcome = Outcome::Failed;
            r.message = e.what();
        }
        std::cout << kTag << outcomeName(r.outcome) << ", " << r.stored << " session(s) stored"
                  << (r.message.empty() ? "" : " (" + r.message + ")") << std::endl;
        std::lock_guard<std::mutex> lk(mu_);
        running_ = false;
    }
}

OscarCardReader::ReadResult OscarCardReader::readOnce() {
    ReadResult r;
    const auto finish = [&](Outcome o, std::string msg) {
        r.outcome = o;
        r.message = std::move(msg);
        std::lock_guard<std::mutex> lk(mu_);
        last_state_ = outcomeName(r.outcome);
        last_message_ = r.message;
        if (!r.version.empty()) last_version_ = r.version;
        last_read_at_ = std::time(nullptr);
        last_stored_ = r.stored;
        return r;
    };

    const Settings s = opt_.settings ? opt_.settings() : Settings{};
    if (!s.set_up()) return finish(Outcome::NotSetUp, "the OSCAR fields in Settings are not filled in");
    const std::string root = opt_.card_root ? opt_.card_root() : "";
    std::error_code ec;
    if (root.empty() || !fs::is_directory(fs::path(root) / "P-SERIES", ec))
        return finish(Outcome::Failed, "no P-SERIES card folder to read");

    // Before: the gate, the profile, the signature. The connection is closed
    // again before OSCAR starts: only OSCAR's process touches the file while
    // it runs.
    std::string before;
    {
        std::string why;
        auto db = OscarDb::open(s.data_dir, &why);
        if (!db) return finish(Outcome::NotSupported, why);
        const auto g = db->gate();
        r.version = g.version;
        if (!g.ok) return finish(Outcome::NotSupported, g.why);
        const auto ps = db->profiles();
        if (std::find(ps.begin(), ps.end(), s.profile) == ps.end())
            return finish(Outcome::NoProfile, "no profile \"" + s.profile + "\" in " + s.data_dir);
        before = db->signature(s.profile);
    }

    const auto run = runner_->run(importCommand(s, root), jobLimitSeconds(cardBytes(root)));

    std::string why;
    auto db = OscarDb::open(s.data_dir, &why);
    if (!db) return finish(Outcome::Failed, why);
    const auto g = db->gate();
    r.version = g.version;
    // OSCAR was never told to upgrade (E4): a database it changed to another
    // schema is not read.
    if (!g.ok) return finish(Outcome::NotSupported, g.why);
    const std::string after = db->signature(s.profile);
    const Outcome o = classifyExit(run, !after.empty() && after != before);
    if (o != Outcome::Imported && o != Outcome::NothingNew) {
        std::string msg = run.error;
        if (msg.empty() && !run.output.empty()) {
            // OSCAR's last line says why.
            auto out = run.output;
            while (!out.empty() && (out.back() == '\n' || out.back() == '\r')) out.pop_back();
            const auto nl = out.find_last_of('\n');
            msg = nl == std::string::npos ? out : out.substr(nl + 1);
        }
        return finish(o, msg);
    }

    // D4: every session not stored yet, or that grew.
    const auto removed = opt_.removed ? opt_.removed() : std::set<std::string>{};
    db->begin();
    const auto heads = db->sessions(s.profile);
    std::optional<CPAPSession> newest;
    for (const auto& h : heads) {
        const std::string key = sessionKey(h);
        const auto known = done_.find(key);
        if (known != done_.end() && h.end_ms <= known->second) continue;
        const auto start = sessionClock(h.start_ms);
        const auto end = sessionClock(h.end_ms);
        if (isRemovedNight(removed, start)) continue;   // SDD-029
        if (known == done_.end() && opt_.db) {
            // First look in this process: a card row already stored at this
            // start, as long, is done. A Bluetooth copy is not (D5).
            bool held = false;
            for (const auto& sp : As11NightImport::storedSpansOverlapping(*opt_.db, opt_.device_id, start, end)) {
                const auto ds = sp.start > start ? sp.start - start : start - sp.start;
                if (!sp.constructed && ds < std::chrono::seconds(1) && sp.end + std::chrono::seconds(1) >= end) {
                    held = true;
                    break;
                }
            }
            if (held) {
                done_[key] = h.end_ms;
                continue;
            }
        }
        std::string err;
        auto parsed = db->read(h, opt_.device_id, opt_.device_name, &err);
        if (!parsed) {
            std::cerr << kTag << err << std::endl;
            continue;
        }
        if (!storeOne(*parsed, h.session_id, root)) continue;
        done_[key] = h.end_ms;
        ++r.stored;
        if (!newest || parsed->session_start > newest->session_start) newest = std::move(*parsed);
    }
    db->end();
    if (r.stored > 0 && opt_.db) {
        opt_.db->aggregateDailySummaryFromSessions(opt_.device_id);   // SDD-026
        if (opt_.on_stored && newest) opt_.on_stored(*newest, r.stored);
    }
    return finish(r.stored > 0 ? Outcome::Imported : o, "");
}

bool OscarCardReader::storeOne(CPAPSession& s, long long session_number, const std::string& card_root) {
    if (!opt_.db || !s.session_start) return false;
    const auto start = *s.session_start;
    if (!opt_.db->saveSession(s)) {
        std::cerr << kTag << "session " << session_number << " could not be saved" << std::endl;
        return false;
    }
    opt_.db->replaceSessionFiles(opt_.device_id, start, cardFilesOf(card_root, session_number));
    // D5, SDD-077 D2: the card wins the stretch it carries.
    As11NightImport::replaceConstructedFiles(*opt_.db, opt_.device_id, start, dataEndOf(s).value_or(start));
    closeWithDataEnd(*opt_.db, opt_.device_id, start, s);   // SDD-037 D2
    return true;
}

Json::Value OscarCardReader::check(const Settings& s, IProcessRunner& runner, bool containerised) {
    Json::Value v(Json::objectValue);
    Json::Value prog(Json::objectValue), dir(Json::objectValue), prof(Json::objectValue);
    prog["found"] = findProgram();

    std::error_code ec;
    if (s.program.empty()) {
        prog["state"] = containerised ? "not_here" : "empty";
    } else if (!fs::is_regular_file(s.program, ec)) {
        prog["state"] = containerised ? "not_here" : "not_found";
    } else {
        const auto r = runner.run(helpCommand(s.program), 30);
        if (!r.started || r.timed_out || r.exit_code != 0) prog["state"] = "not_runnable";
        // The headless import came with OSCAR 2.1.
        else if (r.output.find("--import") == std::string::npos) prog["state"] = "no_batch";
        else prog["state"] = "ok";
    }

    std::vector<std::string> profiles;
    if (s.data_dir.empty()) {
        dir["state"] = "empty";
    } else {
        std::string why;
        auto db = OscarDb::open(s.data_dir, &why);
        if (!db) {
            dir["state"] = "no_db";
        } else {
            const auto g = db->gate();
            dir["version"] = g.version;
            dir["schema"]  = g.schema;
            dir["state"]   = g.ok ? "ok" : "not_supported";
            profiles = db->profiles();
        }
    }
    Json::Value list(Json::arrayValue);
    for (const auto& p : profiles) list.append(p);

    if (s.profile.empty()) prof["state"] = "empty";
    else if (dir["state"].asString() != "ok") prof["state"] = "unknown";
    else prof["state"] = std::find(profiles.begin(), profiles.end(), s.profile) != profiles.end() ? "ok" : "missing";

    v["program"]  = prog;
    v["data_dir"] = dir;
    v["profile"]  = prof;
    v["profiles"] = list;
    v["ok"] = prog["state"].asString() == "ok" && dir["state"].asString() == "ok" && prof["state"].asString() == "ok";
    return v;
}

}  // namespace hms_cpap::oscar
