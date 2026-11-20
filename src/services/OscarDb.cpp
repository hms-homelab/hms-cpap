#include "services/OscarDb.h"

#include <sqlite3.h>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <filesystem>
#include <iostream>
#include <limits>
#include <map>

namespace hms_cpap::oscar {

namespace fs = std::filesystem;
using std::chrono::milliseconds;
using std::chrono::system_clock;

const std::vector<ProvenVersion>& provenVersions() {
    // OSCAR 2.1.1 (2026-10-31) on its schema 19: the one release proven so
    // far (SDD-080 section 7). Each later release is added once proven.
    static const std::vector<ProvenVersion> v{{"2.1.1", 19}};
    return v;
}

std::string sessionKey(const SessionHead& s) { return s.serial + "/" + std::to_string(s.session_id); }

system_clock::time_point sessionClock(long long oscar_ms) {
    return system_clock::time_point(milliseconds(oscar_ms));
}

namespace {

struct Stmt {
    sqlite3_stmt* s{nullptr};
    Stmt(sqlite3* db, const char* sql) {
        if (sqlite3_prepare_v2(db, sql, -1, &s, nullptr) != SQLITE_OK) s = nullptr;
    }
    ~Stmt() {
        if (s) sqlite3_finalize(s);
    }
    Stmt(const Stmt&) = delete;
    Stmt& operator=(const Stmt&) = delete;
    explicit operator bool() const { return s != nullptr; }
    bool row() { return sqlite3_step(s) == SQLITE_ROW; }
    std::string text(int c) const {
        const auto* t = sqlite3_column_text(s, c);
        return t ? reinterpret_cast<const char*>(t) : "";
    }
    long long i64(int c) const { return sqlite3_column_int64(s, c); }
    double f(int c) const { return sqlite3_column_double(s, c); }
    bool null(int c) const { return sqlite3_column_type(s, c) == SQLITE_NULL; }
};

/// The profile's row id, or -1.
long long profileId(sqlite3* db, const std::string& profile) {
    Stmt st(db, "SELECT id FROM profiles WHERE username = ?1");
    if (!st) return -1;
    sqlite3_bind_text(st.s, 1, profile.c_str(), -1, SQLITE_TRANSIENT);
    return st.row() ? st.i64(0) : -1;
}

std::vector<int16_t> int16s(const void* blob, int bytes) {
    std::vector<int16_t> out;
    if (!blob || bytes < 2) return out;
    const auto* b = static_cast<const unsigned char*>(blob);
    out.reserve(static_cast<size_t>(bytes / 2));
    for (int i = 0; i + 1 < bytes; i += 2)
        out.push_back(static_cast<int16_t>(static_cast<uint16_t>(b[i] | (b[i + 1] << 8))));
    return out;
}

std::vector<uint32_t> uint32s(const void* blob, int bytes) {
    std::vector<uint32_t> out;
    if (!blob || bytes < 4) return out;
    const auto* b = static_cast<const unsigned char*>(blob);
    out.reserve(static_cast<size_t>(bytes / 4));
    for (int i = 0; i + 3 < bytes; i += 4)
        out.push_back(static_cast<uint32_t>(b[i]) | (static_cast<uint32_t>(b[i + 1]) << 8) |
                      (static_cast<uint32_t>(b[i + 2]) << 16) | (static_cast<uint32_t>(b[i + 3]) << 24));
    return out;
}

/// The event channels hms-cpap keeps, by OSCAR's channel code.
std::optional<EventType> eventFor(const std::string& code) {
    static const std::map<std::string, EventType> m{
        {"Obstructive", EventType::OBSTRUCTIVE},   {"ClearAirway", EventType::CLEAR_AIRWAY},
        {"Hypopnea", EventType::HYPOPNEA},         {"Apnea", EventType::APNEA},
        {"RERA", EventType::RERA},                 {"FlowLimit", EventType::FLOW_LIMITATION},
        {"VSnore", EventType::VIBRATORY_SNORE},    {"PB", EventType::PERIODIC_BREATHING},
        {"LeakSpan", EventType::LARGE_LEAK},       {"CSR", EventType::CSR},
    };
    const auto it = m.find(code);
    if (it == m.end()) return std::nullopt;
    return it->second;
}

/// An apnea, hypopnea or RERA: its value is its length.
bool hasLength(EventType t) {
    return t == EventType::OBSTRUCTIVE || t == EventType::CLEAR_AIRWAY || t == EventType::HYPOPNEA ||
           t == EventType::APNEA || t == EventType::RERA;
}

/// One list of samples, decoded: (time ms, value).
struct Samples {
    std::string code;
    int channel_type{0};
    bool evenly{false};
    double rate_ms{0};
    long long first_ms{0};
    std::vector<long long> t;
    std::vector<double> v;
};

}  // namespace

OscarDb::~OscarDb() {
    end();
    if (db_) sqlite3_close(db_);
}

OscarDb::OscarDb(OscarDb&& o) noexcept : db_(o.db_), in_tx_(o.in_tx_) {
    o.db_ = nullptr;
    o.in_tx_ = false;
}

OscarDb& OscarDb::operator=(OscarDb&& o) noexcept {
    if (this != &o) {
        end();
        if (db_) sqlite3_close(db_);
        db_ = o.db_;
        in_tx_ = o.in_tx_;
        o.db_ = nullptr;
        o.in_tx_ = false;
    }
    return *this;
}

std::optional<OscarDb> OscarDb::open(const std::string& data_dir, std::string* why) {
    const fs::path file = fs::path(data_dir) / "oscar.db";
    std::error_code ec;
    if (data_dir.empty() || !fs::is_regular_file(file, ec)) {
        if (why) *why = "no oscar.db in " + data_dir;
        return std::nullopt;
    }
    sqlite3* raw = nullptr;
    // A normal open, never SQLITE_OPEN_READONLY (D6), and never CREATE: the
    // file is OSCAR's, and it is there.
    if (sqlite3_open_v2(file.string().c_str(), &raw, SQLITE_OPEN_READWRITE, nullptr) != SQLITE_OK) {
        if (why) *why = std::string("cannot open oscar.db: ") + (raw ? sqlite3_errmsg(raw) : "out of memory");
        if (raw) sqlite3_close(raw);
        return std::nullopt;
    }
    if (sqlite3_exec(raw, "PRAGMA query_only = ON", nullptr, nullptr, nullptr) != SQLITE_OK) {
        if (why) *why = "cannot make the connection to oscar.db query-only";
        sqlite3_close(raw);
        return std::nullopt;
    }
    return OscarDb(raw);
}

bool OscarDb::begin() {
    if (in_tx_) return true;
    in_tx_ = sqlite3_exec(db_, "BEGIN", nullptr, nullptr, nullptr) == SQLITE_OK;
    return in_tx_;
}

void OscarDb::end() {
    if (!in_tx_ || !db_) return;
    sqlite3_exec(db_, "COMMIT", nullptr, nullptr, nullptr);   // a read: nothing to commit
    in_tx_ = false;
}

Gate OscarDb::gate() {
    Gate g;
    {
        Stmt st(db_, "SELECT value FROM app_preferences WHERE key = 'VersionString' LIMIT 1");
        if (st && st.row()) g.version = st.text(0);
    }
    {
        Stmt st(db_, "SELECT COALESCE(MAX(version), 0) FROM schema_version");
        if (st && st.row()) g.schema = static_cast<int>(st.i64(0));
    }
    if (g.version.empty() || g.schema == 0) {
        g.why = "this folder's oscar.db does not say which OSCAR made it";
        return g;
    }
    for (const auto& p : provenVersions()) {
        if (g.version.rfind(p.version_prefix, 0) == 0 && g.schema == p.schema) {
            g.ok = true;
            return g;
        }
    }
    g.why = "OSCAR " + g.version + " (database schema " + std::to_string(g.schema) +
            ") is not supported yet";
    return g;
}

std::vector<std::string> OscarDb::profiles() {
    std::vector<std::string> out;
    Stmt st(db_, "SELECT username FROM profiles ORDER BY username");
    if (!st) return out;
    while (st.row()) out.push_back(st.text(0));
    return out;
}

std::string OscarDb::signature(const std::string& profile) {
    const long long pid = profileId(db_, profile);
    if (pid < 0) return "";
    Stmt st(db_,
            "SELECT (SELECT COUNT(*) FROM sessions s JOIN machines m ON m.id = s.machine_id "
            "        WHERE m.profile_id = ?1) || '/' || "
            "       (SELECT COUNT(*) FROM session_summaries WHERE profile_id = ?1) || '/' || "
            "       (SELECT COUNT(*) FROM session_channels WHERE profile_id = ?1) || '/' || "
            "       (SELECT COALESCE(MAX(s.end_time), 0) FROM sessions s JOIN machines m "
            "          ON m.id = s.machine_id WHERE m.profile_id = ?1)");
    if (!st) return "";
    sqlite3_bind_int64(st.s, 1, pid);
    return st.row() ? st.text(0) : "";
}

std::vector<SessionHead> OscarDb::sessions(const std::string& profile) {
    std::vector<SessionHead> out;
    const long long pid = profileId(db_, profile);
    if (pid < 0) return out;
    Stmt st(db_,
            "SELECT s.id, s.session_id, COALESCE(m.serial_number,''), COALESCE(m.brand,''), "
            "       COALESCE(m.model,''), COALESCE(m.model_number,''), s.start_time, s.end_time, "
            "       s.duration, COALESCE(s.enabled,1), COALESCE(s.summary_only,0), "
            "       COALESCE(ss.obstructive_count,0), COALESCE(ss.clear_airway_count,0), "
            "       COALESCE(ss.hypopnea_count,0), COALESCE(ss.unclassified_count,0), "
            "       COALESCE(ss.rera_count,0) "
            "FROM sessions s JOIN machines m ON m.id = s.machine_id "
            "LEFT JOIN session_summaries ss ON ss.session_id = s.id "
            "WHERE m.profile_id = ?1 AND m.loader_name <> 'Journal' "
            "ORDER BY s.start_time, s.id");
    if (!st) return out;
    sqlite3_bind_int64(st.s, 1, pid);
    while (st.row()) {
        SessionHead h;
        h.row_id = st.i64(0);
        h.session_id = st.i64(1);
        h.serial = st.text(2);
        h.brand = st.text(3);
        h.model = st.text(4);
        h.model_number = st.text(5);
        h.start_ms = st.i64(6);
        h.end_ms = st.i64(7);
        h.duration_ms = st.i64(8);
        h.enabled = st.i64(9) != 0;
        h.summary_only = st.i64(10) != 0;
        h.oa = static_cast<int>(st.i64(11));
        h.ca = static_cast<int>(st.i64(12));
        h.h = static_cast<int>(st.i64(13));
        h.ua = static_cast<int>(st.i64(14));
        h.rera = static_cast<int>(st.i64(15));
        out.push_back(std::move(h));
    }
    return out;
}

std::unique_ptr<CPAPSession> OscarDb::read(const SessionHead& head, const std::string& device_id,
                                           const std::string& device_name, std::string* why) {
    auto fail = [&](std::string w) -> std::unique_ptr<CPAPSession> {
        if (why) *why = std::move(w);
        return nullptr;
    };
    if (head.end_ms <= head.start_ms) return fail("session " + sessionKey(head) + " has no length");

    // ── its lists of samples ──
    std::vector<Samples> lists;
    {
        Stmt st(db_,
                "SELECT c.channel_code, COALESCE(c.type,0), el.event_type, el.first_time, el.rate, "
                "       el.gain, el.offset, COALESCE(ed.compression_method,0), ed.data_blob, "
                "       ed.time_blob "
                "FROM event_lists el JOIN channels c "
                "  ON c.channel_id = el.channel_id AND c.profile_id = el.profile_id "
                "LEFT JOIN event_data ed ON ed.eventlist_id = el.id "
                "WHERE el.session_id = ?1 ORDER BY c.channel_code, el.eventlist_index");
        if (!st) return fail(std::string("event lists: ") + sqlite3_errmsg(db_));
        sqlite3_bind_int64(st.s, 1, head.row_id);
        while (st.row()) {
            if (st.i64(7) != 0) {
                // Only uncompressed lists have been seen from OSCAR: said,
                // never guessed at.
                return fail("session " + sessionKey(head) + ": OSCAR's samples are compressed (method " +
                            std::to_string(st.i64(7)) + ")");
            }
            Samples s;
            s.code = st.text(0);
            s.channel_type = static_cast<int>(st.i64(1));
            s.evenly = st.i64(2) == 0;
            s.first_ms = st.i64(3);
            s.rate_ms = st.f(4);
            const double gain = st.f(5), offset = st.f(6);
            const auto raw = int16s(sqlite3_column_blob(st.s, 8), sqlite3_column_bytes(st.s, 8));
            std::vector<uint32_t> times;
            if (!s.evenly) {
                if (st.null(9)) continue;   // a timed list with no times: nothing to place
                times = uint32s(sqlite3_column_blob(st.s, 9), sqlite3_column_bytes(st.s, 9));
            }
            const size_t n = s.evenly ? raw.size() : std::min(raw.size(), times.size());
            s.t.reserve(n);
            s.v.reserve(n);
            for (size_t i = 0; i < n; ++i) {
                const long long t = s.evenly ? s.first_ms + static_cast<long long>(std::llround(i * s.rate_ms))
                                             : s.first_ms + static_cast<long long>(times[i]);
                s.t.push_back(t);
                s.v.push_back(raw[i] * gain + offset);
            }
            lists.push_back(std::move(s));
        }
    }

    auto session = std::make_unique<CPAPSession>();
    session->device_id = device_id;
    session->device_name = device_name;
    session->manufacturer = DeviceManufacturer::PHILIPS;
    session->serial_number = head.serial;
    session->product_name = head.model;
    {
        std::string digits;
        for (char c : head.model_number) {
            if (std::isdigit(static_cast<unsigned char>(c))) digits.push_back(c);
            else break;
        }
        if (!digits.empty()) {
            try {
                session->model_id = std::stoi(digits);
            } catch (...) {
            }
        }
    }
    session->session_start = sessionClock(head.start_ms);
    session->session_end = sessionClock(head.end_ms);
    const long long length_ms = head.duration_ms > 0 ? head.duration_ms : head.end_ms - head.start_ms;
    session->duration_seconds = static_cast<int>(length_ms / 1000);
    session->mask_on_periods.push_back({*session->session_start, *session->session_end});
    // OSCAR stores a session once it is closed (section 3): its summary is in.
    session->has_summary = true;
    session->status = CPAPSession::Status::COMPLETED;
    session->file_complete = true;

    // ── events: flags and spans ──
    // OSCAR places an event at its end, its value its length in seconds, so
    // the onset is the time less the length. To be confirmed against the
    // user's own OSCAR day view (SDD-080 section 7 item 3).
    for (const auto& s : lists) {
        if (s.channel_type != 4 && s.channel_type != 8 && s.channel_type != 16) continue;
        const auto type = eventFor(s.code);
        if (!type) continue;
        const bool span = s.channel_type == 16;
        for (size_t i = 0; i < s.t.size(); ++i) {
            const double len = (span || hasLength(*type)) ? std::max(0.0, s.v[i]) : 0.0;
            const long long onset = s.t[i] - static_cast<long long>(std::llround(len * 1000.0));
            session->events.emplace_back(*type, sessionClock(onset), len);
        }
        session->has_events = true;
    }
    std::sort(session->events.begin(), session->events.end(),
              [](const CPAPEvent& a, const CPAPEvent& b) { return a.timestamp < b.timestamp; });

    // ── the flow: one series, gaps left empty ──
    std::vector<const Samples*> flow_lists;
    for (const auto& s : lists)
        if (s.code == "FlowRate" && s.evenly && s.rate_ms > 0 && !s.t.empty()) flow_lists.push_back(&s);
    if (!flow_lists.empty()) {
        std::sort(flow_lists.begin(), flow_lists.end(),
                  [](const Samples* a, const Samples* b) { return a->first_ms < b->first_ms; });
        const double rate_ms = flow_lists.front()->rate_ms;
        const double hz = 1000.0 / rate_ms;
        const long long base_ms = flow_lists.front()->first_ms;
        const long long last_ms = flow_lists.back()->t.back();
        const size_t n = static_cast<size_t>((last_ms - base_ms) / rate_ms) + 1;
        std::vector<double> flow(n, std::numeric_limits<double>::quiet_NaN());
        for (const auto* l : flow_lists) {
            for (size_t i = 0; i < l->t.size(); ++i) {
                const long long at = static_cast<long long>(std::llround((l->t[i] - base_ms) / rate_ms));
                if (at >= 0 && static_cast<size_t>(at) < n) flow[static_cast<size_t>(at)] = l->v[i];
            }
        }
        const auto base = sessionClock(base_ms);

        // The trends hms-cpap keeps per minute, by OSCAR's channel code.
        struct Trend {
            const char* code;
            std::map<long, std::pair<double, int>> by_minute;   // minute -> (sum, n)
        };
        Trend pressure{"Pressure", {}}, leak{"LeakTotal", {}}, snore{"Snore", {}}, epap{"EPAP", {}};
        for (const auto& s : lists) {
            if (s.channel_type != 32) continue;
            Trend* tr = s.code == pressure.code ? &pressure
                        : s.code == leak.code   ? &leak
                        : s.code == snore.code  ? &snore
                        : s.code == epap.code   ? &epap
                                                : nullptr;
            if (!tr) continue;
            for (size_t i = 0; i < s.t.size(); ++i) {
                if (s.t[i] < base_ms) continue;
                auto& cell = tr->by_minute[static_cast<long>((s.t[i] - base_ms) / 60000)];
                cell.first += s.v[i];
                ++cell.second;
            }
        }
        const auto mean = [](const Trend& tr, long minute) -> std::optional<double> {
            const auto it = tr.by_minute.find(minute);
            if (it == tr.by_minute.end() || it->second.second == 0) return std::nullopt;
            return it->second.first / it->second.second;
        };

        // The same single pass of the shared breath detector every flow-only
        // reader uses, bucketed by the minute a breath starts in.
        const auto breaths = EDFParser::detectBreaths(flow, hz);
        const int per_minute = static_cast<int>(hz * 60);
        const int minutes = per_minute > 0 ? static_cast<int>(flow.size()) / per_minute : 0;
        std::vector<std::vector<EDFParser::BreathCycle>> by_minute(static_cast<size_t>(std::max(0, minutes)));
        for (const auto& b : breaths) {
            const int m = b.start_idx / std::max(1, per_minute);
            if (m >= 0 && m < minutes) by_minute[static_cast<size_t>(m)].push_back(b);
        }
        for (int m = 0; m < minutes; ++m) {
            const int from = m * per_minute, to = from + per_minute;
            int valid = 0;
            double sum = 0, hi = -1e9, lo = 1e9;
            for (int i = from; i < to; ++i) {
                if (std::isnan(flow[static_cast<size_t>(i)])) continue;
                ++valid;
                sum += flow[static_cast<size_t>(i)];
                hi = std::max(hi, flow[static_cast<size_t>(i)]);
                lo = std::min(lo, flow[static_cast<size_t>(i)]);
            }
            if (valid < per_minute / 2) continue;
            BreathingSummary row(base + std::chrono::minutes(m));
            row.pressure_measured = false;   // the waveform is flow only
            row.avg_flow_rate = sum / valid;
            row.max_flow_rate = hi;
            row.min_flow_rate = lo;
            EDFParser::calculateRespiratoryMetrics(flow, {}, by_minute[static_cast<size_t>(m)], hz, m, row);
            // TOTAL leak only, as the plain Philips reader keeps it: no
            // unintentional leak is derived for a Philips minute.
            row.leak_rate = std::nullopt;
            row.therapy_pressure = mean(pressure, m);
            row.total_leak = mean(leak, m);
            row.snore_index = mean(snore, m);
            row.epr_pressure = mean(epap, m);
            session->breathing_summary.push_back(row);
        }
        session->breaths.reserve(breaths.size());
        for (const auto& b : breaths) {
            Breath out;
            out.onset = base + std::chrono::microseconds(static_cast<long long>(b.start_idx / hz * 1e6));
            out.tidal_volume = b.tidal_volume;
            out.inspiratory_time = b.inspiratory_time;
            out.expiratory_time = b.expiratory_time;
            out.flow_limitation = b.flow_limitation;
            session->breaths.push_back(out);
        }
    }

    session->calculateMetrics();

    // ── OSCAR's per-channel statistics fill what the minutes cannot ──
    if (session->metrics) {
        Stmt st(db_,
                "SELECT c.channel_code, sc.wavg, sc.min, sc.max, sc.median, sc.p95 "
                "FROM session_channels sc JOIN channels c "
                "  ON c.channel_id = sc.channel_id AND c.profile_id = sc.profile_id "
                "WHERE sc.session_id = ?1 AND sc.count > 0 AND c.channel_code = 'Pressure'");
        if (st) {
            sqlite3_bind_int64(st.s, 1, head.row_id);
            if (st.row()) {
                auto& mt = *session->metrics;
                if (!mt.avg_pressure) mt.avg_pressure = st.f(1);
                if (!mt.min_pressure) mt.min_pressure = st.f(2);
                if (!mt.max_pressure) mt.max_pressure = st.f(3);
                if (!mt.pressure_p50) mt.pressure_p50 = st.f(4);
                if (!mt.pressure_p95) mt.pressure_p95 = st.f(5);
            }
        }
        // A cross-check only: the counts are the events'.
        const auto& mt = *session->metrics;
        if (mt.obstructive_apneas != head.oa || mt.clear_airway_apneas != head.ca ||
            mt.hypopneas != head.h || mt.reras != head.rera) {
            std::cerr << "OSCAR: session " << sessionKey(head) << ": its events count OA " << mt.obstructive_apneas
                      << " CA " << mt.clear_airway_apneas << " H " << mt.hypopneas << " RERA " << mt.reras
                      << ", OSCAR's summary OA " << head.oa << " CA " << head.ca << " H " << head.h << " RERA "
                      << head.rera << std::endl;
        }
    }
    return session;
}

}  // namespace hms_cpap::oscar
