#pragma once
//
// SDD-074: every device's clock setting, in one place the readers ask.
//
// Loaded from cpap_machine_clock at startup and changed by the Devices page
// (PUT /api/devices/clock). A device with no setting reads on this host's
// zone, as before. The O2 ring's "Ring clock" is the entry of its own device id
// (kOximetryDeviceId).
//
#include "utils/MachineZone.h"

#include <chrono>
#include <map>
#include <string>

namespace hms_cpap {

class IDatabase;

namespace machine_clocks {

/// The device's zone; the host's when it has no setting.
MachineZone zoneFor(const std::string& device_id);

/// Set (or, with the host zone, clear) one device's zone in memory.
void set(const std::string& device_id, const MachineZone& zone);

/// Forget every setting (tests).
void clear();

/// Every stored setting into memory; a setting that no longer parses (a zone
/// the OS dropped) is logged and read as the host.
void loadAll(IDatabase& db);

/// Store a setting and use it from now on. False when it is invalid (why) or
/// was not stored.
bool save(IDatabase& db, const std::string& device_id, const std::string& setting, std::string* why);

/// The machine's wall clock now, as a label (D7). host_now is a real instant.
long long nowWall(const std::string& device_id, long long host_now);
std::chrono::system_clock::time_point nowWall(const std::string& device_id,
                                              std::chrono::system_clock::time_point host_now);

/// A machine wall time (a label) as a real instant; a gap or a fold is logged.
long long instantOf(const std::string& device_id, long long wall_label);

/// The offset a session that starts at this label was read with (the
/// machine_offset_s column, D3 / open question 3).
long long offsetAtWall(const std::string& device_id, long long wall_label);

/// The device's settings, for the startup log (D11): device id -> setting.
std::map<std::string, std::string> all();

/// The device id this hms-cpap stores its CPAP nights under (config
/// device_id), for the readers that work below the device (the card's
/// folder discovery). "" until set: the host's zone.
void setPrimaryDevice(const std::string& device_id);
std::string primaryDevice();

/// SDD-074 decision 2: times are shown on the machine's clock (the default,
/// the stored text) or on this computer's. Set from config display_clock.
void setDisplayHost(bool host);
bool displayHost();

/// What to add to a stored machine wall time (a label) to show it on the
/// display clock: 0 on the machine's clock, else this computer's offset minus
/// the machine's at that instant.
long long displayShift(const std::string& device_id, long long wall_label);

}  // namespace machine_clocks

/// D5: a night closed by the clock ends at the machine's clock now, never
/// before its start, the same on every database engine.
inline std::chrono::system_clock::time_point machineEndNow(
    const std::string& device_id, std::chrono::system_clock::time_point session_start) {
    const auto now = machine_clocks::nowWall(device_id, std::chrono::system_clock::now());
    return now < session_start ? session_start : now;
}

}  // namespace hms_cpap
