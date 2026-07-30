#include "services/CleaningPublisher.h"

#include "services/CleaningStatus.h"

#include <ctime>
#include <map>
#include <string>

namespace hms_cpap {

namespace {

std::string toJson(const Json::Value& v) {
    Json::StreamWriterBuilder w;
    w["indentation"] = "";
    return Json::writeString(w, v);
}

}  // namespace

CleaningPublisher::CleaningPublisher(PublishFn publish,
                                     std::string device_id,
                                     std::string device_name)
    : publish_(std::move(publish)),
      device_id_(std::move(device_id)),
      device_name_(std::move(device_name)) {}

std::string CleaningPublisher::entityKey(const Entry& entry) {
    // Id, not a slug of the label. A task the user renames must keep its entity
    // rather than orphaning the old one and quietly creating a second.
    return "task_" + std::to_string(entry.task_id);
}

std::string CleaningPublisher::formatDateTime(long long epoch) {
    if (epoch <= 0) return "";
    const std::time_t t = static_cast<std::time_t>(epoch);
    std::tm tm{};
#ifdef _WIN32
    gmtime_s(&tm, &t);
#else
    gmtime_r(&t, &tm);
#endif
    // gmtime, matching how cleaning timestamps are stored and rendered
    // everywhere else in this feature: the parsers read the ring's printed wall
    // clock as if it were UTC, so gmtime is what gives that same wall clock back.
    char buf[20];
    std::strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M", &tm);
    return buf;
}

std::string CleaningPublisher::stateTopic(const std::string& leaf) const {
    return "cpap/" + device_id_ + "/cleaning/" + leaf;
}

Json::Value CleaningPublisher::buildDeviceInfo() const {
    // Same identifiers as DataPublisherService and SupplyPublisher so HA groups
    // cleaning under the existing CPAP device rather than inventing a third one.
    Json::Value device;
    device["identifiers"].append(device_id_);
    device["name"] = device_name_;
    device["manufacturer"] = "ResMed";
    return device;
}

bool CleaningPublisher::send(const std::string& topic, const std::string& payload,
                             bool retain) const {
    if (!publish_) return false;
    return publish_(topic, payload, retain);
}

std::vector<CleaningPublisher::Entry> CleaningPublisher::collect(IDatabase& db) {
    // Profile names, so a household with a travel setup can tell its entities
    // apart in HA.
    std::map<int, std::string> profile_names;
    for (const auto& p : db.listEquipmentProfiles(/*include_deleted=*/true)) {
        profile_names[p.id] = p.name;
    }

    std::vector<Entry> out;
    // listCleaningTasks hides tombstones, which is right for the API and wrong
    // here: a deleted task still needs its entity cleared exactly once. Nothing
    // else in the codebase needs deleted rows, so rather than widen that method
    // the publisher tolerates them being absent and simply clears whatever it
    // still sees as disabled.
    for (const auto& t : db.listCleaningTasks(0)) {
        Entry e;
        e.task_id          = t.id;
        const auto pn      = profile_names.find(t.profile_id);
        e.profile_name     = (pn == profile_names.end()) ? "" : pn->second;
        e.task_key         = t.task_key;
        e.label            = t.label;
        e.interval_days    = t.interval_days;
        e.time_minutes     = t.time_minutes;
        e.start_date_epoch = t.start_date_epoch;
        e.last_done_epoch  = t.last_done_epoch;
        e.enabled          = t.enabled;
        e.deleted          = t.deleted;
        out.push_back(e);
    }
    return out;
}

CleaningPublisher::Result CleaningPublisher::publishSnapshot(
    const std::vector<Entry>& entries, long long now_epoch) {

    Result result;
    if (entries.empty()) return result;

    const Json::Value device = buildDeviceInfo();
    Json::Value due_tasks(Json::arrayValue);

    for (const auto& e : entries) {
        const std::string key = entityKey(e);
        const std::string state_leaf = key + "/state";
        const std::string attributes_topic = stateTopic(key + "/attributes");
        const std::string config_topic =
            "homeassistant/sensor/" + device_id_ + "/cleaning_" + key + "/config";

        // A task the user switched off, or deleted, must not leave a sensor
        // behind reporting a schedule that is no longer running. An EMPTY
        // retained payload on the discovery topic is how MQTT discovery removes
        // an entity; clearing the state and attributes too stops a stale value
        // reappearing if the entity is ever recreated.
        if (!e.enabled || e.deleted) {
            result.all_ok &= send(config_topic, "");
            result.all_ok &= send(stateTopic(state_leaf), "");
            result.all_ok &= send(attributes_topic, "");
            ++result.cleared;
            result.published = true;
            continue;
        }

        const auto status = computeCleaningStatus(e.start_date_epoch, e.interval_days,
                                                  e.time_minutes, e.last_done_epoch,
                                                  e.enabled, now_epoch);

        const bool is_due = status.state == CleaningState::Due;
        if (is_due) {
            ++result.due;
            due_tasks.append(e.label.empty() ? e.task_key : e.label);
        }

        Json::Value attributes;
        attributes["profile"]       = e.profile_name;
        attributes["task_key"]      = e.task_key;
        attributes["label"]         = e.label;
        attributes["interval_days"] = e.interval_days;
        attributes["days_until"]    = status.days_until;
        attributes["next_due"]      = formatDateTime(status.next_due_epoch);
        attributes["last_done"]     = formatDateTime(e.last_done_epoch);
        result.all_ok &= send(attributes_topic, toJson(attributes));

        Json::Value config;
        config["name"]                  = e.label.empty() ? e.task_key : e.label;
        config["unique_id"]             = device_id_ + "_cleaning_" + key;
        config["state_topic"]           = stateTopic(state_leaf);
        config["json_attributes_topic"] = attributes_topic;
        config["icon"]                  = "mdi:spray-bottle";
        config["device"]                = device;
        result.all_ok &= send(config_topic, toJson(config));

        // The state is the canonical lowercase string, shared with the API and
        // the cloud, so an automation written against one works against both.
        result.all_ok &= send(stateTopic(state_leaf), cleaningStateString(status.state));

        ++result.entities;
        result.published = true;
    }

    // One household-wide summary, so an automation can say "anything to clean?"
    // without subscribing to every task. Mirrors the supplies binary_sensor.
    {
        const std::string any_leaf = "any_due";
        const std::string attributes_topic = stateTopic(any_leaf + "/attributes");

        Json::Value attributes;
        attributes["tasks"] = due_tasks;
        attributes["count"] = result.due;
        result.all_ok &= send(attributes_topic, toJson(attributes));

        Json::Value config;
        config["name"]                  = "Cleaning Due";
        config["unique_id"]             = device_id_ + "_cleaning_any_due";
        config["state_topic"]           = stateTopic(any_leaf);
        config["json_attributes_topic"] = attributes_topic;
        config["payload_on"]            = "ON";
        config["payload_off"]           = "OFF";
        config["icon"]                  = "mdi:spray-bottle";
        config["device"]                = device;
        result.all_ok &= send(
            "homeassistant/binary_sensor/" + device_id_ + "/cleaning_any_due/config",
            toJson(config));

        result.all_ok &= send(stateTopic(any_leaf), result.due > 0 ? "ON" : "OFF");
        result.published = true;
    }

    return result;
}

CleaningPublisher::Result CleaningPublisher::publishFromDatabase(IDatabase& db,
                                                                 long long now_epoch) {
    return publishSnapshot(collect(db), now_epoch);
}

}  // namespace hms_cpap
