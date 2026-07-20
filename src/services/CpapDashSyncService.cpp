#include "services/CpapDashSyncService.h"
#include "utils/AppConfig.h"
#include "utils/TimeCompat.h"

#include <curl/curl.h>
#include <nlohmann/json.hpp>

#include <algorithm>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <random>
#include <sstream>

namespace hms_cpap {

using json = nlohmann::json;

namespace {

constexpr const char* kSyncPath = "/v1/equipment/sync";
constexpr int kMaxPasses = 2;

std::string trimTrailingSlash(std::string s) {
    while (!s.empty() && s.back() == '/') s.pop_back();
    return s;
}

// Json::Value cell (executeQuery yields strings or null) -> std::string.
std::string cell(const Json::Value& row, const char* key) {
    if (!row.isMember(key) || row[key].isNull()) return "";
    return row[key].asString();
}

// SQLite stores booleans as 0/1, Postgres renders them as "t"/"f", MySQL as 0/1.
bool cellBool(const Json::Value& row, const char* key) {
    const std::string v = cell(row, key);
    return v == "1" || v == "t" || v == "true" || v == "TRUE";
}

int cellInt(const Json::Value& row, const char* key) {
    const std::string v = cell(row, key);
    if (v.empty()) return 0;
    try { return std::stoi(v); } catch (const std::exception&) { return 0; }
}

std::string jstr(const json& j, const char* key) {
    auto it = j.find(key);
    if (it == j.end() || !it->is_string()) return "";
    return it->get<std::string>();
}

bool jbool(const json& j, const char* key, bool def) {
    auto it = j.find(key);
    if (it == j.end() || !it->is_boolean()) return def;
    return it->get<bool>();
}

int jint(const json& j, const char* key, int def) {
    auto it = j.find(key);
    if (it == j.end() || !it->is_number_integer()) return def;
    return it->get<int>();
}

size_t curlWriteCb(void* ptr, size_t size, size_t nmemb, void* userdata) {
    static_cast<std::string*>(userdata)->append(static_cast<char*>(ptr), size * nmemb);
    return size * nmemb;
}

// Serialise one local profile for the push. `server_id` <= 0 means the cloud has
// never seen this row and must match it on client_uuid.
json profilePayload(const IDatabase::EquipmentProfile& p, int server_id) {
    json j;
    if (server_id > 0) j["id"] = server_id;
    j["client_uuid"] = p.client_uuid;
    j["name"]        = p.name;
    j["active"]      = p.active;
    j["deleted"]     = p.deleted;
    j["updated_at"]  = p.updated_at;
    return j;
}

json itemPayload(const IDatabase::EquipmentItem& it, int server_id, int server_profile_id) {
    json j;
    if (server_id > 0)         j["id"]         = server_id;
    if (server_profile_id > 0) j["profile_id"] = server_profile_id;
    j["client_uuid"]      = it.client_uuid;
    j["type_key"]         = it.type_key;
    j["brand"]            = it.brand;
    j["model"]            = it.model;
    j["variant"]          = it.variant;
    j["started_using_at"] = it.started_using_at;
    j["notes"]            = it.notes;
    j["active"]           = it.active;
    j["deleted"]          = it.deleted;
    j["updated_at"]       = it.updated_at;
    // -1 is the "use the type default" sentinel; the wire spells that null.
    if (it.replace_after_days >= 0) j["replace_after_days"] = it.replace_after_days;
    else                            j["replace_after_days"] = nullptr;
    return j;
}

} // namespace

// ─────────────────────────────────────────────────────────────────────────────
// Helpers
// ─────────────────────────────────────────────────────────────────────────────

std::string CpapDashSyncService::makeUuid() {
    static thread_local std::mt19937_64 rng{std::random_device{}()};
    std::uniform_int_distribution<int> hex(0, 15);

    static const char* digits = "0123456789abcdef";
    std::string out;
    out.reserve(36);
    for (int i = 0; i < 36; ++i) {
        if (i == 8 || i == 13 || i == 18 || i == 23) { out.push_back('-'); continue; }
        if (i == 14) { out.push_back('4'); continue; }               // version 4
        int v = hex(rng);
        if (i == 19) v = (v & 0x3) | 0x8;                            // variant 10xx
        out.push_back(digits[v]);
    }
    return out;
}

long long CpapDashSyncService::parseTimestampEpoch(const std::string& ts) {
    // Accepts what all four writers produce: SQLite "YYYY-MM-DD HH:MM:SS",
    // hms-cpap's normalised "YYYY-MM-DDTHH:MM:SSZ", and the cloud's Postgres
    // "YYYY-MM-DDTHH:MM:SS+02". Anything else is "unknown" (-1), which loses to
    // local so an unreadable stamp can never destroy local data.
    if (ts.size() < 19) return -1;

    struct tm tm {};
    int y = 0, mo = 0, d = 0, h = 0, mi = 0, s = 0;
    char sep = 0;
    if (std::sscanf(ts.c_str(), "%4d-%2d-%2d%c%2d:%2d:%2d",
                    &y, &mo, &d, &sep, &h, &mi, &s) != 7) return -1;
    if (sep != 'T' && sep != ' ') return -1;
    if (mo < 1 || mo > 12 || d < 1 || d > 31) return -1;

    tm.tm_year = y - 1900;
    tm.tm_mon  = mo - 1;
    tm.tm_mday = d;
    tm.tm_hour = h;
    tm.tm_min  = mi;
    tm.tm_sec  = s;
    tm.tm_isdst = 0;

    time_t base = timegm_utc(&tm);
    if (base == static_cast<time_t>(-1)) return -1;
    long long epoch = static_cast<long long>(base);

    // Optional trailing offset, skipping any fractional seconds.
    for (size_t i = 19; i < ts.size(); ++i) {
        const char c = ts[i];
        if (c == '+' || c == '-') {
            int oh = 0, om = 0;
            const std::string rest = ts.substr(i + 1);
            if (std::sscanf(rest.c_str(), "%2d:%2d", &oh, &om) < 1) return -1;
            const long long off = oh * 3600LL + om * 60LL;
            epoch += (c == '+') ? -off : off;
            break;
        }
        if (c == 'Z' || c == 'z') break;
    }
    return epoch;
}

std::string CpapDashSyncService::statePath() const {
    if (!state_path_.empty()) return state_path_;
    return (std::filesystem::path(AppConfig::dataDir()) / "cpapdash_sync.json").string();
}

CpapDashSyncService::State CpapDashSyncService::loadState() const {
    State st;
    std::ifstream f(statePath());
    if (!f.is_open()) return st;

    std::stringstream ss;
    ss << f.rdbuf();
    json j = json::parse(ss.str(), nullptr, false);
    if (j.is_discarded() || !j.is_object()) return st;

    st.cursor             = jstr(j, "cursor");
    st.pushed_through     = jstr(j, "pushed_through");
    st.default_profile_id = jint(j, "default_profile_id", 0);
    for (const char* key : {"profile_ids", "item_ids"}) {
        auto it = j.find(key);
        if (it == j.end() || !it->is_object()) continue;
        auto& into = (std::string(key) == "profile_ids") ? st.profile_ids : st.item_ids;
        for (auto e = it->begin(); e != it->end(); ++e)
            if (e.value().is_number_integer()) into[e.key()] = e.value().get<int>();
    }
    return st;
}

bool CpapDashSyncService::saveState(const State& s) const {
    json j;
    j["cursor"]             = s.cursor;
    j["pushed_through"]     = s.pushed_through;
    j["default_profile_id"] = s.default_profile_id;
    j["profile_ids"]        = json::object();
    j["item_ids"]           = json::object();
    for (const auto& [uuid, id] : s.profile_ids) j["profile_ids"][uuid] = id;
    for (const auto& [uuid, id] : s.item_ids)    j["item_ids"][uuid]    = id;

    const std::filesystem::path path = statePath();
    std::error_code ec;
    if (path.has_parent_path()) std::filesystem::create_directories(path.parent_path(), ec);

    std::ofstream f(path, std::ios::trunc);
    if (!f.is_open()) {
        std::cerr << "[cpapdash] cannot write sync state to " << path.string() << std::endl;
        return false;
    }
    f << j.dump(2);
    return true;
}

std::string CpapDashSyncService::cursor() const { return loadState().cursor; }

std::vector<CpapDashSyncService::RawItem> CpapDashSyncService::rawItems() const {
    std::vector<RawItem> out;
    if (!db_) return out;

    // Parameterless on purpose: executeQuery's placeholder dialect differs per
    // backend ($1 vs ?), and this needs to read identically on all three.
    const Json::Value rows = db_->executeQuery(
        "SELECT id, client_uuid, type_key, profile_id, deleted, updated_at"
        " FROM cpap_equipment_items ORDER BY id");

    for (const auto& r : rows) {
        RawItem it;
        it.id          = cellInt(r, "id");
        it.client_uuid = cell(r, "client_uuid");
        it.type_key    = cell(r, "type_key");
        it.profile_id  = cellInt(r, "profile_id");
        it.deleted     = cellBool(r, "deleted");
        it.updated_at  = cell(r, "updated_at");
        if (it.id > 0) out.push_back(std::move(it));
    }
    return out;
}

int CpapDashSyncService::backfillUuids() {
    if (!db_) return 0;
    int n = 0;

    for (auto p : db_->listEquipmentProfiles(/*include_deleted=*/true)) {
        if (!p.client_uuid.empty()) continue;
        p.client_uuid = makeUuid();
        if (db_->upsertEquipmentProfile(p) > 0) ++n;
    }
    // Tombstoned items are deliberately skipped: listEquipmentItems() cannot see
    // them, and a tombstone with no uuid was never mirrored, so the cloud has
    // nothing to delete.
    for (auto it : db_->listEquipmentItems(/*include_history=*/true)) {
        if (!it.client_uuid.empty()) continue;
        it.client_uuid = makeUuid();
        if (db_->upsertEquipmentItem(it) > 0) ++n;
    }
    return n;
}

// ─────────────────────────────────────────────────────────────────────────────
// Transport
// ─────────────────────────────────────────────────────────────────────────────

bool CpapDashSyncService::post(const std::string& body, std::string& out) const {
    const std::string url = trimTrailingSlash(settings_.api_url) + kSyncPath;

    if (transport_) return transport_(url, body, out);

    CURL* c = curl_easy_init();
    if (!c) return false;

    struct curl_slist* h = nullptr;
    const std::string auth = "Authorization: Bearer " + settings_.token;
    h = curl_slist_append(h, auth.c_str());
    h = curl_slist_append(h, "Content-Type: application/json");
    h = curl_slist_append(h, "accept: application/json");

    curl_easy_setopt(c, CURLOPT_URL, url.c_str());
    curl_easy_setopt(c, CURLOPT_HTTPHEADER, h);
    curl_easy_setopt(c, CURLOPT_POSTFIELDS, body.c_str());
    curl_easy_setopt(c, CURLOPT_WRITEFUNCTION, curlWriteCb);
    curl_easy_setopt(c, CURLOPT_WRITEDATA, &out);
    curl_easy_setopt(c, CURLOPT_TIMEOUT, 30L);

    const CURLcode rc = curl_easy_perform(c);
    long status = 0;
    curl_easy_getinfo(c, CURLINFO_RESPONSE_CODE, &status);
    curl_slist_free_all(h);
    curl_easy_cleanup(c);

    if (rc != CURLE_OK) {
        std::cerr << "[cpapdash] transport error: " << curl_easy_strerror(rc) << std::endl;
        return false;
    }
    if (status < 200 || status >= 300) {
        // 401 here means the pasted token was revoked or is wrong. Nothing is
        // written locally; the user keeps working offline until they repaste it.
        std::cerr << "[cpapdash] sync rejected with HTTP " << status << std::endl;
        return false;
    }
    return true;
}

// ─────────────────────────────────────────────────────────────────────────────
// One push/apply round trip
// ─────────────────────────────────────────────────────────────────────────────

bool CpapDashSyncService::exchange(const std::string& watermark, State& state, Result& acc,
                                   bool& needs_second_pass, std::string& new_watermark) {
    needs_second_pass = false;
    new_watermark     = watermark;

    const long long wm_epoch = watermark.empty() ? -1 : parseTimestampEpoch(watermark);
    auto changedSince = [&](const std::string& updated_at, long long& row_epoch) {
        row_epoch = parseTimestampEpoch(updated_at);
        if (wm_epoch < 0) return true;              // never synced: push everything
        if (row_epoch < 0) return true;             // unreadable stamp: push, never skip
        return row_epoch > wm_epoch;
    };
    long long max_pushed = -1;

    // ── Build the push ───────────────────────────────────────────────────────
    auto profiles = db_->listEquipmentProfiles(/*include_deleted=*/true);
    std::map<std::string, int> profile_uuid_to_local;   // uuid -> LOCAL profile id
    std::map<int, std::string> local_profile_uuid;      // LOCAL profile id -> uuid
    for (const auto& p : profiles) {
        if (p.client_uuid.empty()) continue;
        profile_uuid_to_local[p.client_uuid] = p.id;
        local_profile_uuid[p.id] = p.client_uuid;
    }

    json push_profiles = json::array();
    for (const auto& p : profiles) {
        if (p.client_uuid.empty()) continue;
        long long e = -1;
        if (!changedSince(p.updated_at, e)) continue;
        const auto bound = state.profile_ids.find(p.client_uuid);
        push_profiles.push_back(
            profilePayload(p, bound == state.profile_ids.end() ? 0 : bound->second));
        max_pushed = std::max(max_pushed, e);
    }

    // Server profile id for a local profile, 0 when the cloud has not seen it yet.
    auto serverProfileId = [&](int local_profile_id) -> int {
        auto u = local_profile_uuid.find(local_profile_id);
        if (u == local_profile_uuid.end()) return 0;
        auto b = state.profile_ids.find(u->second);
        return b == state.profile_ids.end() ? 0 : b->second;
    };

    json push_items = json::array();
    for (const auto& it : db_->listEquipmentItems(/*include_history=*/true)) {
        if (it.client_uuid.empty()) continue;
        long long e = -1;
        if (!changedSince(it.updated_at, e)) continue;
        const auto bound = state.item_ids.find(it.client_uuid);
        const int sp = serverProfileId(it.profile_id);
        if (sp <= 0) needs_second_pass = true;
        push_items.push_back(
            itemPayload(it, bound == state.item_ids.end() ? 0 : bound->second, sp));
        max_pushed = std::max(max_pushed, e);
    }

    // Local tombstones, which listEquipmentItems() cannot return. Only rows that
    // already carry a uuid are worth sending: anything else was never mirrored.
    for (const auto& raw : rawItems()) {
        if (!raw.deleted || raw.client_uuid.empty()) continue;
        long long e = -1;
        if (!changedSince(raw.updated_at, e)) continue;

        IDatabase::EquipmentItem ghost;
        ghost.client_uuid = raw.client_uuid;
        ghost.type_key    = raw.type_key;   // the cloud drops items with no type_key
        ghost.profile_id  = raw.profile_id;
        ghost.active      = false;
        ghost.deleted     = true;
        ghost.updated_at  = raw.updated_at;
        const auto bound = state.item_ids.find(raw.client_uuid);
        push_items.push_back(
            itemPayload(ghost, bound == state.item_ids.end() ? 0 : bound->second,
                        serverProfileId(raw.profile_id)));
        max_pushed = std::max(max_pushed, e);
    }

    acc.pushed_profiles += static_cast<int>(push_profiles.size());
    acc.pushed_items    += static_cast<int>(push_items.size());

    json req;
    req["since"]    = state.cursor;
    req["profiles"] = push_profiles;
    req["items"]    = push_items;

    std::string raw_response;
    if (!post(req.dump(), raw_response)) {
        acc.error = "cpapdash sync failed: cloud unreachable or rejected the token";
        return false;
    }

    json resp = json::parse(raw_response, nullptr, false);
    if (resp.is_discarded() || !resp.is_object()) {
        acc.error = "cpapdash sync failed: malformed response";
        return false;
    }

    // ── Apply the response ───────────────────────────────────────────────────
    // Nothing below here can run before the response parsed cleanly, which is what
    // keeps a failed sync free of partial writes.

    if (resp.contains("default_profile_id") && resp["default_profile_id"].is_number_integer())
        state.default_profile_id = resp["default_profile_id"].get<int>();

    std::map<std::string, IDatabase::EquipmentProfile> local_profile_by_uuid;
    for (const auto& p : profiles)
        if (!p.client_uuid.empty()) local_profile_by_uuid[p.client_uuid] = p;

    // Strictly-newer wins; ties and unreadable stamps keep the local row. This is
    // the guard against a cloud response clobbering a more recent local edit.
    auto remoteWins = [&](const std::string& remote_ts, const std::string& local_ts) {
        const long long r = parseTimestampEpoch(remote_ts);
        const long long l = parseTimestampEpoch(local_ts);
        if (r < 0 || l < 0) return false;
        return r > l;
    };

    if (resp.contains("profiles") && resp["profiles"].is_array()) {
        for (const auto& rp : resp["profiles"]) {
            if (!rp.is_object()) continue;
            const std::string uuid = jstr(rp, "client_uuid");
            if (uuid.empty()) continue;

            const int server_id = jint(rp, "id", 0);
            if (server_id > 0) state.profile_ids[uuid] = server_id;

            const bool rdel = jbool(rp, "deleted", false);
            const auto found = local_profile_by_uuid.find(uuid);

            if (found == local_profile_by_uuid.end()) {
                if (rdel) continue;                       // a tombstone we never had
                IDatabase::EquipmentProfile p;
                p.client_uuid = uuid;
                p.name        = jstr(rp, "name");
                if (p.name.empty()) p.name = "My CPAP";
                p.active      = jbool(rp, "active", true);
                const int id = db_->upsertEquipmentProfile(p);
                if (id > 0) {
                    ++acc.applied_profiles;
                    profile_uuid_to_local[uuid] = id;
                    local_profile_uuid[id] = uuid;
                }
                continue;
            }

            IDatabase::EquipmentProfile local = found->second;
            if (!remoteWins(jstr(rp, "updated_at"), local.updated_at)) { ++acc.kept_local; continue; }

            if (rdel && !local.deleted) {
                if (db_->tombstoneEquipmentProfile(local.id)) ++acc.deleted_locally;
                continue;
            }
            local.name    = jstr(rp, "name").empty() ? local.name : jstr(rp, "name");
            local.active  = jbool(rp, "active", local.active);
            local.deleted = rdel;
            if (db_->upsertEquipmentProfile(local) > 0) ++acc.applied_profiles;
        }
    }

    // Server profile id -> local profile id, now that new profiles exist locally.
    std::map<int, int> server_to_local_profile;
    for (const auto& [uuid, server_id] : state.profile_ids) {
        auto l = profile_uuid_to_local.find(uuid);
        if (l != profile_uuid_to_local.end()) server_to_local_profile[server_id] = l->second;
    }

    std::map<std::string, RawItem> local_item_by_uuid;   // includes tombstones
    for (const auto& raw : rawItems())
        if (!raw.client_uuid.empty()) local_item_by_uuid[raw.client_uuid] = raw;

    if (resp.contains("items") && resp["items"].is_array()) {
        for (const auto& ri : resp["items"]) {
            if (!ri.is_object()) continue;
            const std::string uuid = jstr(ri, "client_uuid");
            if (uuid.empty()) continue;

            const int server_id = jint(ri, "id", 0);
            if (server_id > 0) state.item_ids[uuid] = server_id;

            const bool rdel  = jbool(ri, "deleted", false);
            const auto found = local_item_by_uuid.find(uuid);

            if (found != local_item_by_uuid.end() &&
                !remoteWins(jstr(ri, "updated_at"), found->second.updated_at)) {
                ++acc.kept_local;
                continue;
            }
            if (found == local_item_by_uuid.end() && rdel) continue;

            if (found != local_item_by_uuid.end() && rdel) {
                if (!found->second.deleted && db_->tombstoneEquipmentItem(found->second.id))
                    ++acc.deleted_locally;
                continue;
            }

            IDatabase::EquipmentItem item;
            item.id          = (found == local_item_by_uuid.end()) ? 0 : found->second.id;
            item.client_uuid = uuid;
            item.type_key    = jstr(ri, "type_key");
            if (item.type_key.empty()) item.type_key = jstr(ri, "slot");   // app alias
            if (item.type_key.empty()) continue;
            // category is deliberately left empty: the backend resolves it from the
            // type, which is what keeps the one-machine-per-profile rule working.
            item.brand            = jstr(ri, "brand");
            item.model            = jstr(ri, "model");
            item.variant          = jstr(ri, "variant");
            item.started_using_at = jstr(ri, "started_using_at");
            item.notes            = jstr(ri, "notes");
            item.active           = jbool(ri, "active", true);
            item.deleted          = false;
            item.replace_after_days =
                (ri.contains("replace_after_days") && ri["replace_after_days"].is_number_integer())
                    ? ri["replace_after_days"].get<int>()
                    : -1;

            const int remote_profile = jint(ri, "profile_id", 0);
            const auto mapped = server_to_local_profile.find(remote_profile);
            if (mapped != server_to_local_profile.end())      item.profile_id = mapped->second;
            else if (found != local_item_by_uuid.end())       item.profile_id = found->second.profile_id;
            else                                              item.profile_id = db_->ensureDefaultEquipmentProfile();
            if (item.profile_id <= 0) continue;

            // A rejection here is almost always the one-machine-per-profile index
            // refusing a second machine. Local keeps what it has; the row is not
            // fatal to the rest of the batch.
            if (db_->upsertEquipmentItem(item) > 0) ++acc.applied_items;
            else                                    ++acc.kept_local;
        }
    }

    const std::string server_time = jstr(resp, "server_time");
    if (!server_time.empty()) state.cursor = server_time;

    // Advance only as far as the newest row actually pushed, so an edit made
    // during the round trip is picked up next time instead of being skipped.
    if (max_pushed >= 0) {
        char buf[32];
        std::time_t t = static_cast<std::time_t>(max_pushed);
        struct tm tm {};
        gmtime_r(&t, &tm);
        std::strftime(buf, sizeof(buf), "%Y-%m-%dT%H:%M:%SZ", &tm);
        new_watermark = buf;
    }
    acc.cursor = state.cursor;
    ++acc.passes;
    return true;
}

// ─────────────────────────────────────────────────────────────────────────────
// Public entry points
// ─────────────────────────────────────────────────────────────────────────────

CpapDashSyncService::Result CpapDashSyncService::syncNow() {
    Result r;
    if (!enabled()) {
        r.error = "cpapdash sync is disabled";
        return r;
    }
    if (!db_) {
        r.error = "cpapdash sync has no database";
        return r;
    }

    // Backfill first and unconditionally: a uuid is what makes a retried sync
    // update instead of duplicate, so it must exist before the first push even if
    // that push then fails. It changes nothing the user can see.
    r.uuids_backfilled = backfillUuids();

    State state = loadState();
    std::string watermark = state.pushed_through;

    for (int pass = 0; pass < kMaxPasses; ++pass) {
        bool needs_second = false;
        std::string next_watermark;
        const auto bindings_before = state.profile_ids;
        if (!exchange(watermark, state, r, needs_second, next_watermark)) {
            // Nothing written on this pass. State on disk is untouched when the
            // first pass fails, so the next attempt repeats exactly this work.
            if (pass > 0) { state.pushed_through = watermark; saveState(state); }
            r.ok = false;
            return r;
        }
        // Only worth a second pass if this response actually taught us a profile
        // binding we did not have; otherwise pass 2 would send the same unresolved
        // rows again and waste a round trip.
        if (!needs_second || state.profile_ids == bindings_before ||
            pass + 1 == kMaxPasses) {
            state.pushed_through = next_watermark;
            break;
        }
        // Re-push from the SAME watermark: pass 1 sent items whose profile had no
        // server id, so the cloud filed them under its default setup. The binding
        // learned above lets pass 2 move them to the right one.
    }

    saveState(state);
    dirty_ = false;
    r.ok = true;
    r.error.clear();
    return r;
}

void CpapDashSyncService::markDirty() {
    if (!enabled() || !settings_.auto_sync) return;
    dirty_ = true;
}

void CpapDashSyncService::sweep() {
    if (!dirty_ || !enabled() || !settings_.auto_sync) return;
    const Result r = syncNow();
    if (!r.ok) {
        // Stay dirty and try again on the next burst; local data is unaffected.
        std::cerr << "[cpapdash] " << r.error << " (staying local-only)" << std::endl;
        dirty_ = true;
    }
}

} // namespace hms_cpap
