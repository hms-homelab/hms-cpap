#include "services/SupplyPublisher.h"
#include "services/SupplyStatus.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <map>
#include <set>
#include <utility>

namespace hms_cpap {

namespace {

std::string toJson(const Json::Value& value) {
    Json::StreamWriterBuilder builder;
    builder["indentation"] = "";  // Compact JSON
    return Json::writeString(builder, value);
}

// Human label for an entity name: "Travel Setup Mask Days Left".
std::string titleize(const std::string& slug) {
    std::string out;
    bool at_word_start = true;
    for (char c : slug) {
        if (c == '_') {
            out += ' ';
            at_word_start = true;
            continue;
        }
        out += at_word_start ? static_cast<char>(std::toupper(static_cast<unsigned char>(c))) : c;
        at_word_start = false;
    }
    return out;
}

}  // namespace

SupplyPublisher::SupplyPublisher(PublishFn publish,
                                 std::string device_id,
                                 std::string device_name)
    : publish_(std::move(publish)),
      device_id_(std::move(device_id)),
      device_name_(std::move(device_name)) {
}

std::string SupplyPublisher::slugify(const std::string& text) {
    std::string out;
    bool pending_separator = false;
    for (char c : text) {
        unsigned char uc = static_cast<unsigned char>(c);
        if (std::isalnum(uc)) {
            if (pending_separator && !out.empty()) out += '_';
            pending_separator = false;
            out += static_cast<char>(std::tolower(uc));
        } else {
            pending_separator = true;  // collapse runs; never leaves a trailing '_'
        }
    }
    return out;
}

std::string SupplyPublisher::entityKey(const Entry& entry) {
    std::string profile = slugify(entry.profile_name);
    std::string type = slugify(entry.type_key);
    if (profile.empty()) return type;
    if (type.empty()) return profile;
    return profile + "_" + type;
}

std::vector<std::string> SupplyPublisher::entityKeys(const std::vector<Entry>& entries) {
    // Deterministic disambiguation: the lowest item id of a profile+type keeps the
    // clean key, so adding a second mask never renames the first one's entity.
    std::map<std::string, int> lowest_id;
    for (const auto& entry : entries) {
        std::string key = entityKey(entry);
        auto found = lowest_id.find(key);
        if (found == lowest_id.end() || entry.item_id < found->second) {
            lowest_id[key] = entry.item_id;
        }
    }

    std::vector<std::string> keys;
    keys.reserve(entries.size());
    std::set<std::string> used;
    for (const auto& entry : entries) {
        std::string key = entityKey(entry);
        if (lowest_id[key] != entry.item_id || used.count(key) > 0) {
            key += "_" + std::to_string(entry.item_id);
        }
        used.insert(key);
        keys.push_back(key);
    }
    return keys;
}

std::string SupplyPublisher::formatDate(long long epoch) {
    if (epoch <= 0) return "";
    std::time_t t = static_cast<std::time_t>(epoch);
    std::tm tm{};
    if (gmtime_r(&t, &tm) == nullptr) return "";
    char buf[16];
    if (std::strftime(buf, sizeof(buf), "%Y-%m-%d", &tm) == 0) return "";
    return std::string(buf);
}

std::vector<SupplyPublisher::Entry> SupplyPublisher::collect(IDatabase& db) {
    std::map<int, std::string> profile_names;
    for (const auto& profile : db.listEquipmentProfiles(false)) {
        profile_names[profile.id] = profile.name;
    }

    std::map<std::string, int> type_defaults;  // resolved once per type_key
    auto defaultFor = [&](const std::string& type_key) {
        auto found = type_defaults.find(type_key);
        if (found != type_defaults.end()) return found->second;
        auto type = db.resolveEquipmentType(type_key);
        int days = type ? type->default_replace_after_days : supplyDefaultDays(type_key);
        type_defaults[type_key] = days;
        return days;
    };

    std::vector<Entry> entries;
    for (const auto& item : db.listEquipmentItems(false)) {
        auto profile = profile_names.find(item.profile_id);
        if (profile == profile_names.end()) continue;  // orphan of a tombstoned profile

        Entry entry;
        entry.item_id       = item.id;
        entry.profile_name  = profile->second;
        entry.type_key      = item.type_key;
        entry.category      = item.category;
        entry.brand         = item.brand;
        entry.model         = item.model;
        entry.started_epoch = item.started_epoch;
        entry.interval_days = item.replace_after_days >= 0 ? item.replace_after_days
                                                           : defaultFor(item.type_key);
        entries.push_back(entry);
    }

    std::sort(entries.begin(), entries.end(),
              [](const Entry& a, const Entry& b) { return a.item_id < b.item_id; });
    return entries;
}

SupplyPublisher::Result SupplyPublisher::publishFromDatabase(IDatabase& db, long long now_epoch) {
    return publishSnapshot(collect(db), now_epoch);
}

Json::Value SupplyPublisher::buildDeviceInfo() const {
    // Same identifiers as DataPublisherService so HA groups supplies under the
    // existing CPAP device rather than creating a second one.
    Json::Value device;
    device["identifiers"].append(device_id_);
    device["name"] = device_name_;
    device["manufacturer"] = "ResMed";

    return device;
}

std::string SupplyPublisher::stateTopic(const std::string& leaf) const {
    return "cpap/" + device_id_ + "/supplies/" + leaf;
}

void SupplyPublisher::setLedger(LedgerIO io) { ledger_ = std::move(io); }

SupplyPublisher::LedgerIO SupplyPublisher::fileLedger(std::string path) {
    LedgerIO io;
    io.read = [path]() -> std::string {
        std::ifstream in(path);
        if (!in) return "";
        std::ostringstream ss;
        ss << in.rdbuf();
        return ss.str();
    };
    io.write = [path](const std::string& text) {
        std::error_code ec;
        std::filesystem::create_directories(
            std::filesystem::path(path).parent_path(), ec);
        // Write-then-rename: a torn ledger reads as empty and would replay every
        // event once. Cheap to avoid, annoying to receive at 3am.
        const std::string tmp = path + ".tmp";
        {
            std::ofstream out(tmp, std::ios::trunc);
            if (!out) return;
            out << text;
        }
        std::filesystem::rename(tmp, path, ec);
    };
    return io;
}

std::map<std::string, std::string> SupplyPublisher::loadLedger() const {
    std::map<std::string, std::string> states;
    if (!ledger_.read) return states;
    const std::string text = ledger_.read();
    if (text.empty()) return states;

    Json::Value root;
    Json::CharReaderBuilder builder;
    std::string errs;
    std::istringstream in(text);
    if (!Json::parseFromStream(builder, in, &root, &errs) || !root.isObject()) {
        return states;  // corrupt ledger: treat as fresh start, never throw
    }
    for (const auto& key : root.getMemberNames()) {
        if (root[key].isString()) states[key] = root[key].asString();
    }
    return states;
}

void SupplyPublisher::saveLedger(const std::map<std::string, std::string>& states) const {
    if (!ledger_.write) return;
    Json::Value root(Json::objectValue);
    for (const auto& [key, state] : states) root[key] = state;
    ledger_.write(toJson(root));
}

bool SupplyPublisher::sendEvent(const std::string& payload) const {
    if (!publish_) return true;
    // retain=false: an event describes a moment. Retaining it would replay the
    // crossing to every subscriber that reconnects, forever.
    return publish_("cpap/" + device_id_ + "/supplies/event", payload, false);
}

bool SupplyPublisher::send(const std::string& topic, const std::string& payload) const {
    if (!publish_) return false;
    return publish_(topic, payload, true);  // retained: HA must see supplies on restart
}

SupplyPublisher::Result SupplyPublisher::publishSnapshot(const std::vector<Entry>& entries,
                                                         long long now_epoch) {
    Result result;

    std::vector<std::string> keys = entityKeys(entries);
    Json::Value device = buildDeviceInfo();

    Json::Value due_items(Json::arrayValue);

    // What each tracked entity reads on THIS cycle, compared against the ledger
    // below to find crossings. Insertion-ordered by key so events are emitted
    // deterministically regardless of catalog order.
    struct Observed {
        std::string state, profile, type_key;
        int         days_left{0};
        std::string replace_by;
    };
    std::map<std::string, Observed> observed;

    last_events_.clear();

    for (size_t i = 0; i < entries.size(); ++i) {
        const Entry& entry = entries[i];
        const std::string& key = keys[i];

        SupplyStatus status =
            computeSupplyStatusForInterval(entry.started_epoch, entry.interval_days, now_epoch);

        // Machines and undated accessories have no wear clock. Publishing them
        // would create a sensor pinned at 0 forever, so skip them outright.
        if (status.state == SupplyState::Untracked) {
            ++result.skipped;
            continue;
        }

        const bool is_due =
            status.state == SupplyState::DueSoon || status.state == SupplyState::Overdue;

        int wear_percent = static_cast<int>(std::lround(status.wear_fraction * 100.0));
        wear_percent = std::max(0, std::min(100, wear_percent));

        const std::string attributes_topic = stateTopic(key + "/attributes");
        const std::string label = titleize(key);

        Json::Value attributes;
        attributes["profile"]    = entry.profile_name;
        attributes["type"]       = entry.type_key;
        attributes["brand"]      = entry.brand;
        attributes["model"]      = entry.model;
        attributes["replace_by"] = formatDate(status.replace_by_epoch);
        attributes["state"]      = supplyStateString(status.state);
        result.all_ok &= send(attributes_topic, toJson(attributes));

        // days_left sensor
        {
            const std::string leaf = key + "/days_left";
            Json::Value config;
            config["name"]                 = label + " Days Left";
            config["unique_id"]            = device_id_ + "_supply_" + key + "_days_left";
            config["state_topic"]          = stateTopic(leaf);
            config["json_attributes_topic"] = attributes_topic;
            config["unit_of_measurement"]  = "d";
            config["state_class"]          = "measurement";
            config["icon"]                 = "mdi:calendar-clock";
            config["device"]               = device;

            result.all_ok &= send("homeassistant/sensor/" + device_id_ + "/supply_" + key +
                                      "_days_left/config",
                                  toJson(config));
            result.all_ok &= send(stateTopic(leaf), std::to_string(status.days_left));
        }

        // wear_percent sensor
        {
            const std::string leaf = key + "/wear_percent";
            Json::Value config;
            config["name"]                 = label + " Wear";
            config["unique_id"]            = device_id_ + "_supply_" + key + "_wear_percent";
            config["state_topic"]          = stateTopic(leaf);
            config["json_attributes_topic"] = attributes_topic;
            config["unit_of_measurement"]  = "%";
            config["state_class"]          = "measurement";
            config["icon"]                 = "mdi:progress-wrench";
            config["device"]               = device;

            result.all_ok &= send("homeassistant/sensor/" + device_id_ + "/supply_" + key +
                                      "_wear_percent/config",
                                  toJson(config));
            result.all_ok &= send(stateTopic(leaf), std::to_string(wear_percent));
        }

        ++result.entities;
        observed[key] = {supplyStateString(status.state), entry.profile_name,
                         entry.type_key, status.days_left,
                         formatDate(status.replace_by_epoch)};
        if (is_due) {
            ++result.due;
            Json::Value due_entry;
            due_entry["entity"]     = key;
            due_entry["profile"]    = entry.profile_name;
            due_entry["type"]       = entry.type_key;
            due_entry["state"]      = supplyStateString(status.state);
            due_entry["days_left"]  = status.days_left;
            due_entry["replace_by"] = formatDate(status.replace_by_epoch);
            due_items.append(due_entry);
        }
    }

    // -- Crossings -----------------------------------------------------------
    // Compare this cycle against the last one and emit one event per entity
    // whose state actually moved. A first sighting counts as a crossing only if
    // it lands on due_soon/overdue: announcing a brand new, perfectly fresh mask
    // is noise, but a mask that is ALREADY overdue when first entered is exactly
    // what the user needs told.
    {
        std::map<std::string, std::string> previous = loadLedger();
        std::map<std::string, std::string> current;

        for (const auto& [key, obs] : observed) {
            current[key] = obs.state;

            auto prev = previous.find(key);
            const std::string from = prev == previous.end() ? "" : prev->second;
            if (from == obs.state) continue;                       // no crossing
            if (from.empty() && obs.state == "fresh") continue;    // new and fine

            Event ev;
            ev.entity_key = key;
            ev.profile    = obs.profile;
            ev.type_key   = obs.type_key;
            ev.from       = from;
            ev.to         = obs.state;
            ev.days_left  = obs.days_left;
            ev.replace_by = obs.replace_by;

            Json::Value payload;
            payload["entity"]     = ev.entity_key;
            payload["profile"]    = ev.profile;
            payload["type"]       = ev.type_key;
            payload["from"]       = ev.from;
            payload["state"]      = ev.to;
            payload["days_left"]  = ev.days_left;
            payload["replace_by"] = ev.replace_by;

            result.all_ok &= sendEvent(toJson(payload));
            last_events_.push_back(std::move(ev));
            ++result.events;
        }

        // Entities gone from the catalog drop out of the ledger with `current`,
        // so re-adding an item later is treated as a first sighting.
        saveLedger(current);
    }

    // Nothing tracked at all: stay silent rather than register a lone binary
    // sensor for a household that has not entered any equipment yet.
    if (result.entities == 0) {
        return result;
    }

    {
        const std::string attributes_topic = stateTopic("due/attributes");

        Json::Value attributes;
        attributes["due_count"]     = result.due;
        attributes["tracked_count"] = result.entities;
        attributes["items"]         = due_items;
        result.all_ok &= send(attributes_topic, toJson(attributes));

        Json::Value config;
        config["name"]                  = "CPAP Supplies Due";
        config["unique_id"]             = device_id_ + "_supplies_due";
        config["state_topic"]           = stateTopic("due");
        config["json_attributes_topic"] = attributes_topic;
        config["device_class"]          = "problem";
        config["icon"]                  = "mdi:autorenew";
        config["device"]                = device;

        result.all_ok &=
            send("homeassistant/binary_sensor/" + device_id_ + "/supplies_due/config",
                 toJson(config));
        result.all_ok &= send(stateTopic("due"), result.due > 0 ? "ON" : "OFF");
    }

    result.published = true;
    return result;
}

}  // namespace hms_cpap
