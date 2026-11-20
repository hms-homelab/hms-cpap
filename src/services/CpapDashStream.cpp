#include "services/CpapDashStream.h"

#include "services/CardFileFeed.h"
#include "services/CpapDashSyncService.h"
#include "utils/MachineClocks.h"
#include "services/OximetryOutbox.h"
#include "utils/MachineIdentity.h"

#include <openssl/evp.h>

#include <algorithm>
#include <cctype>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <sstream>

namespace fs = std::filesystem;

namespace hms_cpap {

namespace cdstream {

namespace {

std::string lower(std::string s) {
    for (auto& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

bool endsWith(const std::string& s, const std::string& suffix) {
    return s.size() >= suffix.size() && s.compare(s.size() - suffix.size(), suffix.size(), suffix) == 0;
}

std::vector<std::string> segments(const std::string& rel) {
    std::vector<std::string> out;
    std::string cur;
    for (char c : rel) {
        if (c == '/' || c == '\\') {
            out.push_back(cur);
            cur.clear();
        } else {
            cur += c;
        }
    }
    out.push_back(cur);
    return out;
}

bool isDate8(const std::string& s) {
    return s.size() == 8 && std::all_of(s.begin(), s.end(), [](char c) { return c >= '0' && c <= '9'; });
}

/// "P0" .. "P99": a Philips card's folder of session files.
bool isPFolder(const std::string& s) {
    return s.size() >= 2 && (s[0] == 'P' || s[0] == 'p') &&
           std::all_of(s.begin() + 1, s.end(), [](char c) { return c >= '0' && c <= '9'; });
}

std::string compact(const Json::Value& v) {
    Json::StreamWriterBuilder w;
    w["indentation"] = "";
    return Json::writeString(w, v);
}

}  // namespace

std::string md5Hex(const std::string& bytes, std::size_t from, std::size_t to) {
    to   = std::min(to, bytes.size());
    from = std::min(from, to);
    unsigned char digest[EVP_MAX_MD_SIZE];
    unsigned int  len = 0;
    EVP_MD_CTX*   ctx = EVP_MD_CTX_new();
    EVP_DigestInit_ex(ctx, EVP_md5(), nullptr);
    EVP_DigestUpdate(ctx, bytes.data() + from, to - from);
    EVP_DigestFinal_ex(ctx, digest, &len);
    EVP_MD_CTX_free(ctx);
    std::ostringstream o;
    for (unsigned int i = 0; i < len; ++i)
        o << std::hex << std::setw(2) << std::setfill('0') << static_cast<int>(digest[i]);
    return o.str();
}

bool isEdfName(const std::string& name) { return endsWith(lower(name), ".edf"); }

bool isGrowingName(const std::string& name) {
    const auto n = lower(name);
    return endsWith(n, "_brp.edf") || endsWith(n, "_pld.edf") || endsWith(n, "_sad.edf") ||
           endsWith(n, "_sa2.edf") ||
           // SDD-080, platform SDD-061 D10: a DreamStation 2 appends the open
           // session's .B05 as it records.
           endsWith(n, ".b05") ||
           isGrowingOximetryName(name);   // SDD-079 D5: the live spool is append-only too
}

std::size_t edfHeaderBytes(const std::string& bytes) {
    // EDF: an 8-byte version "0       ", then the header's own length as 8
    // ASCII digits at 184.
    if (bytes.size() < 256 || bytes[0] != '0') return 0;
    std::string field = bytes.substr(184, 8);
    field.erase(std::remove(field.begin(), field.end(), ' '), field.end());
    if (field.empty() || !std::all_of(field.begin(), field.end(), [](char c) { return c >= '0' && c <= '9'; }))
        return 0;
    const unsigned long long n = std::stoull(field);
    if (n < 256 || (n - 256) % 256 != 0) return 0;
    return static_cast<std::size_t>(n);
}

std::string ackedMd5(const std::string& name, const std::string& bytes, std::size_t upto) {
    const std::size_t from = isEdfName(name) ? std::min(edfHeaderBytes(bytes), upto) : 0;
    return md5Hex(bytes, from, upto);
}

Plan planSend(const std::string& name, const std::string& bytes,
              const std::optional<IDatabase::CpapDashSent>& row) {
    Plan p;
    const uint64_t size = bytes.size();
    if (!row) {
        if (size == 0) {
            p.why = "empty";
            return p;
        }
        p.send = true;
        p.whole = true;
        p.why = "new";
        return p;
    }
    const uint64_t acked = row->acked_to < 0 ? 0 : static_cast<uint64_t>(row->acked_to);
    const bool prefix_same = acked <= size && ackedMd5(name, bytes, acked) == row->acked_md5;

    if (prefix_same && acked == size) {
        p.why = "acked";
        p.held = true;
        return p;
    }
    if (prefix_same && isGrowingName(name) && acked > 0 && acked >= edfHeaderBytes(bytes)) {
        p.send = true;
        p.offset = acked;
        p.why = "grew";
        return p;
    }
    if (size == 0) {
        p.why = "empty";
        return p;
    }
    p.send = true;
    p.whole = true;
    p.why = prefix_same ? "changed" : (acked > size ? "shorter" : "rewritten");
    return p;
}

bool isCardPath(const std::string& rel) {
    if (rel.empty()) return false;
    const auto segs = segments(rel);
    for (const auto& s : segs) {
        if (s.empty() || s[0] == '.') return false;
    }
    const auto name = lower(segs.back());
    if (endsWith(name, ".tmp") || endsWith(name, ".part")) return false;
    if (segs.size() == 1) return true;
    const auto top = lower(segs[0]);
    if (top == "settings") return true;
    if (top == "datalog") return segs.size() == 3 && isDate8(segs[1]);
    // SDD-080, platform SDD-061 D10: a DreamStation 2's card, exactly as the
    // machine wrote it (encrypted, never opened here): P-SERIES/<folder>/
    // PROP.BIN, LOG.SEQ ..., and P-SERIES/<folder>/Pn/<session>.B01|B02|B05.
    if (top == "p-series") {
        if (segs.size() == 3) return true;
        if (segs.size() == 4) return isPFolder(segs[2]);
    }
    return false;
}

std::optional<CardHeaders> cardHeadersFor(const std::string& rel) {
    if (rel.empty()) return std::nullopt;
    const auto segs = segments(rel);
    for (const auto& s : segs) {
        if (s.empty() || s == "." || s == "..") return std::nullopt;
    }
    CardHeaders h;
    h.filename = segs.back();
    if (segs.size() == 1) {
        h.date_folder = "STR";
        if (lower(h.filename) == "str.edf") h.filename = "STR.edf";
        else h.rel_dir = "\\";
        return h;
    }
    if (segs.size() == 3 && lower(segs[0]) == "datalog" && isDate8(segs[1])) {
        h.date_folder = segs[1];
        return h;
    }
    h.date_folder = segs[segs.size() - 2];
    for (std::size_t i = 0; i + 1 < segs.size(); ++i) {
        if (i) h.rel_dir += "\\";
        h.rel_dir += segs[i];
    }
    return h;
}

std::string binaryHeader(uint32_t req_id, uint32_t seq, bool last, uint16_t kind) {
    std::string h(12, '\0');
    auto put32 = [&](std::size_t at, uint32_t v) {
        for (int i = 0; i < 4; ++i) h[at + i] = static_cast<char>((v >> (8 * i)) & 0xff);
    };
    auto put16 = [&](std::size_t at, uint16_t v) {
        h[at]     = static_cast<char>(v & 0xff);
        h[at + 1] = static_cast<char>((v >> 8) & 0xff);
    };
    put32(0, req_id);
    put32(4, seq);
    put16(8, last ? 1 : 0);
    put16(10, kind);
    return h;
}

std::string helloFrame(const std::string& version, const std::string& source,
                       const std::string& machine_clock) {
    Json::Value v;
    v["t"]      = "hello";
    v["mode"]   = "hms";
    v["fw"]     = version;
    // In `state`: the socket passes the hello's state to ingest unchanged
    // (device.connected), and ingest reads the source from there.
    v["state"]["source"] = source;
    if (!machine_clock.empty()) v["state"][kMachineClockField] = machine_clock;   // SDD-074
    return compact(v);
}

std::string roundFrame() {
    Json::Value v;
    v["t"]    = "round";
    v["done"] = true;
    return compact(v);
}

std::string sourceKind(bool bluetooth_on, bool bridge_receiver_on, const std::string& data_dir,
                       const std::string& local_dir) {
    if (bluetooth_on) return "ble";
    if (bridge_receiver_on && !data_dir.empty()) {
        std::error_code ec;
        const fs::path base = fs::path(data_dir) / "sleepbridge";
        // The local folder read is a bridge's own card folder: a bridge, even
        // before its first rebuild put a DATALOG there.
        if (!local_dir.empty()) {
            const fs::path b = fs::weakly_canonical(base, ec);
            const fs::path l = fs::weakly_canonical(fs::path(local_dir), ec);
            const auto rel = l.lexically_relative(b);
            if (!ec && !rel.empty() && *rel.begin() != ".." && rel != ".") return "bridge";
        }
        if (fs::is_directory(base, ec)) {
            for (const auto& e : fs::directory_iterator(base, ec)) {
                const auto name = e.path().filename().string();
                if (!e.is_directory(ec) || name.empty() || name[0] == '.') continue;
                // A sender's folder holds its card once a rebuild wrote one.
                if (fs::exists(e.path() / "DATALOG", ec) || fs::exists(e.path() / "STR.edf", ec) ||
                    fs::exists(e.path() / "P-SERIES", ec))   // SDD-080: a DreamStation 2's card
                    return "bridge";
            }
        }
    }
    return "ezshare";
}

namespace {

// The frames are written key by key, in the contract fixtures' order
// (platform SDD-061 section 7: the same bytes in both repos), not through
// jsoncpp, which sorts an object's keys.
std::string q(const std::string& s) { return Json::valueToQuotedString(s.c_str()); }
std::string n(uint64_t v) { return std::to_string(v); }

// offset .. md5: the part every file_ready shares.
std::string rangeKeys(uint64_t offset, uint64_t total_size, uint32_t chunks, uint64_t size,
                      const std::string& md5) {
    // eof: these bytes reach the file's end as it stands on disk now.
    return "\"offset\":" + n(offset) + ",\"total_size\":" + n(total_size) +
           ",\"eof\":" + (offset + size == total_size ? "true" : "false") + ",\"chunks\":" + n(chunks) +
           ",\"size\":" + n(size) + ",\"md5\":" + q(md5);
}

}  // namespace

std::string fileReadyFrame(uint32_t req_id, const CardHeaders& h, uint64_t offset,
                           uint64_t total_size, uint32_t chunks, uint64_t size,
                           const std::string& md5, const std::string& machine_serial,
                           const std::string& source) {
    std::string s = "{\"t\":\"file_ready\",\"req_id\":" + n(req_id) + ",\"date_folder\":" + q(h.date_folder) +
                    ",\"rel_dir\":" + q(h.rel_dir) + ",\"filename\":" + q(h.filename) + "," +
                    rangeKeys(offset, total_size, chunks, size, md5) +
                    ",\"machine_serial\":" + q(machine_serial);
    if (!source.empty()) s += ",\"source\":" + q(source);   // SDD-079 D2
    return s + "}";
}

std::string sourceNoteFrame(uint32_t req_id, const CardHeaders& h, uint64_t acked_to,
                            const std::string& machine_serial, const std::string& source) {
    // md5 of the empty string: a whole, empty read.
    return fileReadyFrame(req_id, h, acked_to, acked_to, 0, 0, md5Hex("", 0, 0), machine_serial, source);
}

std::string oximetryReadyFrame(uint32_t req_id, const OxiHeaders& oxi, uint64_t offset,
                               uint64_t total_size, uint32_t chunks, uint64_t size,
                               const std::string& md5) {
    std::string o = "{\"name\":" + q(oxi.name) + ",\"format\":" + q(oxi.format);
    if (!oxi.source.empty()) o += ",\"source\":" + q(oxi.source);
    if (!oxi.device_name.empty()) o += ",\"device_name\":" + q(oxi.device_name);
    o += "}";
    return "{\"t\":\"file_ready\",\"req_id\":" + n(req_id) + ",\"kind\":\"oximetry\"," +
           rangeKeys(offset, total_size, chunks, size, md5) + ",\"oxi\":" + o + "}";
}

bool isGrowingOximetryName(const std::string& name) {
    const auto l = lower(name);
    return l.rfind("live-", 0) == 0 && endsWith(l, ".jsonl");
}

std::string isoNowUtc() {
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

}  // namespace cdstream

// ── the uploader ─────────────────────────────────────────────────────────────

namespace {

const char* kTag = "[cpapdash-stream] ";
// The gateway's close reason when the token is revoked (D7): "hms token
// revoked: <revoked|regenerated|password_changed|account_deleted>".
const char* kRevokedPrefix = "hms token revoked";

std::string keyOf(const std::string& root, const std::string& rel) { return root + "\n" + rel; }
// A round's queue key: no card path starts with a control character.
std::string roundKeyOf(const std::string& root) { return root + "\n\x01round"; }

bool readFile(const fs::path& p, std::string& out) {
    std::ifstream in(p, std::ios::binary);
    if (!in) return false;
    std::ostringstream ss;
    ss << in.rdbuf();
    out = ss.str();
    return static_cast<bool>(in) || in.eof();
}

constexpr uint32_t kChunksLost = UINT32_MAX;

// [bytes] from [from] to the end as binary frames under [req_id]: how many,
// or kChunksLost when the link dropped.
uint32_t sendChunks(CpapDashLink& link, uint32_t req_id, const std::string& bytes, std::size_t from,
                    std::size_t chunk_bytes) {
    const std::size_t len = bytes.size() - from;
    const std::size_t chunk = std::max<std::size_t>(chunk_bytes, 1);
    const uint32_t chunks = static_cast<uint32_t>((len + chunk - 1) / chunk);
    for (uint32_t i = 0; i < chunks; ++i) {
        const std::size_t at = from + static_cast<std::size_t>(i) * chunk;
        const std::size_t n = std::min(chunk, bytes.size() - at);
        std::string frame = cdstream::binaryHeader(req_id, i, i + 1 == chunks, cdstream::kKindFileBytes);
        frame.append(bytes, at, n);
        if (!link.sendBinary(frame)) return kChunksLost;
    }
    return chunks;
}

// Root files first, then SETTINGS, then the nights oldest first.
int groupOf(const std::string& rel) {
    if (rel.find('/') == std::string::npos) return 0;
    std::string top = rel.substr(0, rel.find('/'));
    for (auto& c : top) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    return top == "SETTINGS" ? 1 : 2;
}

}  // namespace

Json::Value CpapDashStreamer::Status::toJson() const {
    Json::Value v;
    v["stream"]      = stream;
    v["state"]       = state;
    v["http_status"] = http_status;
    v["reason"]      = reason;
    v["since"]       = since;
    v["last_ack_at"] = last_ack_at;
    v["files_acked"] = static_cast<Json::Int64>(files_acked);
    v["bytes_acked"] = static_cast<Json::Int64>(bytes_acked);
    v["queued"]      = static_cast<Json::UInt64>(queued);
    v["install_id"]  = install_id;
    v["oximetry_acked"] = static_cast<Json::Int64>(oximetry_acked);   // SDD-079 D8
    v["oximetry_state"] = oximetry_state;
    return v;
}

CpapDashStreamer::CpapDashStreamer(std::shared_ptr<IDatabase> db, LinkFactory factory,
                                   std::string install_id, std::string version)
    : db_(std::move(db)), factory_(std::move(factory)), install_id_(std::move(install_id)),
      version_(std::move(version)) {
    status_.install_id = install_id_;
    status_.since = cdstream::isoNowUtc();
    // Ingest keeps chunks per (serial, req_id) until they go stale; starting
    // from the clock means a restart never reuses a req_id it may still hold.
    next_req_ = static_cast<uint32_t>(std::time(nullptr));
    if (next_req_ == 0) next_req_ = 1;
}

CpapDashStreamer::~CpapDashStreamer() { stop(); }

void CpapDashStreamer::setTiming(const Timing& t) {
    std::lock_guard<std::mutex> lk(mu_);
    timing_ = t;
}

void CpapDashStreamer::setRootsProvider(RootsProvider p) {
    std::lock_guard<std::mutex> lk(mu_);
    roots_provider_ = std::move(p);
}

void CpapDashStreamer::setSourceProvider(std::function<std::string()> p) {
    std::lock_guard<std::mutex> lk(mu_);
    source_provider_ = std::move(p);
}

void CpapDashStreamer::setOximetryBackfill(std::function<void()> f) {
    std::lock_guard<std::mutex> lk(mu_);
    oximetry_backfill_ = std::move(f);
}

void CpapDashStreamer::apply(const Settings& s) {
    {
        std::lock_guard<std::mutex> lk(mu_);
        if (applied_) {
            // The index is cleared when the switch goes off or on.
            if (s.stream != settings_.stream) clear_pending_ = true;
            if (s.stream != settings_.stream || s.token != settings_.token) ++gen_;
        }
        applied_ = true;
        settings_ = s;
        status_.stream = s.stream;
    }
    cv_.notify_all();
}

void CpapDashStreamer::start() {
    if (worker_.joinable()) return;
    stop_ = false;
    worker_ = std::thread([this] { run(); });
}

void CpapDashStreamer::stop() {
    stop_ = true;
    cv_.notify_all();
    if (worker_.joinable()) worker_.join();
}

void CpapDashStreamer::fileWritten(const std::string& root, const std::string& rel,
                                   const std::string& machine, const std::string& source) {
    // The oximetry outbox is flat: its own file names, never a card path.
    if (source == card_feed::kSourceOximetry ? !oximetry_outbox::metaFor(root, rel)
                                             : !cdstream::isCardPath(rel))
        return;
    {
        std::lock_guard<std::mutex> lk(mu_);
        auto& m = roots_[root];
        if (!machine.empty()) m.machine = machine;
        if (!source.empty()) m.source = source;
        if (!settings_.stream) return;   // the index is walked in full at switch-on
    }
    Work w{root, rel, machine};
    w.source = source;
    enqueue(std::move(w), false);
}

void CpapDashStreamer::rootWritten(const std::string& root, const std::string& machine,
                                   const std::string& source) {
    {
        std::lock_guard<std::mutex> lk(mu_);
        auto& m = roots_[root];
        if (!machine.empty()) m.machine = machine;
        if (!source.empty()) m.source = source;
        if (!settings_.stream) return;
    }
    Work w{root, "", machine};
    w.source = source;
    enqueue(std::move(w), false);
}

void CpapDashStreamer::burstEnded(const std::string& root) {
    std::function<std::string()> source_of;
    {
        std::lock_guard<std::mutex> lk(mu_);
        roots_[root];
        if (!settings_.stream) return;
        source_of = source_provider_;
    }
    // Only an ez Share install's bursts end a round; a bridge or Bluetooth
    // night closes by its own source's rules.
    if ((source_of ? source_of() : std::string("ezshare")) != "ezshare") return;
    Work w;
    w.root = root;
    w.round = true;
    enqueue(std::move(w), false);
}

CpapDashStreamer::Status CpapDashStreamer::status() const {
    std::lock_guard<std::mutex> lk(mu_);
    Status s = status_;
    s.queued = queue_.size();
    return s;
}

std::string CpapDashStreamer::loadOrCreateInstallId(const std::string& data_dir) {
    const fs::path p = fs::path(data_dir) / "cpapdash_install_id";
    std::string id;
    if (readFile(p, id)) {
        id.erase(std::remove_if(id.begin(), id.end(), [](unsigned char c) { return std::isspace(c); }),
                 id.end());
        if (id.size() == 36) return id;
    }
    id = CpapDashSyncService::makeUuid();
    std::error_code ec;
    fs::create_directories(p.parent_path(), ec);
    const fs::path tmp = p.string() + ".tmp";
    {
        std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
        out << id << "\n";
    }
    fs::rename(tmp, p, ec);
    if (ec) std::cerr << kTag << "could not keep the install id in " << p << ": " << ec.message() << std::endl;
    return id;
}

std::vector<CpapDashStreamer::Root> CpapDashStreamer::discoverRoots(
    const std::string& data_dir, const std::vector<std::string>& card_roots) {
    std::vector<Root> out;
    std::error_code ec;
    std::set<std::string> seen;
    for (const auto& r : card_roots) {
        if (r.empty() || !fs::is_directory(r, ec) || !seen.insert(r).second) continue;
        out.push_back({r, "", card_feed::kSourceCard});
    }
    if (data_dir.empty()) return out;
    for (const char* sub : {"as11", "sleepbridge"}) {
        const fs::path base = fs::path(data_dir) / sub;
        if (!fs::is_directory(base, ec)) continue;
        for (const auto& e : fs::directory_iterator(base, ec)) {
            if (!e.is_directory(ec)) continue;
            const std::string name = e.path().filename().string();
            if (name.empty() || name[0] == '.' || name == "unknown") continue;
            const std::string root = e.path().string();
            if (!seen.insert(root).second) continue;
            // as11/<serial>: the folder is the machine, and every file in it
            // was built from Bluetooth (SDD-079 D1). A Sleep Bridge rebuilds
            // the machine's own card: card.
            const bool as11 = std::string(sub) == "as11";
            out.push_back({root, as11 ? name : "", as11 ? card_feed::kSourceBleAs11 : card_feed::kSourceCard});
        }
    }
    // SDD-079 D5: the oximetry outbox, after every card root.
    const std::string outbox = oximetry_outbox::rootFor(data_dir);
    if (fs::is_directory(outbox, ec) && seen.insert(outbox).second)
        out.push_back({outbox, "", card_feed::kSourceOximetry});
    return out;
}

// ── queue ────────────────────────────────────────────────────────────────────

void CpapDashStreamer::enqueue(Work w, bool front) {
    {
        std::lock_guard<std::mutex> lk(mu_);
        if (w.round) {
            // Bursts that end while CpapDash is away fold into one: the
            // round still waiting moves behind whatever was queued since.
            const std::string key = roundKeyOf(w.root);
            if (!queued_keys_.insert(key).second) {
                queue_.erase(std::remove_if(queue_.begin(), queue_.end(),
                                            [&](const Work& q) { return q.round && q.root == w.root; }),
                             queue_.end());
            }
            if (front) queue_.push_front(std::move(w));
            else queue_.push_back(std::move(w));
            cv_.notify_all();
            return;
        }
        const std::string key = keyOf(w.root, w.rel);
        if (!queued_keys_.insert(key).second) {
            if (!w.machine.empty()) {
                for (auto& q : queue_)
                    if (q.root == w.root && q.rel == w.rel && q.machine.empty()) q.machine = w.machine;
            }
            return;
        }
        if (front) queue_.push_front(std::move(w));
        else queue_.push_back(std::move(w));
    }
    cv_.notify_all();
}

std::optional<CpapDashStreamer::Work> CpapDashStreamer::popWork() {
    std::lock_guard<std::mutex> lk(mu_);
    if (queue_.empty()) return std::nullopt;
    Work w = std::move(queue_.front());
    queue_.pop_front();
    queued_keys_.erase(w.round ? roundKeyOf(w.root) : keyOf(w.root, w.rel));
    return w;
}

void CpapDashStreamer::markAllRootsDirty() {
    std::vector<Root> roots;
    RootsProvider provider;
    std::function<void()> backfill;
    {
        std::lock_guard<std::mutex> lk(mu_);
        provider = roots_provider_;
        backfill = oximetry_backfill_;
    }
    // SDD-079 D7: recordings stored before 7.0.0 into the outbox first, so
    // this walk sends them.
    if (backfill) {
        try {
            backfill();
        } catch (const std::exception& e) {
            std::cerr << kTag << "oximetry backfill: " << e.what() << std::endl;
        }
    }
    {
        std::lock_guard<std::mutex> lk(mu_);
        for (const auto& [r, info] : roots_) roots.push_back({r, info.machine, info.source});
    }
    if (provider) {
        for (auto& r : provider()) {
            {
                std::lock_guard<std::mutex> lk(mu_);
                auto& m = roots_[r.path];
                if (!r.machine.empty()) m.machine = r.machine;
                if (!r.source.empty()) m.source = r.source;
            }
            roots.push_back(std::move(r));
        }
    }
    // SDD-079 D5: the oximetry outbox after every card root, so a night's
    // card files are ahead of its oximetry on every walk.
    std::stable_partition(roots.begin(), roots.end(),
                          [](const Root& r) { return r.source != card_feed::kSourceOximetry; });
    for (auto& r : roots) {
        Work w{r.path, "", r.machine};
        w.source = r.source;
        enqueue(std::move(w), false);
    }
}

void CpapDashStreamer::doPendingClear() {
    bool clear = false;
    {
        std::lock_guard<std::mutex> lk(mu_);
        std::swap(clear, clear_pending_);
        if (clear) {
            queue_.clear();
            queued_keys_.clear();
        }
    }
    if (!clear) return;
    settled_.clear();
    told_no_serial_.clear();
    noted_roots_.clear();   // SDD-079 D3: the source marks go with the index
    if (db_ && !db_->clearCpapDashSent("")) std::cerr << kTag << "the index could not be cleared" << std::endl;
    else std::cout << kTag << "index cleared (the switch changed)" << std::endl;
}

bool CpapDashStreamer::changed(uint64_t gen) const {
    std::lock_guard<std::mutex> lk(mu_);
    return gen_ != gen;
}

void CpapDashStreamer::waitForChange(uint64_t gen, std::optional<std::chrono::milliseconds> at_most) {
    std::unique_lock<std::mutex> lk(mu_);
    auto done = [&] { return stop_.load() || gen_ != gen || clear_pending_; };
    if (at_most) cv_.wait_for(lk, *at_most, done);
    else cv_.wait(lk, done);
}

void CpapDashStreamer::setState(const std::string& state, int http_status, const std::string& reason) {
    bool changed = false;
    std::function<void(const Status&)> listener;
    Status now;
    {
        std::lock_guard<std::mutex> lk(mu_);
        changed = status_.state != state || status_.http_status != http_status || status_.reason != reason;
        if (changed) status_.since = cdstream::isoNowUtc();
        status_.state = state;
        status_.http_status = http_status;
        status_.reason = reason;
        listener = state_listener_;
        now = status_;
    }
    // SDD-073 D6: the change goes out as it happens (the setup watch socket).
    if (changed && listener) listener(now);
}

void CpapDashStreamer::setStateListener(std::function<void(const Status&)> l) {
    std::lock_guard<std::mutex> lk(mu_);
    state_listener_ = std::move(l);
}

// ── the worker ───────────────────────────────────────────────────────────────

void CpapDashStreamer::run() {
    std::chrono::milliseconds backoff{0};
    while (!stopping()) {
        doPendingClear();
        Settings s;
        uint64_t gen = 0;
        Timing t;
        bool refused = false;
        {
            std::lock_guard<std::mutex> lk(mu_);
            s = settings_;
            gen = gen_;
            t = timing_;
            refused = refused_gen_ == gen_;
        }
        if (backoff.count() == 0) backoff = t.backoff_min;
        if (!s.stream) {
            setState("off");
            waitForChange(gen, std::nullopt);
            continue;
        }
        if (s.token.empty()) {
            setState("no_token");
            waitForChange(gen, std::nullopt);
            continue;
        }
        if (refused) {
            // A 401 stops the uploader until the token or the switch changes
            // (D9). The collectors carry on regardless.
            waitForChange(gen, std::nullopt);
            continue;
        }
        auto link = factory_ ? factory_() : nullptr;
        if (!link) {
            setState("unavailable", 0, "this build has no WebSocket client");
            waitForChange(gen, std::nullopt);
            continue;
        }

        setState("connecting");
        const auto c = link->connect(s.token, install_id_, "hms-cpap/" + version_, t.connect_timeout);
        if (!c.ok) {
            if (c.http_status == 401 || c.http_status == 403) {
                {
                    std::lock_guard<std::mutex> lk(mu_);
                    refused_gen_ = gen;
                }
                std::cerr << kTag << "CpapDash refused the token (HTTP " << c.http_status
                          << "); nothing is sent until it changes" << std::endl;
                setState("refused", c.http_status, c.error);
                continue;
            }
            std::cerr << kTag << "CpapDash unreachable: " << c.error
                      << (c.http_status ? " (HTTP " + std::to_string(c.http_status) + ")" : "")
                      << "; trying again in " << backoff.count() / 1000 << " s" << std::endl;
            setState("unreachable", c.http_status, c.error);
            waitForChange(gen, backoff);
            backoff = std::min(backoff * 2, t.backoff_max);
            continue;
        }

        const SessionEnd end = session(*link, gen);
        link->close();
        if (end.connected) backoff = t.backoff_min;
        switch (end.end) {
            case End::Stop:
            case End::SettingsChanged:
                break;
            case End::Bye: {
                // Two kinds, both waited out exactly as asked: a socket
                // deploy's bye (no reason; back within seconds, resuming from
                // the index), and CpapDash turning this install away (ingest
                // refused it, 300 s, or did not answer, 5 s; then a 1008
                // close), whose reason goes to Settings.
                const auto wait = std::chrono::milliseconds(
                    std::max<long long>(end.retry_in_s, 1) * 1000);
                std::cout << kTag << "CpapDash said bye" << (end.reason.empty() ? "" : ": " + end.reason)
                          << "; back in " << wait.count() / 1000 << " s" << std::endl;
                setState(end.reason.empty() ? "connecting" : "closed", 0, end.reason);
                waitForChange(gen, wait);
                break;
            }
            case End::Closed:
                if (end.reason.rfind(kRevokedPrefix, 0) == 0) {
                    // The gateway closes every socket of a revoked token
                    // (D7). As a 401: nothing more is sent until the token or
                    // the switch changes; the collectors carry on.
                    {
                        std::lock_guard<std::mutex> lk(mu_);
                        refused_gen_ = gen;
                    }
                    std::cerr << kTag << "CpapDash closed the connection: " << end.reason
                              << "; nothing is sent until the token changes" << std::endl;
                    setState("revoked", 0, end.reason);
                    break;
                }
                [[fallthrough]];
            default:
                std::cerr << kTag << "connection lost" << (end.reason.empty() ? "" : ": " + end.reason)
                          << "; trying again in " << backoff.count() / 1000 << " s" << std::endl;
                // A close CpapDash gave a reason for (a revoked token, D7) is
                // said in Settings as its own; the rest is the network.
                setState(end.end == End::Closed && !end.reason.empty() ? "closed" : "unreachable", 0,
                         end.reason);
                waitForChange(gen, backoff);
                backoff = std::min(backoff * 2, t.backoff_max);
                break;
        }
    }
    setState("off");
}

std::optional<CpapDashStreamer::SessionEnd> CpapDashStreamer::handle(CpapDashLink& link,
                                                                     const CpapDashLink::Frame& f) {
    using Kind = CpapDashLink::Frame::Kind;
    if (f.kind == Kind::Ping) return std::nullopt;
    if (f.kind == Kind::Closed) {
        SessionEnd e;
        e.end = End::Closed;
        e.reason = f.close_reason;
        return e;
    }
    Json::Value v;
    Json::CharReaderBuilder rb;
    std::string errs;
    std::istringstream in(f.text);
    if (!Json::parseFromStream(rb, in, &v, &errs) || !v.isObject()) {
        std::cerr << kTag << "unreadable frame from CpapDash" << std::endl;
        return std::nullopt;
    }
    const std::string t = v.get("t", "").asString();
    if (t == "ping") {
        link.sendText(R"({"t":"pong"})");
    } else if (t == "bye") {
        SessionEnd e;
        e.end = End::Bye;
        e.retry_in_s = v.get("retry_in_s", 5).asInt();
        e.reason = v.get("reason", "").asString();
        return e;
    } else if (t == "error") {
        std::cerr << kTag << "CpapDash: " << v.get("error", "").asString() << std::endl;
    } else if (t == "file_ack") {
        // An ack for a file this connection no longer waits on: nothing moves.
    }
    return std::nullopt;
}

CpapDashStreamer::SessionEnd CpapDashStreamer::session(CpapDashLink& link, uint64_t gen) {
    Timing t;
    {
        std::lock_guard<std::mutex> lk(mu_);
        t = timing_;
    }
    SessionEnd end;
    std::function<std::string()> source_of;
    {
        std::lock_guard<std::mutex> lk(mu_);
        source_of = source_provider_;
    }
    // Asked on every hello: a change of source shows on the next connect.
    // SDD-074: so is a change of the machine's clock setting.
    link.sendText(cdstream::helloFrame(
        version_, source_of ? source_of() : "ezshare",
        machine_clocks::zoneFor(machine_clocks::primaryDevice()).setting()));
    const auto hello_by = std::chrono::steady_clock::now() + t.hello_timeout;
    bool acked = false;
    while (!acked) {
        if (stopping()) return {End::Stop};
        if (changed(gen)) return {End::SettingsChanged};
        if (std::chrono::steady_clock::now() > hello_by) {
            end.end = End::HelloFailed;
            end.reason = "no answer to hello";
            return end;
        }
        auto f = link.next(std::chrono::milliseconds(100));
        if (!f) continue;
        if (f->kind == CpapDashLink::Frame::Kind::Text) {
            Json::Value v;
            Json::CharReaderBuilder rb;
            std::istringstream in(f->text);
            if (Json::parseFromStream(rb, in, &v, nullptr) && v.isObject() &&
                v.get("t", "").asString() == "ack") {
                acked = true;
                continue;
            }
        }
        if (auto e = handle(link, *f)) return *e;
    }

    end.connected = true;
    std::cout << kTag << "connected to CpapDash" << std::endl;
    setState("connected");
    // SDD-079 D6: a new connection is the next event a held oximetry waits for.
    oxi_held_ = false;
    {
        std::lock_guard<std::mutex> lk(mu_);
        status_.oximetry_state = "sending";
    }
    // D8, D9: every connection starts from the index, over every root held.
    markAllRootsDirty();

    auto last_rx = std::chrono::steady_clock::now();
    while (true) {
        if (stopping()) return {End::Stop, 0, "", true};
        if (changed(gen)) return {End::SettingsChanged, 0, "", true};
        while (auto f = link.next(std::chrono::milliseconds(0))) {
            last_rx = std::chrono::steady_clock::now();
            if (auto e = handle(link, *f)) {
                e->connected = true;
                return *e;
            }
        }
        if (std::chrono::steady_clock::now() - last_rx > t.silence) {
            return {End::Silence, 0, "CpapDash went silent", true};
        }
        auto w = popWork();
        if (!w) {
            if (auto f = link.next(std::chrono::milliseconds(250))) {
                last_rx = std::chrono::steady_clock::now();
                if (auto e = handle(link, *f)) {
                    e->connected = true;
                    return *e;
                }
            }
            continue;
        }
        if (w->round) {
            // Everything queued ahead of it has been sent and acked (or
            // refused, which ingest has answered too).
            if (!link.sendText(cdstream::roundFrame())) {
                enqueue(*w, true);
                SessionEnd lost;
                lost.end = End::Closed;
                lost.connected = true;
                return lost;
            }
            continue;
        }
        if (w->rel.empty()) {
            scanRoot(*w);
            continue;
        }
        SessionEnd lost;
        const bool oxi = sourceFor(w->root, w->source) == card_feed::kSourceOximetry;
        if ((oxi ? sendOximetry(link, *w, gen, lost) : sendFile(link, *w, gen, lost)) == SendResult::Lost) {
            // Not acked: it goes again first on the next connection, from
            // the index, so nothing past the last ack is lost.
            enqueue(*w, true);
            lost.connected = true;
            return lost;
        }
        last_rx = std::chrono::steady_clock::now();
    }
}

std::string CpapDashStreamer::machineFor(const std::string& root, const std::string& given) {
    if (!given.empty()) return given;
    {
        std::lock_guard<std::mutex> lk(mu_);
        auto it = roots_.find(root);
        if (it != roots_.end() && !it->second.machine.empty()) return it->second.machine;
    }
    auto it = machines_.find(root);
    if (it != machines_.end() && !it->second.empty()) return it->second;
    const auto id = readIdentification(root);
    if (!id.serial.empty()) machines_[root] = id.serial;
    return id.serial;
}

std::string CpapDashStreamer::sourceFor(const std::string& root, const std::string& given) {
    if (!given.empty()) return given;
    std::lock_guard<std::mutex> lk(mu_);
    auto it = roots_.find(root);
    if (it != roots_.end() && !it->second.source.empty()) return it->second.source;
    // A root no collector named: what absent means on the wire (SDD-061 D1).
    return card_feed::kSourceCard;
}

void CpapDashStreamer::scanRoot(const Work& w) {
    std::error_code ec;
    if (!fs::is_directory(w.root, ec)) return;
    std::vector<std::string> rels;
    const fs::path base(w.root);
    const std::string source = sourceFor(w.root, w.source);
    if (source == card_feed::kSourceOximetry) {
        // The outbox is flat (SDD-079 D4): every file it would send, by name.
        for (const auto& e : fs::directory_iterator(base, ec)) {
            if (!e.is_regular_file(ec)) continue;
            const std::string name = e.path().filename().string();
            if (oximetry_outbox::metaFor(w.root, name)) rels.push_back(name);
        }
        std::sort(rels.begin(), rels.end());
    }
    for (auto it = source == card_feed::kSourceOximetry
                       ? fs::recursive_directory_iterator()
                       : fs::recursive_directory_iterator(base, fs::directory_options::skip_permission_denied, ec);
         it != fs::recursive_directory_iterator(); it.increment(ec)) {
        if (ec) break;
        const auto rel = fs::relative(it->path(), base, ec).generic_string();
        if (ec) continue;
        if (it->is_directory(ec)) {
            // Only the card's own folders are walked into.
            if (it.depth() == 0) {
                std::string top = rel;
                for (auto& c : top) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
                // SDD-080: and a DreamStation 2's P-SERIES (platform SDD-061 D10).
                if (top != "DATALOG" && top != "SETTINGS" && top != "P-SERIES") it.disable_recursion_pending();
            }
            continue;
        }
        if (!it->is_regular_file(ec) || !cdstream::isCardPath(rel)) continue;
        rels.push_back(rel);
    }
    std::sort(rels.begin(), rels.end(), [](const std::string& a, const std::string& b) {
        const int ga = groupOf(a), gb = groupOf(b);
        return ga != gb ? ga < gb : a < b;
    });
    std::vector<Work> changed;
    for (const auto& rel : rels) {
        const fs::path p = base / rel;
        Stamp now;
        now.size = fs::file_size(p, ec);
        if (ec) continue;
        now.mtime = static_cast<long long>(fs::last_write_time(p, ec).time_since_epoch().count());
        auto s = settled_.find(keyOf(w.root, rel));
        if (s != settled_.end() && s->second == now) continue;
        Work f{w.root, rel, w.machine};
        f.source = source;
        changed.push_back(std::move(f));
    }
    // In place of the scan, in path order: ahead of anything queued after it,
    // a burst's end above all, so that end goes once these are acked.
    for (auto it = changed.rbegin(); it != changed.rend(); ++it) enqueue(*it, true);
}

CpapDashStreamer::SendResult CpapDashStreamer::sendFile(CpapDashLink& link, const Work& w,
                                                       uint64_t gen, SessionEnd& end) {
    Timing t;
    {
        std::lock_guard<std::mutex> lk(mu_);
        t = timing_;
    }
    const fs::path p = fs::path(w.root) / w.rel;
    std::error_code ec;
    Stamp stamp;
    stamp.size = fs::file_size(p, ec);
    if (ec) return SendResult::Done;   // gone since it was noted
    stamp.mtime = static_cast<long long>(fs::last_write_time(p, ec).time_since_epoch().count());

    const std::string machine = machineFor(w.root, w.machine);
    if (machine.empty()) {
        if (told_no_serial_.insert(w.root).second)
            std::cerr << kTag << w.root << " names no machine serial yet; its files wait" << std::endl;
        return SendResult::Done;
    }
    const auto headers = cdstream::cardHeadersFor(w.rel);
    if (!headers) return SendResult::Done;
    const std::string source = sourceFor(w.root, w.source);

    // SDD-079 D3: a constructed root's files acked before 7.0.0 say their
    // source first, before any new bytes of that root go, or a night still
    // growing could have its constructed bytes taken as the card's.
    if (source != card_feed::kSourceCard && !noted_roots_.count(w.root)) {
        switch (sendSourceNotes(link, w.root, source, machine, gen, end)) {
            case NotesResult::Lost: return SendResult::Lost;
            case NotesResult::Refused: return SendResult::Done;   // waits for the next event
            case NotesResult::Done: break;
        }
    }

    std::string bytes;
    if (!readFile(p, bytes)) return SendResult::Done;
    const std::string name = fs::path(w.rel).filename().string();
    const auto row = db_ ? db_->getCpapDashSent(w.root, w.rel) : std::nullopt;
    const auto plan = cdstream::planSend(name, bytes, row);
    const std::string key = keyOf(w.root, w.rel);
    if (!plan.send) {
        settled_[key] = stamp;
        return SendResult::Done;
    }

    const uint32_t req_id = nextReq();
    const std::size_t from = static_cast<std::size_t>(plan.offset);
    const std::size_t len = bytes.size() - from;
    const uint32_t chunks = sendChunks(link, req_id, bytes, from, t.chunk_bytes);
    const std::string md5 = cdstream::md5Hex(bytes, from, bytes.size());
    if (chunks == kChunksLost ||
        !link.sendText(cdstream::fileReadyFrame(req_id, *headers, plan.offset, bytes.size(), chunks,
                                                len, md5, machine, source))) {
        end.end = End::Closed;
        end.reason = "";
        return SendResult::Lost;
    }

    const auto acked = awaitAck(link, req_id, gen, end);
    if (!acked) return SendResult::Lost;
    // ok:true is a complete read that landed, or bytes ingest already holds:
    // the index advances (D6). ok:false is nothing kept: an incomplete read,
    // or a range starting past what ingest holds (a purge, say). Then the
    // file's row is reset so its next attempt goes WHOLE from 0; sending the
    // same range again would be refused forever.
    if (!*acked) {
        std::cerr << kTag << w.rel << " not kept by CpapDash (from byte " << plan.offset
                  << "); it goes again whole" << std::endl;
        if (db_) {
            IDatabase::CpapDashSent reset;
            reset.root = w.root;
            reset.path = w.rel;
            reset.machine = machine;
            reset.acked_to = 0;
            reset.acked_md5 = "";   // matches nothing: the plan is whole
            reset.updated_at = cdstream::isoNowUtc();
            reset.source = row ? row->source : "";   // not an ok ack: the source mark stays
            db_->upsertCpapDashSent(reset);
        }
        settled_.erase(key);
        // A refused range is tried again whole straight away; a refused whole
        // file waits for its next note or the next connection, so a file
        // CpapDash keeps refusing never loops.
        if (plan.offset > 0) enqueue(w, false);
        return SendResult::Done;
    }
    IDatabase::CpapDashSent r;
    r.root = w.root;
    r.path = w.rel;
    r.machine = machine;
    r.acked_to = static_cast<long long>(bytes.size());
    r.acked_md5 = cdstream::ackedMd5(name, bytes, bytes.size());
    r.updated_at = cdstream::isoNowUtc();
    r.source = source;   // SDD-079 D3: the source this ack delivered
    if (db_ && db_->upsertCpapDashSent(r)) {
        settled_[key] = stamp;
    } else {
        std::cerr << kTag << "the index did not take " << w.rel << std::endl;
    }
    {
        std::lock_guard<std::mutex> lk(mu_);
        status_.files_acked++;
        status_.bytes_acked += static_cast<long long>(len);
        status_.last_ack_at = r.updated_at;
    }
    return SendResult::Done;
}

uint32_t CpapDashStreamer::nextReq() {
    const uint32_t req_id = next_req_++;
    if (next_req_ == 0) next_req_ = 1;
    return req_id;
}

std::optional<bool> CpapDashStreamer::awaitAck(CpapDashLink& link, uint32_t req_id, uint64_t gen,
                                               SessionEnd& end) {
    Timing t;
    {
        std::lock_guard<std::mutex> lk(mu_);
        t = timing_;
    }
    const auto by = std::chrono::steady_clock::now() + t.ack_timeout;
    while (true) {
        if (stopping()) {
            end.end = End::Stop;
            return std::nullopt;
        }
        if (changed(gen)) {
            end.end = End::SettingsChanged;
            return std::nullopt;
        }
        if (std::chrono::steady_clock::now() > by) {
            end.end = End::AckTimeout;
            end.reason = "no file_ack";
            return std::nullopt;
        }
        auto f = link.next(std::chrono::milliseconds(100));
        if (!f) continue;
        if (f->kind == CpapDashLink::Frame::Kind::Text) {
            Json::Value v;
            Json::CharReaderBuilder rb;
            std::istringstream in(f->text);
            if (Json::parseFromStream(rb, in, &v, nullptr) && v.isObject() &&
                v.get("t", "").asString() == "file_ack" && v.get("req_id", 0).asUInt64() == req_id)
                return v.get("ok", false).asBool();
        }
        if (auto e = handle(link, *f)) {
            end = *e;
            return std::nullopt;
        }
    }
}

CpapDashStreamer::NotesResult CpapDashStreamer::sendSourceNotes(CpapDashLink& link,
                                                                const std::string& root,
                                                                const std::string& source,
                                                                const std::string& machine,
                                                                uint64_t gen, SessionEnd& end) {
    if (!db_) return NotesResult::Done;
    bool all_taken = true;
    for (auto row : db_->listCpapDashSent(root)) {
        // A row that already says its source, or one acked at 0 (a note at
        // offset 0 would make ingest discard the file: SDD-061 D2), needs none.
        if (!row.source.empty() || row.acked_to <= 0) continue;
        const auto headers = cdstream::cardHeadersFor(row.path);
        if (!headers) continue;
        const uint32_t req_id = nextReq();
        if (!link.sendText(cdstream::sourceNoteFrame(req_id, *headers,
                                                     static_cast<uint64_t>(row.acked_to),
                                                     row.machine.empty() ? machine : row.machine,
                                                     source))) {
            end.end = End::Closed;
            end.reason = "";
            return NotesResult::Lost;
        }
        const auto ok = awaitAck(link, req_id, gen, end);
        if (!ok) return NotesResult::Lost;
        if (!*ok) {
            // Not taken: the row is left as it is (no reset, the bytes are
            // held), and the note goes again on the next event.
            std::cerr << kTag << row.path << ": CpapDash did not take its source note" << std::endl;
            all_taken = false;
            continue;
        }
        row.source = source;
        row.updated_at = cdstream::isoNowUtc();
        if (!db_->upsertCpapDashSent(row)) {
            std::cerr << kTag << "the index did not take the source of " << row.path << std::endl;
            all_taken = false;
        }
    }
    if (!all_taken) return NotesResult::Refused;
    noted_roots_.insert(root);
    return NotesResult::Done;
}

CpapDashStreamer::SendResult CpapDashStreamer::sendOximetry(CpapDashLink& link, const Work& w,
                                                           uint64_t gen, SessionEnd& end) {
    // D6: CpapDash did not take oximetry on this connection. The outbox and
    // the index stay as they are; the next connection sends it.
    if (oxi_held_) return SendResult::Done;
    Timing t;
    {
        std::lock_guard<std::mutex> lk(mu_);
        t = timing_;
    }
    const auto meta = oximetry_outbox::metaFor(w.root, w.rel);
    if (!meta) return SendResult::Done;
    const fs::path p = fs::path(w.root) / w.rel;
    std::error_code ec;
    Stamp stamp;
    stamp.size = fs::file_size(p, ec);
    if (ec) return SendResult::Done;
    stamp.mtime = static_cast<long long>(fs::last_write_time(p, ec).time_since_epoch().count());

    std::string bytes;
    if (!readFile(p, bytes)) return SendResult::Done;
    // The live spool goes in whole lines only (SDD-061 D5): a line being
    // appended right now waits for the next note.
    if (meta->format == "live-jsonl") {
        const auto nl = bytes.rfind('\n');
        bytes.resize(nl == std::string::npos ? 0 : nl + 1);
    }
    const auto row = db_ ? db_->getCpapDashSent(w.root, w.rel) : std::nullopt;
    const auto plan = cdstream::planSend(w.rel, bytes, row);
    const std::string key = keyOf(w.root, w.rel);
    if (!plan.send) {
        if (bytes.size() == stamp.size) settled_[key] = stamp;
        return SendResult::Done;
    }

    cdstream::OxiHeaders oxi{meta->name, meta->format, meta->source, meta->device_name};
    const uint32_t req_id = nextReq();
    const std::size_t from = static_cast<std::size_t>(plan.offset);
    const std::size_t len = bytes.size() - from;
    const uint32_t chunks = sendChunks(link, req_id, bytes, from, t.chunk_bytes);
    if (chunks == kChunksLost ||
        !link.sendText(cdstream::oximetryReadyFrame(req_id, oxi, plan.offset, bytes.size(), chunks, len,
                                                    cdstream::md5Hex(bytes, from, bytes.size())))) {
        end.end = End::Closed;
        end.reason = "";
        return SendResult::Lost;
    }
    const auto acked = awaitAck(link, req_id, gen, end);
    if (!acked) return SendResult::Lost;
    if (!*acked) {
        // D6: not a failure. Oximetry is held for the rest of this connection,
        // card files carry on, and the index does not move.
        std::cerr << kTag << "CpapDash did not take oximetry (" << meta->name
                  << "); held until the next connection" << std::endl;
        oxi_held_ = true;
        settled_.erase(key);
        std::lock_guard<std::mutex> lk(mu_);
        status_.oximetry_state = "held";
        return SendResult::Done;
    }
    IDatabase::CpapDashSent r;
    r.root = w.root;
    r.path = w.rel;
    r.machine = "";
    r.acked_to = static_cast<long long>(bytes.size());
    r.acked_md5 = cdstream::ackedMd5(w.rel, bytes, bytes.size());
    r.updated_at = cdstream::isoNowUtc();
    r.source = card_feed::kSourceOximetry;
    if (db_ && db_->upsertCpapDashSent(r)) {
        if (bytes.size() == stamp.size) settled_[key] = stamp;
    } else {
        std::cerr << kTag << "the index did not take " << w.rel << std::endl;
    }
    {
        std::lock_guard<std::mutex> lk(mu_);
        status_.oximetry_acked++;
        status_.bytes_acked += static_cast<long long>(len);
        status_.last_ack_at = r.updated_at;
    }
    return SendResult::Done;
}

}  // namespace hms_cpap
