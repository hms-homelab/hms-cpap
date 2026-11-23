#include "services/OscarCardReader.h"

#include "services/As11NightImport.h"
#include "services/NightLanded.h"
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

/// SDD-083: the session numbers of a card's session files.
std::set<long long> cardSessionNumbers(const std::string& card_root) {
    std::set<long long> out;
    std::error_code ec;
    for (fs::recursive_directory_iterator it(fs::path(card_root) / "P-SERIES",
                                             fs::directory_options::skip_permission_denied, ec),
         end;
         !ec && it != end; it.increment(ec)) {
        if (!it->is_regular_file(ec)) continue;
        const auto ext = upper(it->path().extension().string());
        if (ext != ".B01" && ext != ".B02" && ext != ".B05") continue;
        if (const auto n = numberOf(it->path().stem().string())) out.insert(*n);
    }
    return out;
}

/// The session number at the end of a "<serial>/<session>" key.
std::optional<long long> numberOfKey(const std::string& key) {
    const auto slash = key.rfind('/');
    try {
        return std::stoll(slash == std::string::npos ? key : key.substr(slash + 1));
    } catch (...) {
        return std::nullopt;
    }
}

}  // namespace

void OscarCardReader::restoreHead(const std::string& key, long long start_ms,
                                  const std::set<std::string>& removed_now, std::set<std::string>& restored) {
    if (!opt_.db || removed_now.empty()) return;
    const std::string night = strDayForSessionStart(sessionLabel(start_ms, opt_.device_id));   // F1
    if (!removed_now.count(night)) return;
    const std::string dashed = night.substr(0, 4) + "-" + night.substr(4, 2) + "-" + night.substr(6, 2);
    if (!restored.count(dashed)) {
        for (const auto& d : restoreUploadedNights(*opt_.db, opt_.device_id, std::vector<std::string>{night})) {
            std::cout << kTag << "restored removed night " << d << " (uploaded card)" << std::endl;
            restored.insert(d);
        }
    }
    // Stored once before it was removed: stored again now.
    if (restored.count(dashed)) done_.erase(key);
}

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
    decltype(card_reads_) left;
    {
        std::lock_guard<std::mutex> lk(mu_);
        stop_ = true;
    }
    cv_.notify_all();
    if (worker_.joinable()) worker_.join();
    {
        std::lock_guard<std::mutex> lk(mu_);
        left.swap(card_reads_);
    }
    // SDD-083: an upload waiting on a read that will not run hears so.
    for (auto& [root, waiters] : left) {
        ReadResult r;
        r.outcome = Outcome::Failed;
        r.message = "hms-cpap is stopping";
        for (auto& w : waiters) w->set_value(r);
    }
}

std::shared_future<OscarCardReader::ReadResult> OscarCardReader::readCard(const std::string& card_root,
                                                                         const std::string& why) {
    auto waiter = std::make_shared<std::promise<ReadResult>>();
    std::shared_future<ReadResult> f = waiter->get_future().share();
    {
        std::lock_guard<std::mutex> lk(mu_);
        if (worker_.joinable() && !stop_) {
            card_reads_[card_root].push_back(waiter);
            why_ = why;
            waiter.reset();
        }
    }
    if (waiter) {
        ReadResult r;
        r.outcome = Outcome::Failed;
        r.message = "the OSCAR reader is not running";
        waiter->set_value(r);
        return f;
    }
    cv_.notify_all();
    return f;
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
    // SDD-081: where OSCAR runs, and the helper when one is remembered.
    const bool remote = opt_.remote && opt_.remote->remembered();
    v["where"] = remote ? "helper" : "here";
    if (opt_.remote) v["helper"] = opt_.remote->status();
    if (remote && last_state_ == "never") v["state"] = "never";
    return v;
}

void OscarCardReader::run() {
    for (;;) {
        std::string why;
        // SDD-083: an uploaded card asked for, with everyone waiting on it.
        std::optional<std::string> card;
        std::vector<std::shared_ptr<std::promise<ReadResult>>> waiters;
        {
            std::unique_lock<std::mutex> lk(mu_);
            cv_.wait(lk, [&] { return stop_ || pending_ || !card_reads_.empty(); });
            if (stop_) return;
            if (!card_reads_.empty()) {
                auto it = card_reads_.begin();
                card = it->first;
                waiters = std::move(it->second);
                card_reads_.erase(it);
                why = "a card was uploaded";
            } else {
                pending_ = false;
                why = why_;
            }
            running_ = true;
        }
        std::cout << kTag << "reading the card (" << why << ")" << std::endl;
        ReadResult r;
        try {
            r = card ? readOnce(*card, true) : readOnce();
        } catch (const std::exception& e) {
            r.outcome = Outcome::Failed;
            r.message = e.what();
        }
        std::cout << kTag << outcomeName(r.outcome) << ", " << r.stored << " session(s) stored"
                  << (r.message.empty() ? "" : " (" + r.message + ")") << std::endl;
        {
            std::lock_guard<std::mutex> lk(mu_);
            running_ = false;
        }
        for (auto& w : waiters) w->set_value(r);
    }
}

