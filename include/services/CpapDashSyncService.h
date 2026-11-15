#pragma once
//
// CpapDashSyncService (SDD-004, phase 6) — OPTIONAL mirror of the local equipment
// tables to hms-cpapdash-api (`POST /v1/equipment/sync`).
//
// LOCAL IS THE SOURCE OF TRUTH. The cloud is a mirror the user opts into; every
// equipment feature works untouched with `cpapdash.enabled = false`, and a failed
// or unreachable cloud degrades to local-only with no data loss and no partial
// writes. Sync is a reconcile, never a replace.
//
// Auth is a pasted long-lived TOKEN (SDD-004 open question, option (b)): no cloud
// password at rest and the user can revoke it. Modelled on the SleepHQ
// integration, which is this repo's precedent for an opt-in external service with
// credentials, including its markDirty()/sweep() shape — the burst loop drives the
// work, so no new scheduler and no detached threads.
//
// Reconcile rules:
//   * every row carries a `client_uuid`; rows lacking one are backfilled BEFORE the
//     first push, which is what makes a retried sync idempotent instead of
//     duplicating rows,
//   * last-write-wins per row by `updated_at`, compared as EPOCHS (local writes
//     "...Z", the cloud writes a Postgres offset stamp — a string compare across
//     the two is wrong),
//   * a tie, or a timestamp either side cannot parse, is resolved in favour of
//     LOCAL: a cloud response must never delete or overwrite something the user
//     changed more recently,
//   * `deleted:true` rows are tombstones and are applied locally as tombstones,
//   * server ids are bound to local rows (by uuid) so later syncs update rather
//     than re-insert, and `server_time` is stored as the next cursor.
//
// The HTTP transport is injected so the whole reconcile is testable with NO
// network; the default transport is libcurl.
//
#include <functional>
#include <map>
#include <memory>
#include <string>

#include "database/IDatabase.h"

namespace hms_cpap {

class CpapDashSyncService {
public:
    // url, request body (JSON) -> response body in `out`; false = transport failure
    // (network down, non-2xx, no token). A false return must leave local data
    // exactly as it was.
    using Transport = std::function<bool(const std::string& url,
                                         const std::string& body,
                                         std::string& out)>;

    // Mirrors the config.json "cpapdash" block. Kept as its own struct rather than
    // reaching into AppConfig so this service has no build-order dependency on it;
    // main.cpp copies the four fields across.
    struct Settings {
        bool        enabled  = false;
        std::string api_url  = "https://api.cpapdash.com";
        std::string token;                  // long-lived, pasted; never a password
        bool        auto_sync = false;      // sync after local edits, on the burst sweep

        /// Build from the four config fields, in ONE place.
        ///
        /// SDD-006 section 5: main.cpp did this inline at startup and nothing
        /// re-applied it afterwards, so a token changed through PUT /api/config
        /// updated the file and the in-memory AppConfig and never reached the
        /// running service. The wizard's restart hid it; the Settings page does
        /// not restart, so it did not. Both call sites now go through here.
        static Settings from(bool enabled, const std::string& api_url,
                             const std::string& token, bool auto_sync) {
            Settings s;
            s.enabled = enabled;
            if (!api_url.empty()) s.api_url = api_url;
            s.token = token;
            s.auto_sync = auto_sync;
            return s;
        }
    };

    struct Result {
        bool        ok = false;
        std::string error;                  // empty when ok
        int  uuids_backfilled  = 0;
        int  pushed_profiles   = 0;
        int  pushed_items      = 0;
        int  applied_profiles  = 0;         // remote rows written locally
        int  applied_items     = 0;
        int  pushed_tasks      = 0;         // SDD-007 cleaning
        int  applied_tasks     = 0;
        int  tasks_held_back   = 0;         // profile not mirrored yet
        int  deleted_locally   = 0;         // remote tombstones applied
        int  kept_local        = 0;         // remote rows rejected: local was newer
        int  passes            = 0;
        std::string cursor;                 // cursor after this sync
    };

    CpapDashSyncService() = default;

    void setDatabase(std::shared_ptr<IDatabase> db) { db_ = std::move(db); }
    void setSettings(Settings s) { settings_ = std::move(s); }
    const Settings& settings() const { return settings_; }

    // Inject the HTTP transport (tests). Unset = libcurl against settings().api_url.
    void setTransport(Transport t) { transport_ = std::move(t); }

    // Where the cursor + server-id bindings live. Default: dataDir()/cpapdash_sync.json.
    void setStatePath(std::string p) { state_path_ = std::move(p); }
    std::string statePath() const;

    bool enabled() const { return settings_.enabled && !settings_.token.empty(); }

    // One full exchange: backfill uuids, push what changed, apply the response.
    // Safe to call when disabled (returns ok=false and touches nothing).
    Result syncNow();

    // SleepHQ-style debounce: a local edit marks the mirror stale, and the burst
    // loop's sweep() does the network work. No-op unless auto_sync is on.
    void markDirty();
    void sweep();
    bool isDirtyForTest() const { return dirty_; }

    // Cursor stored from the last successful sync ("" = never synced).
    std::string cursor() const;

    // RFC 4122 v4, lowercase hex with dashes.
    static std::string makeUuid();

    // Parse "YYYY-MM-DD[T ]HH:MM:SS[.frac][Z|±HH[:MM]]" to unix seconds.
    // -1 when unparseable — callers treat that as "unknown", which loses to local.
    static long long parseTimestampEpoch(const std::string& ts);

private:
    // Cursor + the uuid -> server id bindings. Persisted next to the database
    // rather than in it: the equipment tables are shared with two other agents'
    // backends, and sync state is this service's private bookkeeping.
    struct State {
        std::string cursor;                        // opaque server_time
        std::string pushed_through;                // local ISO watermark
        std::map<std::string, int> profile_ids;    // client_uuid -> server id
        std::map<std::string, int> item_ids;       // client_uuid -> server id
        std::map<std::string, int> task_ids;       // SDD-007 cleaning: uuid -> server id
        std::string cleaning_cursor;               // separate feed, separate cursor
        int default_profile_id = 0;                // server-side default setup
    };

    // One local item row INCLUDING tombstones. listEquipmentItems() hides
    // tombstones by contract, so local deletes would never reach the cloud
    // without this raw read.
    struct RawItem {
        int         id = 0;
        std::string client_uuid;
        std::string type_key;
        int         profile_id = 0;
        bool        deleted = false;
        std::string updated_at;
    };

    State loadState() const;
    bool  saveState(const State& s) const;

    std::vector<RawItem> rawItems() const;

    // Give every uuid-less row a uuid. Returns how many were written.
    int backfillUuids();

    // One push/apply round trip against `watermark`. `needs_second_pass` comes back
    // true when an item was pushed whose profile had no server binding yet: the
    // cloud folded it into its default setup, and the binding learned from this
    // response lets the very next pass put it where it belongs.
    bool exchange(const std::string& watermark, State& state, Result& acc,
                  bool& needs_second_pass, std::string& new_watermark);

    // `path` because there are two feeds now: equipment and cleaning each have
    // their own bulk endpoint and their own cursor.
    bool post(const std::string& path, const std::string& body, std::string& out) const;
    // SDD-007: the cleaning round trip. Runs AFTER the equipment loop so profile
    // server ids are already bound; a task whose profile has none yet is held
    // back rather than pushed with a dangling reference.
    bool exchangeCleaning(State& state, Result& acc);

    std::shared_ptr<IDatabase> db_;
    Settings    settings_;
    Transport   transport_;
    std::string state_path_;
    bool        dirty_ = false;
};

} // namespace hms_cpap
