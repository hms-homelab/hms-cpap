#pragma once
//
// SupplyPublisher — publishes SDD-004 supply wear to Home Assistant over MQTT.
//
// hms-cpap is self-hosted: there is no push infrastructure, so HA entities ARE
// the reminder mechanism. Each tracked accessory becomes two sensors (days left,
// wear percent) plus a single household-wide binary_sensor that is ON whenever
// anything is due_soon or overdue. Republished each burst cycle, so no new
// scheduler is needed.
//
// Wear is COMPUTED here via SupplyStatus and never stored (SDD-004). Machines and
// undated accessories are UNTRACKED and are deliberately skipped: an entity that
// always reads 0 is worse than no entity.
//
// Testability: this class never touches an MQTT client. It publishes through a
// caller-supplied sink, so tests capture every topic/payload with no broker
// running. The database read is likewise split out (collect) from the payload
// building (publishSnapshot) so the interesting logic needs no database either.
//
#include "database/IDatabase.h"

#include <json/json.h>

#include <functional>
#include <string>
#include <map>
#include <vector>

namespace hms_cpap {

class SupplyPublisher {
public:
    /// Sink for one MQTT message. Returns false if the message could not be sent.
    using PublishFn =
        std::function<bool(const std::string& topic, const std::string& payload, bool retain)>;

    /// One accessory joined with its profile and its effective replacement
    /// interval. Untracked entries are allowed here; publishSnapshot skips them.
    struct Entry {
        int         item_id{0};
        std::string profile_name;
        std::string type_key;
        std::string category;                // "machine" | "accessory"
        std::string brand;
        std::string model;
        long long   started_epoch{0};        // 0 == unset (untracked)
        int         interval_days{-1};       // effective interval; <= 0 == untracked
    };

    struct Result {
        int  entities{0};       ///< tracked accessories published
        int  skipped{0};        ///< untracked items deliberately not published
        int  due{0};            ///< tracked accessories due_soon or overdue
        int  events{0};         ///< edge-triggered transition events emitted
        bool published{false};  ///< false when nothing at all was sent
        bool all_ok{true};      ///< false when the sink rejected any message
    };

    // -- Transition events ---------------------------------------------------
    //
    // The sensors above are LEVEL: they say "this mask has -3 days left", and
    // they are retained, so a subscriber that connects late still sees the
    // truth. What they cannot say is "this mask just went overdue" — a retained
    // value looks identical on the cycle it changed and on the 500 cycles after
    // it. An automation built on level state either fires once and misses later
    // items or re-fires every burst cycle.
    //
    // So a state CHANGE also emits an event on a separate topic, NOT retained
    // (an event that replays on every reconnect is not an event). Edge-triggered:
    // one message per crossing, including the crossing back to fresh so an
    // automation can clear whatever it raised.
    //
    // Restarts must not re-fire, so the last published state per entity is
    // persisted. It lives in a sidecar file rather than the equipment tables on
    // purpose: it is local notification bookkeeping, and putting it in the DB
    // would sync it to the cloud, where it means nothing and would fight the
    // last-write-wins merge.

    /// One state crossing. `from` is empty the first time an entity is seen.
    struct Event {
        std::string entity_key;
        std::string profile;
        std::string type_key;
        std::string from;        ///< "", "fresh", "due_soon", "overdue"
        std::string to;          ///< "fresh", "due_soon", "overdue"
        int         days_left{0};
        std::string replace_by;  ///< "YYYY-MM-DD"
    };

    /// Read/write seam for the persisted state ledger. Tests inject memory;
    /// production injects fileLedger(). An unset ledger keeps events in-process
    /// only, which still suppresses per-cycle repeats within one run.
    struct LedgerIO {
        std::function<std::string()>            read;   ///< "" when absent
        std::function<void(const std::string&)> write;
    };

    void setLedger(LedgerIO io);

    /// Ledger backed by a JSON file. A missing or corrupt file reads as empty:
    /// losing the ledger costs at most one duplicate event per entity, so it is
    /// never worth failing a publish over.
    static LedgerIO fileLedger(std::string path);

    /// Events emitted by the most recent publishSnapshot, in entity order.
    const std::vector<Event>& lastEvents() const { return last_events_; }

    /// @param publish     message sink (never null in practice; a null sink no-ops)
    /// @param device_id   MQTT device id — MUST match DataPublisherService's
    ///                    CPAP_DEVICE_ID so the entities group under the same HA device
    /// @param device_name friendly device name shown in HA
    SupplyPublisher(PublishFn publish,
                    std::string device_id,
                    std::string device_name = "CPAP");

    /// Read the equipment catalog and publish. Convenience wrapper around
    /// collect() + publishSnapshot(). An empty catalog publishes nothing.
    Result publishFromDatabase(IDatabase& db, long long now_epoch);

    /// Publish a prepared snapshot. Pure with respect to the database.
    Result publishSnapshot(const std::vector<Entry>& entries, long long now_epoch);

    /// Join live profiles + items + type defaults. Performs no wear filtering:
    /// the tracked/untracked decision belongs to publishSnapshot.
    static std::vector<Entry> collect(IDatabase& db);

    /// Lowercase, underscore-separated, alnum-only slug. Stable forever: an entity
    /// id must not move when unrelated code changes ("Travel Setup" -> travel_setup).
    static std::string slugify(const std::string& text);

    /// Entity key for an entry: "<profile>_<type>". Callers that may hold two
    /// items of the same type in one profile use entityKeys() instead, which
    /// disambiguates deterministically.
    static std::string entityKey(const Entry& entry);

    /// Entity keys for a whole snapshot, in the same order as @p entries. The
    /// first item (by item id) of a given profile+type keeps the clean key; any
    /// further one gets "_<item_id>" appended, so keys stay unique and stable.
    static std::vector<std::string> entityKeys(const std::vector<Entry>& entries);

    /// "YYYY-MM-DD" in UTC; empty string for a non-positive epoch.
    static std::string formatDate(long long epoch);

private:
    bool send(const std::string& topic, const std::string& payload) const;
    bool sendEvent(const std::string& payload) const;
    std::string stateTopic(const std::string& leaf) const;
    Json::Value buildDeviceInfo() const;

    std::map<std::string, std::string> loadLedger() const;
    void saveLedger(const std::map<std::string, std::string>& states) const;

    PublishFn   publish_;
    std::string device_id_;
    std::string device_name_;
    LedgerIO    ledger_;
    std::vector<Event> last_events_;
};

}  // namespace hms_cpap
