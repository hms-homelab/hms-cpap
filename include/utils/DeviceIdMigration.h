#pragma once
//
// SDD-051: every install its own device id.
//
// The id used to default to a fixed value (a real machine's serial until
// 5.4.11, "cpapdash" in 5.4.12), so two instances on one broker that kept it
// published under the same topics and fought over one Home Assistant device.
// A first run now generates "cpapdash_" + 8 hex characters, and an install
// still on one of the old defaults is moved to a generated id once:
//
//   1. beginDeviceIdBackfill: the new id, and the old one as
//      device_id_previous, are written to config.json FIRST, so a crash
//      anywhere later resumes with the same new id instead of minting another.
//   2. IDatabase::moveDeviceId re-files the rows, in one transaction.
//   3. clearRetainedDevice empties the old id's retained Home Assistant
//      topics, so no ghost device is left replaying stale values.
//   4. device_id_previous is dropped.
//
// An id someone chose is pinned (device_id_pinned) and never moved.
//
#include "utils/AppConfig.h"
#include "mqtt_config.h"

#include <string>

namespace hms_cpap {

/// "cpapdash_" + 8 lowercase hex characters.
std::string generateDeviceId();

/// One of the fixed defaults an install could be on without having chosen it.
bool isOldDefaultDeviceId(const std::string& id);

/// Step 1. True when a move is under way after the call: one just begun (the
/// config now holds a new id and the old one as device_id_previous) or one an
/// earlier start left unfinished. The caller saves the config when it changed.
bool beginDeviceIdBackfill(AppConfig& config);

/// Step 3. Empties every retained topic under the old id on the broker
/// (homeassistant/<component>/<old>/... and cpap/<old>/...). False when the
/// broker could not be reached, so a later start tries again.
bool clearRetainedDevice(const hms::MqttConfig& mqtt, const std::string& old_id);

}  // namespace hms_cpap
