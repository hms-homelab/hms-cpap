#include "mule/MuleDs2.h"

#include "mule/Ds2rFile.h"
#include "sleepbridge/BatchStore.h"   // durableWrite

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>

namespace hms_cpap::mule {

namespace fs  = std::filesystem;
namespace ds2 = cpapdash::ds2ble;

namespace {

constexpr const char* kTag = "[mule ds2] ";

std::string compact(const Json::Value& v) {
    Json::StreamWriterBuilder w;
    w["indentation"] = "";
    return Json::writeString(w, v);
}

std::string today() { return Ds2Store::localDate(static_cast<long long>(std::time(nullptr))); }

CommandAnswer refusal(int status, const std::string& result, const std::string& why) {
    CommandAnswer a;
    a.status          = status;
    a.body["ok"]      = false;
    a.body["result"]  = result;
    a.body["error"]   = why;
    return a;
}

/// The token a failed ds2_read answered with: the miner's own when it says
/// one (result.ds2_read.error, or the frame's error), else "no_answer".
std::string failToken(const Json::Value& result) {
    const Json::Value& inner = result["result"];
    if (inner.isObject() && inner["ds2_read"].isObject() && inner["ds2_read"]["error"].isString() &&
        !inner["ds2_read"]["error"].asString().empty())
        return inner["ds2_read"]["error"].asString();
    if (result["error"].isString() && !result["error"].asString().empty()) return result["error"].asString();
    return "no_answer";
}

}  // namespace

MuleDs2::MuleDs2(Options opt, MuleService& service)
    : opt_(std::move(opt)), svc_(service), store_(opt_.data_dir), tree_(opt_.data_dir) {}

void MuleDs2::setCardMode(std::function<std::string()> f) {
    std::lock_guard<std::mutex> lk(mu_);
    card_mode_ = std::move(f);
}

std::string MuleDs2::cardMode() const {
    std::function<std::string()> f;
    {
        std::lock_guard<std::mutex> lk(mu_);
        f = card_mode_;
    }
    const std::string m = f ? f() : std::string("both");
    return m == "plain" || m == "sealed" ? m : "both";
}

bool MuleDs2::everyStreamOwned(const UnitDs2& u, const std::string& serial) const {
    const Json::Value& owned = u.own;
    const Json::Value& own   = owned[serial];
    if (!own.isObject()) return false;
    for (const char* s : {"1", "2", "3", "4"})
        if (!own.isMember(s)) return false;
    return true;
}

void MuleDs2::setImportDue(std::function<void()> f) {
    std::lock_guard<std::mutex> lk(mu_);
    import_due_ = std::move(f);
}

void MuleDs2::setNightSources(std::function<std::vector<std::string>()> f) {
    std::lock_guard<std::mutex> lk(mu_);
    night_sources_ = std::move(f);
}

void MuleDs2::setReadSchedule(std::function<std::pair<std::string, std::string>()> f) {
    std::lock_guard<std::mutex> lk(mu_);
    schedule_ = std::move(f);
}

int MuleDs2::dailyCheck(const ds2time::Input& base) {
    if (ds2time::normaliseMode(base.read_mode) == "manual") return 0;
    int asked = 0;
    const Json::Value status = svc_.status();
    for (const auto& u : status["units"]) {
        const std::string unit = u["serial"].asString();
        const Json::Value& d   = u["state"]["ds2"];
        if (!d.isObject() || !d.get("enabled", false).asBool() || !d.get("bonded", false).asBool()) continue;
        ds2time::Input in = base;
        {
            std::lock_guard<std::mutex> lk(mu_);
            const UnitDs2 s   = load(unit);
            in.paired_at       = s.paired_at;
            in.last_attempt_at = s.last_read.isObject() ? s.last_read.get("attempt_at", 0).asInt64() : 0;
        }
        const auto dec = ds2time::decide(in);
        if (!dec.due) continue;
        // Away, or a read or pairing under way: asked again at the next check.
        const auto a = readNow(unit, /*automatic=*/true);
        if (a.status == 200) {
            ++asked;
            std::cout << kTag << unit << ": the owner's daily read at " << in.read_at << std::endl;
        }
    }
    return asked;
}

MuleDs2::UnitDs2 MuleDs2::load(const std::string& unit) const {
    UnitDs2 u;
    std::ifstream in(fs::path(opt_.units_root) / unit / "ds2.json");
    if (!in) return u;
    std::stringstream ss;
    ss << in.rdbuf();
    Json::Value v;
    Json::CharReaderBuilder rb;
    std::istringstream is(ss.str());
    if (!Json::parseFromStream(rb, is, &v, nullptr) || !v.isObject()) return u;
    u.import_from = v.get("import_from", "").asString();
    if (!Ds2Store::localMidnight(u.import_from)) u.import_from.clear();
    u.peer      = v.get("peer", "").asString();
    u.paired_at = v.get("paired_at", 0).asInt64();
    u.machine   = v.get("machine", "").asString();
    u.last_read = v.get("last_read", Json::Value(Json::nullValue));
    if (v["own"].isObject()) u.own = v["own"];
    return u;
}

bool MuleDs2::save(const std::string& unit, const UnitDs2& u) const {
    Json::Value v(Json::objectValue);
    v["import_from"] = u.import_from;
    v["peer"]        = u.peer;
    v["paired_at"]   = static_cast<Json::Int64>(u.paired_at);
    v["machine"]     = u.machine;
    v["last_read"]   = u.last_read;
    v["own"]         = u.own;
    const std::string text = compact(v);
    std::string err;
    const fs::path p = fs::path(opt_.units_root) / unit / "ds2.json";
    if (sleepbridge::durableWrite(p.string(), text.data(), text.size(), &err)) return true;
    std::cerr << kTag << unit << ": " << err << std::endl;
    return false;
}

void MuleDs2::notePeer(const std::string& unit, UnitDs2& u, const std::string& peer) {
    if (!peer.empty() && u.peer.empty() && !u.import_from.empty()) {
        // The first bond this host sees, after the person already picked a day.
        u.peer      = peer;
        u.paired_at = static_cast<long long>(std::time(nullptr));
        save(unit, u);
    } else if (!peer.empty() && peer != u.peer) {
        // A new bond: nights from today on (SDD-064 D10). Its machine is not
        // known until it is read; the one read before may be another.
        u.peer        = peer;
        u.paired_at   = static_cast<long long>(std::time(nullptr));
        u.import_from = today();
        u.machine.clear();
        std::cout << kTag << unit << " bonded with " << peer << ": nights from " << u.import_from
                  << " are imported" << std::endl;
        save(unit, u);
    } else if (u.import_from.empty()) {
        u.import_from = today();
        save(unit, u);
    }
}

void MuleDs2::noteRead(const std::string& unit, const Json::Value& last_read) {
    std::lock_guard<std::mutex> lk(mu_);
    UnitDs2 u   = load(unit);
    u.last_read = last_read;
    save(unit, u);
}

// ── a read ────────────────────────────────────────────────────────────────

CommandAnswer MuleDs2::readNow(const std::string& unit, bool automatic) {
    {
        std::lock_guard<std::mutex> lk(mu_);
        chain_[unit]      = 0;
        meta_tries_[unit] = 0;
    }
    return queueRead(unit, automatic);
}

CommandAnswer MuleDs2::queueRead(const std::string& unit, bool automatic, bool card_meta) {
    const MuleService::Brief b = svc_.brief(unit);
    if (!b.known) return refusal(404, "unknown", "no Mule & Miner \"" + unit + "\" has connected here");
    const Json::Value& d = b.state["ds2"];
    if (!d.isObject() || !d.get("enabled", false).asBool())
        return refusal(409, "mode_off", "the unit is not in DreamStation 2 mode");
    if (!d.get("bonded", false).asBool()) return refusal(409, "not_paired", "the unit holds no DreamStation 2 bond");
    if (!b.online) return refusal(409, "offline", "the unit is not connected");
    if (d.get("busy", false).asBool() ||
        std::any_of(b.unanswered.begin(), b.unanswered.end(),
                    [](const std::string& c) { return c == "ds2_read" || c == "ds2_bond"; }))
        return refusal(409, "busy", "a DreamStation 2 read or pairing is already under way on this unit");

    Json::Value req(Json::objectValue);
    req["cmd"]         = "ds2_read";
    req["max_records"] = opt_.max_records;
    req["card"]        = cardMode();
    {
        std::lock_guard<std::mutex> lk(mu_);
        UnitDs2 u = load(unit);
        notePeer(unit, u, d.get("peer", "").asString());
        std::string machine = u.machine;
        if (machine.empty()) machine = d.get("machine", "").asString();
        // Every stream, each from a bookmark this unit's own reads returned,
        // or from the oldest the machine keeps ("") when there is none yet;
        // the import day then skips what is older. Never a bookmark from
        // elsewhere: from another reader's era the machine can answer
        // "nothing after it" for a stream that has records.
        const Json::Value& owned = u.own;   // read without adding keys
        const Json::Value& own   = machine.empty() ? Json::Value::nullSingleton() : owned[machine];
        Json::Value streams(Json::objectValue);
        for (const char* s : {"1", "2", "3", "4"})
            streams[s] = own.isObject() ? own.get(s, "").asString() : std::string();
        req["streams"] = streams;
        if (!machine.empty()) req["machine"] = Ds2Store::safeSerial(machine);
        if (card_meta) {
            // The read that ends the sync: the tree's counts across every read
            // of it, and FD, the oldest session's start. Only with a bookmark
            // of our own on every stream (or one read empty to its end), so
            // this read brings nothing that would make the counts wrong.
            if (machine.empty() || !everyStreamOwned(u, machine))
                return refusal(409, "not_ready", "a stream has no bookmark of this unit's own yet");
            const auto c = tree_.counts(machine);
            Json::Value meta(Json::objectValue);
            meta["pfn"]      = c.pfn;
            meta["dfn"]      = c.dfn;
            meta["efn"]      = c.efn;
            meta["fd"]       = static_cast<Json::Int64>(c.fd);
            req["card_meta"] = meta;
        }
    }
    CommandAnswer a = svc_.queueCommand(unit, req, automatic);
    if (a.status == 200) a.body["result"] = "reading";
    return a;
}

int MuleDs2::nightEnded(const std::string& by) {
    int asked = 0;
    const Json::Value status = svc_.status();
    for (const auto& u : status["units"]) {
        const std::string unit = u["serial"].asString();
        const Json::Value& d   = u["state"]["ds2"];
        if (!d.isObject() || !d.get("enabled", false).asBool() || !d.get("bonded", false).asBool()) continue;
        const auto a = readNow(unit, /*automatic=*/true);
        if (a.status == 200) {
            ++asked;
            std::cout << kTag << unit << ": " << by << " says the night ended: reading the DreamStation 2"
                      << std::endl;
        } else {
            std::cout << kTag << unit << ": " << by << " says the night ended; not read ("
                      << a.body.get("result", "").asString() << ")" << std::endl;
        }
    }
    return asked;
}

MuleDs2::Applied MuleDs2::apply(const std::string& unit, const std::string& bytes) {
    Applied out;
    // The pinned library reads the file whole, or not at all: a file cut short
    // stores nothing, and every bookmark stays where it was.
    ds2::ReadFile file;
    try {
        file = ds2::parseReadFile(reinterpret_cast<const uint8_t*>(bytes.data()), bytes.size());
    } catch (const std::exception& e) {
        out.error = "bad_file";
        std::cerr << kTag << unit << ": not stored: " << e.what() << std::endl;
        return out;
    }
    out.file_ok = true;
    const ds2::Identity id = ds2r::identity(file);

    std::function<void()> import_due;
    std::lock_guard<std::mutex> lk(mu_);
    import_due = import_due_;
    UnitDs2 u   = load(unit);
    out.serial  = id.serial.empty() ? u.machine : id.serial;
    if (out.serial.empty() && (!file.records.empty() || !file.sealed.empty())) {
        out.error = "no_serial";   // nowhere to put them; the bookmarks stay
        std::cerr << kTag << unit << ": the read carries records but no serial; nothing kept" << std::endl;
        return out;
    }
    if (!id.serial.empty()) store_.writeIdentity(id);
    if (u.import_from.empty()) {
        u.import_from = today();
        save(unit, u);
    }
    const long long cutoff = Ds2Store::localMidnight(u.import_from).value_or(0);

    // One step per record of the machine: its plaintext (RECORD), its card
    // file (SEALED), or both, matched by stream and bookmark, in the order
    // they came (the miner writes a record's RECORD, then its SEALED).
    struct Step {
        uint8_t                   stream{0};
        ds2::Bookmark             bookmark;
        const ds2::ReadRecord*    rec{nullptr};
        const ds2::ReadSealed*    seal{nullptr};
    };
    std::vector<Step> steps;
    std::map<std::string, size_t> at;
    auto key = [](uint8_t s, const ds2::Bookmark& b) { return std::to_string(s) + ":" + ds2r::toHex(b); };
    for (const auto& r : file.records) {
        at[key(r.stream, r.bookmark)] = steps.size();
        steps.push_back({r.stream, r.bookmark, &r, nullptr});
    }
    for (const auto& s : file.sealed) {
        if (s.stream == 0) continue;   // the card's own files, below
        const auto it = at.find(key(s.stream, s.bookmark));
        if (it != at.end() && !steps[it->second].seal) {
            steps[it->second].seal = &s;
        } else {
            at[key(s.stream, s.bookmark)] = steps.size();
            steps.push_back({s.stream, s.bookmark, nullptr, &s});
        }
    }

    Json::Value none(Json::objectValue);
    Json::Value& own = out.serial.empty() ? none : u.own[out.serial];
    if (!own.isObject()) own = Json::Value(Json::objectValue);
    bool stored = true;
    std::string failed;
    for (const auto& step : steps) {
        // SDD-064 D10: a night before the import day (a used machine's earlier
        // owner, nights not wanted) is walked past: bookmarked, never kept,
        // its card file neither. Without its plaintext (a "sealed" read) a
        // record cannot be dated here and is kept.
        bool keep = true;
        long long start = 0;
        if (step.rec && (step.stream == 1 || step.stream == 2)) {
            try {
                const auto blocks = ds2::verifyBody(step.rec->body);
                if (!blocks.empty()) start = static_cast<long long>(blocks.front().timestamp);
                keep = cutoff == 0 || start == 0 || start >= cutoff;
            } catch (const std::exception&) {
                // kept: the parser decides what a damaged record is worth
            }
        }
        if (keep && step.rec) {
            ds2::Record r;
            r.stream       = step.rec->stream;
            r.session      = step.rec->session;
            r.body         = step.rec->body;
            r.bookmark_out = step.rec->bookmark;
            if (store_.writeRecord(out.serial, r).empty()) {
                stored = false, failed = "storage";
                break;
            }
        }
        if (keep && step.seal) {
            const auto p = tree_.place(out.serial, *step.seal);
            if (!p.ok) {
                stored = false, failed = p.error;
                break;
            }
            ++out.card_files;
        }
        if (keep && start > 0 && step.stream == 1) tree_.noteSessionStart(out.serial, start);
        // The record is on disk (or walked past) before its bookmark moves.
        if (!store_.saveBookmark(out.serial, step.stream, step.bookmark)) {
            stored = false, failed = "storage";
            break;
        }
        own[std::to_string(step.stream)] = ds2r::toHex(step.bookmark);
        if (!keep) {
            ++out.walked_past;
            continue;
        }
        if (!step.rec) continue;
        ++out.records;
        if ((step.stream == 1 || step.stream == 2) &&
            std::find(out.sessions.begin(), out.sessions.end(), step.rec->session) == out.sessions.end())
            out.sessions.push_back(step.rec->session);
    }

    // The card's own files (a read that carried card_meta), once every record
    // before them is in place.
    if (stored) {
        int meta = 0;
        for (const auto& s : file.sealed) {
            if (s.stream != 0) continue;
            const auto p = tree_.place(out.serial, s);
            if (!p.ok) {
                stored = false, failed = p.error;
                break;
            }
            ++meta;
        }
        out.card_meta = meta > 0;
    }
    out.stored_ok = stored;

    // Where each stream ends, once every record before it is stored. Ten
    // zero bytes say "none yet": a stream read to its end with nothing in it,
    // asked from the oldest again next time.
    auto st = store_.load();
    if (stored && !out.serial.empty()) {
        auto& m = st.machines[out.serial];
        for (const auto& e : file.stream_ends) {
            if (e.stream < 1 || e.stream > 4) continue;
            const std::string s = std::to_string(e.stream);
            if (!ds2r::noBookmark(e.bookmark)) {
                m.bookmarks[e.stream] = e.bookmark;
                own[s]                = ds2r::toHex(e.bookmark);
            } else if (e.reachedEnd() && !own.isMember(s)) {
                own[s] = "";
            }
        }
    }

    if (!stored)
        out.error = failed;
    else if (file.end)
        out.error = ds2r::resultToken(file.end->result, file.end->status);
    else
        out.error = "stream_broken";
    out.more = stored && file.end && file.end->more;

    const long long now = static_cast<long long>(std::time(nullptr));
    if (!out.serial.empty()) {
        auto& m  = st.machines[out.serial];
        m.serial = out.serial;
        if (!id.model.empty()) m.model = id.model;
        if (!id.firmware.empty()) m.firmware = id.firmware;
        if (id.newest_session) m.newest_session = id.newest_session;
        m.last_attempt_at = now;
        m.last_error      = out.error;
        if (out.error.empty()) {
            m.last_read_at = now;
            m.last_records = out.records;
        }
        store_.save(st);
    }
    // The machine the unit read; another serial than asked (other_machine)
    // forgets it, so the next read asks without a check.
    if (file.end && file.end->result == 12)
        u.machine.clear();
    else if (!out.serial.empty())
        u.machine = out.serial;
    save(unit, u);

    std::cout << kTag << unit << ": " << out.records << " record(s) kept, " << out.card_files
              << " card file(s) placed" << (out.card_meta ? " with the card's PROP.BIN and LOG.SEQ" : "") << ", "
              << out.walked_past << " before the import day walked past" << (out.more ? ", more to read" : "")
              << (out.error.empty() ? "" : " (" + out.error + ")") << std::endl;
    if (out.records > 0 && import_due) import_due();
    return out;
}

void MuleDs2::onAnswer(const MuleService::Answered& a) {
    if (a.cmd == "ds2_bond") {
        const Json::Value& bond = a.result["result"]["ds2_bond"];
        if (a.result.get("ok", false).asBool() && bond.isObject() && bond.get("bonded", false).asBool()) {
            std::lock_guard<std::mutex> lk(mu_);
            UnitDs2 u = load(a.serial);
            notePeer(a.serial, u, bond.get("peer", "").asString());
        }
        return;
    }
    if (a.cmd != "ds2_read") return;

    const long long now = static_cast<long long>(std::time(nullptr));
    Json::Value lr(Json::objectValue);
    {
        std::lock_guard<std::mutex> lk(mu_);
        const UnitDs2 u = load(a.serial);
        if (u.last_read.isObject()) lr = u.last_read;
    }
    lr["attempt_at"]  = static_cast<Json::Int64>(now);
    lr["records"]     = 0;
    lr["walked_past"] = 0;
    lr["more"]        = false;

    std::string outcome;
    bool        next = false, meta_next = false;
    const std::string mode = cardMode();
    if (!a.result.get("ok", false).asBool()) {
        const std::string tok = failToken(a.result);
        lr["error"] = tok;
        outcome     = "not read: " + tok;
        if (tok == "other_machine") {
            std::lock_guard<std::mutex> lk(mu_);
            UnitDs2 u = load(a.serial);
            u.machine.clear();
            save(a.serial, u);
        }
    } else if (a.bytes_cut) {
        lr["error"] = "too_large";
        outcome     = "more than " + std::to_string(kPullMax) + " bytes: not kept";
    } else if (a.bytes.empty()) {
        lr["error"] = "no_bytes";
        outcome     = "no bytes came";
    } else {
        const Applied r   = apply(a.serial, a.bytes);
        lr["error"]       = r.error;
        lr["records"]     = r.records;
        lr["walked_past"] = r.walked_past;
        lr["more"]        = r.more;
        if (r.error.empty()) lr["at"] = static_cast<Json::Int64>(now);
        lr["card_files"]  = r.card_files;
        lr["card_meta"]   = r.card_meta;
        outcome = std::to_string(r.records) + " record(s) kept, " + std::to_string(r.card_files) +
                  " card file(s) placed" + (r.card_meta ? ", the card's PROP.BIN and LOG.SEQ" : "") + ", " +
                  std::to_string(r.walked_past) + " before the import day walked past" +
                  (r.more ? ", more to read" : "") + (r.error.empty() ? "" : " (" + r.error + ")");
        std::lock_guard<std::mutex> lk(mu_);
        if (r.card_files > 0) tree_grew_[a.serial] = true;
        // The machine has more: the next batch now, from the bookmarks just
        // saved. A batch that brought nothing ends the chain (no loop).
        if (r.more && r.records + r.walked_past + r.card_files > 0) next = ++chain_[a.serial] < opt_.max_chain;
        // The sync is over: if the tree grew, one more read carries its counts
        // for the card's PROP.BIN and LOG.SEQ. If that read brought files
        // after all, its counts are stale: asked again, at most three times.
        if (!next && !r.more && r.error.empty() && mode != "plain") {
            if (a.args.isMember("card_meta")) {
                if (r.card_files > 0 && meta_tries_[a.serial] < 3) {
                    ++meta_tries_[a.serial];
                    meta_next = true;
                } else {
                    tree_grew_[a.serial]  = false;
                    meta_tries_[a.serial] = 0;
                }
            } else if (tree_grew_[a.serial]) {
                meta_next = true;
            }
        }
    }
    {
        std::lock_guard<std::mutex> lk(mu_);
        lr["chain"] = chain_[a.serial];
    }
    noteRead(a.serial, lr);
    svc_.setOutcome(a.req_id, outcome);
    if (next || meta_next) {
        // The card's own files are this host's asking, whoever asked the sync.
        const auto q = queueRead(a.serial, a.automatic || meta_next, meta_next);
        if (q.status != 200)
            std::cout << kTag << a.serial << ": the " << (meta_next ? "card's own files were" : "next batch was")
                      << " not asked (" << q.body.get("result", "").asString() << ")" << std::endl;
    }
}

CommandAnswer MuleDs2::setImportFrom(const std::string& unit, const std::string& ymd) {
    if (!svc_.brief(unit).known) return refusal(404, "unknown", "no Mule & Miner \"" + unit + "\" has connected here");
    if (!Ds2Store::localMidnight(ymd)) return refusal(400, "bad_date", "the date is YYYY-MM-DD");
    std::lock_guard<std::mutex> lk(mu_);
    UnitDs2 u     = load(unit);
    u.import_from = ymd;
    if (!save(unit, u)) return refusal(500, "storage", "the date could not be saved");
    CommandAnswer a;
    a.body["ok"]          = true;
    a.body["import_from"] = ymd;
    return a;
}

Json::Value MuleDs2::view(const std::string& unit) const {
    Json::Value v(Json::objectValue);
    std::function<std::vector<std::string>()> sources;
    std::function<std::pair<std::string, std::string>()> schedule;
    UnitDs2 u;
    {
        std::lock_guard<std::mutex> lk(mu_);
        u        = load(unit);
        sources  = night_sources_;
        schedule = schedule_;
        const auto c = chain_.find(unit);
        v["chain"] = c == chain_.end() ? 0 : c->second;
    }
    v["import_from"]         = u.import_from.empty() ? today() : u.import_from;
    v["import_from_default"] = u.import_from.empty();
    v["machine"]             = u.machine;
    v["last_read"]           = u.last_read;
    bool first_done          = u.last_read.isObject() && u.last_read.get("at", 0).asInt64() > 0;
    if (!u.machine.empty()) {
        const auto st = store_.load();
        const auto m  = st.machines.find(u.machine);
        if (m != st.machines.end()) {
            v["model"]          = m->second.model;
            v["firmware"]       = m->second.firmware;
            v["newest_session"] = m->second.newest_session;
            first_done          = first_done || !m->second.bookmarks.empty();
        }
    }
    v["first_read_done"] = first_done;
    Json::Value ns(Json::arrayValue);
    if (sources)
        for (const auto& s : sources()) ns.append(s);
    v["night_sources"] = ns;
    v["trigger"]       = ns.empty() ? "manual" : "night_end";
    // SDD-065 D4a: the owner's choice, and this unit's next daily read.
    const auto sched = schedule ? schedule() : std::make_pair(std::string("daily"), std::string(ds2time::kDefaultReadAt));
    v["read_mode"] = sched.first;
    v["read_at"]   = sched.second;
    ds2time::Input in;
    in.now             = static_cast<long long>(std::time(nullptr));
    in.read_mode       = sched.first;
    in.read_at         = sched.second;
    in.paired_at       = u.paired_at;
    in.last_attempt_at = u.last_read.isObject() ? u.last_read.get("attempt_at", 0).asInt64() : 0;
    v["next_read_at"]  = static_cast<Json::Int64>(ds2time::decide(in).next_at);
    // SDD-065 D8: the card tree, for OSCAR and SleepHQ.
    v["card"] = cardMode();
    if (!u.machine.empty()) {
        const auto c = tree_.counts(u.machine);
        Json::Value t(Json::objectValue);
        t["dir"] = tree_.root(u.machine);
        t["pfn"] = c.pfn;
        t["dfn"] = c.dfn;
        t["efn"] = c.efn;
        t["fd"]  = static_cast<Json::Int64>(c.fd);
        v["card_tree"] = t;
    }
    return v;
}

}  // namespace hms_cpap::mule
