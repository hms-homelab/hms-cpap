#pragma once
//
// The local names and the forgotten devices of the Devices page, one file:
//
//   <data_dir>/devices.json
//   {"names":     {"sleepbridge/<sender>": "Bedroom bridge"},
//    "forgotten": {"mule/<serial>": 1762300000}}
//
// The key is "<kind>/<id>", the pair GET /api/devices returns. Written whole
// to a temporary file and renamed over the old one. A missing or unreadable
// file reads as empty: default names, nothing hidden. No table, so no SQL on
// any engine.
//
// A name is local: it is never sent to the device, the cloud or MQTT.
// Forgetting stops listing a device until it is heard from again after the
// moment it was forgotten; it deletes no data.
//
#include <json/json.h>

#include <ctime>
#include <mutex>
#include <optional>
#include <string>

namespace hms_cpap::devices {

/// Trimmed, 1 to 40 characters (code points, any language), no control
/// characters. Empty after trimming is valid: it clears the name. `why`
/// says what is wrong when it is not valid.
bool validName(const std::string& name, std::string* trimmed, std::string* why);

class DevicesStore {
public:
    /// `path` empty: kept in memory only.
    explicit DevicesStore(std::string path);

    /// The local name, or "" when none.
    std::string name(const std::string& kind, const std::string& id) const;
    /// Sets (or, with "", clears) the name. False with `why` when invalid.
    bool setName(const std::string& kind, const std::string& id, const std::string& name, std::string* why);

    /// Stops listing the device; its name goes too.
    void forget(const std::string& kind, const std::string& id, std::time_t at);
    /// When it was forgotten, if it is.
    std::optional<std::time_t> forgottenAt(const std::string& kind, const std::string& id) const;
    /// It was heard from again: listed as new.
    void unforget(const std::string& kind, const std::string& id);

    const std::string& path() const { return path_; }

private:
    void load();
    void save() const;   // mu_ held

    std::string        path_;
    mutable std::mutex mu_;
    Json::Value        names_{Json::objectValue};
    Json::Value        forgotten_{Json::objectValue};
};

}  // namespace hms_cpap::devices
