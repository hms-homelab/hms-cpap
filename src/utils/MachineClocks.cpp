// SDD-074: every device's clock setting (see include/utils/MachineClocks.h).
#include "utils/MachineClocks.h"

#include "database/IDatabase.h"
#include "utils/MachineClock.h"

#include <atomic>
#include <ctime>
#include <iostream>
#include <mutex>

namespace hms_cpap::machine_clocks {

namespace {

std::mutex g_mu;
std::map<std::string, MachineZone> g_zones;
std::string g_primary;

}  // namespace

MachineZone zoneFor(const std::string& device_id) {
    std::lock_guard<std::mutex> lk(g_mu);
    const auto it = g_zones.find(device_id);
    return it == g_zones.end() ? MachineZone::host() : it->second;
}

void set(const std::string& device_id, const MachineZone& zone) {
    std::lock_guard<std::mutex> lk(g_mu);
    if (zone.isHost()) g_zones.erase(device_id);
    else g_zones[device_id] = zone;
}

void clear() {
    std::lock_guard<std::mutex> lk(g_mu);
    g_zones.clear();
}

void loadAll(IDatabase& db) {
    for (const auto& [device, setting] : loadClockSettings(db)) {
        std::string why;
        if (auto z = MachineZone::parse(setting, &why)) {
            set(device, *z);
        } else {
            std::cerr << "Clock: " << device << " is set to \"" << setting << "\", which this computer "
                      << "cannot read (" << why << "); its times are read on this computer's clock"
                      << std::endl;
        }
    }
}

bool save(IDatabase& db, const std::string& device_id, const std::string& setting, std::string* why) {
    const auto z = MachineZone::parse(setting, why);
    if (!z) return false;
    if (!saveClockSetting(db, device_id, z->setting())) {
        if (why) *why = "The setting could not be stored";
        return false;
    }
    set(device_id, *z);
    return true;
}

long long nowWall(const std::string& device_id, long long host_now) {
    return zoneFor(device_id).toWall(host_now);
}

std::chrono::system_clock::time_point nowWall(const std::string& device_id,
                                              std::chrono::system_clock::time_point host_now) {
    return wall::point(nowWall(device_id, wall::seconds(host_now)));
}

long long instantOf(const std::string& device_id, long long wall_label) {
    MachineZone::Resolution how = MachineZone::Resolution::Exact;
    const auto zone = zoneFor(device_id);
    const long long u = zone.toInstant(wall_label, &how);
    if (how != MachineZone::Resolution::Exact && !zone.isHost())
        std::cout << "Clock: " << device_id << " " << wall::text(wall_label)
                  << (how == MachineZone::Resolution::Gap
                          ? " does not happen on the machine's clock (a clock change skips it); read as "
                          : " happens twice on the machine's clock; read as the first, ")
                  << wall::text(zone.toWall(u)) << std::endl;
    return u;
}

long long offsetAtWall(const std::string& device_id, long long wall_label) {
    const auto zone = zoneFor(device_id);
    return zone.offsetAt(zone.toInstant(wall_label));
}

void setPrimaryDevice(const std::string& device_id) {
    std::lock_guard<std::mutex> lk(g_mu);
    g_primary = device_id;
}

std::string primaryDevice() {
    std::lock_guard<std::mutex> lk(g_mu);
    return g_primary;
}

namespace {
std::atomic<bool> g_display_host{false};
}

void setDisplayHost(bool host) { g_display_host = host; }
bool displayHost() { return g_display_host; }

long long displayShift(const std::string& device_id, long long wall_label) {
    if (!displayHost()) return 0;
    const auto zone = zoneFor(device_id);
    if (zone.isHost()) return 0;
    const long long u = zone.toInstant(wall_label);
    return MachineZone::host().offsetAt(u) - zone.offsetAt(u);
}

std::map<std::string, std::string> all() {
    std::lock_guard<std::mutex> lk(g_mu);
    std::map<std::string, std::string> out;
    for (const auto& [d, z] : g_zones) out[d] = z.setting();
    return out;
}

}  // namespace hms_cpap::machine_clocks