ImportRun runImport(const Settings& s, const std::string& root, IProcessRunner& runner) {
    ImportRun r;
    const auto finish = [&](Outcome o, std::string msg) {
        r.outcome = o;
        r.message = std::move(msg);
        return r;
    };
    if (!s.set_up()) return finish(Outcome::NotSetUp, "the OSCAR fields in Settings are not filled in");
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
        r.schema = g.schema;
        if (!g.ok) return finish(Outcome::NotSupported, g.why);
        const auto ps = db->profiles();
        if (std::find(ps.begin(), ps.end(), s.profile) == ps.end())
            return finish(Outcome::NoProfile, "no profile \"" + s.profile + "\" in " + s.data_dir);
        before = db->signature(s.profile);
    }

    const auto run = runner.run(importCommand(s, root), jobLimitSeconds(cardBytes(root)));

    std::string why;
    auto db = OscarDb::open(s.data_dir, &why);
    if (!db) return finish(Outcome::Failed, why);
    const auto g = db->gate();
    r.version = g.version;
    r.schema = g.schema;
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
    r.heads = db->sessions(s.profile);
    return finish(o, "");
}

OscarCardReader::ReadResult OscarCardReader::finish(ReadResult r, Outcome o, std::string msg) {
    r.outcome = o;
    r.message = std::move(msg);
    std::lock_guard<std::mutex> lk(mu_);
    last_state_ = outcomeName(r.outcome);
    last_message_ = r.message;
    if (!r.version.empty()) last_version_ = r.version;
    last_read_at_ = std::time(nullptr);
    last_stored_ = r.stored;
    return r;
}

bool OscarCardReader::wanted(const std::string& key, long long start_ms, long long end_ms,
                             const std::set<std::string>& removed) {
    const auto known = done_.find(key);
    if (known != done_.end() && end_ms <= known->second) return false;
    // SDD-083 F1: on the machine's clock, as the sessions are stored.
    const auto start = sessionLabel(start_ms, opt_.device_id);
    const auto end = sessionLabel(end_ms, opt_.device_id);
    if (isRemovedNight(removed, start)) return false;   // SDD-029
    if (known == done_.end() && opt_.db) {
        // First look in this process: a card row already stored at this
        // start, as long, is done. A Bluetooth copy is not (D5).
        for (const auto& sp : As11NightImport::storedSpansOverlapping(*opt_.db, opt_.device_id, start, end)) {
            const auto ds = sp.start > start ? sp.start - start : start - sp.start;
            if (!sp.constructed && ds < std::chrono::seconds(1) && sp.end + std::chrono::seconds(1) >= end) {
                done_[key] = end_ms;
                return false;
            }
        }
        // SDD-083 F1: a card row stored before the machine's clock was
        // applied sits at the instant itself. It is this session already:
        // never stored a second time at its label. (Moving such rows onto
        // the machine's clock is not done here.)
        const auto old_start = sessionClock(start_ms);
        const auto old_end = sessionClock(end_ms);
        if (old_start != start)
            for (const auto& sp : As11NightImport::storedSpansOverlapping(*opt_.db, opt_.device_id, old_start, old_end)) {
                const auto ds = sp.start > old_start ? sp.start - old_start : old_start - sp.start;
                if (!sp.constructed && ds < std::chrono::seconds(1)) {
                    done_[key] = end_ms;
                    return false;
                }
            }
    }
    return true;
}

int OscarCardReader::storeFrom(OscarDb& db, const std::string& profile, const std::string& root,
                               const std::set<std::string>& removed, std::optional<CPAPSession>& newest,
                               std::vector<std::chrono::system_clock::time_point>& landed) {
    int stored = 0;
    db.begin();
    for (const auto& h : db.sessions(profile)) {
        const std::string key = sessionKey(h);
        if (!wanted(key, h.start_ms, h.end_ms, removed)) continue;
        std::string err;
        auto parsed = db.read(h, opt_.device_id, opt_.device_name, &err);
        if (!parsed) {
            std::cerr << kTag << err << std::endl;
            continue;
        }
        if (!storeOne(*parsed, h.session_id, root)) continue;
        done_[key] = h.end_ms;
        ++stored;
        if (parsed->session_start) landed.push_back(*parsed->session_start);
        if (!newest || parsed->session_start > newest->session_start) newest = std::move(*parsed);
    }
    db.end();
    return stored;
}

OscarCardReader::ReadResult OscarCardReader::readOnce() {
    return readOnce(opt_.card_root ? opt_.card_root() : "");
}

