#pragma once
//
// CleaningPublisher (SDD-007) — publishes cleaning schedules to Home Assistant
// over MQTT, alongside SupplyPublisher.
//
// hms-cpap has no phone and no notification planner, so HA entities ARE the
// reminder mechanism. This is the one place SDD-007 deliberately does LESS than
// the cloud's SDD-043: that spec fires notifications from the app's scheduler,
// and this one publishes STATE and stops. When to be nudged is an automation the
// user writes, which is how the rest of their house already works and is
// strictly more flexible than a fixed alert.
//
// Each ENABLED task becomes one sensor reading due / upcoming, with the days and
// the next due date as attributes. A task that is disabled or soft-deleted
// publishes an EMPTY retained payload, which is how MQTT discovery removes an
// entity: leaving the last value retained would strand a sensor in HA reporting
// a schedule the user has switched off.
//
// Due state is COMPUTED here via computeCleaningStatus and never stored, exactly
// as supply wear is.
//
// Testability follows SupplyPublisher exactly: this class never touches an MQTT
// client, publishing through a caller-supplied sink so tests capture every
// topic and payload with no broker. The database read (collect) is split from
// the payload building (publishSnapshot) so the interesting logic needs no
// database either.
//
#include "database/IDatabase.h"

#include <json/json.h>

#include <functional>
#include <string>
#include <vector>

namespace hms_cpap {

class CleaningPublisher {
public:
    /// Sink for one MQTT message. Returns false if the message could not be sent.
    using PublishFn =
        std::function<bool(const std::string& topic, const std::string& payload, bool retain)>;

    /// One task joined with the name of the profile it belongs to.
    struct Entry {
        int         task_id{0};
        std::string profile_name;
        std::string task_key;
        std::string label;
        int         interval_days{0};
        int         time_minutes{510};
        long long   start_date_epoch{0};
        long long   last_done_epoch{0};   // 0 == never
        bool        enabled{false};
        bool        deleted{false};
    };

    struct Result {
        int  entities{0};       ///< enabled tasks published
        int  cleared{0};        ///< disabled/deleted tasks whose entity was removed
        int  due{0};            ///< enabled tasks currently due
        bool published{false};  ///< false when nothing at all was sent
        bool all_ok{true};      ///< false when the sink rejected any message
    };

    CleaningPublisher(PublishFn publish,
                      std::string device_id,
                      std::string device_name = "CPAP");

    /// Read the cleaning tables and publish. Convenience wrapper around
    /// collect() + publishSnapshot(). An empty schedule publishes nothing.
    Result publishFromDatabase(IDatabase& db, long long now_epoch);

    /// Join tasks to their profile names. Includes disabled and tombstoned rows,
    /// because publishSnapshot needs them in order to CLEAR their entities.
    static std::vector<Entry> collect(IDatabase& db);

    /// Build and send everything for one cycle.
    Result publishSnapshot(const std::vector<Entry>& entries, long long now_epoch);

    /// Stable per-entity key. Uses the task id rather than the label, because a
    /// user renaming a task must not orphan its entity and create a second one.
    static std::string entityKey(const Entry& entry);

    /// "YYYY-MM-DD HH:MM" for an epoch, or "" for 0.
    static std::string formatDateTime(long long epoch);

private:
    bool send(const std::string& topic, const std::string& payload, bool retain = true) const;
    std::string stateTopic(const std::string& leaf) const;
    Json::Value buildDeviceInfo() const;

    PublishFn   publish_;
    std::string device_id_;
    std::string device_name_;
};

}  // namespace hms_cpap