OscarCardReader::ReadResult OscarCardReader::readOnce(const std::string& root, bool restore_removed) {
    ReadResult r;
    // SDD-081 D5: while a helper is remembered, the read is its.
    if (opt_.remote && opt_.remote->remembered()) return readRemote(root, restore_removed);

    const Settings s = opt_.settings ? opt_.settings() : Settings{};
    const ImportRun run = runImport(s, root, *runner_);
    r.version = run.version;
    if (run.outcome != Outcome::Imported && run.outcome != Outcome::NothingNew)
        return finish(r, run.outcome, run.message);

    // D4: every session not stored yet, or that grew. Opened and gated again:
    // the database is read only as it stands after OSCAR's exit.
    std::string why;
    auto db = OscarDb::open(s.data_dir, &why);
    if (!db) return finish(r, Outcome::Failed, why);
    const auto g = db->gate();
    if (!g.ok) return finish(r, Outcome::NotSupported, g.why);
    // SDD-083: an uploaded card brings back the removed nights it holds,
    // before the filter reads which nights are removed.
    std::set<std::string> restored;
    if (restore_removed && opt_.db) {
        const auto removed_now = removedNightSet(*opt_.db, opt_.device_id);
        const auto numbers = cardSessionNumbers(root);
        for (const auto& h : run.heads)
            if (numbers.count(h.session_id)) restoreHead(sessionKey(h), h.start_ms, removed_now, restored);
    }
    r.restored.assign(restored.begin(), restored.end());
    const auto removed = opt_.removed ? opt_.removed() : std::set<std::string>{};
    std::optional<CPAPSession> newest;
    std::vector<std::chrono::system_clock::time_point> landed;
    r.stored = storeFrom(*db, s.profile, root, removed, newest, landed);
    r.landed = landed;
    if (r.stored > 0 && opt_.db) {
        opt_.db->aggregateDailySummaryFromSessions(opt_.device_id);   // SDD-026
        night_landed::announceStarts(landed);                         // SDD-013 D-701-3
        if (opt_.on_stored && newest) opt_.on_stored(*newest, r.stored);
    }
    return finish(r, r.stored > 0 ? Outcome::Imported : run.outcome, "");
}

OscarCardReader::ReadResult OscarCardReader::readRemote(const std::string& root, bool restore_removed) {
    ReadResult r;
    std::error_code ec;
    if (root.empty() || !fs::is_directory(fs::path(root) / "P-SERIES", ec))
        return finish(r, Outcome::Failed, "no P-SERIES card folder to read");
    auto removed = opt_.removed ? opt_.removed() : std::set<std::string>{};
    const std::string slice_dir =
        !opt_.slice_dir.empty() ? opt_.slice_dir : (fs::temp_directory_path() / "hms-cpap-oscar-slice").string();
    std::set<std::string> restored;

    // D5.3: the store half's own filter decides what comes back.
    const IRemoteOscar::Want want = [&](const std::vector<std::string>& keys,
                                        const std::vector<std::pair<long long, long long>>& spans) {
        // SDD-083: an uploaded card brings back the removed nights it holds.
        if (restore_removed && opt_.db) {
            const auto removed_now = removedNightSet(*opt_.db, opt_.device_id);
            const auto numbers = cardSessionNumbers(root);
            for (size_t i = 0; i < keys.size() && i < spans.size(); ++i) {
                const auto n = numberOfKey(keys[i]);
                if (n && numbers.count(*n)) restoreHead(keys[i], spans[i].first, removed_now, restored);
            }
            if (!restored.empty()) removed = opt_.removed ? opt_.removed() : std::set<std::string>{};
        }
        std::vector<std::string> out;
        for (size_t i = 0; i < keys.size() && i < spans.size(); ++i)
            if (wanted(keys[i], spans[i].first, spans[i].second, removed)) out.push_back(keys[i]);
        return out;
    };
    const RemoteRead rr = opt_.remote->read(root, want, slice_dir);
    r.version = rr.version;
    r.restored.assign(restored.begin(), restored.end());
    const auto cleanup = [&] {
        std::error_code e;
        if (!rr.slice_dir.empty()) fs::remove_all(rr.slice_dir, e);   // kept for the one pass
    };
    if (rr.outcome != Outcome::Imported && rr.outcome != Outcome::NothingNew) {
        cleanup();
        return finish(r, rr.outcome, rr.message);
    }
    if (rr.slice_dir.empty()) return finish(r, rr.outcome, "");   // nothing wanted

    // D5.5: the store half, unchanged, on the returned file; the gate again.
    std::string why;
    auto db = OscarDb::open(rr.slice_dir, &why);
    if (!db) {
        cleanup();
        return finish(r, Outcome::Failed, why);
    }
    const auto g = db->gate();
    r.version = g.version;
    if (!g.ok) {
        db.reset();
        cleanup();
        return finish(r, Outcome::NotSupported, g.why);
    }
    const auto ps = db->profiles();
    std::optional<CPAPSession> newest;
    std::vector<std::chrono::system_clock::time_point> landed;
    if (!ps.empty()) r.stored = storeFrom(*db, ps.front(), root, removed, newest, landed);
    r.landed = landed;
    db.reset();
    cleanup();
    if (r.stored > 0 && opt_.db) {
        opt_.db->aggregateDailySummaryFromSessions(opt_.device_id);   // SDD-026
        night_landed::announceStarts(landed);                         // SDD-013 D-701-3
        if (opt_.on_stored && newest) opt_.on_stored(*newest, r.stored);
    }
    return finish(r, r.stored > 0 ? Outcome::Imported : rr.outcome, "");
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
